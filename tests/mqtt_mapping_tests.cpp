// MqttMappingRegistry tests — schema parsing, topic-filter matching with
// MQTT wildcards, payload extraction, normalization, length validation,
// overlap detection, and the in-process bridge dispatcher.

#include "reality/mqtt_bridge.hpp"
#include "reality/mqtt_mapping.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace mq = reality::mqtt;

namespace {

int failures = 0;

#define EXPECT(cond, label)                                              \
  do { if (!(cond)) {                                                    \
    std::cerr << "FAIL: " << label << " (" << __FILE__ << ":"           \
              << __LINE__ << ")\n";                                      \
    ++failures;                                                          \
  } } while (0)

mq::MappingRegistry registry_from_string(const std::string& s) {
  return mq::MappingRegistry::from_json(reality::json::parse(s));
}

// ── Topic-filter matching ──────────────────────────────────────────────────

void test_topic_filter_exact() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"a","topicFilter":"sensors/temp","region":{"offset":0,"length":1},
     "extract":{"type":"csv-float"}}
  ]})");
  auto m = reg.match("sensors/temp");
  EXPECT(m.has_value(), "exact match found");
  if (m) EXPECT(m->captures.empty(), "exact match has no captures");
  EXPECT(!reg.match("sensors/humidity").has_value(), "non-matching topic returns nullopt");
}

void test_topic_filter_plus_wildcard() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"zone","topicFilter":"sensors/zone/+/temp",
     "sensorIdTemplate":"zone.{1}.temp",
     "region":{"offset":0,"length":1},
     "extract":{"type":"csv-float"}}
  ]})");
  auto m = reg.match("sensors/zone/3/temp");
  EXPECT(m.has_value(), "+ wildcard matches one level");
  if (m) {
    EXPECT(m->captures.size() == 1, "captures has one element");
    if (!m->captures.empty()) EXPECT(m->captures[0] == "3", "+ captures the wildcard level");
  }
  EXPECT(!reg.match("sensors/zone/3/foo").has_value(), "+ wildcard rejects suffix mismatch");
  EXPECT(!reg.match("sensors/zone/3").has_value(), "+ wildcard requires same level count");
}

void test_topic_filter_hash_wildcard() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"slow","topicFilter":"telemetry/slow/#",
     "sensorIdTemplate":"telemetry.{1}",
     "region":{"offset":0,"length":1},
     "extract":{"type":"raw"}}
  ]})");
  auto m = reg.match("telemetry/slow/device-42/pressure");
  EXPECT(m.has_value(), "# wildcard matches multi-level tail");
  if (m && !m->captures.empty()) {
    EXPECT(m->captures[0] == "device-42/pressure", "# captures the whole tail");
  }
}

void test_topic_filter_first_match_wins() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"specific","topicFilter":"sensors/+/temp",
     "region":{"offset":0,"length":1},"extract":{"type":"csv-float"}},
    {"id":"generic","topicFilter":"sensors/#",
     "region":{"offset":4,"length":1},"extract":{"type":"csv-float"}}
  ]})");
  auto m = reg.match("sensors/abc/temp");
  EXPECT(m.has_value() && m->ruleIndex == 0, "first matching rule wins");
}

// ── sensorIdTemplate substitution ──────────────────────────────────────────

void test_sensor_id_template() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"zone","topicFilter":"sensors/zone/+/temp",
     "sensorIdTemplate":"zone.{1}.temp",
     "region":{"offset":0,"length":1},
     "extract":{"type":"csv-float"}}
  ]})");
  auto m = reg.match("sensors/zone/A1/temp");
  EXPECT(m.has_value(), "match found for sensor id template test");
  if (m) {
    auto sid = reg.resolve_sensor_id(reg.rules()[m->ruleIndex], "sensors/zone/A1/temp", m->captures);
    EXPECT(sid == "zone.A1.temp", "template interpolates {1} -> capture");
  }
}

void test_sensor_id_template_empty() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"flat","topicFilter":"sensors/temp",
     "region":{"offset":0,"length":1},
     "extract":{"type":"csv-float"}}
  ]})");
  auto m = reg.match("sensors/temp");
  if (m) {
    auto sid = reg.resolve_sensor_id(reg.rules()[m->ruleIndex], "sensors/temp", m->captures);
    EXPECT(sid == "sensors/temp", "empty template falls back to topic");
  }
}

