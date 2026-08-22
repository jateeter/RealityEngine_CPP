// CES versioning + deprecation — C++ parity tests.
//
// Mirrors the active Scala replacement deprecation coverage:
//   - the loader propagates schemaVersion / deprecatedAt / replacedBy
//   - the engine stamps mergeBatch with a DeprecationMark when a
//     deprecated sequence fires
//   - ces_deprecated_fires_total renders with the right label set
//
// Fixture: examples/machines/RSFlipFlopDeprecatedDemo.json — same machine
// the AI test exercises.  The reset sequence is marked deprecated; the
// set sequence is not.

#include "reality/reality.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace reality;

namespace {

std::string read_file(const std::filesystem::path& p) {
  std::ifstream in(p);
  if (!in) throw std::runtime_error("cannot read " + p.string());
  std::stringstream ss; ss << in.rdbuf();
  return ss.str();
}

int passed = 0;
int failed = 0;
std::vector<std::string> failureMsgs;

#define EXPECT(cond, msg) do { \
  if (!(cond)) { ++failed; failureMsgs.push_back(std::string(msg) + " at " __FILE__ ":" + std::to_string(__LINE__)); } \
  else { ++passed; } \
} while (0)

void test_loader_propagates_lifecycle(const std::filesystem::path& machinesDir) {
  Machine m = load_machine_from_json_string(read_file(find_machine_file(machinesDir, "RSFlipFlopDeprecatedDemo.json")), "dep-loader");
  bool sawReset = false, sawSet = false;
  for (const auto& seq : m.all_sequences()) {
    if (seq.id == "rs-reset-sequence") {
      sawReset = true;
      EXPECT(seq.schemaVersion == "1.0.0",       "rs-reset-sequence schemaVersion");
      EXPECT(seq.deprecatedAt  == "2026-02-01",  "rs-reset-sequence deprecatedAt");
      EXPECT(seq.replacedBy    == "rs-set-sequence", "rs-reset-sequence replacedBy");
      EXPECT(seq.is_deprecated(),                "rs-reset-sequence is_deprecated");
      EXPECT(seq.days_since_deprecation() > 0,   "rs-reset-sequence ageDays > 0");
    }
    if (seq.id == "rs-set-sequence") {
      sawSet = true;
      EXPECT(seq.schemaVersion == "1.0.0",       "rs-set-sequence schemaVersion");
      EXPECT(!seq.is_deprecated(),               "rs-set-sequence NOT deprecated");
    }
  }
  EXPECT(sawReset, "rs-reset-sequence was scanned");
  EXPECT(sawSet,   "rs-set-sequence was scanned");
}

void test_engine_stamps_deprecation(const std::filesystem::path& machinesDir) {
  Machine m = load_machine_from_json_string(read_file(find_machine_file(machinesDir, "RSFlipFlopDeprecatedDemo.json")), "dep-stamp");
  PerceptualSpaceSimulator sim(0);
  sim.add_machine(m);
  int off = m.perceptualMapping->input.offset;
  Vector v(static_cast<size_t>(off) + 2, 0.0);
  v[static_cast<size_t>(off)] = 0; v[static_cast<size_t>(off) + 1] = 1;   // RESET
  auto step = sim.process_immediate(v);

  const MergeOperation* fired = nullptr;
  for (const auto& op : step.mergeBatch) {
    if (contributed(op, "rs-reset-sequence")) { fired = &op; break; }
  }
  EXPECT(fired != nullptr, "rs-reset-sequence mergeBatch entry");
  if (fired) {
    EXPECT(fired->deprecation.has_value(), "deprecation stamp present");
    if (fired->deprecation) {
      EXPECT(fired->deprecation->since      == "2026-02-01",      "deprecation.since");
      EXPECT(fired->deprecation->replacedBy == "rs-set-sequence", "deprecation.replacedBy");
      EXPECT(fired->deprecation->ageDays > 0,                     "deprecation.ageDays > 0");
    }
  }
}

void test_non_deprecated_has_no_stamp(const std::filesystem::path& machinesDir) {
  Machine m = load_machine_from_json_string(read_file(find_machine_file(machinesDir, "RSFlipFlopDeprecatedDemo.json")), "dep-set");
  PerceptualSpaceSimulator sim(0);
  sim.add_machine(m);
  int off = m.perceptualMapping->input.offset;
  Vector v(static_cast<size_t>(off) + 2, 0.0);
  v[static_cast<size_t>(off)] = 1; v[static_cast<size_t>(off) + 1] = 0;   // SET (not deprecated)
  auto step = sim.process_immediate(v);

  const MergeOperation* setOp = nullptr;
  for (const auto& op : step.mergeBatch) {
    if (contributed(op, "rs-set-sequence")) { setOp = &op; break; }
  }
  EXPECT(setOp != nullptr, "rs-set-sequence mergeBatch entry");
  if (setOp) {
    EXPECT(!setOp->deprecation.has_value(), "rs-set-sequence has no deprecation stamp");
  }
}

void test_prom_emits_deprecated_fires(const std::filesystem::path& machinesDir) {
  Machine m = load_machine_from_json_string(read_file(find_machine_file(machinesDir, "RSFlipFlopDeprecatedDemo.json")), "dep-prom");
  std::map<std::string, Machine> machines{{m.id, m}};
  PerceptualSpaceSimulator sim(0);
  sim.add_machine(m);
  int off = m.perceptualMapping->input.offset;
  Vector v(static_cast<size_t>(off) + 2, 0.0);
  v[static_cast<size_t>(off) + 1] = 1;   // RESET
  sim.process_immediate(v);
  sim.process_immediate(v);

  std::string prom = sim.ces_coverage().to_prometheus_text(machines);
  EXPECT(prom.find("# TYPE ces_deprecated_fires_total counter") != std::string::npos,
         "TYPE line present");
  EXPECT(prom.find("sequence=\"rs-reset-sequence\"") != std::string::npos,
         "sequence label rendered");
  EXPECT(prom.find("replaced_by=\"rs-set-sequence\"") != std::string::npos,
         "replaced_by label rendered");
  EXPECT(prom.find("} 2") != std::string::npos, "counter at value 2");
}

} // namespace

int main(int argc, char** argv) {
  std::filesystem::path machinesDir = argc > 1 ? argv[1] : "../RealityEngine_Machines/machines";

  if (!std::filesystem::exists(find_machine_file(machinesDir, "RSFlipFlopDeprecatedDemo.json"))) {
    std::cerr << "Skipping cesgen_deprecation — RSFlipFlopDeprecatedDemo.json not found in " << machinesDir << "\n";
    return 0;
  }

  test_loader_propagates_lifecycle(machinesDir);
  test_engine_stamps_deprecation(machinesDir);
  test_non_deprecated_has_no_stamp(machinesDir);
  test_prom_emits_deprecated_fires(machinesDir);

  std::cout << "cesgen_deprecation summary\n"
            << "  passed: " << passed << "\n"
            << "  failed: " << failed << "\n";
  if (failed > 0) {
    std::cerr << "FAILURES:\n";
    for (const auto& m : failureMsgs) std::cerr << "  " << m << "\n";
    std::cerr << "cesgen_deprecation FAILED\n";
    return 1;
  }
  std::cout << "cesgen_deprecation passed\n";
  return 0;
}
