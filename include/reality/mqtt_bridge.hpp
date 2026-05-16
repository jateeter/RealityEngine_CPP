#pragma once

// MqttBridge — ingest path that turns MQTT PUBLISH messages into Perception
// Engine signal writes.
//
// The bridge holds an MqttClient and a topic→region map.  When a PUBLISH
// arrives on a subscribed topic, the bridge decodes the payload, applies
// the codec (default: comma-separated ASCII floats), and forwards it to a
// caller-supplied signal-ingest callback that mirrors the body of the
// PerceptionService's existing POST /api/signals endpoint.
//
// Lifecycle: created and started by PerceptionService when MQTT_BROKER_HOST
// is set in the environment; destroyed (and joined) by ~PerceptionService.
// Disabled when no broker is configured — the bridge object is simply not
// constructed.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "reality/mqtt_client.hpp"
#include "reality/json.hpp"

namespace reality {
using Vector = std::vector<double>;
}

namespace reality::mqtt {

// One entry of the topic→region map.  Codec is "csv-float" (default) or
// "single-float" (a single float per message, payload is its ASCII repr).
struct TopicBinding {
  std::string topic;
  std::string sensorId;
  int         offset = 0;
  int         length = 1;
  std::string codec  = "csv-float";
};

// Signal-ingest callback signature.  The bridge calls this on the MQTT I/O
// thread; the implementation is expected to take whatever lock the
// PerceptionService needs and then call its existing ingest path.
using IngestCallback = std::function<void(const std::string& sensorId,
                                          int offset, int length,
                                          const reality::Vector& values,
                                          const std::string& topic)>;

class MqttBridge {
public:
  MqttBridge(ClientConfig clientConfig,
             std::vector<TopicBinding> bindings,
             IngestCallback ingest);
  ~MqttBridge();

  MqttBridge(const MqttBridge&)            = delete;
  MqttBridge& operator=(const MqttBridge&) = delete;

  void start();
  void stop();

  // Read-side stats for the /api/mqtt/status endpoint.
  json::Value status_json() const;

  // Static helpers — parse env config into a ClientConfig + bindings list.
  // Returns nullopt when MQTT_BROKER_HOST is unset or empty (the disabled
  // case).  All MQTT_* env vars are read here so callers don't sprinkle
  // getenv() throughout the service.
  static std::optional<std::pair<ClientConfig, std::vector<TopicBinding>>>
    from_environment();

private:
  void on_message(const std::string& topic, const std::vector<uint8_t>& payload);
  Vector decode_payload(const std::vector<uint8_t>& payload, const std::string& codec) const;

  ClientConfig clientConfig;
  std::vector<TopicBinding> bindings;
  IngestCallback ingest;
  std::unique_ptr<MqttClient> client;
};

}  // namespace reality::mqtt