// ── Payload extraction ─────────────────────────────────────────────────────

void test_extract_csv_single() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"a","topicFilter":"t","region":{"offset":0,"length":1},
     "extract":{"type":"csv-float"},
     "normalize":{"mode":"passthrough","clamp":false}}
  ]})");
  std::string payload = "0.42";
  auto d = reg.decode(reg.rules()[0], {payload.begin(), payload.end()});
  EXPECT(d.valid, "csv single decodes");
  if (d.valid) EXPECT(d.values.size() == 1 && d.values[0] == 0.42, "csv single value parsed");
}

void test_extract_csv_multi() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"a","topicFilter":"t","region":{"offset":0,"length":3},
     "extract":{"type":"csv-float"},
     "normalize":{"mode":"passthrough","clamp":false}}
  ]})");
  std::string payload = "0.1, 0.2, 0.3";
  auto d = reg.decode(reg.rules()[0], {payload.begin(), payload.end()});
  EXPECT(d.valid, "csv multi decodes");
  if (d.valid) {
    EXPECT(d.values.size() == 3, "three CSV values parsed");
    if (d.values.size() == 3)
      EXPECT(d.values[0] == 0.1 && d.values[1] == 0.2 && d.values[2] == 0.3, "values intact");
  }
}

void test_extract_csv_index() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"a","topicFilter":"t","region":{"offset":0,"length":1},
     "extract":{"type":"csv-float","index":1},
     "normalize":{"mode":"passthrough","clamp":false}}
  ]})");
  std::string payload = "10,20,30";
  auto d = reg.decode(reg.rules()[0], {payload.begin(), payload.end()});
  EXPECT(d.valid && d.values.size() == 1 && d.values[0] == 20.0,
         "csv index 1 picks middle value");
}

void test_extract_json_pointer() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"a","topicFilter":"t","region":{"offset":0,"length":1},
     "extract":{"type":"json","pointer":"/value"},
     "normalize":{"mode":"passthrough","clamp":false}}
  ]})");
  std::string payload = R"({"value":7.5,"unit":"C"})";
  auto d = reg.decode(reg.rules()[0], {payload.begin(), payload.end()});
  EXPECT(d.valid && d.values.size() == 1 && d.values[0] == 7.5, "json pointer extracts");
}

void test_extract_json_pointer_nested() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"a","topicFilter":"t","region":{"offset":0,"length":1},
     "extract":{"type":"json","pointer":"/sensor/reading"},
     "normalize":{"mode":"passthrough","clamp":false}}
  ]})");
  std::string payload = R"({"sensor":{"reading":42.0}})";
  auto d = reg.decode(reg.rules()[0], {payload.begin(), payload.end()});
  EXPECT(d.valid && d.values.size() == 1 && d.values[0] == 42.0, "nested json pointer extracts");
}

void test_extract_json_pointer_missing() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"a","topicFilter":"t","region":{"offset":0,"length":1},
     "extract":{"type":"json","pointer":"/missing"},
     "normalize":{"mode":"passthrough","clamp":false}}
  ]})");
  std::string payload = R"({"value":1})";
  auto d = reg.decode(reg.rules()[0], {payload.begin(), payload.end()});
  EXPECT(!d.valid, "missing pointer rejects");
  if (!d.valid) EXPECT(d.error.find("not found") != std::string::npos, "error mentions not-found");
}

// ── Normalization ──────────────────────────────────────────────────────────

void test_normalize_minmax() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"a","topicFilter":"t","region":{"offset":0,"length":1},
     "extract":{"type":"csv-float"},
     "normalize":{"mode":"minmax","min":0,"max":100,"clamp":true}}
  ]})");
  std::string payload = "50";
  auto d = reg.decode(reg.rules()[0], {payload.begin(), payload.end()});
  EXPECT(d.valid && d.values.size() == 1 && d.values[0] == 0.5, "minmax 50/100 -> 0.5");
}

