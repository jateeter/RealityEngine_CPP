#include "reality/http.hpp"
#include "reality/reality.hpp"

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
#include <set>
#include <thread>
#include <algorithm>
#include <cctype>
#include <sstream>

using namespace reality;

namespace {

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
    s.loop = j.at("loop").as_bool(true);
    for (const auto& v : j.at("inputs").is_array() ? j.at("inputs").array() : Json::Array{}) s.inputs.push_back(json::to_numbers(v));
  } else if (s.kind == "sensor") {
    s.sensorId = j.at("sensorId").as_string();
    s.lastValue = json::to_numbers(j.at("lastValue"));
    if (j.at("lastUpdated").is_number()) s.lastUpdated = static_cast<long long>(j.at("lastUpdated").as_number());
    s.ttlMs = static_cast<long>(j.at("ttlMs").as_number(5000));
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

std::string test_source_id(const std::string& machineId, size_t index, const Json& sequence) {
  std::ostringstream out;
  out << "test-" << source_id_part(machineId) << "-" << index << "-" << source_id_part(sequence.at("name").as_string("sequence"));
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
    sync_test_sources_from_reality();
    if (bootstrapLocalAI) {
      try {
        bootstrap_localai();
      } catch (const std::exception& e) {
        std::cerr << "localAI bootstrap skipped: " << e.what() << "\n";
      }
    }
    pushWorker = std::thread([this]() { push_worker_loop(); });
  }

  ~PerceptionService() {
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
    server.route("GET", "/api/health", [](const http::Request&) {
      return ok(Json::Object{{"status", "healthy"}, {"timestamp", static_cast<double>(now_ms())}});
    });
    server.route("GET", "/api/state", [this](const http::Request&) {
      sync_test_sources_from_reality();
      std::lock_guard<std::mutex> lock(stateMutex);
      return ok(engine.state_json(lastPush, autoRunning, autoIntervalMs));
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
    server.route("POST", "/api/signals", [this](const http::Request& req) {
      return ingest_signal(parse_body(req));
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

    size_t added = 0;
    std::lock_guard<std::mutex> lock(stateMutex);
    for (size_t i = 0; i < sequences.array().size(); ++i) {
      const Json& seq = sequences.array()[i];
      if (!seq.at("vectors").is_array()) continue;
      std::string id = test_source_id(machineId, i, seq);
      if (engine.get_source(id)) continue;

      SourceConfig source;
      source.kind = "test";
      source.id = id;
      source.name = machine.at("name").as_string(machineId) + " / " + seq.at("name").as_string("Test sequence");
      source.active = seq.at("active").as_bool(false);
      source.machineId = machineId;
      source.machineName = machine.at("name").as_string(machineId);
      source.sequenceName = seq.at("name").as_string("Test sequence");
      source.region = region;
      source.loop = seq.at("loop").as_bool(false);
      for (const auto& vector : seq.at("vectors").array()) source.inputs.push_back(json::to_numbers(vector));
      if (source.inputs.empty()) continue;
      engine.add_source(source);
      ++added;
    }
    return added;
  }

  size_t sync_test_sources_from_machine_list(const Json& data) {
    size_t added = 0;
    for (const auto& machine : data.at("machines").is_array() ? data.at("machines").array() : Json::Array{}) {
      added += sync_test_sources_from_machine(machine);
    }
    return added;
  }

  size_t sync_test_sources_from_reality() {
    try {
      return sync_test_sources_from_machine_list(json::parse(http::get(realityEngineUrl + "/api/machines")));
    } catch (...) {
      return 0;
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

  http::Response ingest_signal(const Json& body) {
    Vector values = json::to_numbers(body.at("values"));
    if (values.empty()) return http::error_response("values must be a non-empty array", 400);

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
      {"matchAlgorithmOverride", to_string(matchAlgorithm == MatchAlgorithm::Equals ? ComparatorType::Equals : ComparatorType::Gte)},
      {"includeMachineResults", includeMachineResults},
    };
    try {
      std::string raw = http::post_json(realityEngineUrl + "/api/perceive", json::stringify(payload));
      Json parsed = json::parse(raw);
      long long ts = now_ms();
      long long step = 0;
      {
        std::lock_guard<std::mutex> lock(stateMutex);
        if (parsed.at("perceptualSpace").is_array()) engine.update_from_perceptual_space(json::to_numbers(parsed.at("perceptualSpace")));
        engine.advance();
        lastPush = ts;
        step = engine.globalStep;
      }
      Json result = Json::Object{{"success", true}, {"step", parsed}, {"timestamp", static_cast<double>(ts)}, {"globalStep", static_cast<double>(step)}, {"error", nullptr}};
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

  void broadcast_state() const {
    wsHub->broadcast(state_update_message());
  }

  void broadcast_push_result(const Json& result) const {
    Json::Object message{{"type", "push-result"}};
    if (result.is_object()) {
      for (const auto& [key, value] : result.object()) message[key] = value;
    }
    wsHub->broadcast(json::stringify(message));
  }

  void trim_push_records() {
    while (pushRecordOrder.size() > pushRecordCapacity) {
      pushRecords.erase(pushRecordOrder.front());
      pushRecordOrder.pop_front();
    }
  }

  std::string realityEngineUrl;
  std::string localAIBaseUrl;
  std::string localAIMachinesDirectory;
  PerceptionEngine engine;
  mutable std::mutex stateMutex;
  std::atomic_bool pushInFlight{false};
  std::mutex pushQueueMutex;
  std::condition_variable pushQueueCondition;
  std::deque<std::unique_ptr<PushJob>> pushQueue;
  std::map<std::string, PushRecord> pushRecords;
  std::deque<std::string> pushRecordOrder;
  std::shared_ptr<http::Server::WebSocketHub> wsHub = std::make_shared<http::Server::WebSocketHub>();
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
};

} // namespace

int main(int argc, char** argv) {
  int port = argc > 1 ? std::stoi(argv[1]) : 3300;
  std::string realityUrl = argc > 2 ? argv[2] : "http://localhost:3299";
  std::string localAIUrl = argc > 3 ? argv[3] : (std::getenv("LOCAL_AI_API_URL") ? std::getenv("LOCAL_AI_API_URL") : "http://localhost:4000");
  std::string localAIMachinesDir = argc > 4 ? argv[4] : (std::getenv("LOCAL_AI_MACHINES_DIR") ? std::getenv("LOCAL_AI_MACHINES_DIR") : "../localAIStack/data/machines");
  int vectorDimension = argc > 5 ? std::stoi(argv[5]) : (std::getenv("VECTOR_DIMENSION") ? std::stoi(std::getenv("VECTOR_DIMENSION")) : 768);
  bool bootstrapLocalAI = truthy_env(std::getenv("LOCAL_AI_BOOTSTRAP"));
  http::Server server;
  PerceptionService service(realityUrl, localAIUrl, localAIMachinesDir, vectorDimension, bootstrapLocalAI);
  service.mount(server);
  server.listen(port);
  return 0;
}
