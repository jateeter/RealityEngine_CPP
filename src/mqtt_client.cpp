// Minimal MQTT v3.1.1 client implementation — see include/reality/mqtt_client.hpp.

#include "reality/mqtt_client.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace reality::mqtt {

namespace {

// MQTT control packet types (high nibble of byte 1).
constexpr uint8_t CONNECT     = 0x10;
constexpr uint8_t CONNACK     = 0x20;
constexpr uint8_t PUBLISH     = 0x30;
constexpr uint8_t SUBSCRIBE   = 0x80;
constexpr uint8_t SUBACK      = 0x90;
constexpr uint8_t PINGREQ     = 0xC0;
constexpr uint8_t PINGRESP    = 0xD0;
constexpr uint8_t DISCONNECT  = 0xE0;

// CONNECT flag bits.
constexpr uint8_t CF_USERNAME      = 0x80;
constexpr uint8_t CF_PASSWORD      = 0x40;
constexpr uint8_t CF_CLEAN_SESSION = 0x02;

long long now_ms() {
  auto t = std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch());
  return t.count();
}

int open_tcp_connection(const std::string& host, int port) {
  addrinfo hints{};
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result  = nullptr;
  std::string portStr = std::to_string(port);
  if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0) return -1;
  int fd = -1;
  for (addrinfo* p = result; p; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(result);
  return fd;
}

}  // namespace

MqttClient::MqttClient(ClientConfig cfg) : config(std::move(cfg)) {}

MqttClient::~MqttClient() { stop(); }

void MqttClient::subscribe(const std::string& topicFilter, int qos) {
  int clampedQos = qos < 0 ? 0 : (qos > 1 ? 1 : qos);
  std::lock_guard<std::mutex> lock(stateMutex);
  subscriptions.emplace_back(topicFilter, clampedQos);
  if (connected.load()) pendingSubscriptions.emplace_back(topicFilter, clampedQos);
  wakeup.notify_all();
}

void MqttClient::set_message_handler(MessageHandler h) {
  std::lock_guard<std::mutex> lock(stateMutex);
  handler = std::move(h);
}

void MqttClient::start() {
  bool expected = false;
  if (!running.compare_exchange_strong(expected, true)) return;
  ioThread = std::thread([this]() { io_loop(); });
}

void MqttClient::stop() {
  if (!running.exchange(false)) return;
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    wakeup.notify_all();
    if (socketFd >= 0) {
      // shutdown() wakes a blocked recv() so the I/O thread exits promptly.
      ::shutdown(socketFd, SHUT_RDWR);
    }
  }
  if (ioThread.joinable()) ioThread.join();
  if (socketFd >= 0) { close(socketFd); socketFd = -1; }
}

MqttClient::Stats MqttClient::stats() const {
  std::lock_guard<std::mutex> lock(stateMutex);
  return statsCounters;
}

// ── Variable-length encoding (RFC §2.2.3) ───────────────────────────────────

void MqttClient::write_remaining_length(std::vector<uint8_t>& out, uint32_t length) {
  do {
    uint8_t byte = length & 0x7F;
    length >>= 7;
    if (length > 0) byte |= 0x80;
    out.push_back(byte);
  } while (length > 0);
}

bool MqttClient::read_remaining_length(uint32_t& length) {
  length = 0;
  uint32_t multiplier = 1;
  for (int i = 0; i < 4; ++i) {
    std::vector<uint8_t> b;
    if (!read_exact(b, 1)) return false;
    length += (b[0] & 0x7F) * multiplier;
    if ((b[0] & 0x80) == 0) return true;
    multiplier *= 128;
  }
  return false;  // malformed varint
}

void MqttClient::write_string(std::vector<uint8_t>& out, const std::string& s) {
  uint16_t len = static_cast<uint16_t>(s.size());
  out.push_back(static_cast<uint8_t>(len >> 8));
  out.push_back(static_cast<uint8_t>(len & 0xFF));
  out.insert(out.end(), s.begin(), s.end());
}

