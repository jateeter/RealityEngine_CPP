#include "reality/http.hpp"
#include "reality/reality.hpp"

#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <sstream>

using namespace reality;

namespace {

class RealityService {
public:
  RealityService(const std::string& machinesDir, int vectorDimension, const LoadOptions& loadOpts = {})
      : dimension(vectorDimension), simulator(vectorDimension), perception(vectorDimension), machinesDirectory(machinesDir) {
    namespace fs = std::filesystem;
    std::cerr << "Reality Engine startup — machinesDir=" << machinesDir
              << " vectorDimension=" << vectorDimension
              << " strictSta=" << (loadOpts.strictSta ? "true" : "false") << "\n";

    if (!fs::exists(machinesDir)) {
      throw std::runtime_error("machine directory not found: " + machinesDir);
    }
    if (!fs::is_directory(machinesDir)) {
      throw std::runtime_error("machine path is not a directory: " + machinesDir);
    }

    size_t discovered = 0;
    for (const auto& p : fs::recursive_directory_iterator(machinesDir)) {
      if (p.path().extension() == ".json") ++discovered;
    }
    std::cerr << "Reality Engine machine discovery — jsonFiles=" << discovered << "\n";

    auto loadedMachines = load_machines_from_directory(machinesDir, loadOpts);
    for (const auto& m : loadedMachines) add_machine(m);
    std::cerr << "Reality Engine machine registry — loaded=" << loadedMachines.size() << "\n";
  }

