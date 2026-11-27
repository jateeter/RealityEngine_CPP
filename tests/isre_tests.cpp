// The PE half of K-lines (RealityEngine_CI#603): the taxonomy derivation and
// the runs of SURFACE_SPEC "The PE half: ISRE composition".
#include "reality/isre.hpp"

#include <filesystem>
#include <fstream>
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

int main() {
  const fs::path dir = fs::temp_directory_path() / ("isre-test-" + std::to_string(::getpid()));
  fs::create_directories(dir);
  std::ofstream(dir / "re-source-kinds-2.json") << R"({"format":"re-source-kinds","version":2,"purpose":"t",
    "classes":["deterministic","measured","generated"],
    "providerMapping":{"openclaw":"acp","ollama":"localai","localaistack":"localai"},
    "kinds":[{"kind":"test","meaning":"t","class":"deterministic"},
             {"kind":"sensor","meaning":"s","providers":[{"provider":"healthkit","class":"measured"},
               {"provider":"signal","class":"measured"},{"provider":"acp","class":"generated"},
               {"provider":"openai","class":"generated"}]},
             {"kind":"simulated","meaning":"m","class":"measured"},
             {"kind":"feedback","meaning":"f","class":"deterministic"},{"kind":"none","meaning":"n","class":null}],
    "unknownProvider":{"provider":"unattributed","class":"generated"},"mappingFromPrevious":[]})";
  auto t = isre::Taxonomy::load(dir);
  CHECK(t.has_value() && t->name() == "re-source-kinds/2", "version 2 loads");
  CHECK(!isre::Taxonomy::load(dir, 3).has_value(), "a missing version does not");
  if (t) {
    auto e = t->source_entry("sensor", "openclaw.agent.assessment");
    CHECK(e.kind == "sensor" && e.provider == "acp" && e.cls == "generated", "openclaw.* is acp, generated");
    e = t->source_entry("sensor", "");
    CHECK(e.provider == "signal" && e.cls == "measured", "an empty origin is signal");
    e = t->source_entry("sensor", "Mystery");
    CHECK(e.provider == "unattributed" && e.cls == "generated", "an unlisted provider is unattributed, generated");
    e = t->source_entry("simulated", "");
    CHECK(e.kind == "simulated" && !e.provider && e.cls == "measured", "simulated is measured, no provider");
    e = t->source_entry("test", "");
    CHECK(e.kind == "test" && e.cls == "deterministic", "test is deterministic");

    std::vector<isre::Winner> cells{
      {10, false, "a", "test", "", nullptr}, {11, false, "a", "test", "", nullptr},
      {12, false, "b", "test", "", nullptr},
      {20, true, "", "", "", nullptr}, {21, true, "", "", "", nullptr},
      {30, false, "h", "sensor", "healthkit", Json::Object{{"instance", "x"}, {"lamport", 1.0}}},
      {31, false, "h", "sensor", "healthkit", Json::Object{{"instance", "x"}, {"lamport", 2.0}}}};
    Json runs = isre::sources_json(cells, *t);
    const auto& a = runs.array();
    CHECK(a.size() == 5, "runs split by id and by causedBy");
    CHECK(json::stringify(a[0].at("cells")) == "[10,11]" && a[0].at("id").as_string() == "a", "a run is [first, last] of one winner");
    CHECK(a[2].at("kind").as_string() == "feedback" && a[2].at("provider").is_null() && a[2].at("id").is_null(),
          "feedback has no provider and no id");
    CHECK(a[3].at("causedBy").at("lamport").as_number() == 1.0, "causedBy is carried");
  }
  fs::remove_all(dir);
  std::cout << (failures == 0 ? "isre: all passed\n" : "isre: FAILURES\n");
  return failures == 0 ? 0 : 1;
}
