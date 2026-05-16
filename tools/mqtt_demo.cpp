// mqtt_demo — drive the MqttBridge against a live broker + a mappings file,
// print every ingested {sensorId, region, values, ttlMs} as the bridge
// would feed it into the Perception Engine.  No PE/RE required — this is
// the same on_message → extract → normalize → ingest pipeline the
// PerceptionService runs, isolated so the demonstration is clear.
//
// Usage:
//   mqtt_demo <host> <port> <mappings.json> [seconds=30]

#include "reality/mqtt_bridge.hpp"
#include "reality/mqtt_client.hpp"
#include "reality/mqtt_mapping.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace mq = reality::mqtt;

namespace {

struct SensorSnapshot {
  std::string sensorId;
  std::string topic;
  std::string mappingId;
  int offset = 0;
  int length = 0;
  std::vector<double> lastValue;
  long long lastUpdatedMs = 0;
  long long ttlMs = 0;
  long long updates = 0;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "usage: " << argv[0]
              << " <host> <port> <mappings.json> [seconds=30]\n";
    return 2;
  }
  std::string host = argv[1];
  int port = std::atoi(argv[2]);
  std::string mappingsPath = argv[3];
  int seconds = (argc > 4) ? std::atoi(argv[4]) : 30;

  // Load the mapping registry.
  auto registry = std::make_unique<mq::MappingRegistry>(
    mq::MappingRegistry::from_file(mappingsPath));
  std::cout << "loaded " << registry->size() << " mapping rule(s) from "
            << mappingsPath << "\n";
  for (const auto& w : registry->validate_overlaps(false)) {
    std::cerr << "  overlap warning: " << w << "\n";
  }

  // Sensor state — mirrors what /api/sources would expose.  Updated by
  // the bridge ingest callback on every accepted PUBLISH.
  std::map<std::string, SensorSnapshot> sensors;
  std::mutex sensorsMu;

  auto now_ms = []() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
  };

  mq::ClientConfig cfg;
  cfg.brokerHost   = host;
  cfg.brokerPort   = port;
  cfg.clientId     = "re-mqtt-demo";
  cfg.keepaliveSec = 30;

  // Bridge takes an ingest callback (same shape PerceptionService binds) +
  // a push trigger.  We just log here — no PE is running.
  std::atomic<int> pushCount{0};
  mq::MqttBridge bridge(
    cfg, std::move(registry),
    [&](const std::string& sensorId, int offset, int length,
        const reality::Vector& values, long ttlMs,
        const std::string& topic, const std::string& mappingId) {
      std::ostringstream vec;
      vec << "[";
      for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) vec << ", ";
        vec << std::fixed << std::setprecision(4) << values[i];
      }
      vec << "]";
      std::cout << "INGEST sensorId=" << sensorId
                << " region=[" << offset << "," << (offset + length) << ")"
                << " values=" << vec.str()
                << " ttlMs=" << ttlMs
                << " topic=" << topic
                << " mapping=" << mappingId << "\n";
      std::lock_guard<std::mutex> lock(sensorsMu);
      auto& s = sensors[sensorId];
      s.sensorId = sensorId;
      s.topic = topic;
      s.mappingId = mappingId;
      s.offset = offset;
      s.length = length;
      s.lastValue = values;
      s.lastUpdatedMs = now_ms();
      s.ttlMs = ttlMs;
      s.updates += 1;
    },
    [&]() { pushCount.fetch_add(1); });

  bridge.start();
  std::cout << "subscribing for " << seconds << "s ...\n\n";
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  bridge.stop();

  std::cout << "\n──── bridge stats ────\n";
  auto bs = bridge.stats();
  std::cout << "  connected=" << (bridge.is_connected() ? "true" : "false")
            << "  (connection closed on stop)\n";
  std::cout << "  messagesMapped:   " << bs.messagesMapped   << "\n";
  std::cout << "  messagesRejected: " << bs.messagesRejected << "\n";
  std::cout << "  messagesUnmatched:" << bs.messagesUnmatched << "\n";
  std::cout << "  pushesTriggered:  " << bs.pushesTriggered  << "  (debounce → coalesced)\n";

  std::cout << "\n──── /api/sources (synthesized) ────\n";
  long long now = now_ms();
  std::lock_guard<std::mutex> lock(sensorsMu);
  for (const auto& [_, s] : sensors) {
    long long age = now - s.lastUpdatedMs;
    bool stale = s.ttlMs > 0 && age > s.ttlMs;
    std::cout << "  " << s.sensorId
              << "  region=[" << s.offset << "," << (s.offset + s.length) << ")"
              << "  value=";
    for (size_t i = 0; i < s.lastValue.size(); ++i) {
      if (i > 0) std::cout << ",";
      std::cout << std::fixed << std::setprecision(4) << s.lastValue[i];
    }
    std::cout << "  updates=" << s.updates
              << "  ageMs=" << age
              << "  stale=" << (stale ? "yes" : "no") << "\n";
  }
  std::cout << "\n──── /api/mqtt/mappings (synthesized) ────\n";
  // Stats per mapping from the registry's metrics array.
  for (size_t i = 0; i < bridge.registry().size(); ++i) {
    const auto& r = bridge.registry().rules()[i];
    const auto& m = bridge.registry().metrics(i);
    std::cout << "  " << r.id << "  " << r.topicFilter
              << "  received=" << m.received.load()
              << " mapped=" << m.mapped.load()
              << " rejected=" << m.rejected.load();
    {
      std::lock_guard<std::mutex> lk(m.lastErrorMutex);
      if (!m.lastError.empty()) std::cout << "  lastError=\"" << m.lastError << "\"";
    }
    std::cout << "\n";
  }
  return 0;
}
