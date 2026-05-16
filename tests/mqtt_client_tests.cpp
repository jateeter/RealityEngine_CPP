// MqttClient tests — drives the client against an in-process mock broker.
//
// The mock broker accepts exactly one TCP connection, handles MQTT v3.1.1
// CONNECT / SUBSCRIBE / PINGREQ from the client and lets the test push
// PUBLISH frames into it.  Just enough to verify the client's framing,
// variable-length encoding, and message-dispatch path.

#include "reality/mqtt_client.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace mq = reality::mqtt;

namespace {

int failures = 0;

#define EXPECT(cond, label)                                              \
  do { if (!(cond)) {                                                    \
    std::cerr << "FAIL: " << label << " (" << __FILE__ << ":"           \
              << __LINE__ << ")\n";                                      \
    ++failures;                                                          \
  } } while (0)

// ── Minimal in-process MQTT broker ──────────────────────────────────────────

class MockBroker {
public:
  MockBroker() {
    serverFd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ::bind(serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::listen(serverFd, 1);
    socklen_t len = sizeof(addr);
    ::getsockname(serverFd, reinterpret_cast<sockaddr*>(&addr), &len);
    port = ntohs(addr.sin_port);
    acceptThread = std::thread([this]() { accept_loop(); });
  }

  ~MockBroker() {
    running = false;
    if (serverFd >= 0) { ::shutdown(serverFd, SHUT_RDWR); ::close(serverFd); serverFd = -1; }
    if (clientFd >= 0) { ::shutdown(clientFd, SHUT_RDWR); ::close(clientFd); clientFd = -1; }
    if (acceptThread.joinable()) acceptThread.join();
  }

  int get_port() const { return port; }

  // Publish a PUBLISH frame to the connected client.
  bool publish(const std::string& topic, const std::string& payload) {
    if (clientFd < 0) return false;
    std::vector<uint8_t> body;
    uint16_t tl = topic.size();
    body.push_back(static_cast<uint8_t>(tl >> 8));
    body.push_back(static_cast<uint8_t>(tl & 0xFF));
    body.insert(body.end(), topic.begin(), topic.end());
    body.insert(body.end(), payload.begin(), payload.end());
    std::vector<uint8_t> packet{ 0x30 };  // PUBLISH, qos=0, dup=0, retain=0
    write_remaining_length(packet, body.size());
    packet.insert(packet.end(), body.begin(), body.end());
    return write_all(packet);
  }

  bool wait_for_client(int timeoutMs = 2000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
      if (clientConnected.load()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }

  int connect_count() const { return connectCount.load(); }
  int subscribe_count() const { return subscribeCount.load(); }
  int ping_count() const { return pingCount.load(); }

private:
  void accept_loop() {
    while (running.load()) {
      sockaddr_in cli{};
      socklen_t cliLen = sizeof(cli);
      int fd = ::accept(serverFd, reinterpret_cast<sockaddr*>(&cli), &cliLen);
      if (fd < 0) return;
      clientFd = fd;
      handle_client();
      ::close(clientFd);
      clientFd = -1;
      clientConnected = false;
    }
  }

  void handle_client() {
    // Drain CONNECT frame.
    if (!read_packet()) return;
    if ((lastPacket[0] & 0xF0) != 0x10) return;
    ++connectCount;
    // Respond with CONNACK: 0x20, 0x02, 0x00, 0x00.
    std::vector<uint8_t> connack{ 0x20, 0x02, 0x00, 0x00 };
    if (!write_all(connack)) return;
    clientConnected = true;

    while (running.load()) {
      if (!read_packet()) return;
      uint8_t type = lastPacket[0] & 0xF0;
      if (type == 0x80) {
        ++subscribeCount;
        // Respond with SUBACK echoing the packet ID + a single 0x00 return code.
        // (We don't bother parsing the full subscription list — the test only
        // sends one filter at a time.)
        uint16_t pid = (static_cast<uint16_t>(lastPacket[2]) << 8) | lastPacket[3];
        std::vector<uint8_t> suback{ 0x90, 0x03,
                                     static_cast<uint8_t>(pid >> 8),
                                     static_cast<uint8_t>(pid & 0xFF),
                                     0x00 };
        write_all(suback);
      } else if (type == 0xC0) {
        ++pingCount;
        std::vector<uint8_t> pingresp{ 0xD0, 0x00 };
        write_all(pingresp);
      } else if (type == 0xE0) {
        return;
      }
    }
  }

  bool read_packet() {
    lastPacket.clear();
    uint8_t hdr;
    ssize_t n = ::recv(clientFd, &hdr, 1, 0);
    if (n <= 0) return false;
    lastPacket.push_back(hdr);
    uint32_t remaining = 0;
    uint32_t mult = 1;
    for (int i = 0; i < 4; ++i) {
      uint8_t b;
      n = ::recv(clientFd, &b, 1, 0);
      if (n <= 0) return false;
      lastPacket.push_back(b);
      remaining += (b & 0x7F) * mult;
      if ((b & 0x80) == 0) break;
      mult *= 128;
    }
    while (remaining > 0) {
      uint8_t buf[256];
      size_t take = remaining < sizeof(buf) ? remaining : sizeof(buf);
      n = ::recv(clientFd, buf, take, 0);
      if (n <= 0) return false;
      lastPacket.insert(lastPacket.end(), buf, buf + n);
      remaining -= static_cast<uint32_t>(n);
    }
    return true;
  }

  bool write_all(const std::vector<uint8_t>& bytes) {
    size_t sent = 0;
    while (sent < bytes.size()) {
      ssize_t n = ::send(clientFd, bytes.data() + sent, bytes.size() - sent, 0);
      if (n <= 0) return false;
      sent += static_cast<size_t>(n);
    }
    return true;
  }

  void write_remaining_length(std::vector<uint8_t>& out, uint32_t length) {
    do {
      uint8_t byte = length & 0x7F;
      length >>= 7;
      if (length > 0) byte |= 0x80;
      out.push_back(byte);
    } while (length > 0);
  }

  int serverFd = -1;
  int clientFd = -1;
  int port = 0;
  std::atomic<bool> running { true };
  std::atomic<bool> clientConnected { false };
  std::atomic<int> connectCount { 0 };
  std::atomic<int> subscribeCount { 0 };
  std::atomic<int> pingCount { 0 };
  std::thread acceptThread;
  std::vector<uint8_t> lastPacket;
};

// ── Tests ──────────────────────────────────────────────────────────────────

void test_connect_subscribe_publish() {
  MockBroker broker;
  mq::ClientConfig cfg;
  cfg.brokerHost = "127.0.0.1";
  cfg.brokerPort = broker.get_port();
  cfg.clientId = "test-client";
  cfg.keepaliveSec = 60;
  cfg.reconnectDelayMs = 100;

  mq::MqttClient client(cfg);
  std::vector<std::string> received_topics;
  std::vector<std::string> received_payloads;
  std::mutex msgMutex;
  std::condition_variable msgCv;
  client.set_message_handler([&](const std::string& topic, const std::vector<uint8_t>& payload) {
    std::lock_guard<std::mutex> lock(msgMutex);
    received_topics.push_back(topic);
    received_payloads.emplace_back(payload.begin(), payload.end());
    msgCv.notify_all();
  });
  client.subscribe("sensors/temp", 0);
  client.start();

  EXPECT(broker.wait_for_client(2000), "broker accepts client connection");
  // Wait a moment for SUBSCRIBE to land before publishing.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT(broker.subscribe_count() >= 1, "client sends SUBSCRIBE on connect");

  EXPECT(broker.publish("sensors/temp", "21.5"), "broker publishes test message");
  {
    std::unique_lock<std::mutex> lock(msgMutex);
    msgCv.wait_for(lock, std::chrono::milliseconds(2000),
                   [&]() { return !received_topics.empty(); });
  }
  EXPECT(received_topics.size() == 1, "client received exactly one message");
  if (!received_topics.empty()) {
    EXPECT(received_topics[0] == "sensors/temp", "topic matches");
    EXPECT(received_payloads[0] == "21.5", "payload bytes intact");
  }

  client.stop();
}

}  // namespace

int main() {
  test_connect_subscribe_publish();
  if (failures > 0) {
    std::cerr << "\n" << failures << " MQTT client/bridge assertion(s) failed.\n";
    return 1;
  }
  std::cout << "MQTT client/bridge tests: OK\n";
  return 0;
}
