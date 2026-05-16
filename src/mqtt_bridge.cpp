// MqttBridge — see include/reality/mqtt_bridge.hpp.

#include "reality/mqtt_bridge.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace reality::mqtt {

namespace {

const char* get_env(const char* name) {
  const char* v = std::getenv(name);
  return (v && *v) ? v : nullptr;
}

// Parse "comma,separated,floats" into a Vector.  Empty entries become 0.0.
reality::Vector parse_csv_floats(const std::string& s) {
  reality::Vector out;
  std::string current;
  auto flush = [&]() {
    if (current.empty()) { out.push_back(0.0); return; }
    try { out.push_back(std::stod(current)); }
    catch (const std::exception&) { out.push_back(0.0); }
    current.clear();
  };
  for (char c : s) {
    if (c == ',' || c == ' ' || c == '\n' || c == '\r' || c == '\t') flush();
    else current.push_back(c);
  }
  if (!current.empty() || !s.empty()) flush();
  return out;
}

}  // namespace

MqttBridge::MqttBridge(ClientConfig cfg,
                       std::vector<TopicBinding> bindingList,
                       IngestCallback ingestFn)
    : clientConfig(std::move(cfg)),
      bindings(std::move(bindingList)),
      ingest(std::move(ingestFn)),
      client(std::make_unique<MqttClient>(clientConfig)) {
  client->set_message_handler([this](const std::string& topic, const std::vector<uint8_t>& payload) {
    on_message(topic, payload);
  });
  for (const auto& b : bindings) client->subscribe(b.topic, 0);
}

MqttBridge::~MqttBridge() { stop(); }

void MqttBridge::start() {
  if (client) client->start();
}

void MqttBridge::stop() {
  if (client) client->stop();
}

reality::Vector MqttBridge::decode_payload(const std::vector<uint8_t>& payload,
                                           const std::string& codec) const {
  std::string text(payload.begin(), payload.end());
  if (codec == "csv-float" || codec == "single-float") {
    return parse_csv_floats(text);
  }
  // Unknown codec — fall back to CSV so the bridge stays useful even when an
  // operator typos the codec field.  The single value 0.0 keeps the region
  // shape stable.
  return parse_csv_floats(text);
}

void MqttBridge::on_message(const std::string& topic, const std::vector<uint8_t>& payload) {
  // Linear scan — N is small (one binding per sensor) and the bridge is on
  // a slow path (MQTT ingest, not engine hot loop).
  for (const auto& b : bindings) {
    if (b.topic != topic) continue;
    reality::Vector values = decode_payload(payload, b.codec);
    if (static_cast<int>(values.size()) < b.length) values.resize(b.length, 0.0);
    if (static_cast<int>(values.size()) > b.length) values.resize(b.length);
    if (ingest) ingest(b.sensorId, b.offset, b.length, values, topic);
    return;
  }
}

json::Value MqttBridge::status_json() const {
  auto s = client ? client->stats() : MqttClient::Stats{};
  json::Value::Array bindingArray;
  for (const auto& b : bindings) {
    bindingArray.push_back(json::Value::Object{
      {"topic", b.topic},
      {"sensorId", b.sensorId},
      {"offset", static_cast<double>(b.offset)},
      {"length", static_cast<double>(b.length)},
      {"codec", b.codec},
    });
  }
  return json::Value::Object{
    {"connected",        client && client->is_connected()},
    {"brokerHost",       clientConfig.brokerHost},
    {"brokerPort",       static_cast<double>(clientConfig.brokerPort)},
    {"clientId",         clientConfig.clientId},
    {"connectAttempts",  static_cast<double>(s.connectAttempts)},
    {"connectSuccesses", static_cast<double>(s.connectSuccesses)},
    {"messagesReceived", static_cast<double>(s.messagesReceived)},
    {"bytesReceived",    static_cast<double>(s.bytesReceived)},
    {"pingsSent",        static_cast<double>(s.pingsSent)},
    {"reconnects",       static_cast<double>(s.reconnects)},
    {"lastMessageAtMs",  static_cast<double>(s.lastMessageAtMs)},
    {"bindings",         bindingArray},
  };
}

std::optional<std::pair<ClientConfig, std::vector<TopicBinding>>>
MqttBridge::from_environment() {
  const char* host = get_env("MQTT_BROKER_HOST");
  if (!host) return std::nullopt;  // disabled — bridge is not constructed

  ClientConfig cfg;
  cfg.brokerHost = host;
  if (const char* p = get_env("MQTT_BROKER_PORT")) cfg.brokerPort = std::atoi(p);
  if (const char* c = get_env("MQTT_CLIENT_ID"))   cfg.clientId  = c;
  if (const char* u = get_env("MQTT_USERNAME"))    cfg.username  = u;
  if (const char* p = get_env("MQTT_PASSWORD"))    cfg.password  = p;
  if (const char* k = get_env("MQTT_KEEPALIVE"))   cfg.keepaliveSec = std::atoi(k);

  std::vector<TopicBinding> bindings;
  const char* mapRaw = get_env("MQTT_TOPIC_MAP");
  if (mapRaw) {
    // Topic map is a JSON array — [{"topic":"...","sensorId":"...","offset":0,"length":1,"codec":"csv-float"}, ...]
    try {
      json::Value parsed = json::parse(mapRaw);
      if (parsed.is_array()) {
        for (const auto& item : parsed.array()) {
          if (!item.is_object()) continue;
          TopicBinding b;
          b.topic    = item.at("topic").as_string();
          b.sensorId = item.at("sensorId").as_string(b.topic);  // default: sensorId == topic
          b.offset   = static_cast<int>(item.at("offset").as_number(0));
          b.length   = std::max(1, static_cast<int>(item.at("length").as_number(1)));
          b.codec    = item.at("codec").as_string("csv-float");
          if (!b.topic.empty()) bindings.push_back(b);
        }
      }
    } catch (const std::exception& e) {
      std::cerr << "MQTT_TOPIC_MAP parse error: " << e.what() << " — bridge disabled\n";
      return std::nullopt;
    }
  }
  if (bindings.empty()) {
    std::cerr << "MQTT_BROKER_HOST set but MQTT_TOPIC_MAP empty — bridge disabled\n";
    return std::nullopt;
  }
  return std::make_pair(std::move(cfg), std::move(bindings));
}

}  // namespace reality::mqtt
