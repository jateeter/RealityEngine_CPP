#include "reality/http.hpp"
#include "reality/mqtt_bridge.hpp"
#include "reality/reality.hpp"
#include "reality/vector_aggregator.hpp"

#include <cstdlib>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

using namespace reality;

namespace {

namespace fs = std::filesystem;

struct LocalAISensorSpec {
  std::string sensorId;
  std::string name;
  RegionMapping region;
  long ttlMs = 30000;
};

const std::vector<LocalAISensorSpec>& localai_sensor_specs() {
  static const std::vector<LocalAISensorSpec> specs{
    {"localai_rag_retrieval", "localai/rag_retrieval", {52, 4}, 30000},
    {"localai_rag_grading", "localai/rag_grading", {56, 4}, 30000},
    {"localai_agent_activity", "localai/agent_activity", {64, 4}, 30000},
  };
  return specs;
}

const std::vector<std::string>& localai_machine_files() {
  static const std::vector<std::string> files{
    "rag_corrective_cycle.json",
    "session_rag_context.json",
    "session_agent_context.json",
    "ai_load_bridge.json",
    "agent_activity_classifier.json",
  };
  return files;
}

bool truthy_env(const char* value) {
  if (!value) return false;
  std::string v(value);
  return v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "YES";
}

Json parse_body(const http::Request& req) {
  return req.body.empty() ? Json::Object{} : json::parse(req.body);
}

http::Response ok(const Json& value) {
  return http::json_response(json::stringify(value));
}

// Extracts the token from an "Authorization: Bearer <token>" header, if any.
// Header name and scheme match case-insensitively; returns "" when absent.
std::string bearer_token(const http::Request& req) {
  for (const auto& [key, value] : req.headers) {
    std::string lower_key = key;
    std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower_key != "authorization") continue;
    if (value.size() <= 7) return "";
    std::string scheme = value.substr(0, 7);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (scheme != "bearer ") return "";
    std::string token = value.substr(7);
    const auto start = token.find_first_not_of(' ');
    return start == std::string::npos ? std::string{} : token.substr(start);
  }
  return "";
}

struct BootstrapSummary {
  size_t created = 0;
  size_t skipped = 0;
  size_t machinesSeen = 0;
  Json::Array errors;

  Json to_json() const {
    return Json::Object{
      {"created", static_cast<double>(created)},
      {"errors", Json(errors)},
      {"machinesSeen", static_cast<double>(machinesSeen)},
      {"skipped", static_cast<double>(skipped)},
      {"success", true}
    };
  }
};

SourceConfig source_from_json(const Json& j) {
  SourceConfig s;
  s.id = j.at("id").as_string();
  s.kind = j.at("type").as_string(j.at("kind").as_string("simulated"));
  s.name = j.at("name").as_string("source");
  s.active = j.at("active").as_bool(true);
  if (j.at("region").is_object()) {
    s.region.offset = static_cast<int>(j.at("region").at("offset").as_number());
    s.region.length = static_cast<int>(j.at("region").at("length").as_number());
  }
  if (s.kind == "test") {
    s.machineId = j.at("machineId").as_string();
    s.machineName = j.at("machineName").as_string();
    s.sequenceName = j.at("sequenceName").as_string();
    s.sequenceMetadata = j.at("metadata").is_object() ? j.at("metadata") : Json::Object{};
    s.testSequence = j.at("sequence").is_object() ? j.at("sequence") : Json::Object{};
    s.loop = j.at("loop").as_bool(true);
    for (const auto& v : j.at("inputs").is_array() ? j.at("inputs").array() : Json::Array{}) s.inputs.push_back(json::to_numbers(v));
  } else if (s.kind == "sensor") {
    s.sensorId = j.at("sensorId").as_string();
    s.lastValue = json::to_numbers(j.at("lastValue"));
    if (j.at("lastUpdated").is_number()) s.lastUpdated = static_cast<long long>(j.at("lastUpdated").as_number());
    s.ttlMs = static_cast<long>(j.at("ttlMs").as_number(5000));
    s.origin = j.at("origin").as_string("");
  } else {
    s.kind = "simulated";
    s.pattern = sim_pattern_from_string(j.at("pattern").as_string("constant"));
    s.frequency = j.at("frequency").as_number(1.0);
    s.amplitude = j.at("amplitude").as_number(1.0);
    s.dcOffset = j.at("dcOffset").as_number(0.0);
  }
  return s;
}

SourceConfig sensor_source(const LocalAISensorSpec& spec) {
  SourceConfig s;
  s.kind = "sensor";
  s.name = spec.name;
  s.region = spec.region;
  s.active = true;
  s.sensorId = spec.sensorId;
  s.ttlMs = spec.ttlMs;
  s.origin = "localai";
  return s;
}

std::string normalize_endpoint(std::string endpoint) {
  if (endpoint.empty() || endpoint[0] != '/') endpoint = "/" + endpoint;
  auto q = endpoint.find('?');
  std::string path = q == std::string::npos ? endpoint : endpoint.substr(0, q);
  if (path.find("..") != std::string::npos || path.find("//") != std::string::npos) throw std::invalid_argument("invalid endpoint path");
  return endpoint;
}