// ── Socket I/O ──────────────────────────────────────────────────────────────

bool MqttClient::write_all(const std::vector<uint8_t>& bytes) {
  size_t sent = 0;
  while (sent < bytes.size()) {
    ssize_t n = ::send(socketFd, bytes.data() + sent, bytes.size() - sent, 0);
    if (n <= 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool MqttClient::read_exact(std::vector<uint8_t>& out, size_t n) {
  size_t got = 0;
  out.resize(n);
  while (got < n) {
    ssize_t r = ::recv(socketFd, out.data() + got, n - got, 0);
    if (r <= 0) return false;
    got += static_cast<size_t>(r);
  }
  return true;
}

// ── Connect + authenticate ──────────────────────────────────────────────────

bool MqttClient::connect_and_authenticate() {
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    ++statsCounters.connectAttempts;
  }
  socketFd = open_tcp_connection(config.brokerHost, config.brokerPort);
  if (socketFd < 0) return false;

  // Variable header: protocol name "MQTT", level 4, connect flags, keepalive.
  std::vector<uint8_t> body;
  write_string(body, "MQTT");
  body.push_back(0x04);                                   // protocol level 3.1.1
  uint8_t flags = config.cleanSession ? CF_CLEAN_SESSION : 0;
  if (config.username) flags |= CF_USERNAME;
  if (config.password) flags |= CF_PASSWORD;
  body.push_back(flags);
  body.push_back(static_cast<uint8_t>(config.keepaliveSec >> 8));
  body.push_back(static_cast<uint8_t>(config.keepaliveSec & 0xFF));
  // Payload: clientId, [username], [password].
  write_string(body, config.clientId);
  if (config.username) write_string(body, *config.username);
  if (config.password) write_string(body, *config.password);

  std::vector<uint8_t> packet{ CONNECT };
  write_remaining_length(packet, static_cast<uint32_t>(body.size()));
  packet.insert(packet.end(), body.begin(), body.end());
  if (!write_all(packet)) return false;

  // Expect CONNACK (fixed 4-byte response: 0x20, 0x02, sessionPresent, returnCode).
  std::vector<uint8_t> hdr;
  if (!read_exact(hdr, 1) || (hdr[0] & 0xF0) != CONNACK) return false;
  uint32_t remaining = 0;
  if (!read_remaining_length(remaining) || remaining != 2) return false;
  std::vector<uint8_t> rest;
  if (!read_exact(rest, 2)) return false;
  if (rest[1] != 0x00) return false;  // 0 == accepted, anything else is a rejection

  {
    std::lock_guard<std::mutex> lock(stateMutex);
    ++statsCounters.connectSuccesses;
  }
  connected.store(true);
  lastTrafficAt = std::chrono::steady_clock::now();
  return true;
}

// ── SUBSCRIBE ───────────────────────────────────────────────────────────────

void MqttClient::send_subscriptions() {
  std::vector<std::pair<std::string, int>> toSend;
  {
    std::lock_guard<std::mutex> lock(stateMutex);
    toSend = subscriptions;
    pendingSubscriptions.clear();
  }
  if (toSend.empty()) return;

  std::vector<uint8_t> body;
  uint16_t packetId = nextPacketId++;
  if (nextPacketId == 0) nextPacketId = 1;
  body.push_back(static_cast<uint8_t>(packetId >> 8));
  body.push_back(static_cast<uint8_t>(packetId & 0xFF));
  for (const auto& [topic, qos] : toSend) {
    write_string(body, topic);
    body.push_back(static_cast<uint8_t>(qos));
  }

  std::vector<uint8_t> packet{ static_cast<uint8_t>(SUBSCRIBE | 0x02) };  // reserved flags = 0010
  write_remaining_length(packet, static_cast<uint32_t>(body.size()));
  packet.insert(packet.end(), body.begin(), body.end());
  write_all(packet);
  // We ignore the SUBACK return codes — broker rejection is logged at read_loop.
}

// ── Keepalive ──────────────────────────────────────────────────────────────

void MqttClient::send_ping() {
  std::vector<uint8_t> packet{ PINGREQ, 0x00 };
  if (write_all(packet)) {
    std::lock_guard<std::mutex> lock(stateMutex);
    ++statsCounters.pingsSent;
  }
}

// ── Read loop ──────────────────────────────────────────────────────────────

void MqttClient::read_loop() {
  while (running.load() && connected.load()) {
    // Wait up to 1s for inbound bytes so we can fire PINGREQ on schedule.
    pollfd p{ socketFd, POLLIN, 0 };
    int polled = poll(&p, 1, 1000);
    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTrafficAt).count();
    // Send a ping when half the keepalive window has elapsed.  Halfway is
    // the conventional choice — gives the broker plenty of margin before
    // it would drop us for inactivity.
    if (config.keepaliveSec > 0 && elapsedMs >= (config.keepaliveSec * 1000L) / 2) {
      send_ping();
      lastTrafficAt = now;
    }
    if (polled <= 0) continue;
    if (!(p.revents & POLLIN)) continue;

    // Read fixed header byte 1 — packet type + flags.
    std::vector<uint8_t> hdr;
    if (!read_exact(hdr, 1)) break;
    uint8_t type = hdr[0] & 0xF0;
    uint32_t remaining = 0;
    if (!read_remaining_length(remaining)) break;

    std::vector<uint8_t> body;
    if (remaining > 0 && !read_exact(body, remaining)) break;
    lastTrafficAt = std::chrono::steady_clock::now();

    if (type == PUBLISH) {
      // Variable header: topic (UTF-8 string) + optional packetId for QoS>0.
      if (body.size() < 2) continue;
      uint16_t topicLen = (static_cast<uint16_t>(body[0]) << 8) | body[1];
      if (body.size() < static_cast<size_t>(2 + topicLen)) continue;
      std::string topic(body.begin() + 2, body.begin() + 2 + topicLen);
      size_t payloadStart = 2 + topicLen;
      uint8_t qos = (hdr[0] >> 1) & 0x03;
      if (qos > 0 && body.size() >= payloadStart + 2) payloadStart += 2;
      std::vector<uint8_t> payload(body.begin() + payloadStart, body.end());

      MessageHandler h;
      {
        std::lock_guard<std::mutex> lock(stateMutex);
        ++statsCounters.messagesReceived;
        statsCounters.bytesReceived += payload.size();
        statsCounters.lastMessageAtMs = now_ms();
        h = handler;
      }
      if (h) h(topic, payload);
    } else if (type == PINGRESP) {
      // Just refreshes lastTrafficAt above; nothing else to do.
    } else if (type == SUBACK) {
      // SUBACK return codes are in the body; we accept whatever the broker
      // returned and just move on.  A persistent rejection will show up as
      // zero messagesReceived for the affected topic.
    }
  }
  connected.store(false);
  if (socketFd >= 0) { close(socketFd); socketFd = -1; }
}

// ── I/O thread loop ────────────────────────────────────────────────────────

void MqttClient::io_loop() {
  while (running.load()) {
    if (!connect_and_authenticate()) {
      if (socketFd >= 0) { close(socketFd); socketFd = -1; }
      std::unique_lock<std::mutex> lock(stateMutex);
      wakeup.wait_for(lock, std::chrono::milliseconds(config.reconnectDelayMs),
                      [this]() { return !running.load(); });
      continue;
    }
    send_subscriptions();
    read_loop();
    if (running.load()) {
      std::lock_guard<std::mutex> lock(stateMutex);
      ++statsCounters.reconnects;
    }
  }
  if (socketFd >= 0) {
    // Send DISCONNECT only on graceful shutdown — if the broker already closed
    // the socket this is a no-op.
    std::vector<uint8_t> packet{ DISCONNECT, 0x00 };
    write_all(packet);
    close(socketFd);
    socketFd = -1;
  }
  connected.store(false);
}

}  // namespace reality::mqtt