void test_normalize_minmax_clamp() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"a","topicFilter":"t","region":{"offset":0,"length":1},
     "extract":{"type":"csv-float"},
     "normalize":{"mode":"minmax","min":0,"max":100,"clamp":true}}
  ]})");
  std::string payload = "150";  // above max; clamp to 1.0
  auto d = reg.decode(reg.rules()[0], {payload.begin(), payload.end()});
  EXPECT(d.valid && d.values[0] == 1.0, "above-max clamps to 1.0");
}

void test_normalize_linear() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"a","topicFilter":"t","region":{"offset":0,"length":1},
     "extract":{"type":"csv-float"},
     "normalize":{"mode":"linear","scale":2.0,"offset":-1.0,"clamp":false}}
  ]})");
  std::string payload = "0.5";
  auto d = reg.decode(reg.rules()[0], {payload.begin(), payload.end()});
  EXPECT(d.valid && d.values[0] == 0.0, "linear 0.5*2-1 = 0.0");
}

// ── Length + value validation ──────────────────────────────────────────────

void test_length_mismatch_rejected() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"a","topicFilter":"t","region":{"offset":0,"length":3},
     "extract":{"type":"csv-float"},
     "normalize":{"mode":"passthrough","clamp":false}}
  ]})");
  std::string payload = "1,2";   // only 2 values for a length-3 region
  auto d = reg.decode(reg.rules()[0], {payload.begin(), payload.end()});
  EXPECT(!d.valid, "length mismatch is rejected");
  if (!d.valid) EXPECT(d.error.find("region.length") != std::string::npos, "error names region.length");
}

void test_nan_rejected() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"a","topicFilter":"t","region":{"offset":0,"length":1},
     "extract":{"type":"csv-float"},
     "normalize":{"mode":"passthrough","clamp":false}}
  ]})");
  std::string payload = "not-a-number";
  auto d = reg.decode(reg.rules()[0], {payload.begin(), payload.end()});
  EXPECT(!d.valid, "non-finite value is rejected");
}

// ── Overlap detection ──────────────────────────────────────────────────────

void test_overlap_detection() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"a","topicFilter":"x","region":{"offset":0,"length":3},
     "extract":{"type":"csv-float"}},
    {"id":"b","topicFilter":"y","region":{"offset":2,"length":2},
     "extract":{"type":"csv-float"}}
  ]})");
  auto warnings = reg.validate_overlaps(false);
  EXPECT(warnings.size() == 1, "overlap reported as a single warning");
  if (!warnings.empty()) {
    EXPECT(warnings[0].find("\"a\"") != std::string::npos, "warning names mapping a");
    EXPECT(warnings[0].find("\"b\"") != std::string::npos, "warning names mapping b");
  }
  auto suppressed = reg.validate_overlaps(true);
  EXPECT(suppressed.empty(), "allowOverlap=true suppresses warnings");
}

void test_no_overlap_clean() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"a","topicFilter":"x","region":{"offset":0,"length":3},
     "extract":{"type":"csv-float"}},
    {"id":"b","topicFilter":"y","region":{"offset":3,"length":2},
     "extract":{"type":"csv-float"}}
  ]})");
  auto warnings = reg.validate_overlaps(false);
  EXPECT(warnings.empty(), "adjacent (non-overlapping) regions are clean");
}

// ── End-to-end via the in-process dispatcher ───────────────────────────────