  void mount(http::Server& server) {
    server.sse("/api/engine/stream", sseHub);
    server.route("GET", "/", [](const http::Request&) {
      return ok(Json::Object{{"name", "Reality Engine"}, {"version", "1.0.0-cpp"}, {"status", "running"}});
    });
    server.route("GET", "/api", [](const http::Request&) {
      return ok(Json::Object{{"name", "Reality Engine"}, {"version", "1.0.0-cpp"}, {"status", "running"}});
    });
    server.route("GET", "/api/health", [](const http::Request&) {
      return ok(Json::Object{{"status", "healthy"}});
    });
    server.route("GET", "/api/config", [this](const http::Request&) {
      std::shared_lock<std::shared_mutex> lock(registryMutex);
      return ok(Json::Object{{"vectorDimension", static_cast<double>(dimension)}, {"matchThreshold", matchThreshold}, {"qdrantUrl", qdrant_url()}, {"collectionName", collection_name()}});
    });
    server.route("PUT", "/api/config/dimension", [this](const http::Request& req) {
      auto it = req.queryParams.find("dimension");
      if (it != req.queryParams.end() && !it->second.empty()) dimension = std::stoi(it->second);
      return ok(Json::Object{{"success", true}, {"dimension", static_cast<double>(dimension)}});
    });
    server.route("PUT", "/api/config/threshold", [this](const http::Request& req) {
      auto it = req.queryParams.find("threshold");
      if (it != req.queryParams.end() && !it->second.empty()) matchThreshold = std::stod(it->second);
      return ok(Json::Object{{"success", true}, {"threshold", matchThreshold}});
    });
    server.route("POST", "/api/vectors/search", [this](const http::Request& req) {
      auto body = parse_body(req);
      auto query = json::to_numbers(body.at("vector"));
      int limit = static_cast<int>(body.at("limit").as_number(10));
      std::optional<double> threshold;
      if (body.at("threshold").is_number()) threshold = body.at("threshold").as_number();
      Json::Array results;
      std::lock_guard<std::mutex> lock(vectorMutex);
      for (const auto& [_, v] : vectorStore) {
        if (static_cast<int>(results.size()) >= limit) break;
        double score = cosine(query, searchable_vector_values(v));
        if (threshold && score < *threshold) continue;
        results.push_back(Json::Object{{"vector", v}, {"score", score}});
      }
      return ok(Json::Object{{"results", results}});
    });
    server.route("POST", "/api/vectors", [this](const http::Request& req) {
      Json vector = parse_body(req);
      std::string id = vector.at("id").as_string(make_id("vector"));
      vector.object()["id"] = id;
      std::lock_guard<std::mutex> lock(vectorMutex);
      vectorStore[id] = vector;
      return ok(Json::Object{{"success", true}, {"vector", vector}});
    });
    server.route("GET", "/api/vectors/:id", [](const http::Request& req) {
      return ok(Json::Object{{"message", "Vector retrieval endpoint"}, {"id", req.pathParams.at("id")}});
    });
    server.route("DELETE", "/api/vectors/:id", [this](const http::Request& req) {
      std::lock_guard<std::mutex> lock(vectorMutex);
      vectorStore.erase(req.pathParams.at("id"));
      return ok(Json::Object{{"success", true}, {"id", req.pathParams.at("id")}});
    });
    server.route("POST", "/api/sequences/persist", [](const http::Request&) {
      return ok(Json::Object{{"success", true}});
    });
    server.route("GET", "/api/sequences", [this](const http::Request&) {
      Json::Array arr;
      std::lock_guard<std::mutex> lock(sequenceMutex);
      for (const auto& [_, s] : sequences) arr.push_back(s.to_json());
      return ok(Json::Object{{"sequences", arr}});
    });
    server.route("POST", "/api/sequences", [this](const http::Request& req) {
      auto body = parse_body(req);
      CriticalEventSequence seq = sequence_from_json(body);
      auto validation = seq.validate();
      if (!validation.first) return http::json_response(json::stringify(Json::Object{{"error", string_array(validation.second)}}), 400);
      std::lock_guard<std::mutex> lock(sequenceMutex);
      sequences[seq.id] = seq;
      return ok(Json::Object{{"success", true}, {"sequence", seq.to_json()}});
    });
    server.route("GET", "/api/sequences/:id", [this](const http::Request& req) {
      std::lock_guard<std::mutex> lock(sequenceMutex);
      auto it = sequences.find(req.pathParams.at("id"));
      if (it == sequences.end()) return http::error_response("Sequence not found", 404);
      return ok(Json::Object{{"sequence", it->second.to_json()}});
    });
    server.route("DELETE", "/api/sequences/:id", [this](const http::Request& req) {
      std::lock_guard<std::mutex> lock(sequenceMutex);
      if (!sequences.erase(req.pathParams.at("id"))) return http::error_response("Sequence not found", 404);
      return ok(Json::Object{{"success", true}});
    });
    server.route("POST", "/api/sequences/:id/reset", [this](const http::Request& req) {
      std::lock_guard<std::mutex> lock(sequenceMutex);
      auto it = sequences.find(req.pathParams.at("id"));
      if (it == sequences.end()) return http::error_response("Sequence not found", 404);
      it->second.reset();
      return ok(Json::Object{{"success", true}});
    });
    server.route("POST", "/api/sequences/:id/vectors", [this](const http::Request& req) {
      auto vector = vector_from_json(parse_body(req));
      std::lock_guard<std::mutex> lock(sequenceMutex);
      auto it = sequences.find(req.pathParams.at("id"));
      if (it == sequences.end()) return http::error_response("Sequence not found", 404);
      it->second.add_vector(vector);
      return ok(Json::Object{{"success", true}, {"vector", vector.to_json()}});
    });
    server.route("POST", "/api/engine/reset", [this](const http::Request&) {
      std::unique_lock<std::shared_mutex> registryLock(registryMutex);
      std::lock_guard<std::mutex> simulatorLock(simulatorMutex);
      for (auto& [_, m] : machines) m.reset();
      simulator.reset();
      perception.reset();
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
    server.route("GET", "/api/metrics", [this](const http::Request&) {
      // CES coverage telemetry + runtime gauges, Prometheus text format.
      // Metric names + labels match the established /api/metrics shape so one
      // scrape config covers both targets.
      std::string body;
      {
        std::shared_lock<std::shared_mutex> registryLock(registryMutex);
        std::lock_guard<std::mutex> simulatorLock(simulatorMutex);
        // Stamp every line with runtime="cpp" so a single Prometheus scrape
        // config can drive a cross-runtime Grafana dashboard.
        body = simulator.ces_coverage().to_prometheus_text(machines, {{"runtime", "cpp"}});
        std::ostringstream extras;
        extras << "# HELP re_runtime_dimension Current dimension of the shared perceptual space.\n";
        extras << "# TYPE re_runtime_dimension gauge\n";
        extras << "re_runtime_dimension{runtime=\"cpp\"} " << simulator.dimension() << "\n";
        extras << "# HELP re_runtime_required_dimension Max(offset+length) across all registered machine mappings.\n";
        extras << "# TYPE re_runtime_required_dimension gauge\n";
        extras << "re_runtime_required_dimension{runtime=\"cpp\"} " << simulator.required_dimension() << "\n";
        extras << "# HELP re_runtime_mapping_version Monotonic version bumped on every add/remove_machine.\n";
        extras << "# TYPE re_runtime_mapping_version gauge\n";
        extras << "re_runtime_mapping_version{runtime=\"cpp\"} " << simulator.mapping_version() << "\n";
        body += extras.str();
      }
      http::Response r;
      r.status = 200;
      r.body = body;
      r.contentType = "text/plain; version=0.0.4; charset=utf-8";
      return r;
    });
    // Governance / paging contract resolver — identical wire shape to the
    // AI runtime.  Single source of truth for on-call routing.
    server.route("GET", "/api/governance/route", [this](const http::Request& req) {
      auto mid = req.queryParams.find("machineId");
      auto sid = req.queryParams.find("sequenceId");
      auto vQ  = req.queryParams.find("values");
      if (mid == req.queryParams.end() || sid == req.queryParams.end() || vQ == req.queryParams.end()) {
        return http::error_response("machineId, sequenceId, and values query parameters are required", 400);
      }
      std::vector<double> values;
      std::stringstream ss(vQ->second);
      std::string tok;
      while (std::getline(ss, tok, ',')) {
        try { values.push_back(std::stod(tok)); } catch (...) { /* skip */ }
      }
      std::shared_lock<std::shared_mutex> lock(registryMutex);
      auto it = machines.find(mid->second);
      if (it == machines.end()) return http::error_response("Machine not found: " + mid->second, 404);
      auto decision = resolve_governance(it->second, sid->second, values);
      if (!decision) {
        return http::error_response(
          "No triggerConfig rule matches (sequenceId=" + sid->second + ", values=" + vQ->second + ")", 404);
      }
      return ok(Json::Object{{"success", true}, {"decision", to_json(*decision)}});
    });
    server.route("GET", "/api/runtime/vector-space", [this](const http::Request&) {
      std::lock_guard<std::mutex> lock(simulatorMutex);
      return ok(Json::Object{
        {"dimension", static_cast<double>(simulator.dimension())},
        {"requiredDimension", static_cast<double>(simulator.required_dimension())},
        {"encoding", "dense-float64-clamped-0-1"},
        {"mappingVersion", static_cast<double>(simulator.mapping_version())}
      });
    });
    server.route("GET", "/api/runtime/storage-footprint", [this](const http::Request&) {
      return ok(storage_footprint_json());
    });
    server.route("GET", "/api/runtime/options", [this](const http::Request&) {
      std::lock_guard<std::mutex> lock(simulatorMutex);
      return ok(runtime_options());
    });
    server.route("PATCH", "/api/runtime/options", [this](const http::Request& req) {
      auto body = parse_body(req);
      std::lock_guard<std::mutex> lock(simulatorMutex);
      if (body.at("historyLimit").is_number()) simulator.set_history_limit(static_cast<size_t>(body.at("historyLimit").as_number()));
      if (body.at("includeMachineResults").is_bool()) includeMachineResultsDefault = body.at("includeMachineResults").as_bool();
      if (body.at("includePerceptualSpace").is_bool()) includePerceptualSpaceDefault = body.at("includePerceptualSpace").as_bool();
      return ok(runtime_options());
    });
    server.route("GET", "/api/engine/active", [this](const http::Request&) { return ok(Json::Object{{"activeVectors", active_vectors_json()}}); });
    server.route("GET", "/api/engine/history", [this](const http::Request& req) {
      int limit = 0;
      auto it = req.queryParams.find("limit");
      if (it != req.queryParams.end() && !it->second.empty()) limit = std::stoi(it->second);
      Json::Array arr;
      std::lock_guard<std::mutex> lock(historyMutex);
      int count = 0;
      for (const auto& item : engineHistory) {
        if (limit > 0 && count++ >= limit) break;
        arr.push_back(item);
      }
      return ok(Json::Object{{"history", arr}});
    });
    // Trajectory histories — SURFACE_SPEC.md, "Trajectory histories".
    // Ascending stepNumber. `from` selects the first stepNumber to include,
    // `limit` caps the entries returned from there; both default to the whole
    // history. A comparison walks these by index across engines and reports
    // the first disagreement, so the window has to be selectable by step
    // rather than by recency.
    auto trajectory_route = [this](std::vector<TrajectoryEntry> (PerceptualSpaceSimulator::*source)() const) {
      return [this, source](const http::Request& req) {
        long from = 0;
        size_t limit = 0;
        auto fromIt = req.queryParams.find("from");
        if (fromIt != req.queryParams.end() && !fromIt->second.empty()) from = std::stol(fromIt->second);
        auto limitIt = req.queryParams.find("limit");
        if (limitIt != req.queryParams.end() && !limitIt->second.empty())
          limit = static_cast<size_t>(std::max(0L, std::stol(limitIt->second)));
        Json::Array arr;
        for (const auto& entry : (simulator.*source)()) {
          if (entry.stepNumber < from) continue;
          if (limit > 0 && arr.size() >= limit) break;
          arr.push_back(to_json(entry));
        }
        return ok(Json::Object{{"history", arr}});
      };
    };
    server.route("GET", "/api/engine/orev-history", trajectory_route(&PerceptualSpaceSimulator::orev_history));
    server.route("GET", "/api/engine/isre-history", trajectory_route(&PerceptualSpaceSimulator::isre_history));
    server.route("POST", "/api/engine/process", [this](const http::Request& req) {
      auto body = parse_body(req);
      auto vec = json::to_numbers(body.at("vector"));
      Json::Array outputs;
      std::unique_lock<std::shared_mutex> lock(registryMutex);
      for (auto& [_, m] : machines) {
        auto r = m.process_input(vec);
        if (r.machineOutput) outputs.push_back(to_json(*r.machineOutput));
      }
      auto result = Json::Object{{"inputVector", json::numbers(vec)}, {"timestamp", static_cast<double>(now_ms())}, {"outputs", outputs}};
      record_engine_history(Json::Object{{"type", "engine-process"}, {"result", result}});
      return ok(Json::Object{{"result", result}});
    });
    server.route("POST", "/api/perception/observe", [](const http::Request& req) {
      auto body = parse_body(req);
      auto data = json::to_numbers(body.at("data"));
      return ok(Json::Object{
        {"success", true},
        {"inputVector", json::numbers(data)},
        {"transformations", Json::Array{}},
        {"processingTimestamp", static_cast<double>(now_ms())}
      });
    });
    server.route("POST", "/api/sampler/start", [this](const http::Request& req) {
      auto body = parse_body(req);
      samplerRunning = true;
      samplerStrategy = body.at("strategy").as_string("manual");
      samplerIntervalMs = static_cast<long>(body.at("intervalMs").as_number(0));
      return ok(Json::Object{{"success", true}, {"stats", sampler_stats()}});
    });
    server.route("POST", "/api/sampler/stop", [this](const http::Request&) {
      samplerRunning = false;
      return ok(Json::Object{{"success", true}});
    });
    server.route("POST", "/api/sampler/sample", [this](const http::Request& req) {
      auto body = parse_body(req);
      ++samplerSampleCount;
      auto data = json::to_numbers(body.at("data"));
      Json result = Json::Object{{"inputVector", json::numbers(data)}, {"processingTimestamp", static_cast<double>(now_ms())}};
      return ok(Json::Object{{"success", true}, {"result", result}});
    });
    server.route("GET", "/api/sampler/stats", [this](const http::Request&) {
      return ok(Json::Object{{"stats", sampler_stats()}});
    });
    server.route("GET", "/api/machines", [this](const http::Request&) {
      Json::Array arr;
      // The operational machine corpus: what the Reality Engine is running.
      //
      // This served the server registry, which holds machines as declared and
      // is never stepped — and, since registry copies became transition-
      // inhibited, can never advance at all. Both membership and runtime state
      // now come from the simulator, because "what was loaded" and "what is
      // running" are different questions and this endpoint is the second one.
      //
      // The summary form carries no per-vector activation, so this is not the
      // list form of #37 or #58; per-Reality-Event state is served by
      // /api/machines/:id and /api/engine/active, both of which already read
      // the simulator. What it does carry that is runtime-mutable is
      // `arbiterRule`, `outputMergeTransformation` and `outputMergeLocked`.
      // Those are retuned through set_output_merge_transformation, which has to
      // write both copies "so a read of either agrees" — a correctness
      // property maintained by convention. Reading the running machine here
      // makes the answer authoritative instead of dependent on that convention
      // holding at every future call site.
      //
      // Adding live activation to this payload would be a cross-runtime
      // contract change: LSP and Scala return the same summary shape, and a
      // field added here alone is the divergence class #176 and #197 were about.
      std::lock_guard<std::mutex> simulatorLock(simulatorMutex);
      for (const auto& m : machines_in_canonical_order(simulator.running_machines())) {
        arr.push_back(m.to_json());
      }
      return ok(Json::Object{{"machines", arr}});
    });
    server.route("GET", "/api/machines/:id", [this](const http::Request& req) {
      std::shared_lock<std::shared_mutex> lock(registryMutex);
      auto it = machines.find(req.pathParams.at("id"));
      if (it == machines.end()) return http::error_response("Machine not found", 404);
      // Prefer the simulator's copy: add_machine registers the machine twice,
      // here and in the simulator, and only the simulator's is stepped. Serving
      // this from the registry reported every Reality Event with its initial
      // isActive however far the machine had advanced, so activation could not
      // be observed from outside the process — a machine firing on every step
      // still read as holding only its initial REs (#37).
      //
      // The registry copy remains the fallback for a machine the simulator does
      // not hold, which is any machine without a perceptualMapping.
      std::lock_guard<std::mutex> simulatorLock(simulatorMutex);
      if (const Machine* live = simulator.running_machine(req.pathParams.at("id")))
        return ok(Json::Object{{"machine", live->to_json(true)}});
      return ok(Json::Object{{"machine", it->second.to_json(true)}});
    });
    server.route("GET", "/api/buses/semantic", [this](const http::Request&) {
      try {
        return ok(load_semantic_bus_registry());
      } catch (const std::exception& e) {
        return http::error_response(std::string("semantic bus registry unavailable: ") + e.what(), 404);
      }
    });
    server.route("GET", "/api/buses/semantic/:id", [this](const http::Request& req) {
      try {
        Json registry = load_semantic_bus_registry();
        const std::string id = req.pathParams.at("id");
        const Json& buses = registry.at("semanticBuses");
        if (buses.is_array()) {
          for (const Json& bus : buses.array()) {
            if (bus.at("id").as_string() == id) return ok(Json::Object{{"bus", bus}});
          }
        }
        return http::error_response("Semantic bus not found", 404);
      } catch (const std::exception& e) {
        return http::error_response(std::string("semantic bus registry unavailable: ") + e.what(), 404);
      }
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
    server.route("PATCH", "/api/machines/:id", [this](const http::Request& req) {
      auto body = parse_body(req);
      std::unique_lock<std::shared_mutex> registryLock(registryMutex);
      std::lock_guard<std::mutex> simulatorLock(simulatorMutex);
      auto it = machines.find(req.pathParams.at("id"));
      if (it == machines.end()) return http::error_response("Machine not found", 404);
      Machine updated = it->second;
      if (body.at("name").is_string()) updated.name = body.at("name").as_string();
      if (body.at("description").is_string()) updated.description = body.at("description").as_string();
      if (body.at("metadata").is_object()) {
        for (const auto& [k, v] : body.at("metadata").object()) updated.metadata[k] = v;
      }
      remove_machine(req.pathParams.at("id"));
      add_machine(updated);
      return ok(Json::Object{{"success", true}, {"machine", updated.to_json(true)}});
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
      auto resolved = perception.resolve_input_event_vector_for_machine(json::to_numbers(body.at("universalInputSpace")), it->second);
      auto result = it->second.process_input(resolved);
      return ok(to_json(result));
    });
    server.route("POST", "/api/machines/process-universal/all", [this](const http::Request& req) {
      auto body = parse_body(req);
      auto universal = json::to_numbers(body.at("universalInputSpace"));
      std::unique_lock<std::shared_mutex> lock(registryMutex);
      PerceptionMapper resolver(dimension);
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
      PerceptionMapper resolver(dimension);
      auto input = resolver.resolve_input_event_vector_for_machine(json::to_numbers(parse_body(req).at("universalInputSpace")), copy);
      return ok(to_json(copy.process_input(input)));
    });
    server.route("GET", "/api/machines/json/list", [this](const http::Request&) {
      Json::Array arr;
      namespace fs = std::filesystem;
      // Corpus semantics manifest (may be absent — fields are then omitted).
      Json::Object semanticsByKey;
      try {
        Json manifest = load_semantics_manifest();
        semanticsByKey = manifest.at("machines").object();
      } catch (...) {}
      if (fs::exists(machinesDirectory)) for (const auto& p : fs::recursive_directory_iterator(machinesDirectory)) if (p.path().extension() == ".json") {
        std::string relFile = fs::relative(p.path(), machinesDirectory).generic_string();
        Json::Object entry{{"filename", p.path().filename().string()}, {"relFile", relFile}, {"name", p.path().stem().string()}, {"description", ""}, {"version", "1.0.0"}, {"metadata", Json::Object{}}, {"sequenceCount", 0.0}};
        // Manifest keys are "<domain>/<stem>": domains/<d>/X.json -> d/X, else core/X.
        std::string key = relFile.rfind("domains/", 0) == 0 ? relFile.substr(8) : "core/" + relFile;
        if (key.size() > 5 && key.ends_with(".json")) key.erase(key.size() - 5);
        auto found = semanticsByKey.find(key);
        if (found != semanticsByKey.end()) {
          entry["semanticsIri"] = found->second.at("iri").as_string();
          entry["semanticsHash"] = found->second.at("sha256").as_string();
        }
        arr.push_back(entry);
      }
      return ok(Json::Object{{"machines", arr}});
    });
    // Semantic audit trail (SEMANTIC_AUDIT_CONTRACT.md, milestone M5):
    // re:SequenceObservation records emitted while machines process input,
    // IRI-enriched from the corpus semantics manifest at read time.
    server.route("GET", "/api/audit/semantics", [this](const http::Request& req) {
      size_t limit = 100;
      auto limitQuery = req.queryParams.find("limit");
      if (limitQuery != req.queryParams.end()) {
        try { limit = static_cast<size_t>(std::max(0, std::stoi(limitQuery->second))); } catch (...) {}
      }
      // machine name -> ABox base IRI (manifest iri minus the #machine fragment).
      std::map<std::string, std::string> baseByName;
      try {
        Json manifest = load_semantics_manifest();
        for (const auto& [key, entry] : manifest.at("machines").object()) {
          (void)key;
          std::string iri = entry.at("iri").as_string();
          const auto hash = iri.find('#');
          if (hash != std::string::npos) baseByName[entry.at("name").as_string()] = iri.substr(0, hash);
        }
      } catch (...) {}

      std::vector<SequenceObservation> observations;
      {
        std::lock_guard<std::mutex> lock(simulatorMutex);
        observations = simulator.semantic_audit().recent(limit);
      }
      Json::Array records;
      for (const auto& o : observations) {
        auto found = baseByName.find(o.machineName);
        const bool haveBase = found != baseByName.end();
        auto iri = [&](const std::string& prefix, const std::string& local) -> Json {
          if (!haveBase) return nullptr;
          return found->second + "#" + prefix + "-" + sanitize_local_name(local);
        };
        Json::Object record{
          {"type", std::string("re:SequenceObservation")},
          {"at", static_cast<double>(o.at)},
          {"machineId", o.machineId},
          {"machineName", o.machineName},
          {"machineIri", haveBase ? Json(found->second + "#machine") : Json(nullptr)},
          {"sequenceId", o.sequenceId},
          {"sequenceIri", iri("seq", o.sequenceId)},
          {"stepId", o.stepId},
          {"stepIri", iri("step", o.stepId)},
          {"completed", o.completed},
          {"determinationIri", o.determinationId.empty() ? Json(nullptr) : iri("out", o.determinationId)},
          {"actionCode", o.actionCode.empty() ? Json(nullptr) : Json(o.actionCode)},
          {"ragStatus", o.ragStatus.empty() ? Json(nullptr) : Json(o.ragStatus)}};
        records.push_back(record);
      }
      return ok(Json::Object{{"records", records}, {"count", static_cast<double>(records.size())}});
    });
    // OWL semantic identity (roadmap M4): IRI + ABox content hash from the
    // corpus semantics/abox-manifest.json, keyed by machine name. Contract
    // mirrors the Scala and TypeScript engines.
    server.route("GET", "/api/machines/semantics/:name", [this](const http::Request& req) {
      const std::string name = url_decode(req.pathParams.at("name"));
      Json manifest;
      try {
        manifest = load_semantics_manifest();
      } catch (const std::exception& e) {
        return http::error_response(std::string("semantics manifest unavailable: ") + e.what(), 404);
      }
      for (const auto& [key, entry] : manifest.at("machines").object()) {
        if (entry.at("name").as_string() != name) continue;
        return ok(Json::Object{
          {"name", name},
          {"machineKey", key},
          {"semanticsIri", entry.at("iri").as_string()},
          {"semanticsHash", entry.at("sha256").as_string()},
          {"sourceFile", entry.at("sourceFile").as_string()},
          {"ontology", manifest.at("ontology").as_string()}});
      }
      return http::error_response("No semantics manifest entry for machine: " + name, 404);
    });
    server.route("GET", "/api/machines/json/:name", [this](const http::Request& req) {
      std::string name = req.pathParams.at("name");
      if (name.find("..") != std::string::npos) return http::error_response("Invalid machine name: " + name, 400);
      std::string filename = name.ends_with(".json") ? name : name + ".json";
      // Flat path first, then recursive basename search — corpus files may
      // live in domain subdirectories (filenames are globally unique).
      std::filesystem::path path = find_machine_file(machinesDirectory, filename);
      std::ifstream in(path);
      if (!in) return http::error_response("Machine file not found: " + name, 404);
      std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      // Same id canonicalization as startup loading so re-loading a corpus
      // file replaces the startup machine instead of duplicating it.
      std::string stem = path.stem().string();
      std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return std::isalnum(c) ? static_cast<char>(std::tolower(c)) : '-'; });
      Machine m = load_machine_from_json_string(raw, "machine-" + stem);
      std::unique_lock<std::shared_mutex> registryLock(registryMutex);
      std::lock_guard<std::mutex> simulatorLock(simulatorMutex);
      add_machine(m);
      return ok(Json::Object{{"success", true}, {"machine", m.to_json(true)}, {"message", "Machine loaded successfully"}});
    });
    server.route("POST", "/api/machines/json/import", [this](const http::Request& req) {
      Machine m = load_machine_from_json_string(req.body);
      std::unique_lock<std::shared_mutex> registryLock(registryMutex);
      std::lock_guard<std::mutex> simulatorLock(simulatorMutex);
      add_machine(m);
      return ok(Json::Object{{"success", true}, {"machine", m.to_json(true)}});
    });
    server.route("GET", "/api/machines/:id/export", [this](const http::Request& req) {
      std::shared_lock<std::shared_mutex> lock(registryMutex);
      auto it = machines.find(req.pathParams.at("id"));
      if (it == machines.end()) return http::error_response("Machine not found", 404);
      return ok(Json::Object{{"version", "1.0.0"}, {"machine", it->second.to_json(true)}});
    });
    server.route("GET", "/api/machines/:id/checkpoints", [this](const http::Request& req) {
      Json::Array arr;
      std::lock_guard<std::mutex> lock(checkpointMutex);
      for (const auto& [cpId, cp] : checkpoints[req.pathParams.at("id")]) {
        arr.push_back(Json::Object{{"id", cpId}, {"label", cp.label}, {"timestamp", static_cast<double>(cp.timestamp)}});
      }
      return ok(arr);
    });
    // The merge knob, readable always and settable only while unlocked.
    //
    // Declared on the machine (`outputMergeTransformation`, default "or") so it
    // is carried from the moment the machine is interned, and mutable here so a
    // run can be retuned between steps without reloading the corpus. It is a
    // training variable; both properties are needed.
    //
    // The interlock starts LOCKED. Retuning a training variable by accident
    // produces a run whose results mean nothing and which nothing distinguishes
    // from a valid one, so unlocking is a separate, deliberate act.
    //
    // Every write applies to the simulator's machine as well as the registry
    // copy: those are two objects and only the simulator's is stepped, so
    // setting one alone would leave the knob reading differently from the knob
    // in force.
    server.route("GET", "/api/machines/:id/output-merge", [this](const http::Request& req) {
      std::shared_lock<std::shared_mutex> lock(registryMutex);
      auto it = machines.find(req.pathParams.at("id"));
      if (it == machines.end()) return http::error_response("Machine not found", 404);
      std::lock_guard<std::mutex> simulatorLock(simulatorMutex);
      const Machine* live = simulator.running_machine(req.pathParams.at("id"));
      const Machine& m = live ? *live : it->second;
      Json::Array available;
      // Boolean gates first, then the multi-valued chain folds. Order is the
      // enum's, so this list and `output_merge_from_string` stay readable
      // against each other — a name here that the parser rejects is a 400 the
      // UI offered.
      for (const auto& name : {"or", "and", "xor", "nor", "nand",
                               "meet", "join", "strong-conjunction",
                               "strong-disjunction", "discrete-median"})
        available.push_back(std::string(name));
      return ok(Json::Object{
        {"machineId", m.id}, {"machineName", m.name},
        {"outputMergeTransformation", to_string(m.outputMergeTransformation)},
        {"locked", m.outputMergeLocked},
        {"available", available}});
    });
    server.route("PUT", "/api/machines/:id/output-merge", [this](const http::Request& req) {
      auto body = parse_body(req);
      if (!body.at("outputMergeTransformation").is_string())
        return http::error_response("outputMergeTransformation must be a string", 400);
      OutputMergeTransformation t;
      try { t = output_merge_from_string(body.at("outputMergeTransformation").as_string()); }
      catch (const std::exception& e) { return http::error_response(e.what(), 400); }
      std::unique_lock<std::shared_mutex> registryLock(registryMutex);
      auto it = machines.find(req.pathParams.at("id"));
      if (it == machines.end()) return http::error_response("Machine not found", 404);
      // 423 rather than 403: the refusal is about the resource's current state
      // and is cleared by unlocking, not about who is asking.
      if (it->second.outputMergeLocked)
        return http::error_response(
          "outputMergeTransformation is locked; unlock it before changing a training variable", 423);
      it->second.outputMergeTransformation = t;
      std::lock_guard<std::mutex> simulatorLock(simulatorMutex);
      simulator.set_output_merge_transformation(req.pathParams.at("id"), t);
      return ok(Json::Object{{"success", true}, {"machineId", it->second.id},
                             {"outputMergeTransformation", to_string(t)},
                             {"locked", it->second.outputMergeLocked}});
    });
    server.route("PUT", "/api/machines/:id/output-merge/lock", [this](const http::Request& req) {
      auto body = parse_body(req);
      if (!body.at("locked").is_bool())
        return http::error_response("locked must be a boolean", 400);
      const bool locked = body.at("locked").as_bool(true);
      std::unique_lock<std::shared_mutex> registryLock(registryMutex);
      auto it = machines.find(req.pathParams.at("id"));
      if (it == machines.end()) return http::error_response("Machine not found", 404);
      it->second.outputMergeLocked = locked;
      std::lock_guard<std::mutex> simulatorLock(simulatorMutex);
      simulator.set_output_merge_locked(req.pathParams.at("id"), locked);
      return ok(Json::Object{{"success", true}, {"machineId", it->second.id}, {"locked", locked}});
    });
    server.route("POST", "/api/machines/:id/checkpoints", [this](const http::Request& req) {
      auto body = parse_body(req);
      std::shared_lock<std::shared_mutex> registryLock(registryMutex);
      auto it = machines.find(req.pathParams.at("id"));
      if (it == machines.end()) return http::error_response("Machine not found", 404);
      // Snapshot the machine as it is *running*, not as it was loaded. The
      // registry copy is never stepped — the simulator holds the one that is —
      // so checkpointing it captured the machine's initial Reality Event
      // activation whatever state it had reached, and restoring it could not
      // return the machine to the step it was captured at (#37).
      std::lock_guard<std::mutex> simulatorLock(simulatorMutex);
      const Machine* live = simulator.running_machine(req.pathParams.at("id"));
      Checkpoint cp{make_id("checkpoint"), body.at("label").as_string(), now_ms(),
                    live ? *live : it->second};
      {
        std::lock_guard<std::mutex> lock(checkpointMutex);
        checkpoints[it->first][cp.id] = cp;
      }
      return ok(Json::Object{{"success", true}, {"checkpointId", cp.id}});
    });
    server.route("POST", "/api/machines/:machineId/checkpoints/:cpId/restore", [this](const http::Request& req) {
      std::unique_lock<std::shared_mutex> registryLock(registryMutex);
      std::lock_guard<std::mutex> simulatorLock(simulatorMutex);
      std::lock_guard<std::mutex> cpLock(checkpointMutex);
      auto machineIt = checkpoints.find(req.pathParams.at("machineId"));
      if (machineIt == checkpoints.end() || !machineIt->second.count(req.pathParams.at("cpId"))) return http::error_response("Checkpoint not found", 404);
      remove_machine(req.pathParams.at("machineId"));
      add_machine(machineIt->second.at(req.pathParams.at("cpId")).machine);
      return ok(Json::Object{{"success", true}});
    });
    server.route("DELETE", "/api/machines/:machineId/checkpoints/:cpId", [this](const http::Request& req) {
      std::lock_guard<std::mutex> lock(checkpointMutex);
      bool removed = checkpoints[req.pathParams.at("machineId")].erase(req.pathParams.at("cpId")) > 0;
      return ok(Json::Object{{"success", removed}});
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
    // Arbitration records for the most recent step (ARBITER_CONTRACT.md 6).
    //
    // The records were produced and retained on the step all along; nothing
    // served them, so a resolution was indistinguishable from no resolution
    // from outside the process, and acceptance criteria 8 and 9 could not be
    // checked at all. A suppressed contribution in particular has to stay
    // attributable — "the agent's answer was discarded" is exactly the
    // operational fact the domain bus exists to surface.
    //
    // Wire shape matches the Scala runtime's /api/arbitration byte for byte;
    // cross-runtime parity is the acceptance test for this contract, so a
    // divergent shape here would defeat the endpoint's own purpose.
    server.route("GET", "/api/arbitration", [this](const http::Request&) {
      std::lock_guard<std::mutex> lock(simulatorMutex);
      const auto hist = simulator.history();
      Json::Array records;
      if (!hist.empty()) {
        auto emit = [](const Contribution& c) {
          return Json::Object{
              {"provider", c.provider},
              {"determinism", determinism_name(determinism_of(c.provider))},
              {"originId", c.originId},
              {"cesId", c.cesId.empty() ? Json{} : Json{c.cesId}},
              {"outputVectorId", c.outputVectorId.empty() ? Json{} : Json{c.outputVectorId}},
              {"ragStatusCode", c.ragStatusCode.empty() ? Json{} : Json{c.ragStatusCode}},
              {"value", c.value}};
        };
        for (const auto& r : hist.back().arbitration) {
          Json::Array contributors, suppressed;
          for (const auto& c : r.contributors) contributors.push_back(emit(c));
          for (const auto& c : r.suppressed) suppressed.push_back(emit(c));
          records.push_back(Json::Object{
              {"instant", static_cast<double>(r.instant)},
              {"cell", static_cast<double>(r.cell)},
              {"rule", r.rule},
              {"resolved", r.resolved},
              {"contributors", contributors},
              {"suppressed", suppressed}});
        }
      }
      const auto& registry = ArbitrationRegistry::instance();
      return ok(Json::Object{
          {"registryEntries", static_cast<double>(registry.size())},
          {"registrySource", registry.source().empty() ? Json{} : Json{registry.source()}},
          {"shards", static_cast<double>(arbiter_shards())},
          {"count", static_cast<double>(records.size())},
          {"records", records}});
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
    server.route("POST", "/api/perception/diagnostic", [this](const http::Request& req) {
      std::shared_lock<std::shared_mutex> lock(registryMutex);
      PerceptionMapper resolver(dimension);
      return ok(resolver.diagnostic_mapping(json::to_numbers(parse_body(req).at("universalInputSpace")), machines));
    });
    server.route("GET", "/api/demo/multi-step", [this](const http::Request&) {
      return demo_machine_response("Multi-Step State Machine", "MultiStep.json", "Multi-Step State Machine");
    });
    server.route("GET", "/api/demo/data-center", [this](const http::Request&) {
      return demo_machine_response("Data Center Monitoring", "DataCenterMonitoring.json", "Data Center Monitoring");
    });
    server.route("GET", "/api/demo/kleene-star", [this](const http::Request&) {
      return demo_machine_response("Kleene Star Operator", "KleeneStar.json", "Kleene Star Operator");
    });
    server.route("POST", "/api/perceive", [this](const http::Request& req) {
      auto body = parse_body(req);
      std::optional<ComparatorType> overrideType;
      if (body.at("matchAlgorithmOverride").is_string()) overrideType = comparator_from_string(body.at("matchAlgorithmOverride").as_string());
      else if (body.at("matchAlgorithm").is_string()) overrideType = comparator_from_string(body.at("matchAlgorithm").as_string());
      bool includeMachineResults = body.at("includeMachineResults").as_bool(body.at("compact").as_bool(false) ? false : includeMachineResultsDefault);
      bool includePerceptualSpace = body.at("includePerceptualSpace").as_bool(includePerceptualSpaceDefault);

      // Polymorphic input — exactly one of vector / sparseVector / domainVectors.
      // The simulator's tolerant set_vector handles over/under-sized dense inputs.
      Vector assembled;
      bool haveInput = false;
      if (body.at("vector").is_array()) {
        assembled = json::to_numbers(body.at("vector"));
        haveInput = true;
      } else if (body.at("sparseVector").is_array()) {
        size_t maxIdx = 0;
        bool any = false;
        for (const auto& e : body.at("sparseVector").array()) {
          size_t idx = static_cast<size_t>(e.at("index").as_number());
          if (!any || idx > maxIdx) maxIdx = idx;
          any = true;
        }
        size_t len = std::max<size_t>(any ? maxIdx + 1 : 0, static_cast<size_t>(simulator.dimension()));
        assembled.assign(len, 0.0);
        for (const auto& e : body.at("sparseVector").array()) {
          assembled[static_cast<size_t>(e.at("index").as_number())] = e.at("value").as_number();
        }
        haveInput = true;
      } else if (body.at("domainVectors").is_array()) {
        size_t maxEnd = 0;
        for (const auto& r : body.at("domainVectors").array()) {
          size_t off = static_cast<size_t>(r.at("offset").as_number());
          size_t n = r.at("values").is_array() ? r.at("values").array().size() : 0;
          maxEnd = std::max(maxEnd, off + n);
        }
        size_t len = std::max<size_t>(maxEnd, static_cast<size_t>(simulator.dimension()));
        assembled.assign(len, 0.0);
        for (const auto& r : body.at("domainVectors").array()) {
          size_t off = static_cast<size_t>(r.at("offset").as_number());
          auto vals = json::to_numbers(r.at("values"));
          for (size_t i = 0; i < vals.size(); ++i) assembled[off + i] = vals[i];
        }
        haveInput = true;
      }
      if (!haveInput) return http::error_response("Provide exactly one of: vector, sparseVector, domainVectors", 400);

      SimulationStep step;
      {
        std::lock_guard<std::mutex> lock(simulatorMutex);
        step = simulator.process_immediate(assembled, overrideType);
        perception.perceptual_space().set_vector(step.perceptualSpace);
      }
      Json out = to_json(step, includeMachineResults, includePerceptualSpace);
      bool compact = body.at("compact").as_bool(false);
      auto compactQuery = req.queryParams.find("compact");
      if (compactQuery != req.queryParams.end()) compact = compactQuery->second == "true" || compactQuery->second == "1";
      if (compact) add_packed_merge_values(out);
      if (sseHub) sseHub->broadcast(json::stringify(out));
      return ok(out);
    });
  }

private:
  static Json parse_body(const http::Request& req) { return req.body.empty() ? Json::Object{} : json::parse(req.body); }

  // Restrict an ABox IRI local name to the generator's PN_LOCAL subset
  // (scripts/generate-owl.py sanitize()) so runtime IRIs match the corpus.
  static std::string sanitize_local_name(const std::string& local) {
    std::string out;
    out.reserve(local.size());
    for (char c : local) {
      const bool safe = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
      out += safe ? c : '_';
    }
    return out.empty() ? "unnamed" : out;
  }

  // Percent-decode a path parameter (machine names contain spaces).
  static std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
        out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
        i += 2;
      } else {
        out += s[i];
      }
    }
    return out;
  }
  static http::Response ok(const Json& value) { return http::json_response(json::stringify(value)); }
  static Json string_array(const std::vector<std::string>& values) {
    Json::Array out;
    for (const auto& value : values) out.emplace_back(value);
    return out;
  }
  static double cosine(const Vector& a, const Vector& b) {
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) dot += a[i] * b[i];
    for (double v : a) na += v * v;
    for (double v : b) nb += v * v;
    if (na == 0.0 || nb == 0.0) return 0.0;
    return dot / (std::sqrt(na) * std::sqrt(nb));
  }
  static Vector searchable_vector_values(const Json& vector) {
    if (vector.at("vector").is_array()) return json::to_numbers(vector.at("vector"));
    Vector values;
    const Json& elements = vector.at("elements");
    if (!elements.is_array()) return values;
    for (const auto& element : elements.array()) {
      if (element.is_object()) values.push_back(element.at("value").as_number());
      else values.push_back(element.as_number());
    }
    return values;
  }
  static RealityVector vector_from_json(const Json& body) {
    std::vector<VectorElement> elements;
    for (const auto& ej : body.at("elements").is_array() ? body.at("elements").array() : Json::Array{}) {
      VectorElement element;
      element.value = ej.at("value").as_number();
      if (ej.at("comparatorType").is_string()) element.comparatorType = comparator_from_string(ej.at("comparatorType").as_string());
      if (ej.at("threshold").is_number()) element.threshold = ej.at("threshold").as_number();
      elements.push_back(element);
    }
    RealityVector vector(elements, body.at("isInitial").as_bool(false), body.at("id").as_string(make_id("vector")));
    for (const auto& next : body.at("nextVectorIds").is_array() ? body.at("nextVectorIds").array() : Json::Array{}) vector.add_next_vector(next.as_string());
    for (const auto& output : body.at("outputVectors").is_array() ? body.at("outputVectors").array() : Json::Array{}) {
      std::map<std::string, Json> metadata;
      if (output.at("metadata").is_object()) metadata = output.at("metadata").object();
      vector.add_output_vector({output.at("id").as_string(make_id("output")), json::to_numbers(output.at("vector")), metadata, now_ms(), {}});
    }
    return vector;
  }
  static CriticalEventSequence sequence_from_json(const Json& body) {
    CriticalEventSequence seq(body.at("name").as_string("unnamed"), body.at("id").as_string(make_id("sequence")));
    if (body.at("metadata").is_object()) seq.metadata = body.at("metadata").object();
    for (const auto& vector : body.at("vectors").is_array() ? body.at("vectors").array() : Json::Array{}) seq.add_vector(vector_from_json(vector));
    return seq;
  }
  static std::string qdrant_url() {
    const char* value = std::getenv("QDRANT_URL");
    return value ? value : "http://localhost:4333";
  }
  static std::string collection_name() {
    const char* value = std::getenv("COLLECTION_NAME");
    return value ? value : "reality-vectors";
  }

  std::filesystem::path semantic_bus_registry_path() const {
    if (const char* explicitPath = std::getenv("SEMANTIC_BUS_REGISTRY")) {
      if (*explicitPath) return std::filesystem::path(explicitPath);
    }

    std::filesystem::path cursor = std::filesystem::absolute(machinesDirectory);
    for (int i = 0; i < 6 && !cursor.empty(); ++i) {
      std::filesystem::path candidate = cursor / "domains" / "semantic-bus-registry.json";
      if (std::filesystem::exists(candidate)) return candidate;
      cursor = cursor.parent_path();
    }
    return std::filesystem::absolute(machinesDirectory).parent_path() / "domains" / "semantic-bus-registry.json";
  }

  Json load_semantic_bus_registry() const {
    const auto path = semantic_bus_registry_path();
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    Json registry = json::parse(buffer.str());
    if (!registry.is_object() || !registry.at("semanticBuses").is_array()) {
      throw std::runtime_error("invalid registry shape at " + path.string());
    }
    return registry;
  }

  // OWL semantics manifest (RealityEngine_Machines semantics/abox-manifest.json):
  // per-machine semantic identity — ABox IRI + content hash — for the
  // cross-engine semantic-equivalence surface (roadmap milestone M4).
  std::filesystem::path semantics_manifest_path() const {
    if (const char* explicitPath = std::getenv("SEMANTICS_MANIFEST")) {
      if (*explicitPath) return std::filesystem::path(explicitPath);
    }
    std::filesystem::path cursor = std::filesystem::absolute(machinesDirectory);
    for (int i = 0; i < 6 && !cursor.empty(); ++i) {
      std::filesystem::path candidate = cursor / "semantics" / "abox-manifest.json";
      if (std::filesystem::exists(candidate)) return candidate;
      cursor = cursor.parent_path();
    }
    return std::filesystem::absolute(machinesDirectory).parent_path() / "semantics" / "abox-manifest.json";
  }

  Json load_semantics_manifest() const {
    const auto path = semantics_manifest_path();
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    Json manifest = json::parse(buffer.str());
    if (!manifest.is_object() || !manifest.at("machines").is_object()) {
      throw std::runtime_error("invalid semantics manifest shape at " + path.string());
    }
    return manifest;
  }

  int bits_per_element_for_machine(const std::string& machineId) const {
    std::shared_lock<std::shared_mutex> lock(registryMutex);
    auto it = machines.find(machineId);
    if (it == machines.end() || !it->second.perceptualMapping || !it->second.perceptualMapping->bitsPerElement) return 8;
    int bits = *it->second.perceptualMapping->bitsPerElement;
    return is_allowed_bits_per_element(bits) ? bits : 8;
  }

  Json storage_footprint_json() const {
    Json::Array perMachine;
    Json::Object widthHistogram{{"1", 0.0}, {"2", 0.0}, {"4", 0.0}, {"8", 0.0}};
    size_t totalCells = 0;
    size_t totalFloat64Bytes = 0;
    size_t totalPackedBytes = 0;

    std::shared_lock<std::shared_mutex> lock(registryMutex);
    for (const auto& [id, machine] : machines) {
      if (!machine.perceptualMapping) continue;
      int bits = machine.perceptualMapping->bitsPerElement.value_or(8);
      if (!is_allowed_bits_per_element(bits)) bits = 8;
      const size_t cells = static_cast<size_t>(machine.perceptualMapping->input.length + machine.perceptualMapping->output.length);
      auto footprint = storage_footprint(cells, bits);
      totalCells += cells;
      totalFloat64Bytes += footprint.float64Bytes;
      totalPackedBytes += footprint.packedBytes;
      widthHistogram[std::to_string(bits)] = widthHistogram[std::to_string(bits)].as_number() + 1.0;
      perMachine.push_back(Json::Object{
        {"machineId", id},
        {"machineName", machine.name},
        {"bitsPerElement", static_cast<double>(bits)},
        {"inputCells", static_cast<double>(machine.perceptualMapping->input.length)},
        {"outputCells", static_cast<double>(machine.perceptualMapping->output.length)},
        {"float64Bytes", static_cast<double>(footprint.float64Bytes)},
        {"packedBytes", static_cast<double>(footprint.packedBytes)},
        {"shrinkFactor", footprint.shrinkFactor}
      });
    }

    double shrink = totalPackedBytes == 0 ? 0.0 : static_cast<double>(totalFloat64Bytes) / static_cast<double>(totalPackedBytes);
    return Json::Object{
      {"machinesRegistered", static_cast<double>(machines.size())},
      {"totalCells", static_cast<double>(totalCells)},
      {"totalFloat64Bytes", static_cast<double>(totalFloat64Bytes)},
      {"totalPackedBytes", static_cast<double>(totalPackedBytes)},
      {"cumulativeShrinkFactor", shrink},
      {"widthHistogram", widthHistogram},
      {"perMachine", perMachine}
    };
  }

  void add_packed_merge_values(Json& step) const {
    Json& mergeBatch = step.object()["mergeBatch"];
    if (!mergeBatch.is_array()) return;
    for (auto& operation : mergeBatch.array()) {
      if (!operation.is_object()) continue;
      const int bits = bits_per_element_for_machine(operation.at("machineId").as_string());
      const Vector values = json::to_numbers(operation.at("values"));
      Json::Object packed{
        {"bitsPerElement", static_cast<double>(bits)},
        {"length", static_cast<double>(values.size())}
      };
      try {
        packed["base64"] = encode_packed_base64(values, bits);
      } catch (const std::exception& e) {
        packed["error"] = e.what();
      }
      operation.object()["valuesPacked"] = packed;
    }
  }

  void add_machine(const Machine& m) {
    machines[m.id] = m;
    // The registry holds the machine as declared, and it stays that way: only
    // the simulator's copy is stepped by the PE->RE->PE path, so only it may
    // transition. Without this, any endpoint calling process_input on a
    // registry machine advanced a copy nothing else observes and forked the two
    // silently for the life of the process.
    machines[m.id].transitionsInhibited = true;
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
  Json active_vectors_json() const {
    Json::Array active;
    for (const auto& [id, registered] : machines) {
      // Read the machine as it is *running*, not as it was registered.
      //
      // add_machine keeps two copies — one in this registry, one in the
      // simulator — and only the simulator's is stepped. Reading the registry
      // here reported every Reality Event with its post-ingestion isActive no
      // matter how far the machine had advanced: the list was correct at step 0
      // and then frozen for the life of the process. Matching still worked, so
      // mergeBatch, stepNumber and globalStep all agreed with LSP and Scala
      // while GET /api/engine/active stood still at the 27 initial events.
      //
      // This is #37 again, on a second endpoint. That issue added
      // running_machine() for exactly this reason — "the machine as it is
      // running, Reality Event activation included" — and fixed the
      // machine-detail path; this one was left on the registry and reproduced
      // the defect verbatim.
      //
      // Falls back to the registered copy when the simulator holds no such
      // machine. A machine without a perceptualMapping is never added there, so
      // it has no running form to report and its declared initial state is the
      // only truthful answer.
      const Machine* src = simulator.running_machine(id);
      if (src == nullptr) src = &registered;
      // all_sequences() returns by value; bind it once rather than copying the
      // whole vector again per iteration.
      auto sequences = src->all_sequences();
      for (auto& seq : sequences) {
        for (const auto* vector : seq.active_vectors()) {
          active.push_back(Json::Object{
            {"machineId", id},
            {"sequenceId", seq.id},
            {"vector", active_vector_json(*vector)}
          });
        }
      }
    }
    return active;
  }
  Json active_vector_json(const RealityVector& vector) const {
    Json full = vector.to_json();
    Json::Array outs;
    for (const auto& o : full.at("outputVectors").is_array() ? full.at("outputVectors").array() : Json::Array{}) {
      outs.push_back(Json::Object{
        {"id", o.at("id")},
        {"metadata", o.at("metadata")},
        {"vector", o.at("vector")}
      });
    }
    return Json::Object{
      {"elements", full.at("elements")},
      {"id", full.at("id")},
      {"isActive", full.at("isActive")},
      {"isInitial", full.at("isInitial")},
      {"matchAlgorithm", full.at("matchAlgorithm")},
      {"metadata", full.at("metadata")},
      {"nextVectorIds", full.at("nextVectorIds")},
      {"outputVectors", outs},
      {"state", full.at("state")},
      {"wasJustMatched", full.at("wasJustMatched")}
    };
  }
  void record_engine_history(const Json& item) {
    std::lock_guard<std::mutex> lock(historyMutex);
    engineHistory.insert(engineHistory.begin(), item);
    if (engineHistory.size() > 256) engineHistory.resize(256);
  }
  Json sampler_stats() const {
    return Json::Object{
      {"isRunning", samplerRunning},
      {"sampleCount", static_cast<double>(samplerSampleCount)},
      {"bufferSize", 0.0},
      {"strategy", samplerStrategy},
      {"intervalMs", static_cast<double>(samplerIntervalMs)}
    };
  }
  Json runtime_options() const {
    return Json::Object{
      {"historyLimit", static_cast<double>(simulator.history_limit())},
      {"includeMachineResults", includeMachineResultsDefault},
      {"includePerceptualSpace", includePerceptualSpaceDefault},
      {"projectionControls", Json::Object{
        {"includeMachineResults", "boolean request field on /api/perceive"},
        {"includePerceptualSpace", "boolean request field on /api/perceive"},
        {"compact", "sets includeMachineResults false when includeMachineResults is omitted"}
      }}
    };
  }

  http::Response demo_machine_response(const std::string& machineName, const std::string& fileName, const std::string& displayName) {
    std::shared_lock<std::shared_mutex> lock(registryMutex);
    const Machine* machine = nullptr;
    for (const auto& [_, candidate] : machines) {
      if (candidate.name == machineName) {
        machine = &candidate;
        break;
      }
    }
    if (!machine) {
      return http::json_response(json::stringify(Json::Object{
        {"error", displayName + " machine not found. Please ensure " + fileName + " is loaded."}
      }), 404);
    }

    Json::Array sequenceNames;
    for (const auto& seq : machine->all_sequences()) sequenceNames.emplace_back(seq.name);
    const Json& inputSequences = machine->metadata.count("inputSequences") ? machine->metadata.at("inputSequences") : Json(nullptr);
    int inputVectorCount = 0;
    if (inputSequences.is_array() && !inputSequences.array().empty()) {
      const Json& vectors = inputSequences.array().front().at("vectors");
      if (vectors.is_array()) inputVectorCount = static_cast<int>(vectors.array().size());
    }

    Json::Object metadata = machine->metadata;
    metadata["name"] = machine->name;
    metadata["description"] = machine->description;
    metadata["machineId"] = machine->id;
    metadata["totalSequences"] = static_cast<double>(machine->sequence_count());
    metadata["sequenceNames"] = sequenceNames;
    metadata["totalInputVectors"] = static_cast<double>(inputVectorCount);

    return ok(Json::Object{
      {"success", true},
      {"machine", machine->to_json(true)},
      {"metadata", metadata},
      {"sequencesLoaded", static_cast<double>(machine->sequence_count())},
      {"inputVectorsLoaded", static_cast<double>(inputVectorCount)}
    });
  }

  struct Checkpoint {
    std::string id;
    std::string label;
    long long timestamp = 0;
    Machine machine;
  };

  std::map<std::string, Machine> machines;
  std::map<std::string, Json> vectorStore;
  std::map<std::string, CriticalEventSequence> sequences;
  std::map<std::string, std::map<std::string, Checkpoint>> checkpoints;
  mutable std::shared_mutex registryMutex;
  mutable std::mutex simulatorMutex;
  mutable std::mutex vectorMutex;
  mutable std::mutex sequenceMutex;
  mutable std::mutex checkpointMutex;
  mutable std::mutex historyMutex;
  int dimension = 768;
  double matchThreshold = 0.5;
  PerceptualSpaceSimulator simulator;
  PerceptionMapper perception;
  std::string machinesDirectory;
  std::vector<Json> engineHistory;
  std::vector<Vector> buffer;
  RegionMapping bufferedRegion{0, 1};
  long bufferedDelay = 100;
  bool includeMachineResultsDefault = true;
  bool includePerceptualSpaceDefault = true;
  bool samplerRunning = false;
  long samplerIntervalMs = 0;
  long long samplerSampleCount = 0;
  std::string samplerStrategy = "MANUAL";
  std::shared_ptr<http::Server::SseHub> sseHub = std::make_shared<http::Server::SseHub>();
};

} // namespace

int main(int argc, char** argv) {
  try {
    int port = argc > 1 ? std::stoi(argv[1]) : 5301;
    std::string machinesDir = argc > 2 ? argv[2] : "../RealityEngine_Machines/machines";
    int vectorDimension = argc > 3 ? std::stoi(argv[3]) : (std::getenv("VECTOR_DIMENSION") ? std::stoi(std::getenv("VECTOR_DIMENSION")) : 7680);
    // RE_STRICT_STA=1 opts the corpus into life-safety STA enforcement at load.
    // Off by default to match the AI runtime's permissive production behaviour.
    LoadOptions loadOpts;
    if (const char* s = std::getenv("RE_STRICT_STA"); s && *s && std::string(s) != "0") loadOpts.strictSta = true;
    // Declares how each contended universal-vector position resolves. Loaded
    // before the corpus so the first step already arbitrates rather than
    // falling back (ARBITER_CONTRACT.md 5).
    ArbitrationRegistry::instance().load(machinesDir);

    http::Server server;
    RealityService service(machinesDir, vectorDimension, loadOpts);
    service.mount(server);
    server.listen(port);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Reality Engine startup failed: " << e.what() << "\n";
    return 1;
  }
}
