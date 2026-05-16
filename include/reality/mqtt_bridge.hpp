#pragma once

// MqttBridge — wires an MqttClient + MappingRegistry to the Perception
// Engine's signal-ingest path.  The bridge does NOT special-case MQTT
// downstream of decode — every accepted message resolves to
// {sensorId, region, values, ttlMs} and is fed into the same callback the
// HTTP POST /api/signals path uses.  See mqtt_mapping.hpp for the rule
// schema (topic filter, sensorIdTemplate, extract, normalize, push policy).
//
// Lifecycle: PerceptionService constructs the bridge when MQTT_BROKER_HOST
// is set in the environment and a mapping file resolves; ~PerceptionService
// stops the bridge before joining the rest of its workers.  The bridge is
// disabled (and the object simply not constructed) otherwise.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "reality/json.hpp"
#include "reality/mqtt_client.hpp"
#include "reality/mqtt_mapping.hpp"

namespace reality::mqtt {

// Ingest callback signature — the bridge calls this on the MQTT I/O thread
// once a message has passed extraction + normalization + validation.
// PerceptionService binds this to its existing sensor-ingest path.
using IngestCallback = std::function<void(const std::string& sensorId,
                                          int offset, int length,
                                          const reality::Vector& values,
                                          long ttlMs,
                                          const std::string& topic,
                                          const std::string& mappingId)>;

// Push-trigger callback signature — invoked by the bridge after a debounce
// window settles (or immediately when pushMode==Immediate).  PerceptionService
// binds this to its async push helper.
using PushTrigger = std::function<void()>;

class MqttBridge {
public:
  // The bridge takes ownership of the mapping registry; metrics on the
  // registry are mutated as messages arrive and read by /api/mqtt/status.
  MqttBridge(ClientConfig clientConfig,
             std::unique_ptr<MappingRegistry> registry,
             IngestCallback ingest,
             PushTrigger pushTrigger);
  ~MqttBridge();

  MqttBridge(const MqttBridge&)            = delete;
  MqttBridge& operator=(const MqttBridge&) = delete;

  void start();
  void stop();

  // Status snapshot — connection state + bridge-level counters.
  // Mapping-level counters live on the registry; combine them at the
  // call site for the /api/mqtt/status response.
  struct BridgeStats {
    uint64_t messagesReceived = 0;     // PUBLISH frames decoded by the client
    uint64_t messagesMapped   = 0;     // matched a rule
    uint64_t messagesRejected = 0;     // failed extract / normalize / length
    uint64_t messagesUnmatched = 0;    // no mapping rule matched the topic
    uint64_t messagesRetainedDropped = 0;
    uint64_t pushesTriggered = 0;
  };
  BridgeStats stats() const;

  // Read-only view of the registry — owned by the bridge but exposed for
  // the /api/mqtt/mappings endpoint.
  const MappingRegistry& registry() const { return *registry_; }
  MqttClient::Stats client_stats() const { return client_->stats(); }
  bool is_connected() const { return client_ && client_->is_connected(); }
  const ClientConfig& config() const { return clientConfig_; }

  // Env-driven config loader.  Returns nullopt when MQTT_BROKER_HOST is
  // unset, when the mappings file/JSON can't be parsed, or when the parsed
  // registry is empty.  All MQTT_* env vars are read here so callers don't
  // sprinkle getenv() through the service.  Supports either MQTT_MAPPINGS_FILE
  // (path) or MQTT_MAPPINGS_JSON (inline), with a backward-compatible fallback
  // to the legacy MQTT_TOPIC_MAP shape.
  struct EnvConfig {
    ClientConfig                       client;
    std::unique_ptr<MappingRegistry>   registry;
    bool                               allowOverlap = false;
  };
  static std::optional<EnvConfig> from_environment();

  // Driven by tests (no real broker required): feed a synthetic PUBLISH
  // payload through the full mapping pipeline.  Used by the in-process
  // dispatcher test to validate end-to-end mapping flow.
  void inject_message(const std::string& topic, const std::vector<uint8_t>& payload);

private:
  void on_message(const std::string& topic, const std::vector<uint8_t>& payload);
  void note_error(MappingMetrics& m, const std::string& topic, const std::string& err);
  void debounce_loop();
  void schedule_push(long debounceMs);

  ClientConfig clientConfig_;
  std::unique_ptr<MappingRegistry> registry_;
  IngestCallback ingest_;
  PushTrigger pushTrigger_;
  std::unique_ptr<MqttClient> client_;

  // Debounce coordination.  When a message lands with pushMode=Debounced,
  // we update `nextPushAtMs_` to (now + debounceMs).  The debounce thread
  // sleeps until that deadline; if no further updates have landed, fires
  // the push.  pushMode=Immediate triggers the push synchronously.
  std::thread       debounceThread_;
  std::mutex        debounceMutex_;
  std::condition_variable debounceCv_;
  std::atomic<bool> debounceRunning_ { false };
  std::atomic<long long> nextPushAtMs_ { 0 };

  // Bridge-level counters.  Per-mapping counters live on the registry.
  mutable std::mutex statsMutex_;
  BridgeStats stats_;
};

}  // namespace reality::mqtt
