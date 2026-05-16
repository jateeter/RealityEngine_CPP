// MqttBridge — see include/reality/mqtt_bridge.hpp.

#include "reality/mqtt_bridge.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace reality::mqtt {

namespace {

const char* get_env(const char* name) {
  const char* v = std::getenv(name);
  return (v && *v) ? v : nullptr;
}

bool truthy_env(const char* v) {
  if (!v || !*v) return false;
  std::string s(v);
  for (auto& c : s) c = static_cast<char>(::tolower(c));
  return !(s == "0" || s == "false" || s == "no" || s == "off");
}

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch()).count();
}

// Convert the legacy MQTT_TOPIC_MAP shape (the v1 schema) into the richer
// MappingRegistry shape (the v2 schema this commit introduces).  Keeps the
// transition seamless: existing deployments using the v1 env continue to
// work with the same default extract/normalize/push semantics they had.
std::unique_ptr<MappingRegistry> legacy_topic_map_to_registry(const json::Value& legacy) {
  json::Value::Array mappings;
  if (legacy.is_array()) {
    int idCounter = 0;
    for (const auto& item : legacy.array()) {
      if (!item.is_object()) continue;
      mappings.push_back(json::Value::Object{
        {"id", item.at("sensorId").as_string("mqtt-legacy-" + std::to_string(idCounter++))},
        {"topicFilter", item.at("topic").as_string("")},
        {"sensorIdTemplate", item.at("sensorId").as_string("")},
        {"region", json::Value::Object{
          {"offset", item.at("offset").as_number(0)},
          {"length", item.at("length").as_number(1)},
        }},
        {"extract",   json::Value::Object{{"type", item.at("codec").as_string("csv-float")}}},
        {"normalize", json::Value::Object{{"mode", "passthrough"}}},
      });
    }
  }
  json::Value root = json::Value::Object{{"mappings", mappings}};
  return std::make_unique<MappingRegistry>(MappingRegistry::from_json(root));
}

}  // namespace

// ── Ctor / dtor ─────────────────────────────────────────────────────────────

MqttBridge::MqttBridge(ClientConfig cfg,
                       std::unique_ptr<MappingRegistry> registry,
                       IngestCallback ingestFn,
                       PushTrigger pushTriggerFn)
    : clientConfig_(std::move(cfg)),
      registry_(std::move(registry)),
      ingest_(std::move(ingestFn)),
      pushTrigger_(std::move(pushTriggerFn)),
      client_(std::make_unique<MqttClient>(clientConfig_)) {
  client_->set_message_handler([this](const std::string& topic, const std::vector<uint8_t>& payload) {
    on_message(topic, payload);
  });
  // Subscribe to every unique topicFilter declared in the registry.  QoS
  // honours per-rule config; the highest QoS seen for a given filter wins
  // when duplicates appear.
  std::map<std::string, int> uniq;
  for (const auto& r : registry_->rules()) {
    auto it = uniq.find(r.topicFilter);
    if (it == uniq.end() || it->second < r.qos) uniq[r.topicFilter] = r.qos;
  }
  for (const auto& [filter, qos] : uniq) client_->subscribe(filter, qos);
}

MqttBridge::~MqttBridge() { stop(); }

void MqttBridge::start() {
  if (debounceRunning_.exchange(true)) {
    // already running
  } else {
    debounceThread_ = std::thread([this]() { debounce_loop(); });
  }
  if (client_) client_->start();
}

void MqttBridge::stop() {
  if (client_) client_->stop();
  if (debounceRunning_.exchange(false)) {
    debounceCv_.notify_all();
    if (debounceThread_.joinable()) debounceThread_.join();
  }
}

// ── Stats ───────────────────────────────────────────────────────────────────

MqttBridge::BridgeStats MqttBridge::stats() const {
  std::lock_guard<std::mutex> lock(statsMutex_);
  return stats_;
}

// ── Inbound message processing ──────────────────────────────────────────────

void MqttBridge::note_error(MappingMetrics& m, const std::string& topic, const std::string& err) {
  {
    std::lock_guard<std::mutex> lock(m.lastErrorMutex);
    m.lastError = "[" + topic + "] " + err;
  }
  m.lastErrorAtMs.store(now_ms());
  m.rejected.fetch_add(1);
}

void MqttBridge::on_message(const std::string& topic, const std::vector<uint8_t>& payload) {
  {
    std::lock_guard<std::mutex> lock(statsMutex_);
    ++stats_.messagesReceived;
  }
  // Fan out to every rule whose topicFilter matches.  A single PUBLISH on
  // a multi-field sensor topic often drives many mappings (one per
  // JSON-pointer extraction), so we dispatch each rule independently.
  auto matches = registry_->match_all(topic);
  if (matches.empty()) {
    std::lock_guard<std::mutex> lock(statsMutex_);
    ++stats_.messagesUnmatched;
    return;
  }

  bool anyImmediate = false;
  long minDebounce = -1;

  for (const auto& match : matches) {
    auto& rule = registry_->rules()[match.ruleIndex];
    auto& metrics = registry_->metrics(match.ruleIndex);
    metrics.received.fetch_add(1);
    metrics.lastMessageAtMs.store(now_ms());

    auto decoded = registry_->decode(rule, payload);
    if (!decoded.valid) {
      note_error(metrics, topic, decoded.error);
      std::lock_guard<std::mutex> lock(statsMutex_);
      ++stats_.messagesRejected;
      continue;
    }
    std::string sensorId = registry_->resolve_sensor_id(rule, topic, match.captures);
    if (ingest_) {
      ingest_(sensorId, rule.offset, rule.length, decoded.values, rule.ttlMs, topic, rule.id);
    }
    metrics.mapped.fetch_add(1);
    {
      std::lock_guard<std::mutex> lock(statsMutex_);
      ++stats_.messagesMapped;
    }

    switch (rule.pushMode) {
      case PushMode::Immediate: anyImmediate = true; break;
      case PushMode::Debounced:
        if (minDebounce < 0 || rule.debounceMs < minDebounce) minDebounce = rule.debounceMs;
        break;
      case PushMode::Manual: break;
    }
  }

  // Push policy across a fan-out: Immediate wins over Debounced (fires
  // synchronously once for the whole fan-out, not N times).  Debounced
  // schedules a single wakeup using the smallest declared debounce window.
  if (anyImmediate) {
    if (pushTrigger_) pushTrigger_();
    std::lock_guard<std::mutex> lock(statsMutex_);
    ++stats_.pushesTriggered;
  } else if (minDebounce >= 0) {
    schedule_push(minDebounce);
  }
}