std::string source_id_part(const std::string& value) {
  std::string out;
  for (unsigned char c : value) {
    if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    else if (!out.empty() && out.back() != '-') out.push_back('-');
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? "unnamed" : out;
}

std::string replace_all(std::string value, const std::string& needle, const std::string& replacement) {
  size_t pos = 0;
  while ((pos = value.find(needle, pos)) != std::string::npos) {
    value.replace(pos, needle.size(), replacement);
    pos += replacement.size();
  }
  return value;
}

Json merge_objects(Json base, const Json& overlay) {
  if (!base.is_object()) base = Json::Object{};
  if (!overlay.is_object()) return base;
  for (const auto& [key, value] : overlay.object()) base.object()[key] = value;
  return base;
}

std::string decode_json_pointer_token(std::string token) {
  size_t pos = 0;
  while ((pos = token.find("~1", pos)) != std::string::npos) token.replace(pos, 2, "/");
  pos = 0;
  while ((pos = token.find("~0", pos)) != std::string::npos) token.replace(pos, 2, "~");
  return token;
}

const Json* eval_json_pointer(const Json& doc, const std::string& pointer) {
  if (pointer.empty()) return &doc;
  if (pointer[0] != '/') return nullptr;
  const Json* cursor = &doc;
  size_t start = 1;
  while (true) {
    size_t slash = pointer.find('/', start);
    std::string token = decode_json_pointer_token(pointer.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
    if (!cursor) return nullptr;
    if (cursor->is_object()) {
      const auto& obj = cursor->object();
      auto it = obj.find(token);
      if (it == obj.end()) return nullptr;
      cursor = &it->second;
    } else if (cursor->is_array()) {
      if (token.empty() || token.find_first_not_of("0123456789") != std::string::npos) return nullptr;
      size_t idx = static_cast<size_t>(std::stoul(token));
      if (idx >= cursor->array().size()) return nullptr;
      cursor = &cursor->array()[idx];
    } else {
      return nullptr;
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return cursor;
}

std::string json_pointer_top_level_key(const std::string& pointer) {
  if (pointer.empty() || pointer[0] != '/') return "value";
  size_t slash = pointer.find('/', 1);
  return decode_json_pointer_token(pointer.substr(1, slash == std::string::npos ? std::string::npos : slash - 1));
}

bool scalar_to_completion_number(const Json& value, double& out) {
  if (value.is_number()) {
    out = value.as_number();
    return std::isfinite(out);
  }
  if (value.is_bool()) {
    out = value.as_bool() ? 1.0 : 0.0;
    return true;
  }
  if (value.is_string()) {
    const std::string raw = value.as_string();
    char* end = nullptr;
    out = std::strtod(raw.c_str(), &end);
    return end != raw.c_str() && end && *end == '\0' && std::isfinite(out);
  }
  return false;
}

double clamp01(double value) {
  if (!std::isfinite(value)) return 0.0;
  if (value < 0.0) return 0.0;
  if (value > 1.0) return 1.0;
  return value;
}

Json validate_completion_values_array(const Json& values) {
  if (!values.is_array()) throw std::runtime_error("provider completion values must be an array");
  Json::Array out;
  for (const auto& value : values.array()) {
    double n = 0.0;
    if (!scalar_to_completion_number(value, n)) throw std::runtime_error("provider completion value is not a finite number");
    out.emplace_back(n);
  }
  return Json(out);
}

Json response_values_from_content(const Json& contentJson) {
  if (contentJson.at("values").is_array()) return validate_completion_values_array(contentJson.at("values"));
  if (contentJson.at("completion").at("values").is_array()) return validate_completion_values_array(contentJson.at("completion").at("values"));
  return Json(nullptr);
}

Json extract_completion_values_for_mapping(const Json& contentJson, const Json& mapping) {
  const Json& extract = mapping.at("extract");
  if (!extract.is_object() || extract.at("type").as_string() != "json") {
    Json values = response_values_from_content(contentJson);
    if (!values.is_array()) throw std::runtime_error("provider response did not include completion values");
    return values;
  }

  Json::Array values;
  if (extract.at("pointers").is_array()) {
    for (const auto& item : extract.at("pointers").array()) {
      const std::string pointer = item.as_string();
      const Json* value = eval_json_pointer(contentJson, pointer);
      if (!value) throw std::runtime_error("missing required JSON pointer: " + pointer);
      double n = 0.0;
      if (!scalar_to_completion_number(*value, n)) throw std::runtime_error("JSON pointer resolved to a non-finite value: " + pointer);
      values.emplace_back(n);
    }
    return Json(values);
  }

  const std::string pointer = extract.at("pointer").as_string();
  if (!pointer.empty()) {
    const Json* value = eval_json_pointer(contentJson, pointer);
    if (!value) throw std::runtime_error("missing required JSON pointer: " + pointer);
    if (value->is_array()) return validate_completion_values_array(*value);
    double n = 0.0;
    if (!scalar_to_completion_number(*value, n)) throw std::runtime_error("JSON pointer resolved to a non-finite value: " + pointer);
    return Json(Json::Array{n});
  }

  Json fallback = response_values_from_content(contentJson);
  if (!fallback.is_array()) throw std::runtime_error("provider response did not include completion values");
  return fallback;
}

Json normalize_completion_values(Json values, const Json& mapping) {
  if (!values.is_array()) return values;
  const Json& normalize = mapping.at("normalize");
  if (!normalize.is_object()) return values;
  const std::string mode = normalize.at("mode").as_string("passthrough");
  const bool clamp = normalize.at("clamp").as_bool(false);
  Json::Array out;
  for (const auto& item : values.array()) {
    double n = item.as_number();
    if (mode == "minmax") {
      const double min = normalize.at("min").as_number();
      const double max = normalize.at("max").as_number();
      const double span = max - min;
      n = span == 0.0 ? 0.0 : (n - min) / span;
    } else if (mode == "linear") {
      n = n * normalize.at("scale").as_number(1.0) + normalize.at("offset").as_number(0.0);
    }
    out.emplace_back(clamp ? clamp01(n) : n);
  }
  return Json(out);
}

Json completion_values_from_content(const Json& contentJson, const Json& mapping) {
  if (contentJson.is_null()) throw std::runtime_error("provider response content is not valid JSON");
  Json values = mapping.is_object()
    ? extract_completion_values_for_mapping(contentJson, mapping)
    : response_values_from_content(contentJson);
  if (!values.is_array()) throw std::runtime_error("provider response did not include completion values");
  return mapping.is_object() ? normalize_completion_values(values, mapping) : values;
}

Json completion_schema_for_mapping(const Json& mapping) {
  const Json& extract = mapping.at("extract");
  if (mapping.is_object() && extract.is_object() && extract.at("type").as_string() == "json") {
    Json::Array pointers;
    if (extract.at("pointers").is_array()) pointers = extract.at("pointers").array();
    else if (extract.at("pointer").is_string()) pointers.push_back(extract.at("pointer"));
    if (!pointers.empty()) {
      Json::Object properties;
      Json::Array required;
      for (const auto& item : pointers) {
        std::string key = json_pointer_top_level_key(item.as_string());
        if (key.empty()) key = "value";
        properties[key] = Json::Object{{"type", Json::Array{"number", "boolean"}}};
        required.push_back(key);
      }
      return Json::Object{
        {"type", "object"},
        {"additionalProperties", false},
        {"properties", Json(properties)},
        {"required", Json(required)}
      };
    }
  }
  return Json::Object{
    {"type", "object"},
    {"additionalProperties", false},
    {"properties", Json::Object{{"values", Json::Object{{"type", "array"}, {"items", Json::Object{{"type", "number"}}}}}}},
    {"required", Json::Array{"values"}}
  };
}

Json openai_text_format_for_mapping(const Json& mapping) {
  return Json::Object{
    {"format", Json::Object{
      {"type", "json_schema"},
      {"name", "reality_engine_completion"},
      {"strict", true},
      {"schema", completion_schema_for_mapping(mapping)}
    }}
  };
}

std::string test_source_id(const std::string& machineId) {
  std::ostringstream out;
  out << "test-" << source_id_part(machineId);
  return out.str();
}

bool endpoint_allowed(const std::string& endpoint) {
  static const std::vector<std::string> prefixes{
    "/",
    "/health",
    "/chat",
    "/rag/query",
    "/rag/ingest/text",
    "/graph/schema",
    "/graph/rag",
    "/graph/agent",
    "/graphql",
  };
  std::string path = endpoint.substr(0, endpoint.find('?'));
  for (const auto& prefix : prefixes) {
    if (path == prefix || (prefix != "/" && path.rfind(prefix + "/", 0) == 0)) return true;
  }
  return false;
}

class PerceptionService {
public:
  PerceptionService(std::string realityUrl, std::string localAIUrl, std::string localAIMachinesDir, int vectorDimension, bool bootstrapLocalAI)
      : realityEngineUrl(std::move(realityUrl)),
        localAIBaseUrl(std::move(localAIUrl)),
        localAIMachinesDirectory(std::move(localAIMachinesDir)),
        engine(vectorDimension) {
    if (const char* enabled = std::getenv("TRIGGERS_ENABLED")) triggerDispatchEnabled = truthy_env(enabled);
    if (const char* mode = std::getenv("TRIGGER_DISPATCH_MODE")) triggerDispatchMode = mode;
    if (triggerDispatchMode.empty()) triggerDispatchMode = "dry-run";
    if (const char* endpoint = std::getenv("TRIGGER_GRAPHQL_URL")) triggerGraphQLEndpoint = endpoint;
    if (triggerGraphQLEndpoint.empty()) triggerGraphQLEndpoint = localAIBaseUrl + "/graphql";
    load_integration_registry();
    configure_ollama_from_environment();
    sync_test_sources_from_reality();
    if (bootstrapLocalAI) {
      try {
        bootstrap_localai();
      } catch (const std::exception& e) {
        std::cerr << "localAI bootstrap skipped: " << e.what() << "\n";
      }
    }
    pushWorker = std::thread([this]() { push_worker_loop(); });
    // MQTT bridge — disabled unless MQTT_BROKER_HOST is set.  When enabled,
    // the bridge connects to the broker in its own I/O thread and forwards
    // each PUBLISH (after extract + normalize + length-validate) into
    // feed_mqtt_signal() below, which mirrors the body of the existing
    // POST /api/signals handler minus the HTTP shell.  The push trigger
    // fires an async do_push when a mapping with pushMode != "manual"
    // settles its debounce window.
    if (auto envCfg = mqtt::MqttBridge::from_environment()) {
      try {
        size_t mappingCount = envCfg->registry->size();
        mqttBridge = std::make_unique<mqtt::MqttBridge>(
          envCfg->client, std::move(envCfg->registry),
          [this](const std::string& sensorId, int offset, int length,
                 const Vector& values, long ttlMs,
                 const std::string& topic, const std::string& mappingId) {
            feed_mqtt_signal(sensorId, offset, length, values, ttlMs, topic, mappingId);
          },
          [this]() { do_push(/*includeMachineResults=*/false, /*async=*/true); });
        mqttBridge->start();
        std::cerr << "MQTT bridge enabled — broker=" << envCfg->client.brokerHost
                  << ":" << envCfg->client.brokerPort
                  << " mappings=" << mappingCount << "\n";
      } catch (const std::exception& e) {
        std::cerr << "MQTT bridge failed to start: " << e.what() << "\n";
        mqttBridge.reset();
      }
    }
  }

  ~PerceptionService() {
    if (mqttBridge) mqttBridge->stop();
    stop_auto();
    {
      std::lock_guard<std::mutex> lock(pushQueueMutex);
      stopPushWorker = true;
    }
    pushQueueCondition.notify_one();
    if (pushWorker.joinable()) pushWorker.join();
  }

  void mount(http::Server& server) {
    server.route("GET", "/", [](const http::Request&) {
      return ok(Json::Object{{"service", "Perception Engine (C++)"}, {"status", "running"}});
    });
    server.websocket("/ws", wsHub, [this](std::shared_ptr<http::Server::WebSocketConnection> connection) {
      connection->send(state_update_message());
    });
    server.sse("/api/events", sseHub);
    server.route("GET", "/api/health", [](const http::Request&) {
      return ok(Json::Object{{"status", "healthy"}});
    });
    server.route("GET", "/api/state", [this](const http::Request&) {
      sync_test_sources_from_reality();
      std::lock_guard<std::mutex> lock(stateMutex);
      return ok(engine.state_json(lastPush, autoRunning, autoIntervalMs));
    });
    // Prometheus exposition — RealityEngine_Machines docs/PE_METRICS_CONTRACT.md.
    // The semantic_* block must stay byte-identical across PE runtimes after
    // normalizing the runtime label, so the writer below is deliberately
    // literal and must not be "tidied" into a different ordering or wording.
    server.route("GET", "/api/metrics", [this](const http::Request&) {
      int sources = 0;
      long long globalStep = 0;
      long long lastPushMs = 0;
      int vectorSize = 0;
      {
        std::lock_guard<std::mutex> lock(stateMutex);
        sources = static_cast<int>(engine.get_sources().size());
        globalStep = engine.globalStep;
        lastPushMs = lastPush.value_or(0);
        vectorSize = engine.vector_dimension();
      }
      return http::Response{200, semantic_metrics_text(sources, globalStep, vectorSize, lastPushMs),
                            "text/plain; charset=utf-8"};
    });
    server.route("GET", "/api/integrations/localai/status", [this](const http::Request&) {
      return ok(localai_status());
    });
    server.route("GET", "/api/integrations/localai/catalog", [this](const http::Request&) {
      return ok(localai_catalog());
    });
    server.route("POST", "/api/integrations/localai/bootstrap", [this](const http::Request&) {
      return ok(bootstrap_localai());
    });
    server.route("POST", "/api/integrations/localai/invoke", [this](const http::Request& req) {
      return invoke_localai(parse_body(req));
    });
    server.route("GET", "/api/integrations/ollama/status", [this](const http::Request&) {
      return ok(ollama_status());
    });
    server.route("POST", "/api/integrations/ollama/dispatch", [this](const http::Request& req) {
      return dispatch_ollama(parse_body(req));
    });
    server.route("GET", "/api/integrations/openai/status", [this](const http::Request&) {
      return ok(openai_status());
    });
    server.route("POST", "/api/integrations/openai/dispatch", [this](const http::Request& req) {
      return dispatch_openai(parse_body(req));
    });
    server.route("GET", "/api/integrations/acp/status", [this](const http::Request&) {
      return ok(acp_status());
    });
    server.route("POST", "/api/integrations/acp/dispatch", [this](const http::Request& req) {
      return dispatch_acp(parse_body(req));
    });
    server.route("GET", "/api/integrations/healthkit/status", [this](const http::Request&) {
      return ok(healthkit_status());
    });
    server.route("POST", "/api/integrations/healthkit/ingest", [this](const http::Request& req) {
      return ingest_healthkit(parse_body(req), bearer_token(req));
    });
    server.route("GET", "/api/integrations/carekit/status", [this](const http::Request&) {
      return ok(carekit_status());
    });
    server.route("POST", "/api/integrations/carekit/ingest", [this](const http::Request& req) {
      return ingest_carekit(parse_body(req));
    });
    server.route("GET", "/api/integrations/status", [this](const http::Request&) {
      return ok(integration_status());
    });
    server.route("POST", "/api/integrations/completions", [this](const http::Request& req) {
      return ingest_completion(parse_body(req));
    });
    server.route("GET", "/api/triggers/status", [this](const http::Request&) {
      return ok(trigger_status());
    });
    server.route("GET", "/api/dispatch/ledger", [this](const http::Request&) {
      return ok(dispatch_ledger());
    });
    server.route("GET", "/api/dispatch/records/:id", [this](const http::Request& req) {
      return read_dispatch_record(req.pathParams.at("id"));
    });
    server.route("PATCH", "/api/dispatch/records/:id", [this](const http::Request& req) {
      return update_dispatch_record(req.pathParams.at("id"), parse_body(req));
    });
    server.route("POST", "/api/signals", [this](const http::Request& req) {
      return ingest_signal(parse_body(req));
    });
    // MQTT bridge status — connection state, bridge-level counters,
    // per-mapping counters.  Returns "enabled":false when the bridge is
    // not configured (env-driven; see MqttBridge::from_environment).
    server.route("GET", "/api/mqtt/status", [this](const http::Request&) {
      if (!mqttBridge) return ok(Json::Object{{"enabled", false}});
      auto clientStats = mqttBridge->client_stats();
      auto bridgeStats = mqttBridge->stats();
      const auto& cfg = mqttBridge->config();
      return ok(Json::Object{
        {"enabled",          true},
        {"connected",        mqttBridge->is_connected()},
        {"brokerHost",       cfg.brokerHost},
        {"brokerPort",       static_cast<double>(cfg.brokerPort)},
        {"clientId",         cfg.clientId},
        {"connectAttempts",  static_cast<double>(clientStats.connectAttempts)},
        {"connectSuccesses", static_cast<double>(clientStats.connectSuccesses)},
        {"messagesReceived", static_cast<double>(clientStats.messagesReceived)},
        {"bytesReceived",    static_cast<double>(clientStats.bytesReceived)},
        {"pingsSent",        static_cast<double>(clientStats.pingsSent)},
        {"reconnects",       static_cast<double>(clientStats.reconnects)},
        {"lastMessageAtMs",  static_cast<double>(clientStats.lastMessageAtMs)},
        {"bridge", Json::Object{
          {"messagesMapped",   static_cast<double>(bridgeStats.messagesMapped)},
          {"messagesRejected", static_cast<double>(bridgeStats.messagesRejected)},
          {"messagesUnmatched",static_cast<double>(bridgeStats.messagesUnmatched)},
          {"pushesTriggered",  static_cast<double>(bridgeStats.pushesTriggered)},
        }},
      });
    });
    // MQTT mapping registry — the loaded rules with per-mapping counters.
    // Surfaces the authority for how topics project into perceptual space
    // (per the design rule: mappings, not topic strings, encode RE offsets).
    server.route("GET", "/api/mqtt/mappings", [this](const http::Request&) {
      if (!mqttBridge) return ok(Json::Object{{"enabled", false}, {"mappings", Json::Array{}}});
      Json out = mqttBridge->registry().to_json();
      if (out.is_object()) out.object()["enabled"] = true;
      return ok(out);
    });
    // PUT /api/mqtt/mappings — hot-reload the mapping registry without
    // bouncing the broker connection.  Stops the existing bridge, constructs
    // a new one with the same client config + callbacks, and re-starts it.
    // Returns 409 if no broker config exists (PE was started without MQTT).
    server.route("PUT", "/api/mqtt/mappings", [this](const http::Request& req) {
      if (!mqttBridge)
        return http::error_response(
          "no broker config — set MQTT_BROKER_HOST at PE startup before reloading mappings", 409);
      auto body = parse_body(req);
      std::unique_ptr<mqtt::MappingRegistry> newRegistry;
      try {
        newRegistry = std::make_unique<mqtt::MappingRegistry>(mqtt::MappingRegistry::from_json(body));
      } catch (const std::exception& e) {
        return http::error_response(std::string("schema: ") + e.what(), 400);
      }
      if (newRegistry->size() == 0)
        return http::error_response(
          "mappings array is empty — at least one rule is required", 400);
      const char* overlapEnv = std::getenv("MQTT_ALLOW_REGION_OVERLAP");
      bool allowOverlap = overlapEnv &&
        (std::string(overlapEnv) == "1" || std::string(overlapEnv) == "true");
      auto warnings = newRegistry->validate_overlaps(allowOverlap);
      mqtt::ClientConfig cfg = mqttBridge->config();
      mqttBridge->stop();
      mqttBridge = std::make_unique<mqtt::MqttBridge>(
        cfg, std::move(newRegistry),
        [this](const std::string& sId, int off, int len, const Vector& vals, long ttl,
               const std::string& topic, const std::string& mId) {
          feed_mqtt_signal(sId, off, len, vals, ttl, topic, mId);
        },
        [this]() { do_push(false, true); });
      mqttBridge->start();
      hub_broadcast(json::stringify(Json::Object{
        {"type", "mqtt-mappings-reloaded"},
        {"mappings", static_cast<double>(mqttBridge->registry().size())},
        {"timestamp", static_cast<double>(now_ms())},
      }));
      Json::Array warnArr;
      for (const auto& w : warnings) warnArr.push_back(w);
      return ok(Json::Object{
        {"success", true},
        {"enabled", true},
        {"mappings", static_cast<double>(mqttBridge->registry().size())},
        {"warnings", warnArr},
      });
    });
    // POST /api/mqtt/enable — accept brokerUrl + mappings registry, start bridge.
    // POST /api/mqtt/disable — stop bridge and clear it.
    server.route("POST", "/api/mqtt/enable", [this](const http::Request& req) {
      auto body = parse_body(req);
      // Parse mqtt://host:port or host:port
      std::string brokerUrl = body.at("brokerUrl").as_string("");
      if (brokerUrl.empty())
        return http::error_response("brokerUrl is required", 400);
      std::string addr = brokerUrl;
      auto schemeEnd = addr.find("://");
      if (schemeEnd != std::string::npos) addr = addr.substr(schemeEnd + 3);
      std::string brokerHost = addr;
      int brokerPort = 1883;
      auto colonPos = addr.rfind(':');
      if (colonPos != std::string::npos) {
        brokerHost = addr.substr(0, colonPos);
        try { brokerPort = std::stoi(addr.substr(colonPos + 1)); } catch (...) {}
      }
      // Parse mapping registry
      auto mappingsVal = body.at("mappings");
      std::unique_ptr<mqtt::MappingRegistry> newRegistry;
      try {
        newRegistry = std::make_unique<mqtt::MappingRegistry>(mqtt::MappingRegistry::from_json(mappingsVal));
      } catch (const std::exception& e) {
        return http::error_response(std::string("schema: ") + e.what(), 400);
      }
      if (newRegistry->size() == 0)
        return http::error_response("mappings array is empty — at least one rule is required", 400);
      const char* overlapEnv = std::getenv("MQTT_ALLOW_REGION_OVERLAP");
      bool allowOverlap = overlapEnv &&
        (std::string(overlapEnv) == "1" || std::string(overlapEnv) == "true");
      auto warnings = newRegistry->validate_overlaps(allowOverlap);
      // Preserve clientId from existing bridge if present
      std::string clientId = mqttBridge ? mqttBridge->config().clientId : "reality-engine-pe";
      if (mqttBridge) { mqttBridge->stop(); mqttBridge.reset(); }
      mqtt::ClientConfig cfg;
      cfg.brokerHost = brokerHost;
      cfg.brokerPort = brokerPort;
      cfg.clientId   = clientId;
      // Bug 5 fix: extract optional credentials from request body.
      std::string usernameVal = body.at("username").as_string("");
      std::string passwordVal = body.at("password").as_string("");
      if (!usernameVal.empty()) cfg.username = usernameVal;
      if (!passwordVal.empty()) cfg.password = passwordVal;
      try {
        mqttBridge = std::make_unique<mqtt::MqttBridge>(
          cfg, std::move(newRegistry),
          [this](const std::string& sId, int off, int len, const Vector& vals, long ttl,
                 const std::string& topic, const std::string& mId) {
            feed_mqtt_signal(sId, off, len, vals, ttl, topic, mId);
          },
          [this]() { do_push(false, true); });
        mqttBridge->start();
        std::cerr << "MQTT bridge enabled via API — broker=" << brokerHost
                  << ":" << brokerPort << "\n";
      } catch (const std::exception& e) {
        mqttBridge.reset();
        return http::error_response(std::string("MQTT bridge failed to start: ") + e.what(), 500);
      }
      Json::Array warnArr;
      for (const auto& w : warnings) warnArr.push_back(w);
      return ok(Json::Object{
        {"success", true},
        {"enabled", true},
        {"mappings", static_cast<double>(mqttBridge->registry().size())},
        {"warnings", warnArr},
      });
    });
    server.route("POST", "/api/mqtt/disable", [this](const http::Request&) {
      if (mqttBridge) {
        mqttBridge->stop();
        mqttBridge.reset();
        std::cerr << "MQTT bridge disabled via API\n";
      }
      return ok(Json::Object{{"success", true}, {"enabled", false}});
    });

    server.route("POST", "/api/push", [this](const http::Request& req) {
      auto body = parse_body(req);
      bool includeMachineResults = body.at("includeMachineResults").as_bool(!body.at("compact").as_bool(false));
      bool async = body.at("async").as_bool(false);
      return do_push(includeMachineResults, async);
    });
    server.route("GET", "/api/push/:id", [this](const http::Request& req) {
      return read_push_job(req.pathParams.at("id"));
    });
    server.route("POST", "/api/auto/start", [this](const http::Request& req) {
      auto body = parse_body(req);
      start_auto(static_cast<long>(body.at("intervalMs").as_number(1000)));
      return ok(Json::Object{{"success", true}, {"intervalMs", static_cast<double>(autoIntervalMs)}});
    });
    server.route("POST", "/api/auto/stop", [this](const http::Request&) {
      stop_auto();
      return ok(Json::Object{{"success", true}});
    });
    server.route("PATCH", "/api/config", [this](const http::Request& req) {
      auto body = parse_body(req);
      std::string algorithm;
      {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (body.at("matchAlgorithm").is_string()) engine.matchAlgorithm = match_algorithm_from_string(body.at("matchAlgorithm").as_string());
        algorithm = to_string(engine.matchAlgorithm);
      }
      broadcast_state();
      return ok(Json::Object{{"success", true}, {"matchAlgorithm", algorithm}});
    });
    server.route("POST", "/api/reset", [this](const http::Request&) {
      {
        std::lock_guard<std::mutex> lock(stateMutex);
        engine.reset();
        lastPush.reset();
      }
      broadcast_state();
      return ok(Json::Object{{"success", true}});
    });
    server.route("GET", "/api/sources", [this](const http::Request&) {
      sync_test_sources_from_reality();
      Json::Array arr;
      std::lock_guard<std::mutex> lock(stateMutex);
      for (const auto& s : engine.get_sources()) arr.push_back(to_json(s));
      return ok(Json::Object{{"sources", arr}});
    });
    server.route("POST", "/api/sources", [this](const http::Request& req) {
      SourceConfig src;
      {
        std::lock_guard<std::mutex> lock(stateMutex);
        src = engine.add_source(source_from_json(parse_body(req)));
      }
      broadcast_state();
      return ok(Json::Object{{"source", to_json(src)}});
    });
    server.route("PATCH", "/api/sources/:id", [this](const http::Request& req) {
      SourceConfig added;
      {
        std::lock_guard<std::mutex> lock(stateMutex);
        auto existing = engine.get_source(req.pathParams.at("id"));
        if (!existing) return http::error_response("Source not found", 404);
        engine.remove_source(req.pathParams.at("id"));
        auto updated = merge_source_patch(*existing, parse_body(req));
        updated.id = req.pathParams.at("id");
        added = engine.add_source(updated);
      }
      broadcast_state();
      return ok(Json::Object{{"source", to_json(added)}});
    });
    server.route("DELETE", "/api/sources/:id", [this](const http::Request& req) {
      {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (!engine.remove_source(req.pathParams.at("id"))) return http::error_response("Source not found", 404);
      }
      broadcast_state();
      return ok(Json::Object{{"success", true}});
    });
    server.route("POST", "/api/sources/bootstrap-from-machines", [this](const http::Request&) {
      // Fetch /api/machines from RE and materialise one consolidated test source
      // per machine not already registered.  Delegates to the existing private
      // helper which holds stateMutex only for the duration of each individual
      // engine.add_source() call, so the HTTP fetch runs without the lock.
      BootstrapSummary summary = bootstrap_test_sources_from_reality();
      broadcast_state();
      return ok(summary.to_json());
    });
    server.route("POST", "/api/sensors/:sensorId", [this](const http::Request& req) {
      auto values = json::to_numbers(parse_body(req).at("values"));
      bool found = false;
      {
        std::lock_guard<std::mutex> lock(stateMutex);
        found = engine.update_sensor_value(req.pathParams.at("sensorId"), values);
      }
      if (!found) return http::error_response("No sensor source with sensorId \"" + req.pathParams.at("sensorId") + "\"", 404);
      broadcast_state();
      return ok(Json::Object{{"success", true}, {"sensorId", req.pathParams.at("sensorId")}, {"timestamp", static_cast<double>(now_ms())}});
    });
    server.route("GET", "/api/machines", [this](const http::Request&) {
      try {
        std::string raw = http::get(realityEngineUrl + "/api/machines");
        sync_test_sources_from_machine_list(json::parse(raw));
        return http::json_response(raw);
      } catch (const std::exception& e) {
        return http::error_response(e.what(), 502);
      }
    });
  }

private:
  struct PushJob {
    std::string id;
    std::shared_ptr<std::promise<http::Response>> result;
    bool includeMachineResults = true;
  };
  struct PushRecord {
    std::string id;
    std::string status = "queued";
    std::string body = "{}";
    long long createdAt = 0;
    long long updatedAt = 0;
  };
  struct DispatchRecord {
    std::string id;
    std::string envelopeId;
    std::string correlationId;
    std::string status = "recorded";
    std::string mode = "dry-run";
    std::string target;
    std::string machineId;
    std::string sequenceId;
    std::string ragStatusCode;
    std::string processStatus;
    std::string error;
    long long createdAt = 0;
    long long updatedAt = 0;
    int attempts = 0;
    Json providerReceipt = nullptr;
    Json envelope = Json::Object{};
  };
  struct TriggerDispatchSummary {
    int mergeOps = 0;
    int envelopesCreated = 0;
    int dispatchRecordsCreated = 0;
    int droppedNoGovernance = 0;
    int droppedNoDispatch = 0;
    int errors = 0;
  };
  struct DispatchBinding {
    std::string agent;
    std::string trigger;
    std::string action;
    std::string autonomyMode;
    Json::Array actions;
    Json writeBack = nullptr;
  };

  bool sensor_exists(const std::string& sensorId) const {
    for (const auto& s : engine.get_sources()) {
      if (s.kind == "sensor" && s.sensorId == sensorId) return true;
    }
    return false;
  }

  std::optional<SourceConfig> sensor_source_by_id(const std::string& sensorId) const {
    for (const auto& s : engine.get_sources()) {
      if (s.kind == "sensor" && s.sensorId == sensorId) return s;
    }
    return std::nullopt;
  }

  static SourceConfig merge_source_patch(SourceConfig source, const Json& patch) {
    if (patch.at("name").is_string()) source.name = patch.at("name").as_string();
    if (patch.at("active").is_bool()) source.active = patch.at("active").as_bool();
    if (patch.at("region").is_object()) {
      source.region.offset = static_cast<int>(patch.at("region").at("offset").as_number(source.region.offset));
      source.region.length = static_cast<int>(patch.at("region").at("length").as_number(source.region.length));
    }
    if (source.kind == "test") {
      if (patch.at("loop").is_bool()) source.loop = patch.at("loop").as_bool();
    } else if (source.kind == "sensor") {
      if (patch.at("sensorId").is_string()) source.sensorId = patch.at("sensorId").as_string();
      if (patch.at("ttlMs").is_number()) source.ttlMs = static_cast<long>(patch.at("ttlMs").as_number());
    } else {
      if (patch.at("pattern").is_string()) source.pattern = sim_pattern_from_string(patch.at("pattern").as_string());
      if (patch.at("frequency").is_number()) source.frequency = patch.at("frequency").as_number();
      if (patch.at("amplitude").is_number()) source.amplitude = patch.at("amplitude").as_number();
      if (patch.at("dcOffset").is_number()) source.dcOffset = patch.at("dcOffset").as_number();
    }
    return source;
  }

  void start_auto(long intervalMs) {
    stop_auto();
    {
      std::lock_guard<std::mutex> lock(stateMutex);
      autoIntervalMs = intervalMs > 0 ? intervalMs : 1000;
      autoRunning = true;
    }
    {
      std::lock_guard<std::mutex> lock(autoMutex);
      stopAutoWorker = false;
    }
    autoWorker = std::thread([this]() {
      while (true) {
        long interval = 1000;
        {
          std::lock_guard<std::mutex> lock(stateMutex);
          interval = autoIntervalMs;
        }
        std::unique_lock<std::mutex> lock(autoMutex);
        if (autoCv.wait_for(lock, std::chrono::milliseconds(interval), [this]() { return stopAutoWorker; })) return;
        lock.unlock();
        (void)do_push(true, false);
      }
    });
  }

  void stop_auto() {
    {
      std::lock_guard<std::mutex> lock(autoMutex);
      stopAutoWorker = true;
    }
    autoCv.notify_all();
    if (autoWorker.joinable()) autoWorker.join();
    std::lock_guard<std::mutex> lock(stateMutex);
    autoRunning = false;
  }

  std::set<std::string> existing_machine_names() const {
    std::set<std::string> names;
    try {
      Json data = json::parse(http::get(realityEngineUrl + "/api/machines"));
      for (const auto& m : data.at("machines").is_array() ? data.at("machines").array() : Json::Array{}) {
        names.insert(m.at("name").as_string());
      }
    } catch (...) {
    }
    return names;
  }

  size_t sync_test_sources_from_machine(const Json& machine) {
    std::string machineId = machine.at("id").as_string();
    if (machineId.empty()) return 0;
    const Json& mapping = machine.at("perceptualMapping");
    if (!mapping.is_object() || !mapping.at("input").is_object()) return 0;
    RegionMapping region{
      static_cast<int>(mapping.at("input").at("offset").as_number()),
      static_cast<int>(mapping.at("input").at("length").as_number())
    };
    const Json& sequences = machine.at("metadata").at("inputSequences");
    if (!sequences.is_array()) return 0;

    std::lock_guard<std::mutex> lock(stateMutex);
    // One source per machine — the machine encapsulates its test sequences
    // and stages them end-to-end inside a single source.  Concatenated
    // `inputs` plays sequence-1 to completion, then sequence-2, …, then
    // loops the entire set.  `segments` retains per-sequence boundaries
    // for UI display.
    const std::string id = test_source_id(machineId);
    if (engine.get_source(id)) return 0;

    SourceConfig source;
    source.kind = "test";
    source.id = id;
    source.machineId = machineId;
    source.machineName = machine.at("name").as_string(machineId);
    source.region = region;
    source.active = false;  // overridden below if any segment was marked active
    source.loop = true;

    Json::Array segments;
    std::vector<std::string> segmentNames;
    for (size_t i = 0; i < sequences.array().size(); ++i) {
      const Json& seq = sequences.array()[i];
      if (!seq.at("vectors").is_array() || seq.at("vectors").array().empty()) continue;
      std::string segName = seq.at("name").as_string("Test sequence");
      segmentNames.push_back(segName);
      size_t beforeLen = source.inputs.size();
      for (const auto& vector : seq.at("vectors").array())
        source.inputs.push_back(json::to_numbers(vector));
      size_t segLen = source.inputs.size() - beforeLen;
      segments.push_back(Json::Object{
        {"name",   segName},
        {"length", static_cast<double>(segLen)},
      });
      if (seq.at("active").as_bool(false)) source.active = true;
    }
    if (source.inputs.empty()) return 0;

    if (segmentNames.size() == 1) {
      source.sequenceName = segmentNames.front();
      source.name = source.machineName + " / " + segmentNames.front();
    } else {
      std::ostringstream label;
      label << segmentNames.size() << " sequences";
      source.sequenceName = label.str();
      source.name = source.machineName + " / " + label.str();
    }
    source.sequenceMetadata = Json::Object{{"segments", Json(segments)}};
    source.testSequence = Json::Object{};

    engine.add_source(source);
    return 1;
  }

  // Migration: prior bootstrap created one source per sequence.  Clear
  // any pre-existing per-machine source group so the consolidated source
  // above can replace it; runs once per bootstrap call.
  void consolidate_stale_test_sources() {
    std::lock_guard<std::mutex> lock(stateMutex);
    std::map<std::string, std::vector<std::string>> perMachine;
    for (const auto& s : engine.get_sources()) {
      if (s.kind != "test" || s.machineId.empty()) continue;
      perMachine[s.machineId].push_back(s.id);
    }
    for (const auto& [_mid, ids] : perMachine) {
      if (ids.size() > 1) for (const auto& id : ids) engine.remove_source(id);
    }
  }

  size_t sync_test_sources_from_machine_list(const Json& data) {
    return bootstrap_test_sources_from_machine_list(data).created;
  }

  BootstrapSummary bootstrap_test_sources_from_machine_list(const Json& data) {
    cache_machine_catalog(data);
    consolidate_stale_test_sources();
    BootstrapSummary summary;
    for (const auto& machine : data.at("machines").is_array() ? data.at("machines").array() : Json::Array{}) {
      summary.machinesSeen++;
      size_t added = sync_test_sources_from_machine(machine);
      summary.created += added;
      if (added == 0) summary.skipped++;
    }
    return summary;
  }

  size_t sync_test_sources_from_reality() {
    return bootstrap_test_sources_from_reality().created;
  }

  BootstrapSummary bootstrap_test_sources_from_reality() {
    try {
      return bootstrap_test_sources_from_machine_list(json::parse(http::get(realityEngineUrl + "/api/machines")));
    } catch (const std::exception& e) {
      BootstrapSummary summary;
      summary.errors.push_back(e.what());
      return summary;
    } catch (...) {
      BootstrapSummary summary;
      summary.errors.push_back("bootstrap failed");
      return summary;
    }
  }

  Json localai_status() const {
    Json health = nullptr;
    Json root = nullptr;
    bool reachable = false;
    try {
      root = json::parse(http::get(localAIBaseUrl + "/"));
      health = json::parse(http::get(localAIBaseUrl + "/health"));
      reachable = true;
    } catch (const std::exception& e) {
      health = Json::Object{{"error", e.what()}};
    }

    Json::Array sensors;
    {
      std::lock_guard<std::mutex> lock(stateMutex);
      for (const auto& spec : localai_sensor_specs()) {
        sensors.push_back(Json::Object{
          {"sensorId", spec.sensorId},
          {"name", spec.name},
          {"region", to_json(spec.region)},
          {"registered", sensor_exists(spec.sensorId)},
        });
      }
    }

    return Json::Object{
      {"localAIBaseUrl", localAIBaseUrl},
      {"reachable", reachable},
      {"root", root},
      {"health", health},
      {"sensors", sensors},
      {"machineDirectory", localAIMachinesDirectory},
    };
  }

  Json localai_catalog() const {
    Json status = localai_status();
    Json graphSchema = nullptr;
    Json events = nullptr;
    try {
      graphSchema = json::parse(http::get(localAIBaseUrl + "/graph/schema"));
    } catch (const std::exception& e) {
      graphSchema = Json::Object{{"error", e.what()}};
    }
    try {
      events = json::parse(http::get(localAIBaseUrl + "/graphql/events"));
    } catch (const std::exception& e) {
      events = Json::Object{{"error", e.what()}};
    }

    Json::Array endpoints{
      Json::Object{{"id", "health"}, {"method", "GET"}, {"path", "/health"}, {"description", "Operational health for API, Ollama, Qdrant, and Redis."}},
      Json::Object{{"id", "graph_schema"}, {"method", "GET"}, {"path", "/graph/schema"}, {"description", "LangGraph topology and Reality Engine binding schema."}},
      Json::Object{{"id", "graph_rag"}, {"method", "POST"}, {"path", "/graph/rag"}, {"description", "Run corrective RAG graph."}},
      Json::Object{{"id", "graph_agent"}, {"method", "POST"}, {"path", "/graph/agent"}, {"description", "Run ReAct agent graph."}},
      Json::Object{{"id", "rag_query"}, {"method", "POST"}, {"path", "/rag/query"}, {"description", "Run RAG query endpoint."}},
      Json::Object{{"id", "rag_ingest_text"}, {"method", "POST"}, {"path", "/rag/ingest/text"}, {"description", "Ingest text into localAIStack Qdrant collection."}},
      Json::Object{{"id", "chat"}, {"method", "POST"}, {"path", "/chat"}, {"description", "Call Ollama-backed chat endpoint."}},
      Json::Object{{"id", "graphql"}, {"method", "POST"}, {"path", "/graphql"}, {"description", "GraphQL trigger receiver."}},
      Json::Object{{"id", "graphql_events"}, {"method", "GET"}, {"path", "/graphql/events"}, {"description", "Recent GraphQL trigger events."}},
    };

    broadcast_state();
    return Json::Object{
      {"success", true},
      {"status", status},
      {"graphSchema", graphSchema},
      {"recentGraphQLEvents", events},
      {"invokeEndpoint", "/api/integrations/localai/invoke"},
      {"allowedEndpoints", endpoints},
      {"realityBridge", Json::Object{
        {"sensors", Json::Array{
          "localai_rag_retrieval",
          "localai_rag_grading",
          "localai_agent_activity"
        }},
        {"bootstrapEndpoint", "/api/integrations/localai/bootstrap"},
        {"signalEndpoint", "/api/signals"}
      }}
    };
  }

  void load_integration_registry() {
    std::string path;
    if (const char* configured = std::getenv("INTEGRATIONS_CONFIG")) path = configured;
    if (path.empty() && fs::exists("config/integrations.json")) path = "config/integrations.json";
    if (path.empty()) return;

    std::lock_guard<std::mutex> lock(integrationMutex);
    integrationConfigPath = path;
    integrationRegistryLoaded = false;
    integrationRegistryError.clear();
    sourceMappingRegistry.clear();

    try {
      std::ifstream in(path);
      if (!in) throw std::runtime_error("unable to open integrations config: " + path);
      std::ostringstream buffer;
      buffer << in.rdbuf();
      integrationConfig = json::parse(buffer.str());
      const Json& mappings = integrationConfig.at("sourceMappings");
      if (mappings.is_array()) {
        for (const auto& mapping : mappings.array()) {
          std::string id = mapping.at("id").as_string();
          if (!id.empty()) sourceMappingRegistry[id] = mapping;
        }
      }
      const Json& integrations = integrationConfig.at("integrations");
      if (integrations.is_array()) {
        for (const auto& item : integrations.array()) {
          const std::string kind = item.at("kind").as_string();
          if (kind == "ollama") {
            if (item.at("baseUrl").is_string()) ollamaBaseUrl = item.at("baseUrl").as_string();
            if (item.at("model").is_string()) ollamaModel = item.at("model").as_string();
            if (item.at("completionSourceMappingId").is_string()) ollamaCompletionSourceMappingId = item.at("completionSourceMappingId").as_string();
          } else if (kind == "openai") {
            if (item.at("baseUrl").is_string()) openaiBaseUrl = item.at("baseUrl").as_string();
            if (item.at("model").is_string()) openaiModel = item.at("model").as_string();
            if (item.at("completionSourceMappingId").is_string()) openaiCompletionSourceMappingId = item.at("completionSourceMappingId").as_string();
          } else if (kind == "acp" || kind == "openclaw-acp") {
            acpEnabled = item.at("enabled").as_bool(acpEnabled);
            if (item.at("platform").is_string()) acpPlatform = item.at("platform").as_string();
            if (item.at("surface").is_string()) acpSurface = item.at("surface").as_string();
            if (item.at("command").is_string()) acpCommand = item.at("command").as_string();
            if (item.at("gatewayUrl").is_string()) acpGatewayUrl = item.at("gatewayUrl").as_string();
            if (item.at("sessionKey").is_string()) acpSessionKey = item.at("sessionKey").as_string();
            if (item.at("targetAgent").is_string()) acpTargetAgent = item.at("targetAgent").as_string();
            if (item.at("completionSourceMappingId").is_string()) acpCompletionSourceMappingId = item.at("completionSourceMappingId").as_string();
          } else if (kind == "healthkit") {
            if (item.at("bridgeId").is_string()) healthKitBridgeId = item.at("bridgeId").as_string();
            if (item.at("defaultSourceMappingId").is_string()) healthKitDefaultSourceMappingId = item.at("defaultSourceMappingId").as_string();
          } else if (kind == "carekit") {
            if (item.at("bridgeId").is_string()) careKitBridgeId = item.at("bridgeId").as_string();
            if (item.at("defaultSourceMappingId").is_string()) careKitDefaultSourceMappingId = item.at("defaultSourceMappingId").as_string();
          }
        }
      }
      integrationRegistryLoaded = true;
      std::cerr << "Integration registry loaded — path=" << path
                << " sourceMappings=" << sourceMappingRegistry.size() << "\n";
    } catch (const std::exception& e) {
      integrationConfig = Json::Object{};
      integrationRegistryError = e.what();
      std::cerr << "Integration registry failed to load: " << integrationRegistryError << "\n";
    }
  }

  void configure_ollama_from_environment() {
    if (const char* baseUrl = std::getenv("OLLAMA_BASE_URL")) ollamaBaseUrl = baseUrl;
    if (const char* model = std::getenv("OLLAMA_MODEL")) ollamaModel = model;
    if (const char* mapping = std::getenv("OLLAMA_COMPLETION_SOURCE_MAPPING_ID")) ollamaCompletionSourceMappingId = mapping;
    while (!ollamaBaseUrl.empty() && ollamaBaseUrl.back() == '/') ollamaBaseUrl.pop_back();
    if (const char* baseUrl = std::getenv("OPENAI_BASE_URL")) openaiBaseUrl = baseUrl;
    if (const char* model = std::getenv("OPENAI_MODEL")) openaiModel = model;
    if (const char* mapping = std::getenv("OPENAI_COMPLETION_SOURCE_MAPPING_ID")) openaiCompletionSourceMappingId = mapping;
    if (const char* apiKey = std::getenv("OPENAI_API_KEY")) openaiApiKey = apiKey;
    while (!openaiBaseUrl.empty() && openaiBaseUrl.back() == '/') openaiBaseUrl.pop_back();
    if (const char* enabled = std::getenv("ACP_ENABLED")) acpEnabled = truthy_env(enabled);
    if (const char* platform = std::getenv("ACP_PLATFORM")) acpPlatform = platform;
    if (const char* surface = std::getenv("ACP_SURFACE")) acpSurface = surface;
    if (const char* command = std::getenv("ACP_COMMAND")) acpCommand = command;
    if (const char* command = std::getenv("OPENCLAW_ACP_COMMAND")) acpCommand = command;
    if (const char* url = std::getenv("ACP_GATEWAY_URL")) acpGatewayUrl = url;
    if (const char* url = std::getenv("OPENCLAW_GATEWAY_URL")) acpGatewayUrl = url;
    if (const char* session = std::getenv("ACP_SESSION_KEY")) acpSessionKey = session;
    if (const char* session = std::getenv("OPENCLAW_ACP_SESSION")) acpSessionKey = session;
    if (const char* agent = std::getenv("ACP_TARGET_AGENT")) acpTargetAgent = agent;
    if (const char* mapping = std::getenv("ACP_COMPLETION_SOURCE_MAPPING_ID")) acpCompletionSourceMappingId = mapping;
    if (const char* bridgeId = std::getenv("HEALTHKIT_BRIDGE_ID")) healthKitBridgeId = bridgeId;
    if (const char* mapping = std::getenv("HEALTHKIT_DEFAULT_SOURCE_MAPPING_ID")) healthKitDefaultSourceMappingId = mapping;
    if (const char* token = std::getenv("HEALTHKIT_BRIDGE_TOKEN")) healthKitBridgeToken = token;
    if (const char* bridgeId = std::getenv("CAREKIT_BRIDGE_ID")) careKitBridgeId = bridgeId;
    if (const char* mapping = std::getenv("CAREKIT_DEFAULT_SOURCE_MAPPING_ID")) careKitDefaultSourceMappingId = mapping;
    if (const char* token = std::getenv("CAREKIT_BRIDGE_TOKEN")) careKitBridgeToken = token;
  }

  Json configured_source_mapping(const std::string& id) const {
    std::lock_guard<std::mutex> lock(integrationMutex);
    auto it = sourceMappingRegistry.find(id);
    return it == sourceMappingRegistry.end() ? Json(nullptr) : it->second;
  }

  Json integration_status() const {
    std::lock_guard<std::mutex> lock(integrationMutex);
    Json::Array integrations;
    const Json& configuredIntegrations = integrationConfig.at("integrations");
    if (configuredIntegrations.is_array()) {
      for (const auto& item : configuredIntegrations.array()) {
        integrations.push_back(Json::Object{
          {"id", item.at("id").as_string()},
          {"kind", item.at("kind").as_string()},
          {"enabled", item.at("enabled").as_bool(false)}
        });
      }
    }
    Json::Array sourceMappings;
    for (const auto& [id, mapping] : sourceMappingRegistry) {
      sourceMappings.push_back(Json::Object{
        {"id", id},
        {"sensorId", mapping.at("sensorId").as_string()},
        {"sensorIdTemplate", mapping.at("sensorIdTemplate").as_string()},
        {"region", mapping.at("region").is_object() ? mapping.at("region") : Json(nullptr)},
        {"ttlMs", mapping.at("ttlMs").is_number() ? mapping.at("ttlMs") : Json(nullptr)}
      });
    }
    return Json::Object{
      {"loaded", integrationRegistryLoaded},
      {"path", integrationConfigPath.empty() ? Json(nullptr) : Json(integrationConfigPath)},
      {"error", integrationRegistryError.empty() ? Json(nullptr) : Json(integrationRegistryError)},
      {"integrationCount", static_cast<double>(integrations.size())},
      {"sourceMappingCount", static_cast<double>(sourceMappingRegistry.size())},
      {"integrations", integrations},
      {"sourceMappings", sourceMappings},
      {"completionEndpoint", "/api/integrations/completions"},
      {"ollama", Json::Object{
        {"baseUrl", ollamaBaseUrl},
        {"model", ollamaModel},
        {"completionSourceMappingId", ollamaCompletionSourceMappingId},
        {"statusEndpoint", "/api/integrations/ollama/status"},
        {"dispatchEndpoint", "/api/integrations/ollama/dispatch"}
      }},
      {"openai", Json::Object{
        {"baseUrl", openaiBaseUrl},
        {"model", openaiModel},
        {"hasApiKey", !openaiApiKey.empty()},
        {"completionSourceMappingId", openaiCompletionSourceMappingId},
        {"statusEndpoint", "/api/integrations/openai/status"},
        {"dispatchEndpoint", "/api/integrations/openai/dispatch"}
      }},
      {"acp", acp_status()},
      {"healthkit", Json::Object{
        {"bridgeId", healthKitBridgeId},
        {"tokenConfigured", !healthKitBridgeToken.empty()},
        {"registryKey", "healthkit:<typeIdentifier>"},
        {"statusEndpoint", "/api/integrations/healthkit/status"},
        {"ingestEndpoint", "/api/integrations/healthkit/ingest"}
      }},
      {"carekit", Json::Object{
        {"bridgeId", careKitBridgeId},
        {"defaultSourceMappingId", careKitDefaultSourceMappingId},
        {"tokenConfigured", !careKitBridgeToken.empty()},
        {"statusEndpoint", "/api/integrations/carekit/status"},
        {"ingestEndpoint", "/api/integrations/carekit/ingest"}
      }}
    };
  }

  Json healthkit_status() const {
    return Json::Object{
      {"bridgeId", healthKitBridgeId},
      {"enabled", true},
      {"tokenConfigured", !healthKitBridgeToken.empty()},
      {"nativeAppRequired", true},
      {"nativeWorkOutsideRepo", true},
      {"registryKey", "healthkit:<typeIdentifier>"},
      {"statusEndpoint", "/api/integrations/healthkit/status"},
      {"ingestEndpoint", "/api/integrations/healthkit/ingest"},
      {"contract", Json::Object{
        {"transport", "https"},
        {"singleSample", Json::Array{"type", "value", "sourceName"}},
        {"batchSamples", Json::Array{"bridgeId", "samples[]"}},
        {"auth", healthKitBridgeToken.empty() ? "none" : "bridgeToken|bearer"}
      }}
    };
  }

  Json carekit_status() const {
    return Json::Object{
      {"bridgeId", careKitBridgeId},
      {"enabled", true},
      {"defaultSourceMappingId", careKitDefaultSourceMappingId},
      {"tokenConfigured", !careKitBridgeToken.empty()},
      {"nativeAppRequired", true},
      {"nativeWorkOutsideRepo", true},
      {"registryKey", "carekit:<sampleType>"},
      {"statusEndpoint", "/api/integrations/carekit/status"},
      {"ingestEndpoint", "/api/integrations/carekit/ingest"},
      {"contract", Json::Object{
        {"transport", "https"},
        {"singleSample", Json::Array{"bridgeId", "sampleType", "sourceMappingId", "values"}},
        {"batchSamples", Json::Array{"bridgeId", "samples[]"}},
        {"auth", careKitBridgeToken.empty() ? "external-transport" : "bridgeToken"}
      }}
    };
  }

  Json ollama_status() const {
    Json tags = nullptr;
    Json error = nullptr;
    bool reachable = false;
    try {
      tags = json::parse(http::get(ollamaBaseUrl + "/api/tags"));
      reachable = true;
    } catch (const std::exception& e) {
      error = Json(e.what());
    }
    return Json::Object{
      {"baseUrl", ollamaBaseUrl},
      {"model", ollamaModel},
      {"completionSourceMappingId", ollamaCompletionSourceMappingId},
      {"reachable", reachable},
      {"tags", tags},
      {"error", error},
      {"dispatchEndpoint", "/api/integrations/ollama/dispatch"}
    };
  }

  Json openai_status() const {
    Json models = nullptr;
    Json error = nullptr;
    bool reachable = false;
    if (openaiApiKey.empty()) {
      error = "OPENAI_API_KEY is not configured";
    } else {
      try {
        models = json::parse(http::request_json("GET", openaiBaseUrl + "/models", "", openai_headers()));
        reachable = !models.at("error").is_object();
        if (!reachable) error = models.at("error");
      } catch (const std::exception& e) {
        error = Json(e.what());
      }
    }
    return Json::Object{
      {"baseUrl", openaiBaseUrl},
      {"model", openaiModel},
      {"hasApiKey", !openaiApiKey.empty()},
      {"completionSourceMappingId", openaiCompletionSourceMappingId},
      {"reachable", reachable},
      {"models", models},
      {"error", error},
      {"dispatchEndpoint", "/api/integrations/openai/dispatch"}
    };
  }

  Json acp_status() const {
    return Json::Object{
      {"enabled", acpEnabled},
      {"platform", acpPlatform},
      {"surface", acpSurface},
      {"adapter", "openclaw-xacp"},
      {"command", acpCommand},
      {"gatewayUrl", acpGatewayUrl.empty() ? Json(nullptr) : Json(acpGatewayUrl)},
      {"sessionKey", acpSessionKey.empty() ? Json(nullptr) : Json(acpSessionKey)},
      {"targetAgent", acpTargetAgent},
      {"completionSourceMappingId", acpCompletionSourceMappingId},
      {"dispatchEndpoint", "/api/integrations/acp/dispatch"},
      {"completionEndpoint", "/api/integrations/completions"},
      {"noWaitDispatch", true},
      {"contract", Json::Object{
        {"dispatch", "Record an ACP/OpenClaw handoff receipt only; do not run or wait for the harness in the PE cycle."},
        {"completion", "External ACP/OpenClaw adapters commit finished results through /api/integrations/completions."}
      }}
    };
  }

  Json trigger_status() const {
    std::lock_guard<std::mutex> lock(dispatchMutex);
    return Json::Object{
      {"enabled", triggerDispatchEnabled},
      {"mode", triggerDispatchMode},
      {"graphqlEndpoint", triggerGraphQLEndpoint},
      {"records", static_cast<double>(dispatchRecords.size())},
      {"envelopesCreated", static_cast<double>(triggerEnvelopesCreated)},
      {"droppedNoGovernance", static_cast<double>(triggerDroppedNoGovernance)},
      {"droppedNoDispatch", static_cast<double>(triggerDroppedNoDispatch)},
      {"dispatchErrors", static_cast<double>(triggerDispatchErrors)}
    };
  }

  Json dispatch_record_json(const DispatchRecord& r) const {
    Json::Object out{
      {"id", r.id},
      {"envelopeId", r.envelopeId},
      {"correlationId", r.correlationId},
      {"status", r.status},
      {"mode", r.mode},
      {"target", r.target},
      {"machineId", r.machineId},
      {"sequenceId", r.sequenceId},
      {"ragStatusCode", r.ragStatusCode.empty() ? Json(nullptr) : Json(r.ragStatusCode)},
      {"processStatus", r.processStatus.empty() ? Json(nullptr) : Json(r.processStatus)},
      {"attempts", static_cast<double>(r.attempts)},
      {"createdAt", static_cast<double>(r.createdAt)},
      {"updatedAt", static_cast<double>(r.updatedAt)},
      {"providerReceipt", r.providerReceipt.is_null() ? Json(nullptr) : r.providerReceipt},
      {"envelope", r.envelope}
    };
    if (!r.error.empty()) out["error"] = r.error;
    return out;
  }

  Json dispatch_ledger() const {
    Json::Array arr;
    std::lock_guard<std::mutex> lock(dispatchMutex);
    for (const auto& id : dispatchRecordOrder) {
      auto it = dispatchRecords.find(id);
      if (it != dispatchRecords.end()) arr.push_back(dispatch_record_json(it->second));
    }
    return Json::Object{
      {"enabled", triggerDispatchEnabled},
      {"mode", triggerDispatchMode},
      {"records", arr}
    };
  }

  http::Response read_dispatch_record(const std::string& id) const {
    std::lock_guard<std::mutex> lock(dispatchMutex);
    auto it = dispatchRecords.find(id);
    if (it == dispatchRecords.end()) return http::error_response("Dispatch record not found", 404);
    return ok(Json::Object{{"record", dispatch_record_json(it->second)}});
  }

  std::optional<DispatchRecord> dispatch_record_snapshot(const std::string& id) const {
    std::lock_guard<std::mutex> lock(dispatchMutex);
    auto it = dispatchRecords.find(id);
    if (it == dispatchRecords.end()) return std::nullopt;
    return it->second;
  }

  http::Response update_dispatch_record(const std::string& id, const Json& body) {
    if (!body.is_object()) return http::error_response("dispatch update body must be a JSON object", 400);
    DispatchRecord updated;
    {
      std::lock_guard<std::mutex> lock(dispatchMutex);
      auto it = dispatchRecords.find(id);
      if (it == dispatchRecords.end()) return http::error_response("Dispatch record not found", 404);

      DispatchRecord& record = it->second;
      if (body.at("status").is_string()) record.status = body.at("status").as_string();
      if (body.at("error").is_string()) record.error = body.at("error").as_string();
      if (body.at("clearError").as_bool(false)) record.error.clear();
      if (body.at("attempts").is_number()) record.attempts = static_cast<int>(body.at("attempts").as_number());
      else if (body.at("incrementAttempts").as_bool(false)) ++record.attempts;

      Json receipt = record.providerReceipt.is_object() ? record.providerReceipt : Json::Object{};
      if (body.at("providerReceipt").is_object()) receipt = merge_objects(receipt, body.at("providerReceipt"));
      if (body.at("provider").is_string()) receipt.object()["provider"] = body.at("provider").as_string();
      if (body.at("adapter").is_string()) receipt.object()["adapter"] = body.at("adapter").as_string();
      if (body.at("externalRunId").is_string()) receipt.object()["externalRunId"] = body.at("externalRunId").as_string();
      if (!receipt.object().empty()) record.providerReceipt = receipt;

      record.updatedAt = now_ms();
      updated = record;
    }

    hub_broadcast(json::stringify(Json::Object{
      {"type", "dispatch.record.updated"},
      {"dispatchId", updated.id},
      {"status", updated.status},
      {"target", updated.target},
      {"attempts", static_cast<double>(updated.attempts)},
      {"timestamp", static_cast<double>(updated.updatedAt)}
    }));

    return ok(Json::Object{
      {"success", true},
      {"record", dispatch_record_json(updated)}
    });
  }

  http::Response invoke_localai(const Json& body) const {
    std::string method = body.at("method").as_string("POST");
    std::transform(method.begin(), method.end(), method.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::string endpoint;
    if (body.at("endpoint").is_string()) endpoint = body.at("endpoint").as_string();
    else if (body.at("path").is_string()) endpoint = body.at("path").as_string();
    else return http::error_response("localAI invocation requires endpoint or path", 400);
    try {
      endpoint = normalize_endpoint(endpoint);
      if (!endpoint_allowed(endpoint)) return http::error_response("localAI endpoint is not allowed: " + endpoint, 403);
      std::string raw;
      if (method == "GET") {
        raw = http::get(localAIBaseUrl + endpoint);
      } else if (method == "POST") {
        Json payload = body.at("payload").is_null() ? Json::Object{} : body.at("payload");
        raw = http::post_json(localAIBaseUrl + endpoint, json::stringify(payload));
      } else {
        return http::error_response("localAI invocation supports GET and POST only", 400);
      }
      Json parsed = json::parse(raw);
      return ok(Json::Object{{"success", true}, {"endpoint", endpoint}, {"method", method}, {"response", parsed}});
    } catch (const std::exception& e) {
      return http::json_response(json::stringify(Json::Object{{"success", false}, {"endpoint", endpoint}, {"method", method}, {"error", e.what()}}), 502);
    }
  }

  static Json parse_json_or_null(const std::string& raw) {
    try {
      return json::parse(raw);
    } catch (...) {
      return Json(nullptr);
    }
  }

  std::map<std::string, std::string> openai_headers() const {
    if (openaiApiKey.empty()) throw std::runtime_error("OPENAI_API_KEY is not configured");
    return {
      {"Authorization", "Bearer " + openaiApiKey}
    };
  }

  static void assert_openai_response_ready(const Json& response) {
    if (response.at("error").is_object()) {
      throw std::runtime_error(json::stringify(response.at("error")));
    }
    const std::string status = response.at("status").as_string();
    if (!status.empty() && status != "completed") {
      throw std::runtime_error("OpenAI response did not complete: " + status);
    }
    if (response.at("refusal").is_string() && !response.at("refusal").as_string().empty()) {
      throw std::runtime_error("OpenAI response refused: " + response.at("refusal").as_string());
    }
    const Json& output = response.at("output");
    if (!output.is_array()) return;
    for (const auto& item : output.array()) {
      const Json& content = item.at("content");
      if (!content.is_array()) continue;
      for (const auto& part : content.array()) {
        if (part.at("type").as_string() == "refusal") {
          throw std::runtime_error("OpenAI response refused");
        }
        if (part.at("refusal").is_string() && !part.at("refusal").as_string().empty()) {
          throw std::runtime_error("OpenAI response refused: " + part.at("refusal").as_string());
        }
      }
    }
  }

  static std::string openai_response_text(const Json& response) {
    if (response.at("output_text").is_string()) return response.at("output_text").as_string();
    const Json& output = response.at("output");
    if (!output.is_array()) return "";
    std::ostringstream text;
    for (const auto& item : output.array()) {
      const Json& content = item.at("content");
      if (!content.is_array()) continue;
      for (const auto& part : content.array()) {
        std::string type = part.at("type").as_string();
        if ((type == "output_text" || type == "text") && part.at("text").is_string()) {
          if (text.tellp() > 0) text << "\n";
          text << part.at("text").as_string();
        }
      }
    }
    return text.str();
  }

  Json build_ollama_payload(const DispatchRecord& record, const Json& body, const std::string& model) const {
    if (body.at("payload").is_object()) return body.at("payload");
    const std::string systemPrompt = body.at("systemPrompt").as_string(
      "You are a local PE-controlled Ollama adapter. Analyze the trigger envelope and respond as JSON. "
      "When committing a PE completion, include a numeric values array matching the configured source mapping.");
    const std::string userPrompt = body.at("prompt").as_string(
      "Review this RealityEngine trigger envelope and produce a concise JSON result for PE completion ingest.");
    Json::Array messages{
      Json::Object{{"role", "system"}, {"content", systemPrompt}},
      Json::Object{{"role", "user"}, {"content", userPrompt + "\n\n" + json::stringify(record.envelope)}}
    };
    Json::Object payload{
      {"model", model},
      {"stream", false},
      {"messages", messages}
    };
    if (body.at("format").is_string()) payload["format"] = body.at("format").as_string();
    else if (body.at("formatJson").as_bool(true)) payload["format"] = "json";
    if (body.at("options").is_object()) payload["options"] = body.at("options");
    return Json(payload);
  }

  http::Response dispatch_ollama(const Json& body) {
    if (!body.is_object()) return http::error_response("Ollama dispatch body must be a JSON object", 400);
    const std::string dispatchId = body.at("dispatchId").as_string(body.at("id").as_string());
    if (dispatchId.empty()) return http::error_response("Ollama dispatch requires dispatchId", 400);
    auto maybeRecord = dispatch_record_snapshot(dispatchId);
    if (!maybeRecord) return http::error_response("Dispatch record not found", 404);
    DispatchRecord record = *maybeRecord;

    const std::string model = body.at("model").as_string(ollamaModel);
    const std::string sourceMappingId = body.at("sourceMappingId").as_string(ollamaCompletionSourceMappingId);
    Json completionMapping = sourceMappingId.empty() ? Json(nullptr) : configured_source_mapping(sourceMappingId);
    if (!sourceMappingId.empty() && !completionMapping.is_object()) return http::error_response("Unknown sourceMappingId \"" + sourceMappingId + "\"", 404);
    (void)update_dispatch_record(dispatchId, Json::Object{
      {"status", "delivering"},
      {"adapter", "ollama"},
      {"provider", "ollama"},
      {"incrementAttempts", true},
      {"clearError", true},
      {"providerReceipt", Json::Object{
        {"baseUrl", ollamaBaseUrl},
        {"model", model}
      }}
    });

    try {
      Json payload = build_ollama_payload(record, body, model);
      std::string raw = http::post_json(ollamaBaseUrl + "/api/chat", json::stringify(payload));
      Json response = json::parse(raw);
      std::string content = response.at("message").at("content").as_string(response.at("response").as_string());
      Json contentJson = parse_json_or_null(content);

      Json completionResult = nullptr;
      bool completionCommitted = false;
      if (body.at("commitCompletion").as_bool(true) && !sourceMappingId.empty()) {
        Json values = body.at("values").is_array() ? validate_completion_values_array(body.at("values")) : completion_values_from_content(contentJson, completionMapping);
        Json::Object completionBody{
          {"provider", "ollama"},
          {"agent", record.target.empty() ? "ollama" : record.target},
          {"sourceMappingId", sourceMappingId},
          {"correlationId", record.correlationId},
          {"envelopeId", record.envelopeId},
          {"completionId", make_id("ollama-completion")},
          {"values", values},
          {"metadata", Json::Object{
            {"model", model},
            {"dispatchId", dispatchId},
            {"content", content}
          }},
          {"triggerPush", body.at("triggerPush").as_bool(false)}
        };
        http::Response completionResponse = ingest_completion(Json(completionBody));
        completionResult = parse_json_or_null(completionResponse.body);
        completionCommitted = completionResponse.status >= 200 && completionResponse.status < 300;
        if (!completionCommitted) throw std::runtime_error("completion ingest failed: " + completionResponse.body);
      }

      (void)update_dispatch_record(dispatchId, Json::Object{
        {"status", "delivered"},
        {"adapter", "ollama"},
        {"provider", "ollama"},
        {"externalRunId", response.at("created_at").as_string(make_id("ollama-run"))},
        {"providerReceipt", Json::Object{
          {"baseUrl", ollamaBaseUrl},
          {"model", model},
          {"content", content},
          {"completionCommitted", completionCommitted}
        }}
      });

      return ok(Json::Object{
        {"success", true},
        {"dispatchId", dispatchId},
        {"provider", "ollama"},
        {"model", model},
        {"response", response},
        {"contentJson", contentJson},
        {"completionCommitted", completionCommitted},
        {"completion", completionResult},
        {"receipt", Json::Object{
          {"provider", "ollama"},
          {"adapter", "ollama"},
          {"status", "sent"},
          {"externalRunId", response.at("created_at").as_string()},
          {"providerReceipt", Json::Object{{"model", model}, {"completionCommitted", completionCommitted}}}
        }}
      });
    } catch (const std::exception& e) {
      (void)update_dispatch_record(dispatchId, Json::Object{
        {"status", "failed"},
        {"adapter", "ollama"},
        {"provider", "ollama"},
        {"error", e.what()},
        {"providerReceipt", Json::Object{
          {"baseUrl", ollamaBaseUrl},
          {"model", model}
        }}
      });
      return http::json_response(json::stringify(Json::Object{
        {"success", false},
        {"dispatchId", dispatchId},
        {"provider", "ollama"},
        {"model", model},
        {"error", e.what()},
        {"receipt", Json::Object{
          {"provider", "ollama"},
          {"adapter", "ollama"},
          {"status", "failed"},
          {"error", e.what()},
          {"providerReceipt", Json::Object{{"model", model}}}
        }}
      }), 502);
    }
  }

  Json build_openai_payload(const DispatchRecord& record, const Json& body, const std::string& model, const std::string& sourceMappingId) const {
    if (body.at("payload").is_object()) return body.at("payload");
    const Json mapping = sourceMappingId.empty() ? Json(nullptr) : configured_source_mapping(sourceMappingId);
    const std::string instructions = body.at("instructions").as_string(
      "You are a PE-controlled OpenAI adapter. Analyze the trigger envelope and respond as JSON. "
      "When committing a PE completion, include a numeric values array matching the configured source mapping.");
    const std::string prompt = body.at("prompt").as_string(
      "Review this RealityEngine trigger envelope and produce a concise JSON result for PE completion ingest.");
    Json::Object payload{
      {"model", model},
      {"instructions", instructions},
      {"input", prompt + "\n\n" + json::stringify(record.envelope)},
      {"store", body.at("store").as_bool(true)},
      {"stream", false},
      {"metadata", Json::Object{
        {"dispatchId", record.id},
        {"envelopeId", record.envelopeId},
        {"correlationId", record.correlationId},
        {"target", record.target}
      }}
    };
    if (body.at("maxOutputTokens").is_number()) payload["max_output_tokens"] = body.at("maxOutputTokens");
    if (body.at("temperature").is_number()) payload["temperature"] = body.at("temperature");
    if (body.at("reasoning").is_object()) payload["reasoning"] = body.at("reasoning");
    if (body.at("text").is_object()) payload["text"] = body.at("text");
    else if (body.at("responseFormatMode").as_string("json-schema") == "json-object") payload["text"] = Json::Object{{"format", Json::Object{{"type", "json_object"}}}};
    else payload["text"] = openai_text_format_for_mapping(mapping);
    return Json(payload);
  }

  http::Response dispatch_openai(const Json& body) {
    if (!body.is_object()) return http::error_response("OpenAI dispatch body must be a JSON object", 400);
    const std::string dispatchId = body.at("dispatchId").as_string(body.at("id").as_string());
    if (dispatchId.empty()) return http::error_response("OpenAI dispatch requires dispatchId", 400);
    auto maybeRecord = dispatch_record_snapshot(dispatchId);
    if (!maybeRecord) return http::error_response("Dispatch record not found", 404);
    DispatchRecord record = *maybeRecord;

    const std::string model = body.at("model").as_string(openaiModel);
    const std::string sourceMappingId = body.at("sourceMappingId").as_string(openaiCompletionSourceMappingId);
    Json completionMapping = sourceMappingId.empty() ? Json(nullptr) : configured_source_mapping(sourceMappingId);
    if (!sourceMappingId.empty() && !completionMapping.is_object()) return http::error_response("Unknown sourceMappingId \"" + sourceMappingId + "\"", 404);
    (void)update_dispatch_record(dispatchId, Json::Object{
      {"status", "delivering"},
      {"adapter", "openai"},
      {"provider", "openai"},
      {"incrementAttempts", true},
      {"clearError", true},
      {"providerReceipt", Json::Object{
        {"baseUrl", openaiBaseUrl},
        {"model", model},
        {"endpoint", "/responses"}
      }}
    });

    try {
      Json payload = build_openai_payload(record, body, model, sourceMappingId);
      std::string raw = http::request_json("POST", openaiBaseUrl + "/responses", json::stringify(payload), openai_headers());
      Json response = json::parse(raw);
      assert_openai_response_ready(response);
      std::string content = openai_response_text(response);
      Json contentJson = parse_json_or_null(content);

      Json completionResult = nullptr;
      bool completionCommitted = false;
      if (body.at("commitCompletion").as_bool(true) && !sourceMappingId.empty()) {
        Json values = body.at("values").is_array() ? validate_completion_values_array(body.at("values")) : completion_values_from_content(contentJson, completionMapping);
        Json::Object completionBody{
          {"provider", "openai"},
          {"agent", record.target.empty() ? "openai" : record.target},
          {"sourceMappingId", sourceMappingId},
          {"correlationId", record.correlationId},
          {"envelopeId", record.envelopeId},
          {"completionId", response.at("id").as_string(make_id("openai-completion"))},
          {"values", values},
          {"metadata", Json::Object{
            {"model", model},
            {"dispatchId", dispatchId},
            {"responseId", response.at("id").as_string()},
            {"content", content}
          }},
          {"triggerPush", body.at("triggerPush").as_bool(false)}
        };
        http::Response completionResponse = ingest_completion(Json(completionBody));
        completionResult = parse_json_or_null(completionResponse.body);
        completionCommitted = completionResponse.status >= 200 && completionResponse.status < 300;
        if (!completionCommitted) throw std::runtime_error("completion ingest failed: " + completionResponse.body);
      }

      (void)update_dispatch_record(dispatchId, Json::Object{
        {"status", "delivered"},
        {"adapter", "openai"},
        {"provider", "openai"},
        {"externalRunId", response.at("id").as_string()},
        {"providerReceipt", Json::Object{
          {"baseUrl", openaiBaseUrl},
          {"model", model},
          {"responseId", response.at("id").as_string()},
          {"status", response.at("status").as_string()},
          {"completionCommitted", completionCommitted}
        }}
      });

      return ok(Json::Object{
        {"success", true},
        {"dispatchId", dispatchId},
        {"provider", "openai"},
        {"model", model},
        {"response", response},
        {"contentJson", contentJson},
        {"completionCommitted", completionCommitted},
        {"completion", completionResult},
        {"receipt", Json::Object{
          {"provider", "openai"},
          {"adapter", "openai"},
          {"status", "sent"},
          {"externalRunId", response.at("id").as_string()},
          {"providerReceipt", Json::Object{{"model", model}, {"completionCommitted", completionCommitted}}}
        }}
      });
    } catch (const std::exception& e) {
      (void)update_dispatch_record(dispatchId, Json::Object{
        {"status", "failed"},
        {"adapter", "openai"},
        {"provider", "openai"},
        {"error", e.what()},
        {"providerReceipt", Json::Object{
          {"baseUrl", openaiBaseUrl},
          {"model", model},
          {"endpoint", "/responses"}
        }}
      });
      return http::json_response(json::stringify(Json::Object{
        {"success", false},
        {"dispatchId", dispatchId},
        {"provider", "openai"},
        {"model", model},
        {"error", e.what()},
        {"receipt", Json::Object{
          {"provider", "openai"},
          {"adapter", "openai"},
          {"status", "failed"},
          {"error", e.what()},
          {"providerReceipt", Json::Object{{"model", model}, {"endpoint", "/responses"}}}
        }}
      }), 502);
    }
  }

  http::Response dispatch_acp(const Json& body) {
    if (!body.is_object()) return http::error_response("ACP dispatch body must be a JSON object", 400);
    const std::string dispatchId = body.at("dispatchId").as_string(body.at("id").as_string());
    if (dispatchId.empty()) return http::error_response("ACP dispatch requires dispatchId", 400);
    auto maybeRecord = dispatch_record_snapshot(dispatchId);
    if (!maybeRecord) return http::error_response("Dispatch record not found", 404);
    DispatchRecord record = *maybeRecord;

    const std::string targetAgent = body.at("targetAgent").as_string(
      body.at("agent").as_string(record.target.empty() ? acpTargetAgent : record.target));
    const std::string sessionKey = body.at("sessionKey").as_string(acpSessionKey);
    const std::string sourceMappingId = body.at("sourceMappingId").as_string(acpCompletionSourceMappingId);
    const std::string prompt = body.at("prompt").as_string(
      "Handle this RealityEngine trigger envelope through the configured OpenClaw ACP session and return a PE completion values array.");
    const std::string externalRunId = body.at("externalRunId").as_string(make_id("acp-handoff"));

    Json::Object handoff{
      {"protocol", "ACP"},
      {"surface", acpSurface},
      {"platform", acpPlatform},
      {"adapter", "openclaw-xacp"},
      {"command", body.at("command").as_string(acpCommand)},
      {"gatewayUrl", body.at("gatewayUrl").as_string(acpGatewayUrl)},
      {"sessionKey", sessionKey.empty() ? Json(nullptr) : Json(sessionKey)},
      {"targetAgent", targetAgent},
      {"completionEndpoint", "/api/integrations/completions"},
      {"completionSourceMappingId", sourceMappingId},
      {"noWaitDispatch", true},
      {"prompt", prompt},
      {"dispatchId", dispatchId},
      {"envelopeId", record.envelopeId},
      {"correlationId", record.correlationId}
    };
    if (body.at("metadata").is_object()) handoff["metadata"] = body.at("metadata");

    (void)update_dispatch_record(dispatchId, Json::Object{
      {"status", body.at("status").as_string("accepted")},
      {"adapter", "openclaw-xacp"},
      {"provider", "acp"},
      {"externalRunId", externalRunId},
      {"incrementAttempts", body.at("incrementAttempts").as_bool(true)},
      {"clearError", true},
      {"providerReceipt", Json::Object(handoff)}
    });

    return http::json_response(json::stringify(Json::Object{
      {"success", true},
      {"accepted", true},
      {"dispatchId", dispatchId},
      {"provider", "acp"},
      {"platform", acpPlatform},
      {"surface", acpSurface},
      {"externalRunId", externalRunId},
      {"noWaitDispatch", true},
      {"handoff", Json(handoff)}
    }), 202);
  }

  Json bootstrap_localai() {
    Json::Array registeredSensors;
    Json::Array skippedSensors;
    {
      std::lock_guard<std::mutex> lock(stateMutex);
      for (const auto& spec : localai_sensor_specs()) {
        if (sensor_exists(spec.sensorId)) {
          skippedSensors.emplace_back(spec.sensorId);
          continue;
        }
        auto src = engine.add_source(sensor_source(spec));
        registeredSensors.push_back(to_json(src));
      }
    }

    Json::Array importedMachines;
    Json::Array skippedMachines;
    Json::Array failedMachines;
    auto names = existing_machine_names();
    for (const auto& filename : localai_machine_files()) {
      std::filesystem::path path = std::filesystem::path(localAIMachinesDirectory) / filename;
      try {
        std::ifstream in(path);
        if (!in) {
          failedMachines.push_back(Json::Object{{"file", filename}, {"error", "not found"}});
          continue;
        }
        std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Machine machine = load_machine_from_json_string(raw);
        if (names.count(machine.name)) {
          skippedMachines.emplace_back(machine.name);
          continue;
        }
        Json response = json::parse(http::post_json(realityEngineUrl + "/api/machines", raw));
        Json imported = response.at("machine").is_object() ? response.at("machine") : machine.to_json();
        sync_test_sources_from_machine(imported);
        importedMachines.push_back(imported);
        names.insert(machine.name);
      } catch (const std::exception& e) {
        failedMachines.push_back(Json::Object{{"file", filename}, {"error", e.what()}});
      }
    }

    return Json::Object{
      {"success", true},
      {"registeredSensors", registeredSensors},
      {"skippedSensors", skippedSensors},
      {"importedMachines", importedMachines},
      {"skippedMachines", skippedMachines},
      {"failedMachines", failedMachines},
      {"localAIBaseUrl", localAIBaseUrl},
      {"machineDirectory", localAIMachinesDirectory},
    };
  }

  // Internal ingest path used by the MQTT bridge.  Mirrors the body of
  // ingest_signal() minus the HTTP shell: takes the same stateMutex, calls
  // engine.update_sensor_value()/add_source(), then broadcasts a state
  // update over the WebSocket hub so visualizers see the live signal.
  // Errors are logged (no caller to return an HTTP error to).  Per the
  // design rule, MQTT-sourced signals enter via exactly the same path as
  // HTTP /api/signals — nothing downstream knows or cares that the values
  // came from an MQTT broker.
  void feed_mqtt_signal(const std::string& sensorId,
                        int offset, int length,
                        const Vector& values,
                        long ttlMs,
                        const std::string& topic,
                        const std::string& mappingId) {
    if (values.empty()) return;
    if (static_cast<int>(values.size()) != length) return;
    SourceConfig source;
    {
      std::lock_guard<std::mutex> lock(stateMutex);
      bool updated = engine.update_sensor_value(sensorId, values);
      if (updated) {
        if (auto existing = sensor_source_by_id(sensorId)) source = *existing;
      } else {
        source.kind = "sensor";
        source.name = "mqtt:" + topic;
        source.sensorId = sensorId;
        source.region = { offset, length };
        source.active = true;
        source.ttlMs = ttlMs > 0 ? ttlMs : 30000;
        source.lastValue = values;
        source.lastUpdated = now_ms();
        source.origin = "mqtt";
        source = engine.add_source(source);
      }
    }
    hub_broadcast(json::stringify(Json::Object{
      {"type", "mqtt-ingest"},
      {"topic", topic},
      {"mappingId", mappingId},
      {"sensorId", sensorId},
      {"source", to_json(source)},
      {"timestamp", static_cast<double>(now_ms())},
    }));
  }

  http::Response ingest_signal(const Json& body) {
    Vector values = json::to_numbers(body.at("values"));
    if (values.empty()) return http::error_response("values must be a non-empty array", 400);
    const std::string signalOrigin = body.at("origin").as_string("signal");

    SourceConfig source;
    {
      std::lock_guard<std::mutex> lock(stateMutex);
      bool updated = false;
      if (body.at("sensorId").is_string()) {
        const std::string sensorId = body.at("sensorId").as_string();
        updated = engine.update_sensor_value(sensorId, values);
        if (updated) source = sensor_source_by_id(sensorId).value_or(source);
        if (!updated && body.at("region").is_object()) {
          source.kind = "sensor";
          source.name = body.at("name").as_string(sensorId);
          source.sensorId = sensorId;
          source.region = {
            static_cast<int>(body.at("region").at("offset").as_number()),
            static_cast<int>(body.at("region").at("length").as_number()),
          };
          source.active = body.at("active").as_bool(true);
          source.ttlMs = static_cast<long>(body.at("ttlMs").as_number(30000));
          source.lastValue = values;
          source.lastUpdated = now_ms();
          source.origin = signalOrigin;
          source = engine.add_source(source);
          updated = true;
        }
        if (!updated) return http::error_response("No sensor source with sensorId \"" + sensorId + "\"", 404);
      } else if (body.at("region").is_object()) {
        source.kind = "sensor";
        source.name = body.at("name").as_string("external/signal");
        source.sensorId = body.at("name").as_string(make_id("external-sensor"));
        source.region = {
          static_cast<int>(body.at("region").at("offset").as_number()),
          static_cast<int>(body.at("region").at("length").as_number()),
        };
        source.active = true;
        source.ttlMs = static_cast<long>(body.at("ttlMs").as_number(30000));
        source.lastValue = values;
        source.lastUpdated = now_ms();
        source.origin = signalOrigin;
        source = engine.add_source(source);
        updated = true;
      } else {
        return http::error_response("signal requires sensorId or region", 400);
      }
    }

    Json response = Json::Object{
      {"success", true},
      {"timestamp", static_cast<double>(now_ms())},
      {"source", source.id.empty() ? Json(nullptr) : to_json(source)},
    };
    if (body.at("triggerPush").as_bool(false)) {
      response.object()["push"] = json::parse(do_push(!body.at("compactPush").as_bool(false), false).body);
    }
    broadcast_state();
    return ok(response);
  }

  http::Response ingest_completion(const Json& body) {
    if (!body.is_object()) return http::error_response("completion body must be a JSON object", 400);

    const std::string sourceMappingId = body.at("sourceMappingId").as_string(body.at("mappingId").as_string());
    Json mapping = Json::Object{};
    if (!sourceMappingId.empty()) {
      mapping = configured_source_mapping(sourceMappingId);
      if (!mapping.is_object()) return http::error_response("Unknown sourceMappingId \"" + sourceMappingId + "\"", 404);
    }
    if (body.at("sourceMapping").is_object()) mapping = merge_objects(mapping, body.at("sourceMapping"));
    std::string provider = body.at("provider").as_string("external");
    std::string agent = body.at("agent").as_string(body.at("agentId").as_string("agent"));
    std::string sensorId = body.at("sensorId").as_string(mapping.at("sensorId").as_string());
    if (sensorId.empty() && mapping.at("sensorIdTemplate").is_string()) {
      sensorId = mapping.at("sensorIdTemplate").as_string();
      sensorId = replace_all(sensorId, "{provider}", source_id_part(provider));
      sensorId = replace_all(sensorId, "{agent}", source_id_part(agent));
      sensorId = replace_all(sensorId, "{correlationId}", source_id_part(body.at("correlationId").as_string()));
      sensorId = replace_all(sensorId, "{envelopeId}", source_id_part(body.at("envelopeId").as_string()));
    }
    if (sensorId.empty()) sensorId = "agent." + source_id_part(agent) + ".completion";

    Json::Object signal;
    signal["sensorId"] = sensorId;
    signal["name"] = body.at("name").as_string(mapping.at("name").as_string("agent:" + provider + "/" + agent + "/completion"));
    if (mapping.at("region").is_object()) signal["region"] = mapping.at("region");
    signal["values"] = body.at("values").is_array() ? body.at("values") : mapping.at("values");
    signal["active"] = mapping.at("active").as_bool(body.at("active").as_bool(true));
    signal["ttlMs"] = body.at("ttlMs").is_number() ? body.at("ttlMs") : mapping.at("ttlMs").is_number() ? mapping.at("ttlMs") : Json(300000);
    signal["triggerPush"] = body.at("triggerPush").as_bool(false);
    signal["compactPush"] = body.at("compactPush").as_bool(true);
    signal["origin"] = provider;

    http::Response signalResponse = ingest_signal(Json(signal));
    if (signalResponse.status < 200 || signalResponse.status >= 300) return signalResponse;

    Json signalResult = json::parse(signalResponse.body);
    Json::Object completion{
      {"provider", provider},
      {"agent", agent},
      {"sensorId", sensorId},
      {"sourceMappingId", sourceMappingId},
      {"correlationId", body.at("correlationId").as_string()},
      {"envelopeId", body.at("envelopeId").as_string()},
      {"completionId", body.at("completionId").as_string(body.at("id").as_string())},
      {"receivedAt", static_cast<double>(now_ms())}
    };
    if (body.at("metadata").is_object()) completion["metadata"] = body.at("metadata");

    hub_broadcast(json::stringify(Json::Object{
      {"type", "agent.completion.received"},
      {"provider", provider},
      {"agent", agent},
      {"sensorId", sensorId},
      {"sourceMappingId", sourceMappingId},
      {"correlationId", body.at("correlationId").as_string()},
      {"envelopeId", body.at("envelopeId").as_string()},
      {"timestamp", static_cast<double>(now_ms())},
    }));

    return ok(Json::Object{
      {"success", true},
      {"completion", Json(completion)},
      {"signal", signalResult}
    });
  }

  Json resolve_source_mapping_from_body(const Json& body, const std::string& defaultMappingId) const {
    const std::string sourceMappingId = body.at("sourceMappingId").as_string(body.at("mappingId").as_string(defaultMappingId));
    Json mapping = Json::Object{};
    if (!sourceMappingId.empty()) mapping = configured_source_mapping(sourceMappingId);
    if (!sourceMappingId.empty() && !mapping.is_object()) throw std::runtime_error("Unknown sourceMappingId \"" + sourceMappingId + "\"");
    if (body.at("sourceMapping").is_object()) mapping = merge_objects(mapping, body.at("sourceMapping"));
    if (mapping.is_object() && !sourceMappingId.empty()) mapping.object()["id"] = sourceMappingId;
    return mapping;
  }

  // ── HealthKit AI-model helpers ──────────────────────────────────────────

  // Collapse "HKQuantityTypeIdentifierHeartRate" → "heartrate" for sensorId slugs.
  // Mirrors compactHKIdentifier in AI HealthKitBridge.ts.
  static std::string compact_hk_identifier(const std::string& type) {
    static const std::vector<std::string> prefixes = {
      "HKQuantityTypeIdentifier", "HKCategoryTypeIdentifier",
      "HKWorkoutTypeIdentifier",  "HKCorrelationTypeIdentifier",
      "HKDocumentTypeIdentifier", "HKClinicalTypeIdentifier",
      "HKSeriesTypeIdentifier",   "HKTypeIdentifier",
    };
    std::string stripped = type;
    for (const auto& p : prefixes) {
      if (type.size() >= p.size() && type.substr(0, p.size()) == p) {
        stripped = type.substr(p.size());
        break;
      }
    }
    std::string out;
    for (char c : stripped) if (std::isalnum(static_cast<unsigned char>(c))) out += std::tolower(c);
    return out.empty() ? "unknown" : out;
  }

  // Priority: mapping.sensorId > sensorIdTemplate (tokens: {type},{sampleType},{source},{provider},{agent}) > slug.
  static std::string derive_hk_sensor_id(
      const std::string& type, const std::string& source_name, const Json& mapping) {
    const std::string fixed = mapping.at("sensorId").as_string();
    if (!fixed.empty()) return fixed;
    const std::string tpl = mapping.at("sensorIdTemplate").as_string();
    if (!tpl.empty()) {
      std::string id = tpl;
      id = replace_all(id, "{type}",       source_id_part(type));
      id = replace_all(id, "{sampleType}", source_id_part(type));
      id = replace_all(id, "{source}",     source_id_part(source_name));
      id = replace_all(id, "{provider}",   "healthkit");
      id = replace_all(id, "{agent}",      source_id_part(source_name));
      return id;
    }
    const std::string slug   = compact_hk_identifier(type);
    const std::string suffix = source_name.empty() ? "" : "." + source_id_part(source_name);
    return "hk." + slug + suffix;
  }

  // Two-level lookup: healthkit:<type>:<sourceName> wins over healthkit:<type>.
  Json lookup_hk_mapping(const std::string& type, const std::string& source_name) const {
    if (!source_name.empty()) {
      Json specific = configured_source_mapping("healthkit:" + type + ":" + source_name);
      if (specific.is_object()) return specific;
    }
    return configured_source_mapping("healthkit:" + type);
  }

  Json ingest_healthkit_one(const Json& body) {
    const std::string type        = body.at("type").as_string(body.at("sampleType").as_string());
    const std::string source_name = body.at("sourceName").as_string();

    if (type.empty())
      return Json::Object{{"unmapped", true}, {"type", type},
                          {"sourceName", source_name}, {"reason", "sample.type is required"}};

    Vector values;
    if (body.at("values").is_array()) {
      values = json::to_numbers(body.at("values"));
    } else if (body.at("value").is_number()) {
      values = { body.at("value").as_number() };
    }
    if (values.empty())
      return Json::Object{{"unmapped", true}, {"type", type},
                          {"sourceName", source_name}, {"reason", "sample.value must be a finite number"}};

    const std::string sourceMappingId = body.at("sourceMappingId").as_string(body.at("mappingId").as_string());
    Json mapping = Json::Object{};
    if (!sourceMappingId.empty()) {
      mapping = configured_source_mapping(sourceMappingId);
      if (!mapping.is_object())
        return Json::Object{{"unmapped", true}, {"type", type}, {"sourceName", source_name},
                            {"sourceMappingId", sourceMappingId},
                            {"reason", "unknown sourceMappingId \"" + sourceMappingId + "\""}};
      mapping.object()["id"] = sourceMappingId;
    } else {
      mapping = lookup_hk_mapping(type, source_name);
    }
    if (body.at("sourceMapping").is_object()) mapping = merge_objects(mapping, body.at("sourceMapping"));
    if (!mapping.is_object())
      return Json::Object{{"unmapped", true}, {"type", type}, {"sourceName", source_name},
                          {"reason", "no registry mapping (declare healthkit:" + type + "[:<sourceName>])"}};

    if (!mapping.at("region").is_object())
      return Json::Object{{"unmapped", true}, {"type", type},
                          {"sourceName", source_name}, {"reason", "mapping is missing region.offset/region.length"}};

    const std::string sensor_id = derive_hk_sensor_id(type, source_name, mapping);
    const long ttl_ms  = mapping.at("ttlMs").is_number()
                           ? static_cast<long>(mapping.at("ttlMs").as_number()) : 3600000L;
    const std::string name = mapping.at("name").as_string("healthkit:" + type);

    Json::Object signal;
    signal["sensorId"] = sensor_id;
    signal["name"]     = name;
    signal["region"]   = mapping.at("region");
    signal["values"]   = body.at("values").is_array() ? body.at("values") : Json(Json::Array{body.at("value").as_number()});
    signal["active"]   = true;
    signal["ttlMs"]    = static_cast<double>(ttl_ms);
    signal["origin"]   = "healthkit";
    http::Response resp = ingest_signal(Json(signal));
    Json parsed = parse_json_or_null(resp.body);

    if (resp.status < 200 || resp.status >= 300)
      return Json::Object{{"unmapped", true}, {"type", type}, {"sourceName", source_name},
                          {"reason", parsed.at("error").as_string("ingest_signal failed")}};

    Json::Array val_arr;
    for (double v : values) val_arr.push_back(v);
    return Json::Object{
      {"resolved",        true},
      {"sensorId",        sensor_id},
      {"name",            name},
      {"type",            type},
      {"sourceName",      source_name},
      {"sourceMappingId", mapping.at("id").as_string()},
      {"region",          mapping.at("region")},
      {"values",          val_arr},
      {"ttlMs",           static_cast<double>(ttl_ms)},
      {"source",          parsed.at("source")},
    };
  }

  http::Response ingest_healthkit(const Json& body, const std::string& bearerToken = "") {
    if (!body.is_object()) return http::error_response("HealthKit ingest body must be a JSON object", 400);
    const std::string hkToken = body.at("bridgeToken").as_string(body.at("token").as_string());
    if (!healthKitBridgeToken.empty()
        && hkToken != healthKitBridgeToken && bearerToken != healthKitBridgeToken)
      return http::error_response("HealthKit bridge token rejected", 401);

    Json::Array resolved, unmapped;
    try {
      const Json& samples = body.at("samples");
      if (samples.is_array()) {
        for (const auto& sample : samples.array()) {
          Json r = ingest_healthkit_one(sample);
          (r.at("resolved").as_bool(false) ? resolved : unmapped).push_back(r);
        }
      } else {
        Json r = ingest_healthkit_one(body);
        (r.at("resolved").as_bool(false) ? resolved : unmapped).push_back(r);
      }
    } catch (const std::exception& e) {
      return http::error_response(e.what(), 400);
    }

    const bool allResolved = unmapped.empty();
    const int  status      = allResolved ? 200 : (resolved.empty() ? 400 : 207);
    hub_broadcast(json::stringify(Json::Object{
      {"type",     "healthkit.ingest"},
      {"bridgeId", body.at("bridgeId").as_string(healthKitBridgeId)},
      {"samples",  static_cast<double>(resolved.size() + unmapped.size())},
      {"resolved", static_cast<double>(resolved.size())},
      {"unmapped", static_cast<double>(unmapped.size())},
      {"timestamp", static_cast<double>(now_ms())},
    }));
    return http::json_response(json::stringify(Json::Object{
      {"success",  allResolved},
      {"bridgeId", body.at("bridgeId").as_string(healthKitBridgeId)},
      {"resolved", resolved},
      {"unmapped", unmapped},
    }), status);
  }

  Json::Object build_carekit_signal(const Json& body, const Json& mapping) const {
    const std::string bridgeId = body.at("bridgeId").as_string(careKitBridgeId);
    const std::string sampleType = body.at("sampleType").as_string(body.at("type").as_string("task-event"));
    std::string sensorId = body.at("sensorId").as_string(mapping.at("sensorId").as_string());
    if (sensorId.empty() && mapping.at("sensorIdTemplate").is_string()) {
      sensorId = mapping.at("sensorIdTemplate").as_string();
      sensorId = replace_all(sensorId, "{bridgeId}", source_id_part(bridgeId));
      sensorId = replace_all(sensorId, "{sampleType}", source_id_part(sampleType));
      sensorId = replace_all(sensorId, "{type}", source_id_part(sampleType));
      sensorId = replace_all(sensorId, "{taskId}", source_id_part(body.at("taskId").as_string(sampleType)));
      sensorId = replace_all(sensorId, "{carePlanId}", source_id_part(body.at("carePlanId").as_string("care-plan")));
    }
    if (sensorId.empty()) sensorId = "carekit." + source_id_part(sampleType);

    Json::Object signal;
    signal["sensorId"] = sensorId;
    signal["name"] = body.at("name").as_string(mapping.at("name").as_string("carekit:" + sampleType));
    if (mapping.at("region").is_object()) signal["region"] = mapping.at("region");
    if (body.at("region").is_object()) signal["region"] = body.at("region");
    if (body.at("values").is_array()) signal["values"] = body.at("values");
    else if (body.at("value").is_number()) signal["values"] = Json::Array{body.at("value").as_number()};
    else signal["values"] = Json::Array{};
    signal["active"] = body.at("active").as_bool(true);
    signal["ttlMs"] = body.at("ttlMs").is_number() ? body.at("ttlMs") : mapping.at("ttlMs").is_number() ? mapping.at("ttlMs") : Json(300000);
    signal["triggerPush"] = body.at("triggerPush").as_bool(false);
    signal["compactPush"] = body.at("compactPush").as_bool(true);
    signal["origin"] = "carekit";
    return signal;
  }

  Json ingest_carekit_one(const Json& body) {
    Json mapping = resolve_source_mapping_from_body(body, careKitDefaultSourceMappingId);
    Json::Object signal = build_carekit_signal(body, mapping);
    http::Response response = ingest_signal(Json(signal));
    Json parsed = parse_json_or_null(response.body);
    return Json::Object{
      {"success", response.status >= 200 && response.status < 300},
      {"status", static_cast<double>(response.status)},
      {"sampleType", body.at("sampleType").as_string(body.at("type").as_string())},
      {"taskId", body.at("taskId").as_string()},
      {"carePlanId", body.at("carePlanId").as_string()},
      {"sourceMappingId", mapping.at("id").as_string()},
      {"sensorId", signal["sensorId"]},
      {"result", parsed}
    };
  }

  http::Response ingest_carekit(const Json& body) {
    if (!body.is_object()) return http::error_response("CareKit ingest body must be a JSON object", 400);
    if (!careKitBridgeToken.empty() &&
        body.at("bridgeToken").as_string(body.at("token").as_string()) != careKitBridgeToken) {
      return http::error_response("CareKit bridge token rejected", 401);
    }

    Json::Array results;
    bool allOk = true;
    try {
      const Json& samples = body.at("samples");
      if (samples.is_array()) {
        for (const auto& sample : samples.array()) {
          Json merged = merge_objects(body, sample);
          if (merged.is_object()) {
            merged.object().erase("samples");
            merged.object().erase("bridgeToken");
          }
          Json result = ingest_carekit_one(merged);
          allOk = allOk && result.at("success").as_bool(false);
          results.push_back(result);
        }
      } else {
        Json result = ingest_carekit_one(body);
        allOk = result.at("success").as_bool(false);
        results.push_back(result);
      }
    } catch (const std::exception& e) {
      return http::error_response(e.what(), 400);
    }

    hub_broadcast(json::stringify(Json::Object{
      {"type", "carekit.ingest"},
      {"bridgeId", body.at("bridgeId").as_string(careKitBridgeId)},
      {"samples", static_cast<double>(results.size())},
      {"success", allOk},
      {"timestamp", static_cast<double>(now_ms())}
    }));

    return http::json_response(json::stringify(Json::Object{
      {"success", allOk},
      {"bridgeId", body.at("bridgeId").as_string(careKitBridgeId)},
      {"results", results}
    }), allOk ? 200 : 207);
  }

  http::Response do_push(bool includeMachineResults = true, bool async = false) {
    sync_test_sources_from_reality();
    bool expected = false;
    if (!pushInFlight.compare_exchange_strong(expected, true)) {
      {
        std::lock_guard<std::mutex> lock(pushQueueMutex);
        ++coalescedPushRequests;
      }
      return http::json_response(json::stringify(Json::Object{
        {"success", false},
        {"coalesced", true},
        {"step", nullptr},
        {"timestamp", static_cast<double>(now_ms())},
        {"globalStep", current_global_step()},
        {"error", "push already in progress"},
      }), 409);
    }

    auto job = std::make_unique<PushJob>();
    job->id = make_id("push");
    std::string jobId = job->id;
    job->includeMachineResults = includeMachineResults;
    auto promise = std::make_shared<std::promise<http::Response>>();
    if (!async) job->result = promise;
    auto future = promise->get_future();
    {
      std::lock_guard<std::mutex> lock(pushQueueMutex);
      if (pushQueue.size() >= pushQueueCapacity) {
        pushInFlight.store(false);
        return http::json_response(json::stringify(Json::Object{
          {"success", false},
          {"step", nullptr},
          {"timestamp", static_cast<double>(now_ms())},
          {"globalStep", current_global_step()},
          {"error", "push queue is full"},
        }), 429);
      }
      pushRecords[job->id] = {job->id, "queued", "{}", now_ms(), now_ms()};
      pushRecordOrder.push_back(job->id);
      trim_push_records();
      pushQueue.push_back(std::move(job));
    }
    pushQueueCondition.notify_one();
    if (async) {
      return http::json_response(json::stringify(Json::Object{
        {"success", true},
        {"accepted", true},
        {"jobId", jobId},
        {"statusEndpoint", "/api/push/" + jobId}
      }), 202);
    }
    return future.get();
  }

  http::Response read_push_job(const std::string& id) {
    std::lock_guard<std::mutex> lock(pushQueueMutex);
    auto it = pushRecords.find(id);
    if (it == pushRecords.end()) return http::error_response("Push job not found", 404);
    Json result = Json::Object{
      {"id", it->second.id},
      {"status", it->second.status},
      {"createdAt", static_cast<double>(it->second.createdAt)},
      {"updatedAt", static_cast<double>(it->second.updatedAt)}
    };
    if (it->second.status == "completed" || it->second.status == "failed") result.object()["result"] = json::parse(it->second.body);
    return ok(result);
  }

  http::Response execute_push(bool includeMachineResults) {
    Vector vector;
    MatchAlgorithm matchAlgorithm;
    {
      std::lock_guard<std::mutex> lock(stateMutex);
      vector = engine.assemble_vector();
      matchAlgorithm = engine.matchAlgorithm;
    }
    Json payload = Json::Object{
      {"vector", json::numbers(vector)},
      {"matchAlgorithm", to_string(matchAlgorithm)},
      {"matchAlgorithmOverride", to_string(matchAlgorithm == MatchAlgorithm::Equals ? ComparatorType::Equals : ComparatorType::Gte)},
      {"includeMachineResults", includeMachineResults},
      {"includePerceptualSpace", true},
    };
    try {
      std::string raw = http::post_json(realityEngineUrl + "/api/perceive", json::stringify(payload));
      Json parsed = json::parse(raw);
      long long ts = now_ms();
      long long step = 0;
      {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (parsed.at("perceptualSpace").is_array()) {
          auto ps = json::to_numbers(parsed.at("perceptualSpace"));
          ps = aggregator::aggregate_machine_outputs(std::move(ps), parsed.at("machineResults"));
          engine.update_from_perceptual_space(ps);
        }
        engine.advance();
        lastPush = ts;
        step = engine.globalStep;
      }
      // Semantic audit (SEMANTIC_AUDIT_CONTRACT.md): one re:PerceptionEvent
      // per active source region written this push, attributed to the
      // integration that feeds it and joined to the corpus ABox when the
      // source names a machine.
      {
        const auto& bases = semantics_bases();
        std::vector<SourceConfig> active;
        {
          std::lock_guard<std::mutex> lock(stateMutex);
          for (const auto& s : engine.get_sources()) {
            if (s.active) active.push_back(s);
          }
        }
        for (const auto& s : active) {
          const std::string integration = !s.origin.empty() ? s.origin
                                        : (!s.kind.empty() ? s.kind : "unattributed");
          record_perception_event(integration,
                                  !s.machineName.empty() && bases.count(s.machineName) > 0);
        }
      }
      Json dispatch = dispatch_triggers_from_step(parsed);
      Json result = Json::Object{{"success", true}, {"step", parsed}, {"timestamp", static_cast<double>(ts)}, {"globalStep", static_cast<double>(step)}, {"error", nullptr}};
      if (dispatch.is_object()) result.object()["dispatch"] = dispatch;
      broadcast_state();
      broadcast_push_result(result);
      return ok(result);
    } catch (const std::exception& e) {
      Json result = Json::Object{{"success", false}, {"step", nullptr}, {"timestamp", static_cast<double>(now_ms())}, {"globalStep", current_global_step()}, {"error", e.what()}};
      broadcast_push_result(result);
      return ok(result);
    }
  }

  void push_worker_loop() {
    while (true) {
      std::unique_ptr<PushJob> job;
      {
        std::unique_lock<std::mutex> lock(pushQueueMutex);
        pushQueueCondition.wait(lock, [this]() { return stopPushWorker || !pushQueue.empty(); });
        if (stopPushWorker && pushQueue.empty()) return;
        job = std::move(pushQueue.front());
        pushQueue.pop_front();
      }

      try {
        {
          std::lock_guard<std::mutex> lock(pushQueueMutex);
          pushRecords[job->id].status = "running";
          pushRecords[job->id].updatedAt = now_ms();
        }
        auto response = execute_push(job->includeMachineResults);
        {
          std::lock_guard<std::mutex> lock(pushQueueMutex);
          auto& record = pushRecords[job->id];
          record.status = response.status >= 200 && response.status < 300 ? "completed" : "failed";
          record.body = response.body;
          record.updatedAt = now_ms();
        }
        if (job->result) job->result->set_value(response);
      } catch (const std::exception& e) {
        auto response = ok(Json::Object{
          {"success", false},
          {"step", nullptr},
          {"timestamp", static_cast<double>(now_ms())},
          {"globalStep", current_global_step()},
          {"error", e.what()},
        });
        {
          std::lock_guard<std::mutex> lock(pushQueueMutex);
          auto& record = pushRecords[job->id];
          record.status = "failed";
          record.body = response.body;
          record.updatedAt = now_ms();
        }
        if (job->result) job->result->set_value(response);
      } catch (...) {
        auto response = ok(Json::Object{
          {"success", false},
          {"step", nullptr},
          {"timestamp", static_cast<double>(now_ms())},
          {"globalStep", current_global_step()},
          {"error", "unknown push worker error"},
        });
        {
          std::lock_guard<std::mutex> lock(pushQueueMutex);
          auto& record = pushRecords[job->id];
          record.status = "failed";
          record.body = response.body;
          record.updatedAt = now_ms();
        }
        if (job->result) job->result->set_value(response);
      }

      while (true) {
        {
          std::lock_guard<std::mutex> lock(pushQueueMutex);
          if (coalescedPushRequests == 0) {
            pushInFlight.store(false);
            break;
          }
          coalescedPushRequests = 0;
        }
        try {
          execute_push(false);
        } catch (...) {
        }
        {
          std::lock_guard<std::mutex> lock(pushQueueMutex);
          coalescedPushRequests = 0;
          pushInFlight.store(false);
        }
        break;
      }
    }
  }

  // ── Semantic guardrail metrics (docs/PE_METRICS_CONTRACT.md) ─────────────
  // Counters are monotonic for the process lifetime and bumped where records
  // are created, so a ring-buffer eviction never loses a count.

  std::filesystem::path semantics_manifest_path() const {
    if (const char* explicitPath = std::getenv("SEMANTICS_MANIFEST")) {
      if (*explicitPath) return std::filesystem::path(explicitPath);
    }
    const char* machinesEnv = std::getenv("MACHINES_DIR");
    std::filesystem::path cursor = std::filesystem::absolute(
        machinesEnv && *machinesEnv ? machinesEnv : "../RealityEngine_Machines/machines");
    for (int i = 0; i < 6 && !cursor.empty(); ++i) {
      std::filesystem::path candidate = cursor / "semantics" / "abox-manifest.json";
      if (std::filesystem::exists(candidate)) return candidate;
      cursor = cursor.parent_path();
    }
    return {};
  }

  // machine name -> ABox base IRI, cached on the manifest's mtime.
  const std::map<std::string, std::string>& semantics_bases() const {
    std::lock_guard<std::mutex> lock(semanticsMutex);
    const auto path = semantics_manifest_path();
    if (path.empty() || !std::filesystem::exists(path)) {
      semanticsBases.clear();
      semanticsStamp = 0;
      return semanticsBases;
    }
    const auto stamp = static_cast<long long>(
        std::filesystem::last_write_time(path).time_since_epoch().count());
    if (stamp == semanticsStamp) return semanticsBases;
    semanticsBases.clear();
    semanticsStamp = stamp;
    try {
      std::ifstream in(path);
      std::ostringstream buffer;
      buffer << in.rdbuf();
      Json manifest = json::parse(buffer.str());
      for (const auto& [key, entry] : manifest.at("machines").object()) {
        (void)key;
        std::string iri = entry.at("iri").as_string();
        std::string name = entry.at("name").as_string();
        const auto hash = iri.find('#');
        if (!name.empty() && hash != std::string::npos) semanticsBases[name] = iri.substr(0, hash);
      }
    } catch (...) {
      semanticsBases.clear();
    }
    return semanticsBases;
  }

  void record_perception_event(const std::string& integration, bool joined) {
    std::lock_guard<std::mutex> lock(semanticsMutex);
    semanticEvents[integration] += 1;
    semanticEventsJoined[integration] += joined ? 1 : 0;
    if (++semanticAuditRecords > 1000) semanticAuditRecords = 1000;
  }

  static std::string metric_line(const std::string& name, const std::string& help,
                                 const std::string& kind,
                                 const std::vector<std::pair<std::string, std::string>>& labels,
                                 long long value) {
    std::vector<std::pair<std::string, std::string>> all = labels;
    all.emplace_back("runtime", "cpp");
    std::sort(all.begin(), all.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::ostringstream out;
    out << "# HELP " << name << ' ' << help << '\n'
        << "# TYPE " << name << ' ' << kind << '\n'
        << name << '{';
    for (size_t i = 0; i < all.size(); ++i) {
      if (i) out << ',';
      out << all[i].first << "=\"" << all[i].second << "\"";
    }
    out << "} " << value << '\n';
    return out.str();
  }

  std::string semantic_metrics_text(int sources, long long globalStep, int vectorSize,
                                    long long lastPushMs) const {
    const auto& bases = semantics_bases();
    std::lock_guard<std::mutex> lock(semanticsMutex);
    std::string out;
    out += metric_line("perception_engine_sources_total",
                       "Total sensor/test/simulated sources registered.", "gauge", {}, sources);
    out += metric_line("perception_engine_global_step",
                       "Engine globalStep counter (push count since start).", "gauge", {}, globalStep);
    out += metric_line("perception_engine_vector_size",
                       "Configured vector dimension.", "gauge", {}, vectorSize);
    out += metric_line("perception_engine_last_push_ms",
                       "Wall-clock timestamp of the last successful push (0 if never).", "gauge", {},
                       lastPushMs);
    out += metric_line("semantic_manifest_available",
                       "Corpus OWL semantics manifest resolved (1/0).", "gauge", {},
                       bases.empty() ? 0 : 1);
    out += metric_line("semantic_manifest_machines",
                       "Machines carrying a semantic identity in the manifest.", "gauge", {},
                       static_cast<long long>(bases.size()));
    out += metric_line("semantic_audit_buffer_records",
                       "re:PerceptionEvent records held in the audit ring buffer.", "gauge", {},
                       semanticAuditRecords);
    for (const auto& [integration, count] : semanticEvents) {
      out += metric_line("semantic_perception_events_total",
                         "re:PerceptionEvent records emitted, by originating integration.",
                         "counter", {{"integration", integration}}, count);
    }
    for (const auto& [integration, count] : semanticEventsJoined) {
      out += metric_line("semantic_perception_events_iri_joined_total",
                         "Perception events whose machine resolved to a corpus ABox IRI.",
                         "counter", {{"integration", integration}}, count);
    }
    out += metric_line("semantic_dispatch_records_total",
                       "Dispatch records created with a semantics link.", "counter", {},
                       semanticDispatchTotal);
    out += metric_line("semantic_dispatch_records_iri_joined_total",
                       "Dispatch records whose machine resolved to a corpus ABox IRI.", "counter", {},
                       semanticDispatchJoined);
    for (const auto& [rag, count] : semanticEscalations) {
      out += metric_line("semantic_escalation_dispatches_total",
                         "Escalation-class actions dispatched, by RAG status of the determination.",
                         "counter", {{"rag", rag}}, count);
    }
    return out;
  }

  // std::map keeps label values sorted ascending, as the contract requires.
  mutable std::mutex semanticsMutex;
  mutable std::map<std::string, std::string> semanticsBases;
  mutable long long semanticsStamp = 0;
  std::map<std::string, long long> semanticEvents;
  std::map<std::string, long long> semanticEventsJoined;
  std::map<std::string, long long> semanticEscalations;
  long long semanticDispatchTotal = 0;
  long long semanticDispatchJoined = 0;
  long long semanticAuditRecords = 0;

  Json current_global_step() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    return static_cast<double>(engine.globalStep);
  }

  Json current_state_json() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    return engine.state_json(lastPush, autoRunning, autoIntervalMs);
  }

  std::string state_update_message() const {
    return json::stringify(Json::Object{{"type", "state-update"}, {"state", current_state_json()}});
  }

  void hub_broadcast(const std::string& text) const {
    wsHub->broadcast(text);
    if (sseHub) sseHub->broadcast(text);
  }

  void broadcast_state() const {
    hub_broadcast(state_update_message());
  }

  void broadcast_push_result(const Json& result) const {
    Json::Object message{{"type", "push-result"}};
    if (result.is_object()) {
      for (const auto& [key, value] : result.object()) message[key] = value;
    }
    hub_broadcast(json::stringify(message));
  }

  void trim_push_records() {
    while (pushRecordOrder.size() > pushRecordCapacity) {
      pushRecords.erase(pushRecordOrder.front());
      pushRecordOrder.pop_front();
    }
  }

  void trim_dispatch_records() {
    while (dispatchRecordOrder.size() > dispatchRecordCapacity) {
      dispatchRecords.erase(dispatchRecordOrder.front());
      dispatchRecordOrder.pop_front();
    }
  }

  void cache_machine_catalog(const Json& data) {
    Json::Object catalog;
    for (const auto& m : data.at("machines").is_array() ? data.at("machines").array() : Json::Array{}) {
      std::string id = m.at("id").as_string();
      if (!id.empty()) catalog[id] = m;
    }
    std::lock_guard<std::mutex> lock(machineCatalogMutex);
    machineCatalog = std::move(catalog);
  }

  Json machine_catalog_snapshot() const {
    std::lock_guard<std::mutex> lock(machineCatalogMutex);
    return machineCatalog;
  }

  Json::Array copy_string_array(const Json& value) const {
    Json::Array out;
    if (!value.is_array()) return out;
    for (const auto& item : value.array()) {
      if (item.is_string()) out.emplace_back(item.as_string());
    }
    return out;
  }

  std::string select_agent_action(const Json::Array& actions, const Json& values) const {
    if (actions.empty()) return "";
    if (values.is_array()) {
      for (size_t i = 0; i < values.array().size(); ++i) {
        if (values.array()[i].as_number() != 0.0 && i < actions.size()) {
          return actions[i].as_string();
        }
      }
    }
    return actions.front().as_string();
  }

  DispatchBinding dispatch_binding_from_metadata(const Json& metadata, const Json& values = nullptr) const {
    DispatchBinding binding;
    const Json& agentBinding = metadata.at("agentBinding");
    if (agentBinding.is_object()) {
      binding.agent = agentBinding.at("agent").as_string(metadata.at("dispatchableAgent").as_string());
      binding.trigger = agentBinding.at("trigger").as_string(metadata.at("aiTrigger").as_string());
      binding.autonomyMode = agentBinding.at("mode").as_string();
      binding.actions = copy_string_array(agentBinding.at("allowedActions"));
      if (binding.actions.empty()) binding.actions = copy_string_array(metadata.at("agentActions"));
      binding.writeBack = agentBinding.at("writeBack").is_null() ? Json(nullptr) : agentBinding.at("writeBack");
    } else {
      binding.agent = metadata.at("dispatchableAgent").as_string();
      binding.trigger = metadata.at("aiTrigger").as_string();
      binding.actions = copy_string_array(metadata.at("agentActions"));
    }
    binding.action = select_agent_action(binding.actions, values);
    return binding;
  }

  std::string asserted_label(const Json& values) const {
    if (!values.is_array()) return "";
    std::vector<std::string> labels;
    for (size_t i = 0; i < values.array().size(); ++i) {
      if (values.array()[i].as_number() != 0.0) labels.push_back("cell_" + std::to_string(i));
    }
    if (labels.empty()) return "none";
    std::ostringstream out;
    for (size_t i = 0; i < labels.size(); ++i) {
      if (i) out << "+";
      out << labels[i];
    }
    return out.str();
  }

  Json build_trigger_envelope(const Json& op, const Json& machine, const std::string& envelopeId, const std::string& correlationId) const {
    const Json& md = machine.at("metadata");
    const Json& governance = op.at("governance");
    const Json& triggerConfig = md.at("triggerConfig");
    const Json& triggerDispatch = triggerConfig.at("dispatch");
    DispatchBinding binding = dispatch_binding_from_metadata(md, op.at("values"));
    Json::Array semantics;
    const Json& values = op.at("values");
    if (values.is_array()) {
      for (size_t i = 0; i < values.array().size(); ++i) {
        semantics.push_back(Json::Object{
          {"index", static_cast<double>(i)},
          {"label", "cell_" + std::to_string(i)}
        });
      }
    }

    Json::Object outputMapping;
    outputMapping["output"] = op.at("region");
    Json::Object dispatch{
      {"processId", triggerConfig.at("processId").as_string()},
      {"processName", triggerConfig.at("processName").as_string()},
      {"agent", binding.agent},
      {"action", binding.action},
      {"agentActionsCatalog", binding.actions},
      {"trigger", binding.trigger},
      {"autonomyMode", binding.autonomyMode},
      {"writeBack", binding.writeBack},
      {"endpoint", Json::Object{
        {"kind", triggerDispatchMode},
        {"url", triggerDispatchMode == "graphql"
          ? triggerConfig.at("endpoint").as_string(triggerGraphQLEndpoint)
          : ""},
        {"mutation", triggerDispatchMode == "graphql"
          ? triggerDispatch.at("mutation").as_string("updateProcessState")
          : ""},
        {"schemaRef", triggerDispatchMode == "graphql"
          ? triggerDispatch.at("schemaRef").as_string("localAIStack/services/api/routers/graphql_endpoint.py")
          : ""}
      }}
    };

    return Json::Object{
      {"schemaVersion", "1.0.0"},
      {"envelopeType", "ces.terminal.event"},
      {"envelopeId", envelopeId},
      {"correlationId", correlationId},
      {"emittedAtMs", static_cast<double>(now_ms())},
      {"source", Json::Object{
        {"engine", "PE"},
        {"observedEngine", "RE"},
        {"endpoint", realityEngineUrl}
      }},
      {"ces", Json::Object{
        {"machineId", op.at("machineId").as_string()},
        {"machineName", machine.at("name").as_string(op.at("machineId").as_string())},
        {"machineCode", md.at("machineCode").as_string()},
        {"sequenceId", op.at("sequenceId").as_string()},
        {"sequenceName", op.at("sequenceId").as_string()},
        {"outputIndex", op.at("outputIndex").is_number() ? op.at("outputIndex") : Json(0)},
        {"stepNumber", 0},
        {"perceptualMapping", outputMapping},
        {"provenance", op.at("provenance").is_array() ? op.at("provenance") : Json::Array{}},
        {"deprecation", op.at("deprecation").is_null() ? Json(nullptr) : op.at("deprecation")}
      }},
      {"outputVector", Json::Object{
        {"values", values.is_array() ? values : Json::Array{}},
        {"encoding", "vector"},
        {"semantics", semantics},
        {"assertedLabel", asserted_label(values)}
      }},
      {"projection", Json(nullptr)},
      {"governance", governance.is_object() ? governance : Json(nullptr)},
      {"dispatch", dispatch}
    };
  }

  TriggerDispatchSummary dispatch_triggers(const Json& step) {
    TriggerDispatchSummary summary;
    if (!triggerDispatchEnabled) return summary;
    const Json& mergeBatch = step.at("mergeBatch");
    if (!mergeBatch.is_array()) return summary;

    Json machines = machine_catalog_snapshot();
    for (const auto& op : mergeBatch.array()) {
      ++summary.mergeOps;
      if (!op.at("governance").is_object()) {
        ++summary.droppedNoGovernance;
        continue;
      }
      std::string machineId = op.at("machineId").as_string();
      const Json& machine = machines.at(machineId);
      if (!machine.is_object()) {
        ++summary.droppedNoDispatch;
        continue;
      }
      const Json& md = machine.at("metadata");
      DispatchBinding binding = dispatch_binding_from_metadata(md, op.at("values"));
      if (binding.agent.empty() || binding.trigger.empty()) {
        ++summary.droppedNoDispatch;
        continue;
      }

      const std::string envelopeId = make_id("trigger-envelope");
      const std::string correlationId = make_id("trigger-correlation");
      Json envelope = build_trigger_envelope(op, machine, envelopeId, correlationId);
      DispatchRecord record;
      record.id = make_id("dispatch");
      record.envelopeId = envelopeId;
      record.correlationId = correlationId;
      record.mode = triggerDispatchMode;
      // Dispatch is intentionally fire-and-record at this layer; external
      // provider completion returns later as ordinary PE source updates.
      record.status = "recorded";
      record.target = binding.agent;
      record.machineId = machineId;
      record.sequenceId = op.at("sequenceId").as_string();
      record.ragStatusCode = op.at("governance").at("ragStatusCode").as_string();
      record.processStatus = op.at("governance").at("processStatus").as_string();
      record.createdAt = now_ms();
      record.updatedAt = record.createdAt;
      record.attempts = 0;
      record.envelope = envelope;

      {
        std::lock_guard<std::mutex> lock(dispatchMutex);
        dispatchRecords[record.id] = record;
        dispatchRecordOrder.push_back(record.id);
        ++triggerEnvelopesCreated;
        trim_dispatch_records();
      }
      ++summary.envelopesCreated;
      ++summary.dispatchRecordsCreated;

      hub_broadcast(json::stringify(Json::Object{
        {"type", "trigger.envelope.created"},
        {"envelopeId", envelopeId},
        {"correlationId", correlationId},
        {"dispatchId", record.id},
        {"target", binding.agent},
        {"mode", triggerDispatchMode}
      }));
    }
    return summary;
  }

  Json dispatch_triggers_from_step(const Json& step) {
    TriggerDispatchSummary s;
    try {
      s = dispatch_triggers(step);
    } catch (const std::exception& e) {
      ++s.errors;
      std::cerr << "trigger dispatch failed: " << e.what() << "\n";
    }
    {
      std::lock_guard<std::mutex> lock(dispatchMutex);
      triggerDroppedNoGovernance += static_cast<size_t>(s.droppedNoGovernance);
      triggerDroppedNoDispatch += static_cast<size_t>(s.droppedNoDispatch);
      triggerDispatchErrors += static_cast<size_t>(s.errors);
    }
    return Json::Object{
      {"enabled", triggerDispatchEnabled},
      {"mode", triggerDispatchMode},
      {"mergeOps", static_cast<double>(s.mergeOps)},
      {"envelopesCreated", static_cast<double>(s.envelopesCreated)},
      {"dispatchRecordsCreated", static_cast<double>(s.dispatchRecordsCreated)},
      {"droppedNoGovernance", static_cast<double>(s.droppedNoGovernance)},
      {"droppedNoDispatch", static_cast<double>(s.droppedNoDispatch)},
      {"errors", static_cast<double>(s.errors)}
    };
  }

  std::string realityEngineUrl;
  std::string localAIBaseUrl;
  std::string localAIMachinesDirectory;
  PerceptionEngine engine;
  mutable std::mutex stateMutex;
  mutable std::mutex machineCatalogMutex;
  Json machineCatalog = Json::Object{};
  mutable std::mutex integrationMutex;
  bool integrationRegistryLoaded = false;
  std::string integrationConfigPath;
  std::string integrationRegistryError;
  Json integrationConfig = Json::Object{};
  std::map<std::string, Json> sourceMappingRegistry;
  std::string ollamaBaseUrl = "http://localhost:11434";
  std::string ollamaModel = "gpt-oss:20b";
  std::string ollamaCompletionSourceMappingId = "agent-completion-risk";
  std::string openaiBaseUrl = "https://api.openai.com/v1";
  std::string openaiModel = "gpt-5";
  std::string openaiCompletionSourceMappingId = "agent-completion-risk";
  std::string openaiApiKey;
  bool acpEnabled = true;
  std::string acpPlatform = "OpenClaw";
  std::string acpSurface = "xACP";
  std::string acpCommand = "openclaw acp";
  std::string acpGatewayUrl = "ws://127.0.0.1:18789";
  std::string acpSessionKey = "agent:main:main";
  std::string acpTargetAgent = "openclaw";
  std::string acpCompletionSourceMappingId = "acp-openclaw-completion";
  std::string healthKitBridgeId = "healthkit-ios-bridge";
  std::string healthKitDefaultSourceMappingId = "healthkit-activity";
  std::string healthKitBridgeToken;
  std::string careKitBridgeId = "carekit-ios-bridge";
  std::string careKitDefaultSourceMappingId = "carekit-task";
  std::string careKitBridgeToken;
  std::atomic_bool pushInFlight{false};
  std::mutex pushQueueMutex;
  std::condition_variable pushQueueCondition;
  std::deque<std::unique_ptr<PushJob>> pushQueue;
  std::map<std::string, PushRecord> pushRecords;
  std::deque<std::string> pushRecordOrder;
  std::shared_ptr<http::Server::WebSocketHub> wsHub = std::make_shared<http::Server::WebSocketHub>();
  std::shared_ptr<http::Server::SseHub> sseHub = std::make_shared<http::Server::SseHub>();
  std::thread pushWorker;
  std::thread autoWorker;
  std::mutex autoMutex;
  std::condition_variable autoCv;
  bool stopAutoWorker = false;
  bool stopPushWorker = false;
  size_t coalescedPushRequests = 0;
  static constexpr size_t pushQueueCapacity = 1;
  static constexpr size_t pushRecordCapacity = 256;
  bool autoRunning = false;
  long autoIntervalMs = 1000;
  std::optional<long long> lastPush;
  bool triggerDispatchEnabled = false;
  std::string triggerDispatchMode = "dry-run";
  std::string triggerGraphQLEndpoint;
  mutable std::mutex dispatchMutex;
  std::map<std::string, DispatchRecord> dispatchRecords;
  std::deque<std::string> dispatchRecordOrder;
  static constexpr size_t dispatchRecordCapacity = 256;
  size_t triggerEnvelopesCreated = 0;
  size_t triggerDroppedNoGovernance = 0;
  size_t triggerDroppedNoDispatch = 0;
  size_t triggerDispatchErrors = 0;
  // MQTT bridge — optional; null when MQTT_BROKER_HOST is unset.  Owned by
  // PerceptionService so its lifetime is bounded by the service's.
  std::unique_ptr<mqtt::MqttBridge> mqttBridge;
};

} // namespace

int main(int argc, char** argv) {
  int port = argc > 1 ? std::stoi(argv[1]) : 5300;
  std::string realityUrl = argc > 2 ? argv[2] : "http://localhost:5301";
  std::string localAIUrl = argc > 3 ? argv[3] : (std::getenv("LOCAL_AI_API_URL") ? std::getenv("LOCAL_AI_API_URL") : "http://localhost:4000");
  std::string localAIMachinesDir = argc > 4 ? argv[4] : (std::getenv("LOCAL_AI_MACHINES_DIR") ? std::getenv("LOCAL_AI_MACHINES_DIR") : "../localAIStack/data/machines");
  int vectorDimension = argc > 5 ? std::stoi(argv[5]) : (std::getenv("VECTOR_DIMENSION") ? std::stoi(std::getenv("VECTOR_DIMENSION")) : 7680);
  bool bootstrapLocalAI = truthy_env(std::getenv("LOCAL_AI_BOOTSTRAP"));
  http::Server server;
  PerceptionService service(realityUrl, localAIUrl, localAIMachinesDir, vectorDimension, bootstrapLocalAI);
  service.mount(server);
  server.listen(port);
  return 0;
}
