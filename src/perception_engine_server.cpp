#include "reality/http.hpp"
#include "reality/reality.hpp"

#include <cstdlib>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>

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

class PerceptionService {
public:
  PerceptionService(std::string realityUrl, std::string localAIUrl, std::string localAIMachinesDir, int vectorDimension, bool bootstrapLocalAI)
      : realityEngineUrl(std::move(realityUrl)),
        localAIBaseUrl(std::move(localAIUrl)),
        localAIMachinesDirectory(std::move(localAIMachinesDir)),
        engine(vectorDimension) {
    if (bootstrapLocalAI) {
      try {
        bootstrap_localai();
      } catch (const std::exception& e) {
        std::cerr << "localAI bootstrap skipped: " << e.what() << "\n";
      }
    }
  }

  void mount(http::Server& server) {
    server.route("GET", "/", [](const http::Request&) {
      return ok(Json::Object{{"service", "Perception Engine (C++)"}, {"status", "running"}});
    });
    server.route("GET", "/api/health", [](const http::Request&) {
      return ok(Json::Object{{"status", "healthy"}, {"timestamp", static_cast<double>(now_ms())}});
    });
    server.route("GET", "/api/state", [this](const http::Request&) {
      std::lock_guard<std::mutex> lock(stateMutex);
      return ok(engine.state_json(lastPush, autoRunning, autoIntervalMs));
    });
    server.route("GET", "/api/integrations/localai/status", [this](const http::Request&) {
      return ok(localai_status());
    });
    server.route("POST", "/api/integrations/localai/bootstrap", [this](const http::Request&) {
      return ok(bootstrap_localai());
    });
    server.route("POST", "/api/signals", [this](const http::Request& req) {
      return ingest_signal(parse_body(req));
    });
    server.route("POST", "/api/push", [this](const http::Request&) {
      return do_push();
    });
    server.route("POST", "/api/auto/start", [this](const http::Request& req) {
      auto body = parse_body(req);
      std::lock_guard<std::mutex> lock(stateMutex);
      autoIntervalMs = static_cast<long>(body.at("intervalMs").as_number(1000));
      if (autoIntervalMs <= 0) autoIntervalMs = 1000;
      autoRunning = true;
      return ok(Json::Object{{"success", true}, {"intervalMs", static_cast<double>(autoIntervalMs)}});
    });
    server.route("POST", "/api/auto/stop", [this](const http::Request&) {
      std::lock_guard<std::mutex> lock(stateMutex);
      autoRunning = false;
      return ok(Json::Object{{"success", true}});
    });
    server.route("PATCH", "/api/config", [this](const http::Request& req) {
      auto body = parse_body(req);
      std::lock_guard<std::mutex> lock(stateMutex);
      if (body.at("matchAlgorithm").is_string()) engine.matchAlgorithm = match_algorithm_from_string(body.at("matchAlgorithm").as_string());
      return ok(Json::Object{{"success", true}, {"matchAlgorithm", to_string(engine.matchAlgorithm)}});
    });
    server.route("POST", "/api/reset", [this](const http::Request&) {
      std::lock_guard<std::mutex> lock(stateMutex);
      engine.reset();
      lastPush.reset();
      return ok(Json::Object{{"success", true}});
    });
    server.route("GET", "/api/sources", [this](const http::Request&) {
      Json::Array arr;
      std::lock_guard<std::mutex> lock(stateMutex);
      for (const auto& s : engine.get_sources()) arr.push_back(to_json(s));
      return ok(Json::Object{{"sources", arr}});
    });
    server.route("POST", "/api/sources", [this](const http::Request& req) {
      std::lock_guard<std::mutex> lock(stateMutex);
      auto src = engine.add_source(source_from_json(parse_body(req)));
      return ok(Json::Object{{"source", to_json(src)}});
    });
    server.route("PATCH", "/api/sources/:id", [this](const http::Request& req) {
      std::lock_guard<std::mutex> lock(stateMutex);
      auto existing = engine.get_source(req.pathParams.at("id"));
      if (!existing) return http::error_response("Source not found", 404);
      engine.remove_source(req.pathParams.at("id"));
      auto updated = source_from_json(parse_body(req));
      updated.id = req.pathParams.at("id");
      auto added = engine.add_source(updated);
      return ok(Json::Object{{"source", to_json(added)}});
    });
    server.route("DELETE", "/api/sources/:id", [this](const http::Request& req) {
      std::lock_guard<std::mutex> lock(stateMutex);
      return ok(Json::Object{{"success", engine.remove_source(req.pathParams.at("id"))}});
    });
    server.route("POST", "/api/sensors/:sensorId", [this](const http::Request& req) {
      auto values = json::to_numbers(parse_body(req).at("values"));
      std::lock_guard<std::mutex> lock(stateMutex);
      bool found = engine.update_sensor_value(req.pathParams.at("sensorId"), values);
      if (!found) return http::error_response("No sensor source with sensorId \"" + req.pathParams.at("sensorId") + "\"", 404);
      return ok(Json::Object{{"success", true}, {"sensorId", req.pathParams.at("sensorId")}, {"timestamp", static_cast<double>(now_ms())}});
    });
    server.route("GET", "/api/machines", [this](const http::Request&) {
      try {
        return http::json_response(http::get(realityEngineUrl + "/api/machines"));
      } catch (const std::exception& e) {
        return http::error_response(e.what(), 502);
      }
    });
  }

private:
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