// ── Debounce loop ──────────────────────────────────────────────────────────

void MqttBridge::schedule_push(long debounceMs) {
  // Set the deadline to (now + debounceMs).  If multiple messages arrive
  // before the deadline elapses, each updates the deadline forward —
  // resulting in exactly one push fired `debounceMs` after the LAST update.
  long long target = now_ms() + debounceMs;
  long long expected;
  do {
    expected = nextPushAtMs_.load();
    if (expected >= target) return;  // a later deadline is already set
  } while (!nextPushAtMs_.compare_exchange_weak(expected, target));
  debounceCv_.notify_all();
}

void MqttBridge::debounce_loop() {
  while (debounceRunning_.load()) {
    long long target = nextPushAtMs_.load();
    std::unique_lock<std::mutex> lock(debounceMutex_);
    if (target == 0) {
      debounceCv_.wait_for(lock, std::chrono::milliseconds(200),
                           [this]() { return !debounceRunning_.load() || nextPushAtMs_.load() != 0; });
      continue;
    }
    long long sleepFor = target - now_ms();
    if (sleepFor > 0) {
      debounceCv_.wait_for(lock, std::chrono::milliseconds(sleepFor),
                           [this, target]() { return !debounceRunning_.load() || nextPushAtMs_.load() != target; });
      // Loop again — if the target moved forward, we'll re-sleep on the new
      // deadline; if running was cleared we'll exit at the top of the loop.
      continue;
    }
    // Deadline reached.  Clear the pending push and fire.
    long long current = nextPushAtMs_.load();
    if (current > target) continue;            // a later update bumped us
    nextPushAtMs_.store(0);
    lock.unlock();
    if (pushTrigger_) pushTrigger_();
    std::lock_guard<std::mutex> sl(statsMutex_);
    ++stats_.pushesTriggered;
  }
}

// ── Test entry point ───────────────────────────────────────────────────────

void MqttBridge::inject_message(const std::string& topic, const std::vector<uint8_t>& payload) {
  on_message(topic, payload);
}

// ── Env-driven config loader ───────────────────────────────────────────────

std::optional<MqttBridge::EnvConfig> MqttBridge::from_environment() {
  const char* host = get_env("MQTT_BROKER_HOST");
  if (!host) return std::nullopt;

  ClientConfig cfg;
  cfg.brokerHost = host;
  if (const char* p = get_env("MQTT_BROKER_PORT")) cfg.brokerPort = std::atoi(p);
  if (const char* c = get_env("MQTT_CLIENT_ID"))   cfg.clientId   = c;
  if (const char* u = get_env("MQTT_USERNAME"))    cfg.username   = u;
  if (const char* p = get_env("MQTT_PASSWORD"))    cfg.password   = p;
  if (const char* k = get_env("MQTT_KEEPALIVE"))   cfg.keepaliveSec = std::atoi(k);

  std::unique_ptr<MappingRegistry> registry;
  try {
    if (const char* path = get_env("MQTT_MAPPINGS_FILE")) {
      registry = std::make_unique<MappingRegistry>(MappingRegistry::from_file(path));
    } else if (const char* raw = get_env("MQTT_MAPPINGS_JSON")) {
      registry = std::make_unique<MappingRegistry>(MappingRegistry::from_json(json::parse(raw)));
    } else if (const char* legacy = get_env("MQTT_TOPIC_MAP")) {
      // Backwards-compatible v1 → v2 conversion.
      registry = legacy_topic_map_to_registry(json::parse(legacy));
    }
  } catch (const std::exception& e) {
    std::cerr << "MQTT mappings parse error: " << e.what() << " — bridge disabled\n";
    return std::nullopt;
  }
  if (!registry || registry->size() == 0) {
    std::cerr << "MQTT_BROKER_HOST set but no mappings resolved — bridge disabled\n";
    return std::nullopt;
  }

  bool allowOverlap = truthy_env(get_env("MQTT_ALLOW_REGION_OVERLAP"));
  auto warnings = registry->validate_overlaps(allowOverlap);
  for (const auto& w : warnings) {
    std::cerr << "MQTT mapping warning: " << w << "\n";
  }

  EnvConfig out;
  out.client = std::move(cfg);
  out.registry = std::move(registry);
  out.allowOverlap = allowOverlap;
  return out;
}

}  // namespace reality::mqtt
