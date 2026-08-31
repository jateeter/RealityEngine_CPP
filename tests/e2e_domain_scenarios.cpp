// Domain-aware end-to-end tests for the C++ runtime.
//
// Mirrors the active Scala replacement domain E2E coverage: same machine JSONs,
// same triggers, same expected outputs.  These tests exist so a regression in
// either runtime is caught against the identical scenario in the other.
//
// Coverage:
//   * One representative machine per domain prefix (AGX, BSX, CSX, DCX, HSPH,
//     LBL, LSX, TFX) — each fires a known input and the mergeBatch carries the
//     expected output value at the expected region.
//   * DLX001 rising-edge detector — requires two steps; only step 2 produces
//     a mergeBatch entry.
//   * Dynamic dimension growth — adding machines from widely-separated regions
//     bumps the spaceRuntime dimension and the mapping_version counter.
//   * Cross-domain step — three machines in three domains all fire in a single
//     processImmediate() call and produce three mergeBatch entries sorted by
//     machineId (one merge operation per machine after the fold move).
//   * Determinism — running the same scenario twice produces a byte-identical
//     mergeBatch (sequence ordering rule holds across runs).

#include "reality/reality.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace reality;

namespace {

// Resolved at main() from argv[1] or the default "../RealityEngine_Machines/machines"
// so the test works whether it's run from the repo root, from bin/, or invoked
// directly by the Makefile target.
std::filesystem::path AI_MACHINES;

struct DomainCase {
  std::string prefix;
  std::string file;
  std::vector<double> trigger;
  std::vector<double> output;
};

const std::vector<DomainCase> DOMAIN_CASES = {
  {"AGX",  "AGX001_aquaculture-water-quality-stability.json",                            {0, 1, 0, 1}, {0, 1, 0, 0}},
  {"BSX",  "BSX001_integrative-planning-stakeholder-charrette-tracker.json",             {0, 1, 0, 1}, {0, 1, 0, 0}},
  {"CSX",  "CSX001_health-and-human-services-intake-resident-intake-triage.json",         {0, 1, 1, 0}, {0, 1, 0, 0}},
  {"DCX",  "DCX001_power-utility-feed-monitor.json",                                     {1, 0, 0, 1}, {0, 1, 0, 0}},
  {"HSPH", "HSPH001_evaluability-readiness-signal-monitor.json",                          {0, 1, 0, 1}, {0, 1, 0, 0}},
  {"LBL",  "LBL001_whole-person-intake-and-goals-psychiatric-history-intake.json",        {0, 1, 1, 0}, {0, 1, 0, 0}},
  {"LSX",  "LSX001_provisional-patent-filing-invention-intake.json",                      {1, 1, 0, 0}, {0, 1, 0, 0}},
  {"TFX",  "TFX001_rider-experience-stop-crowding-monitor.json",                          {0, 1, 1, 0}, {0, 1, 0, 0}},
};

std::string read_file(const std::filesystem::path& p) {
  std::ifstream in(p);
  if (!in) throw std::runtime_error("cannot read " + p.string());
  std::stringstream ss; ss << in.rdbuf();
  return ss.str();
}

Machine load(const std::string& filename, const std::string& id) {
  return load_machine_from_json_string(read_file(find_machine_file(AI_MACHINES, filename)), id);
}

// Build a dense vector containing `values` placed at `offset`, with zero fill
// elsewhere, sized to fit the highest write.
Vector dense_vector(const std::vector<std::pair<int, std::vector<double>>>& writes) {
  int max = 0;
  for (const auto& [off, vs] : writes) max = std::max(max, off + static_cast<int>(vs.size()));
  Vector v(static_cast<size_t>(max), 0.0);
  for (const auto& [off, vs] : writes) {
    for (size_t i = 0; i < vs.size(); ++i) v[static_cast<size_t>(off) + i] = vs[i];
  }
  return v;
}

int failures = 0;
int passed   = 0;

#define EXPECT(cond, msg) do { \
  if (!(cond)) { ++failures; std::cerr << "FAIL " << msg << " at " << __FILE__ << ":" << __LINE__ << "\n"; } \
  else { ++passed; } \
} while (0)

