#pragma once

// Minimal MQTT v3.1.1 client (RFC: oasis-mqtt-v3.1.1-os).
//
// Implements just enough of the protocol to act as a one-way ingest path for
// the Perception Engine: CONNECT, SUBSCRIBE to a fixed set of topics, receive
// PUBLISH messages, send PINGREQ on a keepalive timer.  No PUBLISH from
// client side, no QoS > 1, no retained-message replay tracking, no TLS.
//
// Why hand-roll the protocol instead of pulling in mosquitto/Paho:
//   1. RealityEngine_CPP holds the line at zero external runtime deps —
//      Boost is a build-time fallback only.
//   2. The PE only needs ingest; the framing logic is roughly 200 lines.
//   3. Vendoring a C library would force a Cmake / package-manager step
//      that violates the Makefile-only build contract.
//
// Threading: one I/O thread per client.  start() launches it; stop() joins.
// on_message callbacks run on the I/O thread — callers must not block.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace reality::mqtt {

struct ClientConfig {
  std::string brokerHost   = "localhost";
  int         brokerPort   = 1883;
  std::string clientId     = "reality-engine-pe";
  std::optional<std::string> username;        // omitted => no auth section
  std::optional<std::string> password;
  int         keepaliveSec = 60;              // PINGREQ cadence
  int         reconnectDelayMs = 2000;        // wait between reconnect attempts
  bool        cleanSession = true;
};

// Message handler signature: (topic, payload_bytes).  Payload is opaque —
// codec interpretation lives in the bridge that owns the MqttClient.
using MessageHandler = std::function<void(const std::string& topic,
                                          const std::vector<uint8_t>& payload)>;

class MqttClient {
public:
  explicit MqttClient(ClientConfig config);
  ~MqttClient();

  MqttClient(const MqttClient&)            = delete;
  MqttClient& operator=(const MqttClient&) = delete;

  // Add a topic to the subscription list.  Effective on the next reconnect;
  // if already connected, sends an immediate SUBSCRIBE.  qos clamped to 0..1.
  void subscribe(const std::string& topicFilter, int qos = 0);

  // Register the handler invoked for every PUBLISH arriving on a subscribed
  // topic.  Single handler — bridge dispatches by topic itself.
  void set_message_handler(MessageHandler handler);

  // Launch the I/O thread.  Returns immediately; the thread connects in the
  // background and retries on failure.  Idempotent.
  void start();

  // Signal the I/O thread to disconnect cleanly and join it.  Idempotent.
  void stop();

  bool is_connected() const { return connected.load(); }

  // Stats — useful for /api/mqtt/status endpoints + Prometheus gauges.
  struct Stats {
    uint64_t connectAttempts = 0;
    uint64_t connectSuccesses = 0;
    uint64_t messagesReceived = 0;
    uint64_t bytesReceived    = 0;
    uint64_t pingsSent        = 0;
    uint64_t reconnects       = 0;
    long long lastMessageAtMs = 0;
  };
  Stats stats() const;

private:
  void io_loop();
  bool connect_and_authenticate();
  void send_subscriptions();
  void read_loop();
  void send_ping();

  // Wire helpers — defined in mqtt_client.cpp.
  void write_remaining_length(std::vector<uint8_t>& out, uint32_t length);
  bool read_remaining_length(uint32_t& length);
  void write_string(std::vector<uint8_t>& out, const std::string& s);
  bool write_all(const std::vector<uint8_t>& bytes);
  bool read_exact(std::vector<uint8_t>& out, size_t n);

  ClientConfig config;
  std::thread ioThread;
  std::atomic<bool> running { false };
  std::atomic<bool> connected { false };
  int socketFd = -1;

  mutable std::mutex stateMutex;
  std::condition_variable wakeup;
  std::vector<std::pair<std::string, int>> subscriptions;
  std::vector<std::pair<std::string, int>> pendingSubscriptions;
  MessageHandler handler;
  uint16_t nextPacketId = 1;
  mutable Stats statsCounters;
  std::chrono::steady_clock::time_point lastTrafficAt;
};

}  // namespace reality::mqtt