// Fan-out: a single message on a topic shared by N rules should drive all N.
// Important because typical sensor payloads carry many fields under one topic,
// each extracted by its own json-pointer rule.
void test_match_all_fan_out() {
  auto reg = registry_from_string(R"({"mappings":[
    {"id":"temp","topicFilter":"s/x/v1","region":{"offset":0,"length":1},
     "extract":{"type":"json","pointer":"/t"},
     "normalize":{"mode":"passthrough","clamp":false}},
    {"id":"humid","topicFilter":"s/x/v1","region":{"offset":1,"length":1},
     "extract":{"type":"json","pointer":"/h"},
     "normalize":{"mode":"passthrough","clamp":false}},
    {"id":"other","topicFilter":"s/y/v1","region":{"offset":2,"length":1},
     "extract":{"type":"csv-float"}}
  ]})");
  auto all = reg.match_all("s/x/v1");
  EXPECT(all.size() == 2, "match_all returns every matching rule");
  if (all.size() == 2) {
    EXPECT(all[0].ruleIndex == 0 && all[1].ruleIndex == 1,
           "match_all preserves declaration order");
  }
  EXPECT(reg.match_all("s/y/v1").size() == 1, "match_all excludes non-matching filters");
  EXPECT(reg.match_all("nothing").empty(), "match_all returns empty for no match");

  // Drive the bridge with a single payload; both x-rules should ingest.
  auto registryPtr = std::make_unique<mq::MappingRegistry>(reg);
  int ingestCount = 0;
  std::vector<std::string> sensors;
  mq::ClientConfig cfg; cfg.brokerHost = "127.0.0.1"; cfg.brokerPort = 1;
  mq::MqttBridge bridge(cfg, std::move(registryPtr),
    [&](const std::string& sid, int, int, const reality::Vector&, long,
        const std::string&, const std::string&) {
      ingestCount++; sensors.push_back(sid);
    },
    []() {});
  bridge.inject_message("s/x/v1", {'{','"','t','"',':','0',',','"','h','"',':','1','}'});
  EXPECT(ingestCount == 2, "one PUBLISH → two ingests under fan-out");
  EXPECT(bridge.stats().messagesMapped == 2, "bridge counts both fan-out mappings");
}

void test_bridge_in_process_dispatch() {
  // The bridge talks to a real broker via MqttClient; for unit testing we
  // use the inject_message() entry point to drive the full extract → normalize
  // → ingest pipeline without a TCP connection.
  std::string mappings = R"({
    "mappings": [
      {"id":"zone-temp","topicFilter":"sensors/zone/+/temp",
       "sensorIdTemplate":"zone.{1}.temp",
       "region":{"offset":0,"length":1},
       "extract":{"type":"json","pointer":"/value"},
       "normalize":{"mode":"minmax","min":0,"max":100,"clamp":true},
       "ttlMs":15000,
       "pushMode":"immediate"}
    ]
  })";
  auto registry = std::make_unique<mq::MappingRegistry>(
    mq::MappingRegistry::from_json(reality::json::parse(mappings)));

  std::vector<std::string> ingestedSensorIds;
  std::vector<reality::Vector> ingestedValues;
  std::vector<long> ingestedTtls;
  std::atomic<int> pushCount{0};

  mq::ClientConfig cfg;
  cfg.brokerHost = "127.0.0.1";
  cfg.brokerPort = 1;  // unreachable — bridge.start() is NOT called

  mq::MqttBridge bridge(cfg, std::move(registry),
    [&](const std::string& sensorId, int, int length,
        const reality::Vector& values, long ttlMs,
        const std::string&, const std::string&) {
      ingestedSensorIds.push_back(sensorId);
      ingestedValues.push_back(values);
      ingestedTtls.push_back(ttlMs);
      EXPECT(static_cast<int>(values.size()) == length, "ingest received length-matched values");
    },
    [&]() { pushCount.fetch_add(1); });

  std::string payload = R"({"value":50.0})";
  bridge.inject_message("sensors/zone/3/temp", {payload.begin(), payload.end()});

  EXPECT(ingestedSensorIds.size() == 1, "one ingest call after one valid message");
  if (!ingestedSensorIds.empty()) {
    EXPECT(ingestedSensorIds[0] == "zone.3.temp", "sensor id resolved via template");
    EXPECT(ingestedValues[0].size() == 1 && ingestedValues[0][0] == 0.5,
           "value normalized to 0.5 via MinMax");
    EXPECT(ingestedTtls[0] == 15000, "TTL carried from mapping");
  }
  EXPECT(pushCount.load() == 1, "immediate push policy fires once per accepted message");

  // Bad payload — length validation should reject and bridge counter should
  // increment.  No ingest, no push.
  std::string bad = R"({"missing":1})";
  bridge.inject_message("sensors/zone/3/temp", {bad.begin(), bad.end()});
  EXPECT(ingestedSensorIds.size() == 1, "rejected payload is not ingested");
  auto stats = bridge.stats();
  EXPECT(stats.messagesRejected >= 1, "bridge counts rejected message");
  EXPECT(stats.messagesUnmatched == 0, "rejected != unmatched");

  // Unmatched topic — should bump unmatched.
  bridge.inject_message("unknown/topic", {});
  auto stats2 = bridge.stats();
  EXPECT(stats2.messagesUnmatched >= 1, "unmatched topic counted");
}

