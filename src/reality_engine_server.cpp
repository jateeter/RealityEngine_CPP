#include "reality/http.hpp"
#include "reality/reality.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <shared_mutex>

using namespace reality;

namespace {

class RealityService {
public:
  RealityService(const std::string& machinesDir, int vectorDimension)
      : dimension(vectorDimension), simulator(vectorDimension), preception(vectorDimension), machinesDirectory(machinesDir) {
    for (const auto& m : load_machines_from_directory(machinesDir)) add_machine(m);
  }

  void mount(http::Server& server) {
    server.route("GET", "/", [](const http::Request&) {
      return ok(Json::Object{{"name", "Reality Engine"}, {"version", "1.0.0-cpp"}, {"status", "running"}});
    });
    server.route("GET", "/api/health", [](const http::Request&) {
      return ok(Json::Object{{"status", "healthy"}, {"timestamp", static_cast<double>(now_ms())}, {"version", "1.0.0-cpp"}});
    });
    server.route("GET", "/api/config", [this](const http::Request&) {
      std::shared_lock<std::shared_mutex> lock(registryMutex);
      return ok(Json::Object{{"vectorDimension", static_cast<double>(dimension)}, {"matchThreshold", 0.5}, {"qdrantUrl", ""}, {"collectionName", "reality-vectors"}});
    });
    server.route("POST", "/api/engine/reset", [this](const http::Request&) {
      std::unique_lock<std::shared_mutex> registryLock(registryMutex);
      std::lock_guard<std::mutex> simulatorLock(simulatorMutex);
      for (auto& [_, m] : machines) m.reset();
      simulator.reset();
      preception.reset();
      return ok(Json::Object{{"success", true}});
    });
    server.route("GET", "/api/engine/stats", [this](const http::Request&) {
      std::shared_lock<std::shared_mutex> lock(registryMutex);
      return ok(Json::Object{{"stats", stats()}});
    });
    server.route("GET", "/api/runtime/metrics", [this](const http::Request&) {
      std::shared_lock<std::shared_mutex> lock(registryMutex);
      return ok(Json::Object{{"stats", stats()}, {"domainWorkerPool", worker_pool_metrics_json()}});
    });
    server.route("GET", "/api/engine/active", [](const http::Request&) { return ok(Json::Object{{"activeVectors", Json::Object{}}}); });
    server.route("GET", "/api/engine/history", [](const http::Request&) { return ok(Json::Object{{"history", Json::Array{}}}); });
    server.route("POST", "/api/engine/process", [this](const http::Request& req) {
      auto body = parse_body(req);
      auto vec = json::to_numbers(body.at("vector"));
      Json::Array outputs;
      std::unique_lock<std::shared_mutex> lock(registryMutex);
      for (auto& [_, m] : machines) {
        auto r = m.process_input(vec);
        if (r.machineOutput) outputs.push_back(to_json(*r.machineOutput));
      }
      return ok(Json::Object{{"result", Json::Object{{"inputVector", json::numbers(vec)}, {"timestamp", static_cast<double>(now_ms())}, {"outputs", outputs}}}});
    });
    server.route("GET", "/api/machines", [this](const http::Request&) {
      Json::Array arr;
      std::shared_lock<std::shared_mutex> lock(registryMutex);
      for (const auto& [_, m] : machines) arr.push_back(m.to_json());
      return ok(Json::Object{{"machines", arr}});
    });
    server.route("GET", "/api/machines/:id", [this](const http::Request& req) {
      std::shared_lock<std::shared_mutex> lock(registryMutex);
      auto it = machines.find(req.pathParams.at("id"));
      if (it == machines.end()) return http::error_response("Machine not found", 404);
      return ok(Json::Object{{"machine", it->second.to_json(true)}});
    });
    server.route("POST", "/api/machines", [this](const http::Request& req) {
      Machine m = load_machine_from_json_string(req.body);
      std::unique_lock<std::shared_mutex> registryLock(registryMutex);
      std::lock_guard<std::mutex> simulatorLock(simulatorMutex);
      add_machine(m);
      return ok(Json::Object{{"success", true}, {"machine", m.to_json(true)}});
    });
    server.route("PUT", "/api/machines/:id", [this](const http::Request& req) {
      Machine m = load_machine_from_json_string(req.body, req.pathParams.at("id"));
      std::unique_lock<std::shared_mutex> registryLock(registryMutex);
      std::lock_guard<std::mutex> simulatorLock(simulatorMutex);
      remove_machine(req.pathParams.at("id"));
      add_machine(m);
      return ok(Json::Object{{"success", true}, {"machine", m.to_json(true)}});
    });
    server.route("DELETE", "/api/machines/:id", [this](const http::Request& req) {
      std::unique_lock<std::shared_mutex> registryLock(registryMutex);
      std::lock_guard<std::mutex> simulatorLock(simulatorMutex);
      bool removed = remove_machine(req.pathParams.at("id"));
      return ok(Json::Object{{"success", removed}});
    });
    server.route("POST", "/api/machines/:id/process", [this](const http::Request& req) {
      std::unique_lock<std::shared_mutex> lock(registryMutex);
      auto it = machines.find(req.pathParams.at("id"));
      if (it == machines.end()) return http::error_response("Machine not found", 404);
      auto body = parse_body(req);
      auto result = it->second.process_input(json::to_numbers(body.at("inputVector")));
      return ok(to_json(result));
    });
    server.route("POST", "/api/machines/:id/process-universal", [this](const http::Request& req) {
      auto id = req.pathParams.at("id");
      std::unique_lock<std::shared_mutex> lock(registryMutex);
      auto it = machines.find(id);
      if (it == machines.end()) return http::error_response("Machine not found", 404);
      auto body = parse_body(req);
      auto resolved = preception.resolve_input_event_vector_for_machine(json::to_numbers(body.at("universalInputSpace")), it->second);
      auto result = it->second.process_input(resolved);
      return ok(to_json(result));
    });
    server.route("POST", "/api/machines/process-universal/all", [this](const http::Request& req) {
      auto body = parse_body(req);
      auto universal = json::to_numbers(body.at("universalInputSpace"));
      std::unique_lock<std::shared_mutex> lock(registryMutex);
      PreceptionEngine resolver(dimension);
      auto resolved = resolver.resolve_inputs_for_machines(universal, machines);
      Json::Object results;
      for (auto& [id, input] : resolved) results[id] = to_json(machines[id].process_input(input));
      return ok(Json::Object{{"results", results}});
    });
    server.route("POST", "/api/machines/:id/whatif", [this](const http::Request& req) {
      std::shared_lock<std::shared_mutex> lock(registryMutex);
      auto it = machines.find(req.pathParams.at("id"));
      if (it == machines.end()) return http::error_response("Machine not found", 404);
      Machine copy = it->second;
      lock.unlock();
      auto result = copy.process_input(json::to_numbers(parse_body(req).at("inputVector")));
      return ok(to_json(result));
    });
    server.route("POST", "/api/machines/:id/whatif-universal", [this](const http::Request& req) {
      std::shared_lock<std::shared_mutex> lock(registryMutex);
      auto it = machines.find(req.pathParams.at("id"));
      if (it == machines.end()) return http::error_response("Machine not found", 404);
      Machine copy = it->second;
      lock.unlock();
      PreceptionEngine resolver(dimension);
      auto input = resolver.resolve_input_event_vector_for_machine(json::to_numbers(parse_body(req).at("universalInputSpace")), copy);
      return ok(to_json(copy.process_input(input)));
    });
    server.route("GET", "/api/machines/json/list", [this](const http::Request&) {
      Json::Array arr;
      namespace fs = std::filesystem;
      if (fs::exists(machinesDirectory)) for (const auto& p : fs::directory_iterator(machinesDirectory)) if (p.path().extension() == ".json")
        arr.push_back(Json::Object{{"filename", p.path().filename().string()}, {"name", p.path().stem().string()}, {"description", ""}, {"version", "1.0.0"}, {"metadata", Json::Object{}}, {"sequenceCount", 0.0}});
      return ok(Json::Object{{"machines", arr}});
    });
    server.route("GET", "/api/machines/json/:name", [this](const http::Request& req) {
      std::string name = req.pathParams.at("name");
      std::filesystem::path path = std::filesystem::path(machinesDirectory) / (name.ends_with(".json") ? name : name + ".json");
      std::ifstream in(path);
      if (!in) return http::error_response("Machine file not found: " + name, 404);
      std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      Machine m = load_machine_from_json_string(raw, "machine-" + path.stem().string());
      std::unique_lock<std::shared_mutex> registryLock(registryMutex);
      std::lock_guard<std::mutex> simulatorLock(simulatorMutex);
      add_machine(m);
      return ok(Json::Object{{"success", true}, {"machine", m.to_json(true)}, {"message", "Machine loaded successfully"}});
    });
    server.route("GET", "/api/machine-graph", [this](const http::Request&) {
      std::lock_guard<std::mutex> lock(simulatorMutex);
      return ok(simulator.machine_graph_data());
    });
    server.route("POST", "/api/perceptual-simulation/step", [this](const http::Request&) {
      std::lock_guard<std::mutex> lock(simulatorMutex);
      auto s = simulator.step();
      if (!s) return ok(Json::Object{{"done", true}, {"success", true}});
      return ok(Json::Object{{"success", true}, {"step", to_json(*s)}});
    });
    server.route("POST", "/api/perceptual-simulation/reset", [this](const http::Request&) {
      std::lock_guard<std::mutex> lock(simulatorMutex);
      simulator.reset();
      return ok(Json::Object{{"success", true}});
    });
    server.route("POST", "/api/perceptual-simulation/start", [this](const http::Request&) {
      std::lock_guard<std::mutex> lock(simulatorMutex);
      simulator.start();
      return ok(Json::Object{{"success", true}});
    });
    server.route("POST", "/api/perceptual-simulation/stop", [this](const http::Request&) {
      std::lock_guard<std::mutex> lock(simulatorMutex);
      simulator.stop();
      return ok(Json::Object{{"success", true}});
    });
    server.route("GET", "/api/perceptual-simulation/state", [this](const http::Request&) {
      std::lock_guard<std::mutex> lock(simulatorMutex);
      return ok(simulator.state_json());
    });
    server.route("GET", "/api/perceptual-simulation/history", [this](const http::Request&) {
      Json::Array arr;
      std::lock_guard<std::mutex> lock(simulatorMutex);
      for (const auto& s : simulator.history()) arr.push_back(to_json(s));
      return ok(Json::Object{{"history", arr}});
    });
    server.route("POST", "/api/perceptual-simulation/configure/chunk", [this](const http::Request& req) {
      auto body = parse_body(req);
      std::lock_guard<std::mutex> lock(simulatorMutex);
      if (body.at("reset").as_bool(false)) buffer.clear();
      for (const auto& v : body.at("vectors").is_array() ? body.at("vectors").array() : Json::Array{}) buffer.push_back(json::to_numbers(v));
      const auto& cfg = body.at("config").is_object() ? body.at("config") : body;
      if (cfg.at("inputRegion").is_object()) {
        bufferedRegion = {static_cast<int>(cfg.at("inputRegion").at("offset").as_number()), static_cast<int>(cfg.at("inputRegion").at("length").as_number())};
        bufferedDelay = static_cast<long>(cfg.at("stepDelayMs").as_number(100));
      }
      return ok(Json::Object{{"success", true}, {"bufferedVectors", static_cast<double>(buffer.size())}});
    });
    server.route("POST", "/api/perceptual-simulation/configure/commit", [this](const http::Request&) {
      std::lock_guard<std::mutex> lock(simulatorMutex);
      simulator.configure(buffer, bufferedRegion, bufferedDelay);
      buffer.clear();
      return ok(Json::Object{{"success", true}});
    });
    server.route("POST", "/api/preception/diagnostic", [this](const http::Request& req) {
      std::shared_lock<std::shared_mutex> lock(registryMutex);
      PreceptionEngine resolver(dimension);
      return ok(resolver.diagnostic_mapping(json::to_numbers(parse_body(req).at("universalInputSpace")), machines));
    });
    server.route("POST", "/api/perceive", [this](const http::Request& req) {
      auto body = parse_body(req);
      std::optional<ComparatorType> overrideType;
      if (body.at("matchAlgorithmOverride").is_string()) overrideType = comparator_from_string(body.at("matchAlgorithmOverride").as_string());
      bool includeMachineResults = body.at("includeMachineResults").as_bool(!body.at("compact").as_bool(false));
      auto vector = json::to_numbers(body.at("vector"));
      SimulationStep step;
      {
        std::lock_guard<std::mutex> lock(simulatorMutex);
        step = simulator.process_immediate(vector, overrideType);
        preception.perceptual_space().set_vector(step.perceptualSpace);
      }
      return ok(to_json(step, includeMachineResults));
    });
  }

private:
  static Json parse_body(const http::Request& req) { return req.body.empty() ? Json::Object{} : json::parse(req.body); }
  static http::Response ok(const Json& value) { return http::json_response(json::stringify(value)); }

