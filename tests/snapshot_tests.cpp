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
  Rig() {
    rt.add_machine(load_machine_from_json_string(kMachine));
    rt.onStepCommitted = [this](long step) {
      if (step < 0) { retention.on_reset(rt.running_machines()); return; }
      retention.on_step(step, ++lamport, rt.last_isre(), rt.last_osre(), *rt.committing_step(), rt.running_machines(), 250);
    };
  }
  void step(double a, double b) {
    Vector v(static_cast<size_t>(rt.dimension()), 0.0);
    v[0] = a; v[1] = b;
    rt.process_immediate(v);
  }
  Json snap(std::optional<long> from = std::nullopt, std::optional<long> to = std::nullopt) {
    snapshot::CaptureRequest cr;
    cr.from = from; cr.to = to;
    cr.instance = "01a2aaaa-0000-7000-8000-0000000f4001";
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
    setenv("RE_SNAPSHOT_STORE", "off", 1);
    CHECK(!snapshot::Store::from_environment().enabled(), "RE_SNAPSHOT_STORE=off disables the store");
    setenv("RE_SNAPSHOT_STORE", "qdrant:https://example.invalid", 1);
    CHECK(!snapshot::Store::from_environment().enabled(), "qdrant (phase 3) is off, never a boot failure");
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
  std::cout << (failures == 0 ? "snapshot: all passed\n" : "snapshot: FAILURES\n");
  return failures == 0 ? 0 : 1;
}