// ── Env-driven config ──────────────────────────────────────────────────────

void test_env_disabled_when_no_host() {
  ::unsetenv("MQTT_BROKER_HOST");
  ::unsetenv("MQTT_MAPPINGS_FILE");
  ::unsetenv("MQTT_MAPPINGS_JSON");
  ::unsetenv("MQTT_TOPIC_MAP");
  auto envCfg = mq::MqttBridge::from_environment();
  EXPECT(!envCfg.has_value(), "bridge disabled when MQTT_BROKER_HOST unset");
}

void test_env_enabled_with_inline_mappings() {
  ::setenv("MQTT_BROKER_HOST", "127.0.0.1", 1);
  ::setenv("MQTT_BROKER_PORT", "1883", 1);
  ::setenv("MQTT_MAPPINGS_JSON",
           R"({"mappings":[{"id":"a","topicFilter":"sensors/a","region":{"offset":0,"length":1},"extract":{"type":"csv-float"}}]})",
           1);
  auto envCfg = mq::MqttBridge::from_environment();
  EXPECT(envCfg.has_value(), "bridge enabled when host + mappings JSON present");
  if (envCfg) {
    EXPECT(envCfg->client.brokerHost == "127.0.0.1", "broker host parsed");
    EXPECT(envCfg->registry && envCfg->registry->size() == 1, "one mapping parsed");
  }
  ::unsetenv("MQTT_BROKER_HOST");
  ::unsetenv("MQTT_BROKER_PORT");
  ::unsetenv("MQTT_MAPPINGS_JSON");
}

void test_env_legacy_topic_map_backcompat() {
  ::setenv("MQTT_BROKER_HOST", "127.0.0.1", 1);
  ::unsetenv("MQTT_MAPPINGS_FILE");
  ::unsetenv("MQTT_MAPPINGS_JSON");
  ::setenv("MQTT_TOPIC_MAP",
           R"([{"topic":"legacy/topic","sensorId":"legacy","offset":7,"length":1,"codec":"csv-float"}])",
           1);
  auto envCfg = mq::MqttBridge::from_environment();
  EXPECT(envCfg.has_value(), "legacy MQTT_TOPIC_MAP still loads");
  if (envCfg && envCfg->registry && envCfg->registry->size() > 0) {
    EXPECT(envCfg->registry->rules()[0].topicFilter == "legacy/topic", "legacy topic carries");
    EXPECT(envCfg->registry->rules()[0].offset == 7, "legacy offset carries");
  }
  ::unsetenv("MQTT_BROKER_HOST");
  ::unsetenv("MQTT_TOPIC_MAP");
}

}  // namespace

int main() {
  test_topic_filter_exact();
  test_topic_filter_plus_wildcard();
  test_topic_filter_hash_wildcard();
  test_topic_filter_first_match_wins();
  test_match_all_fan_out();
  test_sensor_id_template();
  test_sensor_id_template_empty();
  test_extract_csv_single();
  test_extract_csv_multi();
  test_extract_csv_index();
  test_extract_json_pointer();
  test_extract_json_pointer_nested();
  test_extract_json_pointer_missing();
  test_normalize_minmax();
  test_normalize_minmax_clamp();
  test_normalize_linear();
  test_length_mismatch_rejected();
  test_nan_rejected();
  test_overlap_detection();
  test_no_overlap_clean();
  test_bridge_in_process_dispatch();
  test_env_disabled_when_no_host();
  test_env_enabled_with_inline_mappings();
  test_env_legacy_topic_map_backcompat();
  if (failures > 0) {
    std::cerr << "\n" << failures << " MQTT mapping assertion(s) failed.\n";
    return 1;
  }
  std::cout << "MQTT mapping tests: OK\n";
  return 0;
}