  Json localai_status() const {
    Json health = nullptr;
    bool reachable = false;
    try {
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
      {"health", health},
      {"sensors", sensors},
      {"machineDirectory", localAIMachinesDirectory},
    };
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
        importedMachines.push_back(response.at("machine").is_object() ? response.at("machine") : machine.to_json());
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
      response.object()["push"] = json::parse(do_push().body);
    }
    return ok(response);
  }

  http::Response do_push() {
    bool expected = false;
    if (!pushInFlight.compare_exchange_strong(expected, true)) {
      return http::json_response(json::stringify(Json::Object{
        {"success", false},
        {"step", nullptr},
        {"timestamp", static_cast<double>(now_ms())},
        {"globalStep", current_global_step()},
        {"error", "push already in progress"},
      }), 409);
    }

    struct PushGuard {
      std::atomic_bool& flag;
      ~PushGuard() { flag.store(false); }
    } guard{pushInFlight};

    Vector vector;
    MatchAlgorithm matchAlgorithm;
    {
      std::lock_guard<std::mutex> lock(stateMutex);
      vector = engine.assemble_vector();
      matchAlgorithm = engine.matchAlgorithm;
    }
    Json payload = Json::Object{{"vector", json::numbers(vector)}, {"matchAlgorithmOverride", to_string(matchAlgorithm == MatchAlgorithm::Equals ? ComparatorType::Equals : ComparatorType::Gte)}};
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
      return ok(Json::Object{{"success", true}, {"step", parsed}, {"timestamp", static_cast<double>(ts)}, {"globalStep", static_cast<double>(step)}, {"error", nullptr}});
    } catch (const std::exception& e) {
      return ok(Json::Object{{"success", false}, {"step", nullptr}, {"timestamp", static_cast<double>(now_ms())}, {"globalStep", current_global_step()}, {"error", e.what()}});
    }
  }

  Json current_global_step() const {
    std::lock_guard<std::mutex> lock(stateMutex);
    return static_cast<double>(engine.globalStep);
  }

  std::string realityEngineUrl;
  std::string localAIBaseUrl;
  std::string localAIMachinesDirectory;
  PerceptionEngine engine;
  mutable std::mutex stateMutex;
  std::atomic_bool pushInFlight{false};
  bool autoRunning = false;
  long autoIntervalMs = 1000;
  std::optional<long long> lastPush;
};

} // namespace

int main(int argc, char** argv) {
  int port = argc > 1 ? std::stoi(argv[1]) : 3101;
  std::string realityUrl = argc > 2 ? argv[2] : "http://localhost:3100";
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
