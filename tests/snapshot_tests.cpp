// Runtime snapshots (RealityEngine_CI#287): the retention, the body, the
// digest, restore of the active lists and the store's modes.
#include "reality/snapshot.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <unistd.h>

using namespace reality;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond, what)                                                   \
  do {                                                                      \
    if (cond) std::cout << "  PASS: " << what << "\n";                     \
    else { std::cout << "  FAIL: " << what << "  (" #cond ")\n"; ++failures; } \
  } while (0)

namespace {
const char* kMachine =
    R"({"version":"1.0.0","machine":{"name":"Linear","description":"snapshot test","arbiterRule":"PASSTHROUGH",)"
    R"("matchAlgorithm":"gte","perceptualMapping":{"input":{"offset":0,"length":2},"output":{"offset":4,"length":2}},)"
    R"("sequences":[{"id":"s","name":"s","events":[)"
    R"({"id":"e1","elements":[{"value":1,"threshold":0.5},{"value":0,"threshold":0.5}],"isInitial":true,"nextEventIds":["e2"]},)"
    R"({"id":"e2","elements":[{"value":1,"threshold":0.5},{"value":1,"threshold":0.5}],"isInitial":false,)"
    R"("outputEvents":[{"id":"o","vector":[1,1]}]}]}]}})";

struct Rig {
  PerceptualSpaceRuntime rt{16};
  snapshot::Retention retention;
  long long lamport = 100;
  std::string instance = "01a2aaaa-0000-7000-8000-0000000f4001";
  Rig() {
    rt.add_machine(load_machine_from_json_string(kMachine));
    rt.onStepCommitted = [this](long step) {
      if (step < 0) { retention.on_reset(rt.running_machines()); return; }
      retention.on_step(step, instance, ++lamport, rt.last_isre(), rt.last_osre(), *rt.committing_step(), rt.running_machines(), 250);
    };
  }
  void step(double a, double b) {
    Vector v(static_cast<size_t>(rt.dimension()), 0.0);
    v[0] = a; v[1] = b;
    rt.process_immediate(v);
  }
  Json snap(std::optional<long> from = std::nullopt, std::optional<long> to = std::nullopt,
            std::optional<std::set<std::string>> scope = std::nullopt) {
    snapshot::CaptureRequest cr;
    cr.from = from; cr.to = to;
    cr.scope = scope; cr.scopeGiven = scope.has_value();
    cr.fingerprint = std::string(64, 'f');
    cr.machines = 1;
    cr.controls = Json::Object{}; cr.machineControls = Json::Object{}; cr.machineOverrides = Json::Object{};
    cr.arbitration = nullptr;
    cr.running = &rt.running_machines();
    snapshot::Error e;
    auto s = snapshot::capture(retention, cr, e);
    return s ? *s : Json(Json::Object{{"status", static_cast<double>(e.status)}});
  }
};
}  // namespace

int main() {
  {
    Rig r;
    r.step(1, 0);  // step 0: e1 matches, e2 activates
    r.step(0, 0);  // step 1
    Json s = r.snap();
    CHECK(s.at("format").as_string() == "re-snapshot/1", "a re-snapshot/1 body");
    CHECK(s.at("kline").at("span").at("from").as_number() == 1 && s.at("kline").at("span").at("to").as_number() == 1,
          "the span defaults to the newest committed step");
    CHECK(s.at("digest").as_string() == snapshot::digest_of(s), "the digest is over the body");
    Json other = s;
    other.object()["kline"].object()["clocks"].object()["to"].object()["lamport"] = 1.0;
    CHECK(snapshot::digest_of(other) == s.at("digest").as_string(), "the clocks' lamport is outside the digest");
    Json before = r.snap(0, 0);
    bool initial = false;
    for (const auto& a : before.at("kline").at("activeBefore").array()) initial |= a.at("event").as_string() == "e1";
    CHECK(initial, "the first step's activeBefore is the initial events");
    CHECK(r.snap(0, 5).at("status").as_number() == 410, "a span beyond the retained steps is 410");

    // Restore activeAfter into a reset runtime, and the history with it.
    Json after = s.at("kline").at("activeAfter");
    r.rt.reset();
    std::map<std::string, std::vector<PerceptualSpaceRuntime::RestoreEntry>> by;
    for (const auto& a : after.array())
      by[a.at("machine").as_string()].push_back({a.at("sequence").as_string(), a.at("event").as_string(), a.at("justMatched").as_bool()});
    r.rt.restore_active_lists({"Linear"}, by);
    r.rt.restore_history({}, {}, 1);
    CHECK(kline::jcs(snapshot::actives_json(snapshot::actives(r.rt.running_machines()))) == kline::jcs(after),
          "restore puts the active list back exactly");
    r.step(1, 1);
    CHECK(r.rt.isre_history().back().stepNumber == 2, "and the next step is span.to + 1");
  }
  {
    // Restore then capture is an identity (RealityEngine_CI#904), and the
    // clocks name the instance that committed the steps, not the restorer.
    Rig src;
    src.step(1, 0);
    src.step(0, 0);
    src.step(0, 0);
    Json s = src.snap(1, 2);
    CHECK(s.at("kline").at("clocks").at("to").at("instance").as_string() == src.instance,
          "a captured step's clock names the instance that committed it");
    Rig replica;
    replica.instance = "01a2aaaa-0000-7000-8000-0000000f4999";
    replica.step(0, 0);
    CHECK(replica.snap().at("kline").at("clocks").at("to").at("instance").as_string() == replica.instance,
          "and its own steps name it");
    {
      std::map<std::string, std::vector<PerceptualSpaceRuntime::RestoreEntry>> by;
      for (const auto& a : s.at("kline").at("activeAfter").array())
        by[a.at("machine").as_string()].push_back({a.at("sequence").as_string(), a.at("event").as_string(), a.at("justMatched").as_bool()});
      replica.rt.restore_active_lists({"Linear"}, by);
      replica.rt.restore_history({}, {}, 2);
    }
    replica.retention.restore(snapshot::actives(replica.rt.running_machines()), s.at("kline"), s.at("arbitration"));
    Json again = replica.snap();
    CHECK(again.at("digest").as_string() == s.at("digest").as_string(), "a capture right after a restore is the restored snapshot");
    CHECK(kline::jcs(again) == kline::jcs(s), "byte for byte, its clocks included");
    CHECK(again.at("kline").at("clocks").at("from").at("instance").as_string() == src.instance,
          "the restored steps keep the source's instance on a replica");
    CHECK(replica.snap(1, 2).at("digest").as_string() == s.at("digest").as_string(), "naming the restored span is the same");
    CHECK(replica.snap(2, 2).at("status").as_number() == 410, "another span is 410 until the next step");
    CHECK(replica.snap(std::nullopt, std::nullopt, std::set<std::string>{"Linear"}).at("status").as_number() == 410,
          "and so is another scope");
    replica.step(0, 0);
    Json next = replica.snap();
    CHECK(next.at("kline").at("clocks").at("to").at("instance").as_string() == replica.instance,
          "the next step is the replica's own");
    CHECK(kline::jcs(next.at("kline").at("activeBefore")) == kline::jcs(s.at("kline").at("activeAfter")),
          "and starts from the restored state");
  }
  {
    // A narrowed restore is capturable too: the sandbox's re-establishing.
    Rig r;
    r.step(1, 0);
    Json s = r.snap(0, 0, std::set<std::string>{"Linear"});
    r.step(0, 0);
    r.retention.restore(snapshot::actives(r.rt.running_machines()), s.at("kline"), s.at("arbitration"));
    CHECK(r.snap().at("digest").as_string() == s.at("digest").as_string(), "a narrowed restore captures back as itself");
    CHECK(r.snap(std::nullopt, std::nullopt, std::nullopt).at("kline").at("scope").is_array(),
          "with its scope as the default");
  }
  {
    setenv("RE_SNAPSHOT_STORE", "off", 1);
    CHECK(!snapshot::Store::from_environment().enabled(), "RE_SNAPSHOT_STORE=off disables the store");
    setenv("RE_SNAPSHOT_STORE", "qdrant:https://example.invalid", 1);
    CHECK(snapshot::Store::from_environment().mode() == "qdrant", "qdrant:<https url> is the Qdrant store (#900)");
    setenv("RE_SNAPSHOT_STORE", "qdrant:http://qdrant.invalid:4333", 1);  // insecure-transport: negative-test
    {
      snapshot::Store st = snapshot::Store::from_environment();
      CHECK(!st.enabled() && !st.off_reason().empty(), "a plaintext Qdrant URL leaves the store off, with the reason");
    }
    setenv("RE_SNAPSHOT_STORE", "redis:x", 1);
    CHECK(!snapshot::Store::from_environment().enabled(), "an unknown store is off, never a boot failure");
    {
      // Nothing listens on port 1: the request is 503-able, not a crash.
      setenv("RE_SNAPSHOT_STORE", "qdrant:https://127.0.0.1:1", 1);
      snapshot::Store st = snapshot::Store::from_environment();
      bool unavailable = false;
      try { st.list(); } catch (const snapshot::StoreUnavailable&) { unavailable = true; }
      CHECK(unavailable, "an unreachable Qdrant is StoreUnavailable (503), not a boot failure");
    }
    fs::path dir = fs::temp_directory_path() / ("snap-" + std::to_string(getpid()));
    setenv("RE_SNAPSHOT_STORE", ("file:" + dir.string()).c_str(), 1);
    snapshot::Store st = snapshot::Store::from_environment();
    Json body = Json::Object{{"digest", "sha256:" + std::string(64, 'b')},
                             {"corpus", Json::Object{{"machines", 1.0}}},
                             {"kline", Json::Object{{"scope", "corpus"}, {"span", Json::Object{{"from", 1.0}, {"to", 1.0}}},
                                                    {"clocks", Json::Object{{"to", Json::Object{{"lamport", 5.0}}}}}}}};
    st.put(snapshot::id_of(body), body);
    CHECK(st.get(std::string(64, 'b')).has_value(), "a file store keeps a snapshot by id");
    CHECK(st.list().at("snapshots").array().size() == 1, "and lists it");
    CHECK(!st.get("../etc").has_value(), "an id is hex, never a path");
    CHECK(st.remove(std::string(64, 'b')) && !st.get(std::string(64, 'b')), "and drops it");
    fs::remove_all(dir);
    unsetenv("RE_SNAPSHOT_STORE");
  }
  {
    // Nearest (#900): the state at span.to, the sparse dot product, score > 0,
    // highest first, then id.
    auto body = [](char idc, double lamport, std::vector<std::pair<int, double>> cells) {
      Json::Array nz;
      for (auto [i, v] : cells) nz.push_back(Json::Object{{"index", static_cast<double>(i)}, {"value", v}});
      Json::Array pairs{Json::Object{{"stepNumber", 3.0}, {"isre", Json::Object{{"length", 8.0}, {"nonZero", nz}}}},
                        Json::Object{{"stepNumber", 2.0}, {"isre", Json::Object{{"length", 8.0},
                          {"nonZero", Json::Array{Json::Object{{"index", 7.0}, {"value", 9.0}}}}}}}};
      return Json(Json::Object{{"digest", "sha256:" + std::string(64, idc)}, {"corpus", Json::Object{{"machines", 1.0}}},
                               {"kline", Json::Object{{"scope", "corpus"}, {"span", Json::Object{{"from", 2.0}, {"to", 3.0}}},
                                                      {"clocks", Json::Object{{"to", Json::Object{{"lamport", lamport}}}}},
                                                      {"pairs", pairs}}}});
    };
    unsetenv("RE_SNAPSHOT_STORE");
    snapshot::Store st = snapshot::Store::from_environment();
    Json a = body('a', 3, {{1, 1.0}, {2, 1.0}}), b = body('b', 2, {{1, 1.0}, {2, 1.0}}), c = body('c', 1, {{1, 0.5}}),
         d = body('d', 4, {{5, 1.0}});
    for (const Json* x : {&a, &b, &c, &d}) st.put(snapshot::id_of(*x), *x);
    CHECK(snapshot::state_of(a).size() == 2 && !snapshot::state_of(a).count(7), "a snapshot's state is its ISRE at span.to");
    auto q = snapshot::parse_state(Json::Object{{"length", 8.0}, {"nonZero", Json::Array{
      Json::Object{{"index", 1.0}, {"value", 1.0}}, Json::Object{{"index", 2.0}, {"value", 1.0}}}}});
    CHECK(q.has_value(), "a {length, nonZero} query parses");
    CHECK(!snapshot::parse_state(Json::Object{{"nonZero", "x"}}).has_value(), "a malformed one does not");
    Json n = st.nearest(*q, 5).at("snapshots");
    CHECK(n.array().size() == 3, "only snapshots scoring above 0");
    CHECK(n.array()[0].at("id").as_string() == std::string(64, 'a') && n.array()[1].at("id").as_string() == std::string(64, 'b'),
          "equal scores ordered by id");
    CHECK(n.array()[0].at("score").as_number() == 2.0 && n.array()[2].at("score").as_number() == 0.5, "the dot product");
    CHECK(st.nearest(*q, 1).at("snapshots").array().size() == 1, "at most limit");
    Json l = st.list().at("snapshots");
    CHECK(l.array()[0].at("id").as_string() == std::string(64, 'c') && l.array()[3].at("id").as_string() == std::string(64, 'd'),
          "list: oldest first by clocks.to.lamport");
  }
  std::cout << (failures == 0 ? "snapshot: all passed\n" : "snapshot: FAILURES\n");
  return failures == 0 ? 0 : 1;
}
