// K-line capture (RealityEngine_CI#603, RealityEngine_CPP#178): the boundaries
// of SURFACE_SPEC "Boundaries", "Paths that dwell, fork or merge" and
// "Identity, clock and parity", on hand-built sequences, and the store.
#include "reality/kline.hpp"
#include "reality/reality.hpp"

#include <filesystem>
#include <iostream>
#include <set>
#include <string>
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

std::string event(const std::string& id, std::vector<int> values, bool initial,
                  std::vector<std::string> next, bool output) {
  std::string els;
  for (size_t i = 0; i < values.size(); ++i)
    els += (i ? "," : "") + std::string("{\"value\":") + std::to_string(values[i]) + ",\"threshold\":0.5}";
  std::string nx;
  for (size_t i = 0; i < next.size(); ++i) nx += (i ? "," : "") + std::string("\"") + next[i] + "\"";
  std::string out = output ? ",\"outputEvents\":[{\"id\":\"" + id + "-out\",\"vector\":[1,1]}]" : "";
  return "{\"id\":\"" + id + "\",\"elements\":[" + els + "],\"isInitial\":" + (initial ? "true" : "false") +
         (next.empty() ? "" : ",\"nextEventIds\":[" + nx + "]") + out + "}";
}

std::string machine(const std::string& name, int in, int out, const std::string& seq, const std::string& events) {
  return "{\"version\":\"1.0.0\",\"machine\":{\"name\":\"" + name + "\",\"description\":\"kline test\","
         "\"arbiterRule\":\"PASSTHROUGH\",\"matchAlgorithm\":\"gte\","
         "\"perceptualMapping\":{\"input\":{\"offset\":" + std::to_string(in) + ",\"length\":2},"
         "\"output\":{\"offset\":" + std::to_string(out) + ",\"length\":2}},"
         "\"sequences\":[{\"id\":\"" + seq + "\",\"name\":\"" + seq + "\",\"events\":[" + events + "]}]}}";
}

struct Rig {
  PerceptualSpaceRuntime rt{32};
  long long lamport = 1000;
  long long commit = 0;
  kline::Tracker tracker;
  std::vector<std::string> lines;

  explicit Rig(kline::Store store = kline::Store())
      : tracker("01a218ae-9950-76ed-aded-d4cf80228cf4", std::move(store), [this] { return ++lamport; }) {
    tracker.controls().capture = true;
    rt.onStepCommitted = [this](long step) {
      if (step < 0) { tracker.on_reset(); return; }
      ++commit;
      ++lamport;  // the step's own commit tick
      kline::StepView v;
      v.step = step;
      v.commit = commit;
      v.simulation = rt.committing_step();
      v.running = &rt.running_machines();
      v.isre = rt.last_isre();
      v.osre = rt.last_osre();
      v.fingerprint = std::string(64, 'a');
      tracker.on_step(v);
    };
  }
  void add(const std::string& json) { rt.add_machine(load_machine_from_json_string(json)); }
  void step(std::vector<std::pair<int, double>> cells) {
    Vector v(static_cast<size_t>(rt.dimension()), 0.0);
    for (auto [i, x] : cells) v[static_cast<size_t>(i)] = x;
    rt.process_immediate(v);
  }
  std::vector<Json> records() const {
    auto l = tracker.list(std::nullopt, std::nullopt, std::nullopt, 0, 1000);
    return l.at("klines").array();
  }
};

Json find(const std::vector<Json>& recs, const std::string& machine, const std::string& outcome, long openStep) {
  for (const auto& r : recs)
    if (r.at("focal").at("machine").as_string() == machine && r.at("outcome").as_string() == outcome &&
        static_cast<long>(r.at("open").at("step").as_number()) == openStep)
      return r;
  return nullptr;
}

