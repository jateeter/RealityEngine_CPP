// mqtt_demo_agriculture — full broker → mapping → PE-region → CES
// initial-event evaluation against the agriculture-domain example machines.
//
// For each ingested MQTT message:
//   1. The mapping registry projects the raw reading into a status bit at
//      the declared offset (band normalize → 0.0/1.0).
//   2. We accumulate per-machine input regions (4 cells each).
//   3. After each update we evaluate every sequence's initial event against
//      the current 4-cell pattern using GTE comparator semantics and report
//      which ones match.
//   4. When an output sequence's initial event matches, we resolve its
//      governance contract via the machine's triggerConfig — that resolution
//      is the certified, auditable contribution to the domain output space.
//
// Usage:
//   mqtt_demo_agriculture <host> <port> <mappings.json> <ag-machines-dir> [seconds=30]

#include "reality/json.hpp"
#include "reality/mqtt_bridge.hpp"
#include "reality/mqtt_client.hpp"
#include "reality/mqtt_mapping.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace mq = reality::mqtt;
namespace fs = std::filesystem;
using Json = reality::json::Value;

namespace {

struct MachineSummary {
  std::string id;
  std::string name;
  int inputOffset = 0;
  int inputLength = 0;
  Json governance;          // machine.metadata.governance
  Json triggerConfig;       // machine.metadata.triggerConfig
  // sequenceId → initial element pattern (4 cells of {value, threshold}).
  std::map<std::string, std::vector<std::pair<double, double>>> initialPatterns;
  // sequenceId → output vectors (when the initial event emits directly).
  std::map<std::string, Json::Array> initialOutputs;
};

// GTE comparator semantics — element matches when:
//   value >= threshold → expects HIGH → matches when inputValue >= threshold
//   value <  threshold → expects LOW  → matches when inputValue <  threshold
// Vector matches when every element matches.
bool vector_matches(const std::vector<std::pair<double, double>>& pattern,
                    const std::vector<double>& input) {
  if (pattern.size() != input.size()) return false;
  for (size_t i = 0; i < pattern.size(); ++i) {
    bool expectHigh = pattern[i].first >= pattern[i].second;
    bool actualHigh = input[i] >= pattern[i].second;
    if (expectHigh != actualHigh) return false;
  }
  return true;
}

// Pretty-print a 4-cell pattern as a binary string.
std::string fmt_bits(const std::vector<double>& v) {
  std::string s = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i > 0) s += ",";
    s += (v[i] >= 0.5) ? "1" : "0";
  }
  s += "]";
  return s;
}

// Load a single machine JSON file into our summary struct.
MachineSummary load_machine(const fs::path& file) {
  std::ifstream in(file);
  std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Json root = reality::json::parse(raw);
  const Json& m = root.at("machine");
  MachineSummary s;
  s.id   = file.stem().string();
  s.name = m.at("name").as_string();
  const auto& pm = m.at("perceptualMapping");
  s.inputOffset = static_cast<int>(pm.at("input").at("offset").as_number());
  s.inputLength = static_cast<int>(pm.at("input").at("length").as_number());
  const auto& md = m.at("metadata");
  if (md.is_object()) {
    s.governance    = md.at("governance");
    s.triggerConfig = md.at("triggerConfig");
  }
  if (m.at("sequences").is_array()) {
    for (const auto& seq : m.at("sequences").array()) {
      std::string sid = seq.at("id").as_string();
      if (!seq.at("vectors").is_array() || seq.at("vectors").array().empty()) continue;
      const auto& v0 = seq.at("vectors").array()[0];
      if (!v0.at("isInitial").as_bool(false)) continue;
      if (!v0.at("elements").is_array()) continue;
      std::vector<std::pair<double, double>> pattern;
      for (const auto& el : v0.at("elements").array()) {
        double value = el.at("value").as_number();
        double threshold = el.at("threshold").is_number() ? el.at("threshold").as_number() : 0.5;
        pattern.emplace_back(value, threshold);
      }
      s.initialPatterns[sid] = pattern;
      if (v0.at("outputVectors").is_array()) {
        s.initialOutputs[sid] = v0.at("outputVectors").array();
      }
    }
  }
  return s;
}

