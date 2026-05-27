// CES provenance trail — C++ side of the life-safety audit-loop test.
//
// Pairs with the Scala replacement/runtime provenance tests: drives
// the same multi-step chains through the C++ PerceptualSpaceSimulator and
// asserts every emitted mergeBatch entry carries the canonical chain of
// vector IDs.  Same machines, same inputs, same expected provenance.
//
// Headline case: FallDetection fall-confirmed.  Six ticks of input drive
// fall-conf-v1 → v2 → v3 → v4 → v5 → v6, and the output region [4, 3]
// must carry that exact 6-vector chain in its provenance field.

#include "reality/reality.hpp"

#include <algorithm>
#include <chrono>
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

Vector dense(int offset, std::vector<double> values) {
  Vector v(static_cast<size_t>(offset) + values.size(), 0.0);
  for (size_t i = 0; i < values.size(); ++i) v[static_cast<size_t>(offset) + i] = values[i];
  return v;
}

int passed = 0;
int failed = 0;
std::vector<std::string> failureLines;

#define EXPECT(cond, msg) do { \
  if (!(cond)) { ++failed; failureLines.push_back(std::string(msg) + " at " __FILE__ ":" + std::to_string(__LINE__)); } \
  else { ++passed; } \
} while (0)

bool chain_equals(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) return false;
  return true;
}

std::string chain_str(const std::vector<std::string>& c) {
  std::string s = "[";
  for (size_t i = 0; i < c.size(); ++i) { if (i) s += ", "; s += c[i]; }
  s += "]";
  return s;
}

void test_fall_confirmed_red(const std::filesystem::path& machinesDir) {
  Machine m = load_machine_from_json_string(read_file(machinesDir / "FallDetection.json"), "prov-fall-red");
  PerceptualSpaceSimulator sim(0);
  sim.add_machine(m);
  int off = m.perceptualMapping->input.offset;

  std::vector<std::vector<double>> chain = {{1,0},{2,0},{3,0},{3,1},{3,2},{3,3}};
  SimulationStep last;
  for (const auto& step : chain) last = sim.process_immediate(dense(off, step));

  bool found = false;
  for (const auto& op : last.mergeBatch) {
    if (op.sequenceId == "fall-confirmed"
        && op.values.size() == 2 && op.values[0] == 4 && op.values[1] == 3) {
      found = true;
      std::vector<std::string> expected{"fall-conf-v1","fall-conf-v2","fall-conf-v3","fall-conf-v4","fall-conf-v5","fall-conf-v6"};
      EXPECT(chain_equals(op.provenance, expected),
             "fall-confirmed provenance mismatch: got " + chain_str(op.provenance));
      break;
    }
  }
  EXPECT(found, "no mergeBatch entry for fall-confirmed RED [4,3]");
}

void test_fall_slow_collapse(const std::filesystem::path& machinesDir) {
  Machine m = load_machine_from_json_string(read_file(machinesDir / "FallDetection.json"), "prov-fall-slow");
  PerceptualSpaceSimulator sim(0);
  sim.add_machine(m);
  int off = m.perceptualMapping->input.offset;

  std::vector<std::vector<double>> chain = {{0,1},{0,2},{0,3}};
  SimulationStep last;
  for (const auto& step : chain) last = sim.process_immediate(dense(off, step));

  bool found = false;
  for (const auto& op : last.mergeBatch) {
    if (op.sequenceId == "fall-slow-collapse"
        && op.values.size() == 2 && op.values[0] == 4 && op.values[1] == 2) {
      found = true;
      std::vector<std::string> expected{"fall-slow-v1","fall-slow-v2","fall-slow-v3"};
      EXPECT(chain_equals(op.provenance, expected),
             "fall-slow-collapse provenance mismatch: got " + chain_str(op.provenance));
      break;
    }
  }
  EXPECT(found, "no mergeBatch entry for fall-slow-collapse RED [4,2]");
}

void test_rsflipflop_initial(const std::filesystem::path& machinesDir) {
  Machine m = load_machine_from_json_string(read_file(machinesDir / "RSFlipFlop.json"), "prov-rsff-set");
  PerceptualSpaceSimulator sim(0);
  sim.add_machine(m);
  int off = m.perceptualMapping->input.offset;

  auto step = sim.process_immediate(dense(off, {1, 0}));
  bool found = false;
  for (const auto& op : step.mergeBatch) {
    if (op.sequenceId == "rs-set-sequence") {
      found = true;
      EXPECT((op.values == Vector{1.0, 0.0}), "rs-set values mismatch");
      EXPECT(op.provenance.size() == 1 && op.provenance[0] == "rs-event-10",
             "rs-set provenance should be [rs-event-10], got " + chain_str(op.provenance));
      break;
    }
  }
  EXPECT(found, "no mergeBatch entry for rs-set-sequence");
}

void test_provenance_non_empty_property(const std::filesystem::path& machinesDir) {
  // Property: every emitted mergeBatch entry must carry at least its emitter
  // in the provenance chain.  Same property tested in the AI Jest suite.
  Machine m = load_machine_from_json_string(read_file(machinesDir / "RSFlipFlop.json"), "prov-rsff-prop");
  PerceptualSpaceSimulator sim(0);
  sim.add_machine(m);
  int off = m.perceptualMapping->input.offset;

  auto step = sim.process_immediate(dense(off, {0, 1}));
  EXPECT(!step.mergeBatch.empty(), "expected at least one mergeBatch entry");
  for (const auto& op : step.mergeBatch) {
    EXPECT(!op.provenance.empty(),
           "mergeBatch entry for " + op.machineId + "/" + op.sequenceId + " has empty provenance");
  }
}

} // namespace

int main(int argc, char** argv) {
  std::filesystem::path machinesDir = argc > 1 ? argv[1] : "../RealityEngine_Machines/machines";

  if (!std::filesystem::exists(machinesDir / "FallDetection.json")) {
    std::cerr << "Skipping cesgen_provenance — FallDetection.json not found in " << machinesDir << "\n";
    return 0;
  }

  test_fall_confirmed_red(machinesDir);
  test_fall_slow_collapse(machinesDir);
  test_rsflipflop_initial(machinesDir);
  test_provenance_non_empty_property(machinesDir);

  std::cout << "cesgen_provenance summary\n"
            << "  passed: " << passed << "\n"
            << "  failed: " << failed << "\n";
  if (failed > 0) {
    std::cerr << "FAILURES:\n";
    for (const auto& l : failureLines) std::cerr << "  " << l << "\n";
    std::cerr << "cesgen_provenance FAILED\n";
    return 1;
  }
  std::cout << "cesgen_provenance passed\n";
  return 0;
}