std::vector<std::string> path_of(const Json& r) {
  std::vector<std::string> out;
  for (const auto& p : r.at("path").array()) out.push_back(p.at("event").as_string() + "@" +
                                                           std::to_string(static_cast<long>(p.at("step").as_number())));
  return out;
}

}  // namespace

int main() {
  std::cout << "jcs, sha256, fingerprint\n";
  CHECK(kline::jcs(json::parse(R"({"b":1,"a":[true,null,"x\u0001"],"c":1.5e-7})")) ==
            R"({"a":[true,null,"x\u0001"],"b":1,"c":1.5e-7})",
        "JCS orders keys and keeps the minimal escapes");
  CHECK(kline::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of abc");
  {
    auto a = kline::corpus_fingerprint({{"B", json::parse(R"({"name":"B"})")}, {"A", json::parse(R"({"name":"A"})")}});
    auto b = kline::corpus_fingerprint({{"A", json::parse(R"({"name":"A"})")}, {"B", json::parse(R"({"name":"B"})")}});
    CHECK(a == b && a.size() == 64, "the fingerprint does not depend on load order");
  }

  std::cout << "linear: completed, sustain, sustain-end, abandoned\n";
  {
    Rig r;
    r.add(machine("Linear", 0, 10, "lin",
                  event("e0", {1, 0}, true, {"e1"}, false) + "," + event("e1", {0, 1}, false, {"e2"}, false) + "," +
                      event("e2", {1, 1}, false, {}, true)));
    r.step({{0, 1}});              // 0: e0 departs
    r.step({{1, 1}});              // 1: e1
    r.step({{0, 1}, {1, 1}});      // 2: e2 completes
    r.step({{0, 1}, {1, 1}});      // 3: sustain
    r.step({});                    // 4: sustain ends
    r.step({{0, 1}});              // 5: departs again
    r.step({{0, 1}, {1, 1}});      // 6: e1 fails at element 0
    auto recs = r.records();
    Json c = find(recs, "Linear", "completed", 0);
    CHECK(c.is_object(), "a completed K-line opened at step 0");
    if (c.is_object()) {
      CHECK((path_of(c) == std::vector<std::string>{"e0@0", "e1@1", "e2@2"}), "its path is the episode");
      CHECK(static_cast<long>(c.at("close").at("step").as_number()) == 2, "it closes at the first final match");
      CHECK(c.at("sustain").at("ended").as_bool() && static_cast<long>(c.at("sustain").at("through").as_number()) == 3,
            "sustain runs through step 3 and ends");
      CHECK(c.at("pairs").array().size() == 3, "one pair per step of the span");
      CHECK(c.at("difference").is_null() && c.at("reason").is_null(), "no difference, no reason");
    }
    Json a = find(recs, "Linear", "abandoned", 5);
    CHECK(a.is_object(), "the second departure is abandoned");
    if (a.is_object()) {
      const Json& d = a.at("difference");
      CHECK(d.at("expected").array()[0].as_string() == "e1", "difference names the held event");
      CHECK(static_cast<int>(d.at("elementIndex").as_number()) == 0 && static_cast<int>(d.at("cell").as_number()) == 0,
            "element 0, cell 0 failed");
      CHECK(d.at("comparator").as_string() == "gte" && d.at("threshold").as_number() == 0.5, "under gte at 0.5");
    }
    std::set<std::string> ids;
    for (const auto& x : recs) ids.insert(x.at("id").as_string());
    CHECK(ids.size() == recs.size(), "ids are unique");
  }

  std::cout << "one-event: opens and completes on one step; consecutive matches are sustain\n";
  {
    Rig r;
    r.add(machine("One", 2, 12, "one", event("o0", {1, 1}, true, {}, true)));
    r.step({{2, 1}, {3, 1}});  // 0: open + complete
    r.step({{2, 1}, {3, 1}});  // 1: sustain
    r.step({});                // 2: ends
    r.step({{2, 1}, {3, 1}});  // 3: a new K-line
    auto recs = r.records();
    CHECK(recs.size() == 2, "two K-lines: the sustain did not open a third");
    Json c = find(recs, "One", "completed", 0);
    CHECK(c.is_object() && static_cast<long>(c.at("close").at("step").as_number()) == 0, "m = n = 0");
    if (c.is_object())
      CHECK(static_cast<long long>(c.at("close").at("lamport").as_number()) ==
                static_cast<long long>(c.at("open").at("lamport").as_number()) + 1,
            "open and close are consecutive ticks");
    CHECK(find(recs, "One", "completed", 3).is_object(), "the next opens after a step without a match");
  }

  std::cout << "dwell: one K-line holding a set of events\n";
  {
    Rig r;
    r.add(machine("Dwell", 4, 14, "dw",
                  event("d0", {1, 0}, true, {"d1"}, false) + "," + event("d1", {0, 1}, false, {"d1", "d2"}, false) +
                      "," + event("d2", {1, 1}, false, {}, true)));
    r.step({{4, 1}});           // 0: d0 departs, holds d1
    r.step({{5, 1}});           // 1: d1 dwells, holds d1 and d2
    r.step({{5, 1}});           // 2: d1 again; d2 fails and is re-activated
    r.step({{4, 1}, {5, 1}});   // 3: d2 completes
    auto recs = r.records();
    Json c = find(recs, "Dwell", "completed", 0);
    CHECK(c.is_object(), "the dwell completes as one K-line");
    if (c.is_object())
      CHECK((path_of(c) == std::vector<std::string>{"d0@0", "d1@1", "d1@2", "d2@3"}), "the dwell is recorded again");
    CHECK(recs.size() == 1, "and no other K-line");
  }

  std::cout << "merge: the younger K-line is subsumed by the older\n";
  {
    Rig r;
    r.add(machine("Merge", 6, 16, "mg",
                  event("m0", {1, 0}, true, {"m1"}, false) + "," + event("m1", {1, 0}, false, {"m2"}, false) + "," +
                      event("m2", {1, 0}, false, {}, true)));
    // A failing event is cleared before successors are activated, so a merge
    // needs the held successor to stay active: here L1's m2 matches (a final
    // event stays active) on the step L2's m1 tries to activate it.
    r.step({{6, 1}});  // 0: L1 holds m1
    r.step({{6, 1}});  // 1: L1 -> m2; L2 departs holding m1
    r.step({{6, 1}});  // 2: L1's m2 completes; L2's m1 -> m2 is refused: subsumed
    auto recs = r.records();
    Json older = find(recs, "Merge", "completed", 0);
    Json younger = find(recs, "Merge", "subsumed", 1);
    CHECK(older.is_object(), "the older completes and keeps the event");
    CHECK(younger.is_object(), "the younger is subsumed");
    if (older.is_object() && younger.is_object())
      CHECK(younger.at("subsumedBy").as_string() == older.at("id").as_string(), "subsumedBy names the older");
  }

  std::cout << "reset: open K-lines close interrupted\n";
  {
    Rig r;
    r.add(machine("Linear", 0, 10, "lin",
                  event("e0", {1, 0}, true, {"e1"}, false) + "," + event("e1", {0, 1}, false, {"e2"}, false) + "," +
                      event("e2", {1, 1}, false, {}, true)));
    r.step({{0, 1}});
    r.rt.reset();
    auto recs = r.records();
    Json i = find(recs, "Linear", "interrupted", 0);
    CHECK(i.is_object() && i.at("reason").as_string() == "reset", "closed interrupted, reason reset");
    CHECK(r.tracker.open_ids().empty(), "nothing is left open");
  }

  std::cout << "store: durable, and a restart closes the checkpoint as shutdown\n";
  {
    const fs::path dir = fs::temp_directory_path() / ("kline-test-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    {
      Rig r{kline::Store(dir)};
      r.add(machine("Linear", 0, 10, "lin",
                    event("e0", {1, 0}, true, {"e1"}, false) + "," + event("e1", {0, 1}, false, {"e2"}, false) + "," +
                        event("e2", {1, 1}, false, {}, true)));
      r.step({{0, 1}});
      r.step({{1, 1}});
      r.step({{0, 1}, {1, 1}});  // completed
      r.step({{0, 1}});          // a new one, left open
      CHECK(fs::exists(dir / "open.json"), "the open table is checkpointed");
    }
    {
      long long lamport = 5000;
      kline::Tracker t("01a218ae-9950-76ed-aded-d4cf80228cf4", kline::Store(dir), [&] { return ++lamport; });
      t.controls().capture = true;
      t.recover();
      auto recs = t.list(std::nullopt, std::nullopt, std::nullopt, 0, 1000).at("klines").array();
      CHECK(recs.size() == 2, "the completed K-line survived, and the open one was closed");
      Json s = find(recs, "Linear", "interrupted", 3);
      CHECK(s.is_object() && s.at("reason").as_string() == "shutdown", "closed interrupted, reason shutdown");
      if (s.is_object()) CHECK(s.at("feedbackTruncated").as_bool(), "without the ring it says its scope is cut short");
      std::string bundle = t.export_bundle({}, 0, {});
      auto nl = bundle.find('\n');
      Json header = json::parse(bundle.substr(0, nl));
      CHECK(header.at("format").as_string() == "re-kline-bundle/1", "an export is a re-kline-bundle/1");
      CHECK(header.at("sha256").as_string() == kline::sha256_hex(bundle.substr(nl + 1)),
            "its sha256 covers the lines after the header");
    }
    fs::remove_all(dir);
  }

  // The budget warning reaches /api/health through the store's cache, which
  // any thread can read without the engine's lock (SURFACE_SPEC "The K-line
  // store", budget): cached by a budget check, cleared once the segment is
  // exported and evicted, and naming the budget's own control.
  {
    fs::path dir = fs::temp_directory_path() / ("kline-warn-" + std::to_string(getpid()));
    fs::remove_all(dir);
    kline::Store store(dir);
    store.append({Json::Object{{"format", "re-kline/1"}, {"id", "a:1"}, {"close", Json::Object{{"commit", 1.0}}}}}, "fp-a", 1);
    store.append({Json::Object{{"format", "re-kline/1"}, {"id", "a:2"}, {"close", Json::Object{{"commit", 2.0}}}}}, "fp-b", 2);
    kline::Controls c;
    c.storeMaxBytes = 0;
    CHECK(store.cached_warnings().empty(), "no warning before a budget check");
    store.enforce_budget(c);
    auto w = store.cached_warnings();
    CHECK(w.size() == 1 && w[0].rfind("klineStoreMaxBytes reached", 0) == 0,
          "a sealed, unexported segment over budget is kept and warned about");
    std::set<std::string> all;
    for (const auto& m : store.records()) all.insert(m.segment);
    store.mark_exported(all);
    store.enforce_budget(c);
    CHECK(store.cached_warnings().empty(), "exported and evicted, the warning clears");
    kline::Controls pe = c;
    pe.budgetName = "isreStoreMaxBytes";
    kline::Store second(dir / "pe");
    second.append({Json::Object{{"format", "re-isre-composition/1"}, {"re", Json::Object{{"commit", 1.0}}}}}, "f1", 1);
    second.append({Json::Object{{"format", "re-isre-composition/1"}, {"re", Json::Object{{"commit", 2.0}}}}}, "f2", 2);
    second.enforce_budget(pe);
    CHECK(!second.cached_warnings().empty() && second.cached_warnings()[0].rfind("isreStoreMaxBytes reached", 0) == 0,
          "the composition store names its own budget");
    fs::remove_all(dir);
  }

  std::cout << (failures == 0 ? "kline: all passed\n" : "kline: FAILURES\n");
  return failures == 0 ? 0 : 1;
}