// Resolve governance for a fired output via triggerConfig.  Walks
// machine.metadata.triggerConfig.rules looking for a sequenceId + outputMatches
// match.  Returns the resolved PagingDecision-shaped JSON.
Json resolve_governance(const MachineSummary& m,
                        const std::string& sequenceId,
                        const std::vector<double>& outputValues) {
  if (!m.triggerConfig.is_object()) return Json(nullptr);
  const auto& rules = m.triggerConfig.at("rules");
  if (!rules.is_array()) return Json(nullptr);
  for (const auto& rule : rules.array()) {
    if (rule.at("sequenceId").as_string() != sequenceId) continue;
    if (!rule.at("outputMatches").is_array()) continue;
    const auto& expected = rule.at("outputMatches").array();
    if (expected.size() != outputValues.size()) continue;
    bool match = true;
    for (size_t i = 0; i < expected.size(); ++i) {
      if (!expected[i].is_number()) { match = false; break; }
      if (expected[i].as_number() != outputValues[i]) { match = false; break; }
    }
    if (!match) continue;
    Json::Object out;
    out["sequenceId"]     = sequenceId;
    out["ragStatusCode"]  = rule.at("ragStatusCode").as_string();
    out["processStatus"]  = rule.at("processStatus").as_string();
    out["description"]    = rule.at("description").as_string();
    if (m.governance.is_object()) {
      out["ownerTeam"]         = m.governance.at("ownerTeam").as_string();
      out["runbook"]           = m.governance.at("runbook").as_string();
      out["escalationPolicy"]  = m.governance.at("escalationPolicy").as_string();
      if (m.governance.at("contact").is_object()) {
        out["contact"]         = m.governance.at("contact");
      }
      if (m.governance.at("sla").is_object()) {
        const auto& slaObj = m.governance.at("sla");
        const auto& slaForStatus = slaObj.at(rule.at("processStatus").as_string());
        if (slaForStatus.is_number()) out["slaSeconds"] = slaForStatus.as_number();
      }
    }
    return Json(out);
  }
  return Json(nullptr);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    std::cerr << "usage: " << argv[0]
              << " <host> <port> <mappings.json> <ag-machines-dir> [seconds=30]\n";
    return 2;
  }
  std::string host = argv[1];
  int port = std::atoi(argv[2]);
  std::string mappingsPath = argv[3];
  std::string agDir = argv[4];
  int seconds = (argc > 5) ? std::atoi(argv[5]) : 30;

  // Load 4 representative ag machines.
  std::vector<std::string> machineFiles = {
    "AGX001_aquaculture-water-quality-stability.json",
    "AGX005_aquaculture-dissolved-oxygen-control.json",
    "AGX026_indoor-grow-house-vpd-climate-management.json",
    "AGX032_indoor-grow-house-co2-enrichment-safety.json",
  };
  std::vector<MachineSummary> machines;
  for (const auto& f : machineFiles) {
    machines.push_back(load_machine(fs::path(agDir) / f));
  }
  std::cout << "loaded " << machines.size() << " agriculture machines:\n";
  for (const auto& m : machines) {
    std::cout << "  " << m.id << "  input=[" << m.inputOffset
              << "," << (m.inputOffset + m.inputLength)
              << ")  initial-events=" << m.initialPatterns.size() << "\n";
  }

  // Load the mapping registry.
  auto registry = std::make_unique<mq::MappingRegistry>(
    mq::MappingRegistry::from_file(mappingsPath));
  std::cout << "loaded " << registry->size() << " mapping rules from " << mappingsPath << "\n\n";

  // Per-machine input region (initialized to zeros).
  std::map<std::string, std::vector<double>> regionByMachine;
  for (const auto& m : machines) {
    regionByMachine[m.id] = std::vector<double>(m.inputLength, 0.0);
  }
  std::mutex regionsMu;
  std::set<std::string> firingHistory;   // dedup repeated "still firing" prints

  // Bridge with an ingest callback that updates the right machine's cells.
  mq::ClientConfig cfg;
  cfg.brokerHost = host; cfg.brokerPort = port;
  cfg.clientId = "re-mqtt-demo-ag";
  cfg.keepaliveSec = 30;

  mq::MqttBridge bridge(
    cfg, std::move(registry),
    [&](const std::string& sensorId, int offset, int length,
        const reality::Vector& values, long /*ttlMs*/,
        const std::string& /*topic*/, const std::string& mappingId) {
      std::lock_guard<std::mutex> lock(regionsMu);
      // Locate the machine whose input region contains this offset.
      for (const auto& m : machines) {
        if (offset >= m.inputOffset && offset + length <= m.inputOffset + m.inputLength) {
          int localIdx = offset - m.inputOffset;
          for (int i = 0; i < length; ++i) {
            regionByMachine[m.id][localIdx + i] = values[i];
          }
          // Print the updated state + which initial events match.
          const auto& region = regionByMachine[m.id];
          std::cout << "[" << m.id << "] cell " << offset << " = "
                    << std::fixed << std::setprecision(0) << values[0]
                    << "  (sensor=" << sensorId << ", mapping=" << mappingId << ")\n";
          std::cout << "  region " << fmt_bits(region) << "  → ";
          int firedCount = 0;
          for (const auto& [sid, pattern] : m.initialPatterns) {
            if (vector_matches(pattern, region)) {
              std::cout << "MATCH " << sid << " ";
              ++firedCount;
              // If the initial event has outputs, resolve governance for one.
              auto it = m.initialOutputs.find(sid);
              if (it != m.initialOutputs.end() && !it->second.empty()) {
                const auto& ov = it->second.front();
                std::vector<double> outVals;
                if (ov.at("vector").is_array()) {
                  for (const auto& x : ov.at("vector").array()) {
                    if (x.is_number()) outVals.push_back(x.as_number());
                  }
                }
                auto gov = resolve_governance(m, sid, outVals);
                std::string key = m.id + ":" + sid;
                if (firingHistory.insert(key).second && gov.is_object()) {
                  std::cout << "\n  ▶ OUTPUT " << ov.at("id").as_string()
                            << " values=" << reality::json::stringify(ov.at("vector"))
                            << "\n  ▶ GOVERNANCE " << reality::json::stringify(gov) << "\n";
                }
              }
            }
          }
          if (firedCount == 0) std::cout << "no initial-event match yet";
          std::cout << "\n";
          break;
        }
      }
    },
    []() { /* push noop */ });

  bridge.start();
  std::cout << "subscribing for " << seconds << "s ...\n\n";
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  bridge.stop();

  std::cout << "\n──── final state per machine ────\n";
  std::lock_guard<std::mutex> lock(regionsMu);
  for (const auto& m : machines) {
    const auto& region = regionByMachine[m.id];
    std::cout << "  " << m.id << "  region=" << fmt_bits(region) << "  ";
    std::vector<std::string> matched;
    for (const auto& [sid, pat] : m.initialPatterns) {
      if (vector_matches(pat, region)) matched.push_back(sid);
    }
    if (matched.empty()) std::cout << "no initial event matches";
    else {
      std::cout << "matches: ";
      for (size_t i = 0; i < matched.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << matched[i];
      }
    }
    std::cout << "\n";
  }
  return 0;
}