  void add_machine(const Machine& m) {
    machines[m.id] = m;
    if (m.perceptualMapping) {
      try { simulator.add_machine(m); } catch (...) {}
    }
  }
  bool remove_machine(const std::string& id) {
    simulator.remove_machine(id);
    return machines.erase(id) > 0;
  }
  Json stats() const {
    int vectors = 0;
    for (const auto& [_, m] : machines) vectors += m.total_vector_count();
    return Json::Object{{"totalMachines", static_cast<double>(machines.size())}, {"totalVectors", static_cast<double>(vectors)}, {"domainWorkerPool", worker_pool_metrics_json()}};
  }

  std::map<std::string, Machine> machines;
  mutable std::shared_mutex registryMutex;
  mutable std::mutex simulatorMutex;
  int dimension = 768;
  PerceptualSpaceSimulator simulator;
  PreceptionEngine preception;
  std::string machinesDirectory;
  std::vector<Vector> buffer;
  RegionMapping bufferedRegion{0, 1};
  long bufferedDelay = 100;
};

} // namespace

int main(int argc, char** argv) {
  int port = argc > 1 ? std::stoi(argv[1]) : 3100;
  std::string machinesDir = argc > 2 ? argv[2] : "examples/machines";
  int vectorDimension = argc > 3 ? std::stoi(argv[3]) : (std::getenv("VECTOR_DIMENSION") ? std::stoi(std::getenv("VECTOR_DIMENSION")) : 768);
  http::Server server;
  RealityService service(machinesDir, vectorDimension);
  service.mount(server);
  server.listen(port);
  return 0;
}