void test_single_domain_trigger(const DomainCase& dc) {
  Machine m = load(dc.file, "e2e-" + dc.prefix);
  PerceptualSpaceRuntime sim(0);
  sim.add_machine(m);

  const auto& in  = m.perceptualMapping->input;
  const auto& out = m.perceptualMapping->output;
  Vector v = dense_vector({{in.offset, dc.trigger}});

  SimulationStep step = sim.process_immediate(v);

  EXPECT(step.mergeBatch.size() == 1, dc.prefix + ": expected exactly 1 mergeBatch entry, got " + std::to_string(step.mergeBatch.size()));
  if (step.mergeBatch.size() == 1) {
    const auto& op = step.mergeBatch[0];
    EXPECT(op.machineId == m.id, dc.prefix + ": mergeBatch machineId mismatch");
    EXPECT(op.region.offset == out.offset && op.region.length == out.length, dc.prefix + ": mergeBatch region mismatch");
    EXPECT(op.values == dc.output, dc.prefix + ": mergeBatch values mismatch");

    // The debug perceptualSpace projection must agree with the mergeBatch.
    for (size_t i = 0; i < dc.output.size(); ++i) {
      EXPECT(step.perceptualSpace[static_cast<size_t>(out.offset) + i] == dc.output[i],
             dc.prefix + ": perceptualSpace projection mismatch at index " + std::to_string(i));
    }
  }
}

void test_dlx_rising_edge() {
  Machine m = load("DLX001_rising-edge-detector.json", "dlx001-e2e");
  PerceptualSpaceRuntime sim(0);
  sim.add_machine(m);

  const auto& in  = m.perceptualMapping->input;
  const auto& out = m.perceptualMapping->output;

  SimulationStep s1 = sim.process_immediate(dense_vector({{in.offset, {0, 0, 0, 0}}}));
  EXPECT(s1.mergeBatch.empty(), "DLX001: step 1 should produce no mergeBatch entries");

  SimulationStep s2 = sim.process_immediate(dense_vector({{in.offset, {1, 0, 0, 0}}}));
  EXPECT(s2.mergeBatch.size() == 1, "DLX001: step 2 should produce exactly 1 mergeBatch entry");
  if (s2.mergeBatch.size() == 1) {
    EXPECT(s2.mergeBatch[0].region.offset == out.offset, "DLX001: region offset mismatch");
    EXPECT((s2.mergeBatch[0].values == Vector{1.0, 0.0}), "DLX001: values mismatch");
  }
}

void test_dimension_growth() {
  PerceptualSpaceRuntime sim(0);
  EXPECT(sim.dimension() == 0, "fresh sim should have dimension 0");
  EXPECT(sim.mapping_version() == 0, "fresh sim should have mapping_version 0");
  EXPECT(sim.required_dimension() == 0, "fresh sim should have required_dimension 0");

  Machine m1 = load("AGX001_aquaculture-water-quality-stability.json", "grow-agx");
  Machine m2 = load("CSX001_health-and-human-services-intake-resident-intake-triage.json", "grow-csx");
  Machine m3 = load("DCX001_power-utility-feed-monitor.json", "grow-dcx");

  sim.add_machine(m1);
  EXPECT(sim.mapping_version() == 1, "mapping_version after 1st add");
  sim.add_machine(m2);
  EXPECT(sim.mapping_version() == 2, "mapping_version after 2nd add");
  sim.add_machine(m3);
  EXPECT(sim.mapping_version() == 3, "mapping_version after 3rd add");

  int expected = 0;
  for (const auto* m : {&m1, &m2, &m3}) {
    expected = std::max(expected, m->perceptualMapping->input.offset  + m->perceptualMapping->input.length);
    expected = std::max(expected, m->perceptualMapping->output.offset + m->perceptualMapping->output.length);
  }
  EXPECT(sim.required_dimension() == expected, "required_dimension == max(offset+length)");
  EXPECT(sim.dimension() >= expected, "dimension covers required_dimension");
}

void test_three_domains_in_one_step() {
  std::vector<DomainCase> picked = {
    *std::find_if(DOMAIN_CASES.begin(), DOMAIN_CASES.end(), [](const DomainCase& c){ return c.prefix == "AGX"; }),
    *std::find_if(DOMAIN_CASES.begin(), DOMAIN_CASES.end(), [](const DomainCase& c){ return c.prefix == "HSPH"; }),
    *std::find_if(DOMAIN_CASES.begin(), DOMAIN_CASES.end(), [](const DomainCase& c){ return c.prefix == "TFX"; }),
  };

  PerceptualSpaceRuntime sim(0);
  std::vector<Machine> ms;
  for (size_t i = 0; i < picked.size(); ++i) {
    ms.push_back(load(picked[i].file, "cross-" + picked[i].prefix + "-" + std::to_string(i)));
    sim.add_machine(ms.back());
  }

  std::vector<std::pair<int, std::vector<double>>> writes;
  for (size_t i = 0; i < ms.size(); ++i) writes.push_back({ms[i].perceptualMapping->input.offset, picked[i].trigger});

  SimulationStep step = sim.process_immediate(dense_vector(writes));

  EXPECT(step.mergeBatch.size() == 3, "three-domain step should produce 3 mergeBatch entries, got " + std::to_string(step.mergeBatch.size()));

  // mergeBatch sorted by machineId, which is a total order now that the batch
  // carries one operation per machine (FOLD_PLACEMENT.md 6). Strictly less
  // rather than less-or-equal: a repeated machineId would mean the fold failed
  // to collapse a machine's contributions to one.
  for (size_t i = 1; i < step.mergeBatch.size(); ++i) {
    EXPECT(step.mergeBatch[i - 1].machineId < step.mergeBatch[i].machineId,
           "three-domain mergeBatch not sorted at index " + std::to_string(i));
  }
}

