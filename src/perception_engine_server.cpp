#include "reality/http.hpp"
#include "reality/reality.hpp"

#include <iostream>

using namespace reality;

namespace {

Json parse_body(const http::Request& req) {
  return req.body.empty() ? Json::Object{} : json::parse(req.body);
}

http::Response ok(const Json& value) {
  return http::json_response(json::stringify(value));
}

SourceConfig source_from_json(const Json& j) {
  SourceConfig s;
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

class PerceptionService {
public:
  explicit PerceptionService(std::string realityUrl) : realityEngineUrl(std::move(realityUrl)) {}

  void mount(http::Server& server) {
    server.route("GET", "/", [](const http::Request&) {
      return ok(Json::Object{{"service", "Perception Engine (C++)"}, {"status", "running"}});
    });
    server.route("GET", "/api/health", [](const http::Request&) {
      return ok(Json::Object{{"status", "healthy"}, {"timestamp", static_cast<double>(now_ms())}});
    });
    server.route("GET", "/api/state", [this](const http::Request&) {
      return ok(engine.state_json(lastPush, autoRunning, autoIntervalMs));
    });
    server.route("POST", "/api/push", [this](const http::Request&) {
      return do_push();
    });
    server.route("POST", "/api/auto/start", [this](const http::Request& req) {
      auto body = parse_body(req);
      autoIntervalMs = static_cast<long>(body.at("intervalMs").as_number(1000));
      if (autoIntervalMs <= 0) autoIntervalMs = 1000;
      autoRunning = true;
      return ok(Json::Object{{"success", true}, {"intervalMs", static_cast<double>(autoIntervalMs)}});
    });
    server.route("POST", "/api/auto/stop", [this](const http::Request&) {
      autoRunning = false;
      return ok(Json::Object{{"success", true}});
    });
    server.route("PATCH", "/api/config", [this](const http::Request& req) {
      auto body = parse_body(req);
      if (body.at("matchAlgorithm").is_string()) engine.matchAlgorithm = match_algorithm_from_string(body.at("matchAlgorithm").as_string());
      return ok(Json::Object{{"success", true}, {"matchAlgorithm", to_string(engine.matchAlgorithm)}});
    });
    server.route("POST", "/api/reset", [this](const http::Request&) {
      engine.reset();
      lastPush.reset();
      return ok(Json::Object{{"success", true}});
    });
    server.route("GET", "/api/sources", [this](const http::Request&) {
      Json::Array arr;
      for (const auto& s : engine.get_sources()) arr.push_back(to_json(s));
      return ok(Json::Object{{"sources", arr}});
    });
    server.route("POST", "/api/sources", [this](const http::Request& req) {
      auto src = engine.add_source(source_from_json(parse_body(req)));
      return ok(Json::Object{{"source", to_json(src)}});
    });
    server.route("PATCH", "/api/sources/:id", [this](const http::Request& req) {
      auto existing = engine.get_source(req.pathParams.at("id"));
      if (!existing) return http::error_response("Source not found", 404);
      engine.remove_source(req.pathParams.at("id"));
      auto updated = source_from_json(parse_body(req));
      updated.id = req.pathParams.at("id");
      auto added = engine.add_source(updated);
      return ok(Json::Object{{"source", to_json(added)}});
    });
    server.route("DELETE", "/api/sources/:id", [this](const http::Request& req) {
      return ok(Json::Object{{"success", engine.remove_source(req.pathParams.at("id"))}});
    });
    server.route("POST", "/api/sensors/:sensorId", [this](const http::Request& req) {
      auto values = json::to_numbers(parse_body(req).at("values"));
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
  http::Response do_push() {
    Vector vector = engine.assemble_vector();
    Json payload = Json::Object{{"vector", json::numbers(vector)}, {"matchAlgorithmOverride", to_string(engine.matchAlgorithm == MatchAlgorithm::Equals ? ComparatorType::Equals : ComparatorType::Gte)}};
    try {
      std::string raw = http::post_json(realityEngineUrl + "/api/perceive", json::stringify(payload));
      Json parsed = json::parse(raw);
      if (parsed.at("perceptualSpace").is_array()) engine.update_from_perceptual_space(json::to_numbers(parsed.at("perceptualSpace")));
      engine.advance();
      lastPush = now_ms();
      return ok(Json::Object{{"success", true}, {"step", parsed}, {"timestamp", static_cast<double>(*lastPush)}, {"globalStep", static_cast<double>(engine.globalStep)}, {"error", nullptr}});
    } catch (const std::exception& e) {
      return ok(Json::Object{{"success", false}, {"step", nullptr}, {"timestamp", static_cast<double>(now_ms())}, {"globalStep", static_cast<double>(engine.globalStep)}, {"error", e.what()}});
    }
  }

  PerceptionEngine engine;
  std::string realityEngineUrl;
  bool autoRunning = false;
  long autoIntervalMs = 1000;
  std::optional<long long> lastPush;
};

} // namespace

int main(int argc, char** argv) {
  int port = argc > 1 ? std::stoi(argv[1]) : 3001;
  std::string realityUrl = argc > 2 ? argv[2] : "http://localhost:3000";
  http::Server server;
  PerceptionService service(realityUrl);
  service.mount(server);
  server.listen(port);
  return 0;
}