void test_determinism() {
  Machine m = load("AGX001_aquaculture-water-quality-stability.json", "det-agx");
  Vector v = dense_vector({{m.perceptualMapping->input.offset, {0, 1, 0, 1}}});

  PerceptualSpaceRuntime s1(0); s1.add_machine(m);
  PerceptualSpaceRuntime s2(0); s2.add_machine(m);

  SimulationStep a = s1.process_immediate(v);
  SimulationStep b = s2.process_immediate(v);

  EXPECT(a.mergeBatch.size() == b.mergeBatch.size(), "determinism: mergeBatch size mismatch");
  for (size_t i = 0; i < a.mergeBatch.size() && i < b.mergeBatch.size(); ++i) {
    EXPECT(a.mergeBatch[i].machineId  == b.mergeBatch[i].machineId,  "determinism: machineId mismatch at " + std::to_string(i));
    EXPECT(a.mergeBatch[i].sequenceIds == b.mergeBatch[i].sequenceIds, "determinism: sequenceIds mismatch at " + std::to_string(i));
    EXPECT(a.mergeBatch[i].region.offset == b.mergeBatch[i].region.offset, "determinism: region offset mismatch");
    EXPECT(a.mergeBatch[i].values == b.mergeBatch[i].values, "determinism: values mismatch");
  }
}

void test_polymorphic_assembly() {
  // The AI test asserts dense vs narrow-grown equivalence — verify the same
  // semantics hold in C++: a dense vector whose length is smaller than the
  // spaceRuntime's dimension still produces the right mergeBatch because the
  // tolerant set_vector zero-fills the tail.
  Machine m = load("AGX001_aquaculture-water-quality-stability.json", "poly-agx");

  PerceptualSpaceRuntime s1(0); s1.add_machine(m);
  PerceptualSpaceRuntime s2(0); s2.add_machine(m);

  const int inOff = m.perceptualMapping->input.offset;

  Vector wide(static_cast<size_t>(s1.dimension()), 0.0);
  for (int i = 0; i < 4; ++i) wide[static_cast<size_t>(inOff) + i] = std::vector<double>{0, 1, 0, 1}[i];

  Vector narrow(static_cast<size_t>(inOff + 4), 0.0);
  for (int i = 0; i < 4; ++i) narrow[static_cast<size_t>(inOff) + i] = std::vector<double>{0, 1, 0, 1}[i];

  SimulationStep a = s1.process_immediate(wide);
  SimulationStep b = s2.process_immediate(narrow);

  EXPECT(a.mergeBatch.size() == b.mergeBatch.size(), "polymorphic: mergeBatch size mismatch");
  if (a.mergeBatch.size() == 1 && b.mergeBatch.size() == 1) {
    EXPECT(a.mergeBatch[0].values == b.mergeBatch[0].values, "polymorphic: values mismatch");
    EXPECT((a.mergeBatch[0].values == Vector{0.0, 1.0, 0.0, 0.0}), "polymorphic: unexpected output");
  }
}

} // namespace

int main(int argc, char** argv) {
  // Allow override via argv[1]; otherwise look in the sibling machine corpus repo.
  AI_MACHINES = argc > 1 ? argv[1] : "../RealityEngine_Machines/machines";
  if (!std::filesystem::exists(AI_MACHINES)) {
    // Try the sibling layout from a deeper CWD (e.g. bin/).
    auto alt = std::filesystem::path("..") / AI_MACHINES;
    if (std::filesystem::exists(alt)) AI_MACHINES = alt;
  }
  if (!std::filesystem::exists(AI_MACHINES)) {
    std::cerr << "Skipping e2e_domain_scenarios — corpus not found at " << AI_MACHINES << "\n";
    return 0;
  }

  for (const auto& dc : DOMAIN_CASES) test_single_domain_trigger(dc);
  test_dlx_rising_edge();
  test_dimension_growth();
  test_three_domains_in_one_step();
  test_determinism();
  test_polymorphic_assembly();

  std::cout << "E2E domain scenarios summary\n"
            << "  expectations passed:  " << passed   << "\n"
            << "  expectations failed:  " << failures << "\n";
  if (failures > 0) { std::cerr << "RealityEngine_CPP domain scenario tests FAILED\n"; return 1; }
  std::cout << "RealityEngine_CPP domain scenario tests passed\n";
  return 0;
}
