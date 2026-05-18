// End-to-end Yuma → AI cascade — the multi-machine flow that single-machine
// tests can't see.
//
// What this exercises
// -------------------
// Live MQTT from yuma.lateraledge.cloud lands at perceptualSpace[40:44) /
// [84:88) / [184:188) / [228:232).  In the production pipeline this drives:
//
//   step N      AGX051 (Aqua Maintenance)           → [256:260) [1,0,0,0]   URGENT_MAINT
//               AGX052 (DO Probe Reliability)       → [260:264) [0,0,0,1]   NORMAL
//               AGX053 (VPD HVAC Service)           → [264:268) [0,0,0,1]   NORMAL
//               AGX054 (CO2 Safety Compliance)      → [268:272) [0,0,0,1]   NORMAL
//   step N+1    AGX055 (Facility AI Synthesis)      → [3959:3971) AQUA_URGENT one-hot
//   step N+2    AgYieldOptimizationAI reads [3959:3971) — bridge contract
//
// Each fired (sequenceId, output) MUST resolve to a localAIStack trigger
// envelope (PagingDecision present, dispatchableAgent populated).  The two
// envelopes that come out the bottom of this chain share a correlationId
// in the production dispatcher — see examples/triggers/ai_trigger.scenario_aqua_urgent_chain.json.
//
// Companion to:
//   examples/triggers/ai_trigger.agx051_urgent_maint.example.json
//   examples/triggers/ai_trigger.agx055_aqua_urgent.example.json
//   examples/triggers/ai_trigger.scenario_aqua_urgent_chain.json
//   tests/e2e_ai_trigger_dispatch.cpp                    (single-machine envelope tests)

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

std::filesystem::path AI_MACHINES;

int passed = 0;
int failed = 0;
std::vector<std::string> failureMsgs;

#define EXPECT(cond, msg) do { \
  if (!(cond)) { ++failed; failureMsgs.push_back(std::string(msg) + " at " __FILE__ ":" + std::to_string(__LINE__)); } \
  else { ++passed; } \
} while (0)

std::string read_file(const std::filesystem::path& p) {
  std::ifstream in(p);
  if (!in) throw std::runtime_error("cannot read " + p.string());
  std::stringstream ss; ss << in.rdbuf();
  return ss.str();
}

Machine load(const std::string& filename, const std::string& id) {
  return load_machine_from_json_string(read_file(AI_MACHINES / filename), id);
}

// Build a dense input vector by overlaying small write blocks onto a
// zero-filled vector sized to fit the highest write.
Vector dense_writes(const std::vector<std::pair<int, std::vector<double>>>& writes) {
  int max = 0;
  for (const auto& [off, vs] : writes) max = std::max(max, off + static_cast<int>(vs.size()));
  Vector v(static_cast<size_t>(max), 0.0);
  for (const auto& [off, vs] : writes) {
    for (size_t i = 0; i < vs.size(); ++i) v[static_cast<size_t>(off) + i] = vs[i];
  }
  return v;
}

// Locate a mergeBatch entry for a given machine; returns nullptr when no
// such entry was produced this step.
const MergeOperation* find_merge(const SimulationStep& step, const std::string& machineId) {
  for (const auto& op : step.mergeBatch) if (op.machineId == machineId) return &op;
  return nullptr;
}

std::string vector_to_string(const Vector& v) {
  std::ostringstream out; out << "[";
  for (size_t i = 0; i < v.size(); ++i) { if (i) out << ","; out << v[i]; }
  out << "]"; return out.str();
}

// Zero a slice of the input vector — used to keep sensor cells quiescent
// across ticks where we don't want the upstream machine to re-fire.
void zero_region(Vector& v, int offset, int length) {
  for (int i = offset; i < offset + length; ++i) v[static_cast<size_t>(i)] = 0.0;
}

// The four AGX051-054 share identical input patterns (lifted from the
// inputSequences[] declared in each machine's JSON).  Centralised here so
// the cascade test won't drift if the generator script regenerates them.
const std::vector<double> TIER1_NORMAL_INPUT      = {1, 1, 0, 1};         // single tick → NORMAL output [0,0,0,1]
const std::vector<std::vector<double>> TIER1_URGENT_INPUT_TICKS = {       // 3-tick escalation → URGENT output [1,0,0,0]
  {1, 1, 1, 1},
  {1, 0, 1, 0},
  {0, 0, 0, 0},
};

void test_full_cascade() {
  Machine agx051 = load("AGX051_yuma-aqua-maintenance-forecaster.json",      "casc-agx051");
  Machine agx052 = load("AGX052_yuma-do-probe-reliability-tracker.json",     "casc-agx052");
  Machine agx053 = load("AGX053_yuma-vpd-hvac-service-planner.json",         "casc-agx053");
  Machine agx054 = load("AGX054_yuma-co2-safety-compliance-officer.json",    "casc-agx054");
  Machine agx055 = load("AGX055_yuma-facility-ai-synthesis-bridge.json",     "casc-agx055");
  Machine yieldAI = load("AgYieldOptimizationAI.json",                       "casc-agyield");

  PerceptualSpaceSimulator sim(0);
  sim.add_machine(agx051);
  sim.add_machine(agx052);
  sim.add_machine(agx053);
  sim.add_machine(agx054);
  sim.add_machine(agx055);
  sim.add_machine(yieldAI);

  // ── Stage 1: drive the 3-tick AGX051 escalation while AGX052/053/054
  //             stream NORMAL each tick ────────────────────────────────────
  //
  // Each AGX needs *its own* input pattern: AGX051 walks through
  // stable → watch → fail to reach URGENT_MAINT (3 ticks), while
  // AGX052/053/054 receive their single-tick NORMAL pattern on every tick.
  // After tick 3, perceptualSpace[256:272) is the AGX055 AQUA_URGENT input
  // pattern [1,0,0,0, 0,0,0,1, 0,0,0,1, 0,0,0,1] — directly lifted from
  // AGX055.inputSequences[0].vectors[0].
  std::optional<MergeOperation> m051_final;
  for (size_t tick = 0; tick < TIER1_URGENT_INPUT_TICKS.size(); ++tick) {
    Vector v = dense_writes({
      {40,  TIER1_URGENT_INPUT_TICKS[tick]},
      {84,  TIER1_NORMAL_INPUT},
      {184, TIER1_NORMAL_INPUT},
      {228, TIER1_NORMAL_INPUT},
    });
    SimulationStep s = sim.process_immediate(v);
    if (auto* m = find_merge(s, agx051.id)) m051_final = *m;

    // AGX055 must not fire during stage 1 — the URGENT bit isn't in
    // perceptualSpace[256] until AGX051's terminal tick merges, and the
    // simulator takes snapshots BEFORE applying merges.  Catches a
    // regression where the snapshot/merge ordering inverts.
    EXPECT(find_merge(s, agx055.id) == nullptr,
           "stage 1 tick " + std::to_string(tick) + ": AGX055 fired before bridge inputs propagated");
  }

  EXPECT(m051_final.has_value(),                                   "stage 1: AGX051 never fired URGENT_MAINT across 3-tick escalation");
  if (m051_final) {
    EXPECT(m051_final->region.offset == 256 && m051_final->region.length == 4,
           "AGX051 region != [256:260)");
    EXPECT(m051_final->values == Vector({1, 0, 0, 0}),             "AGX051 final output != URGENT_MAINT one-hot — got " + vector_to_string(m051_final->values));
    EXPECT(m051_final->sequenceId == "agx-051-urgent-maint",       "AGX051 fired wrong sequenceId — got " + m051_final->sequenceId);
    EXPECT(m051_final->governance.has_value(),                     "AGX051 mergeBatch has no governance (envelope would be dropped)");
    if (m051_final->governance) {
      EXPECT(m051_final->governance->ragStatusCode == "RED",       "AGX051 governance.rag != RED");
      EXPECT(m051_final->governance->ownerTeam == "agriculture-operations",
                                                                   "AGX051 governance.ownerTeam != agriculture-operations");
      EXPECT(m051_final->governance->slaSeconds.value_or(0) == 900,"AGX051 governance.sla != 900s (error tier)");
    }
  }

  // Inter-stage contract: perceptualSpace[256:272) must now hold the
  // AGX055 AQUA_URGENT input pattern verbatim.
  EXPECT(sim.perceptual_space().vector().size() >= 272,            "perceptualSpace not grown to 272 after stage 1");
  if (sim.perceptual_space().vector().size() >= 272) {
    const auto& ps = sim.perceptual_space().vector();
    EXPECT(ps[256] == 1.0 && ps[257] == 0.0 && ps[258] == 0.0 && ps[259] == 0.0, "perceptualSpace[256:260) != URGENT_MAINT");
    EXPECT(ps[260] == 0.0 && ps[261] == 0.0 && ps[262] == 0.0 && ps[263] == 1.0, "perceptualSpace[260:264) != NORMAL");
    EXPECT(ps[264] == 0.0 && ps[265] == 0.0 && ps[266] == 0.0 && ps[267] == 1.0, "perceptualSpace[264:268) != NORMAL");
    EXPECT(ps[268] == 0.0 && ps[269] == 0.0 && ps[270] == 0.0 && ps[271] == 1.0, "perceptualSpace[268:272) != NORMAL");
  }

  // ── Stage 2: AGX055 reads the merged tier-1 outputs and fires ───────────
  //
  // Carry-forward pattern: snapshot perceptualSpace, zero the tier-1 sensor
  // input cells so AGX051-054 don't re-trigger, and feed it back through.
  // [256:272) remains exactly as stage 1 left it — that is AGX055's input.
  Vector stage2 = sim.perceptual_space().vector();
  zero_region(stage2, 40, 4);
  zero_region(stage2, 84, 4);
  zero_region(stage2, 184, 4);
  zero_region(stage2, 228, 4);
  SimulationStep s2 = sim.process_immediate(stage2);

  const MergeOperation* m055 = find_merge(s2, agx055.id);
  EXPECT(m055 != nullptr,                                          "stage 2: AGX055 did not fire — bridge contract broken");
  if (m055) {
    EXPECT(m055->region.offset == 3959 && m055->region.length == 12, "AGX055 region != [3959:3971) — projection contract broken");
    EXPECT(m055->sequenceId == "agx-055-aqua-urgent",              "AGX055 sequenceId != agx-055-aqua-urgent — wrong CES fired");
    EXPECT(m055->values == Vector({1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}),
           "AGX055 output != AQUA_URGENT one-hot — got " + vector_to_string(m055->values));
    EXPECT(m055->governance.has_value(),                           "AGX055 mergeBatch has no governance");
    if (m055->governance) {
      EXPECT(m055->governance->ragStatusCode == "RED",             "AGX055 governance.rag != RED");
      EXPECT(m055->governance->ownerTeam == "agriculture-operations","AGX055 governance.ownerTeam != agriculture-operations");
      EXPECT(m055->governance->slaSeconds.value_or(0) == 600,      "AGX055 governance.sla != 600s (error tier)");
    }
  }

  // AgYieldOptimizationAI must not have fired yet — its snapshot was taken
  // before AGX055's merge landed.
  EXPECT(find_merge(s2, yieldAI.id) == nullptr,
         "stage 2: AgYieldOptimizationAI fired before AGX055 projection landed");

  // ── Stage 3: AgYieldOptimizationAI now sees the bridge bit pattern ──────
  //
  // We do not assert what CES the AI fires (depends on its internal regex
  // state and isn't load-bearing for the envelope contract).  We DO assert
  // the projection landed: perceptualSpace[3959] carries the bit AGX055
  // wrote, which is exactly the bit AgYieldOptimizationAI's elem_0 reads.
  Vector stage3 = sim.perceptual_space().vector();
  zero_region(stage3, 40, 4);
  zero_region(stage3, 84, 4);
  zero_region(stage3, 184, 4);
  zero_region(stage3, 228, 4);
  zero_region(stage3, 256, 16);  // suppress AGX055 re-fire — the projection has been booked

  EXPECT(stage3.size() >= 3971,                                    "perceptualSpace not grown to 3971 after stage 2");
  if (stage3.size() >= 3971) {
    EXPECT(stage3[3959] == 1.0,                                    "stage 3: AQUA_URGENT bit at [3959] missing — AgYieldOptimizationAI will not see the cascade");
    for (int i = 3960; i < 3971; ++i) {
      EXPECT(stage3[static_cast<size_t>(i)] == 0.0,                "stage 3: stray bit at [" + std::to_string(i) + "] — one-hot projection violated");
    }
  }

  SimulationStep s3 = sim.process_immediate(stage3);
  (void)s3;  // Projection landing is the contract; AI's fire-or-not is not.
}

// Stable-path counter-test: when AGX051 fires NORMAL (not URGENT) and the
// other three are also NORMAL, AGX055 must fire FACILITY_STABLE — a GREEN
// envelope, NOT a paging event.  Catches a regression where the bridge
// incorrectly fans every change into the URGENT slot.
void test_facility_stable_path() {
  Machine agx051 = load("AGX051_yuma-aqua-maintenance-forecaster.json",   "stable-agx051");
  Machine agx052 = load("AGX052_yuma-do-probe-reliability-tracker.json",  "stable-agx052");
  Machine agx053 = load("AGX053_yuma-vpd-hvac-service-planner.json",      "stable-agx053");
  Machine agx054 = load("AGX054_yuma-co2-safety-compliance-officer.json", "stable-agx054");
  Machine agx055 = load("AGX055_yuma-facility-ai-synthesis-bridge.json",  "stable-agx055");

  PerceptualSpaceSimulator sim(0);
  sim.add_machine(agx051);
  sim.add_machine(agx052);
  sim.add_machine(agx053);
  sim.add_machine(agx054);
  sim.add_machine(agx055);

  Vector step1 = dense_writes({
    {40,  TIER1_NORMAL_INPUT},   // AGX051: NORMAL
    {84,  TIER1_NORMAL_INPUT},   // AGX052: NORMAL
    {184, TIER1_NORMAL_INPUT},   // AGX053: NORMAL
    {228, TIER1_NORMAL_INPUT},   // AGX054: NORMAL
  });
  SimulationStep s1 = sim.process_immediate(step1);
  const MergeOperation* m051 = find_merge(s1, agx051.id);
  EXPECT(m051 != nullptr && (m051 ? m051->values : Vector{}) == Vector({0, 0, 0, 1}),
         "stable path step N: AGX051 != NORMAL — got " +
         (m051 ? vector_to_string(m051->values) : std::string("<no merge>")));

  Vector step2 = sim.perceptual_space().vector();
  zero_region(step2, 40, 4);
  zero_region(step2, 84, 4);
  zero_region(step2, 184, 4);
  zero_region(step2, 228, 4);
  SimulationStep s2 = sim.process_immediate(step2);

  const MergeOperation* m055 = find_merge(s2, agx055.id);
  EXPECT(m055 != nullptr,                                          "stable path step N+1: AGX055 did not fire on FACILITY_STABLE input");
  if (m055) {
    EXPECT(m055->sequenceId == "agx-055-facility-stable",          "stable path: AGX055 sequenceId != agx-055-facility-stable");
    EXPECT(m055->values == Vector({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}),
           "stable path: AGX055 output != FACILITY_STABLE one-hot — got " + vector_to_string(m055->values));
    EXPECT(m055->governance.has_value() && m055->governance->ragStatusCode == "GREEN",
           "stable path: FACILITY_STABLE should resolve to GREEN, not paging");
  }
}

// Determinism guard — the cascade must produce a byte-identical mergeBatch
// across two runs of the same scenario.  Same expectation enforced for
// single-domain steps in e2e_domain_scenarios.cpp::test_determinism.
void test_cascade_determinism() {
  auto run = []() {
    Machine agx051 = load("AGX051_yuma-aqua-maintenance-forecaster.json",      "det-agx051");
    Machine agx052 = load("AGX052_yuma-do-probe-reliability-tracker.json",     "det-agx052");
    Machine agx053 = load("AGX053_yuma-vpd-hvac-service-planner.json",         "det-agx053");
    Machine agx054 = load("AGX054_yuma-co2-safety-compliance-officer.json",    "det-agx054");
    Machine agx055 = load("AGX055_yuma-facility-ai-synthesis-bridge.json",     "det-agx055");
    PerceptualSpaceSimulator sim(0);
    sim.add_machine(agx051); sim.add_machine(agx052); sim.add_machine(agx053);
    sim.add_machine(agx054); sim.add_machine(agx055);
    std::vector<SimulationStep> steps;
    for (const auto& tick : TIER1_URGENT_INPUT_TICKS) {
      Vector v = dense_writes({{40, tick}, {84, TIER1_NORMAL_INPUT}, {184, TIER1_NORMAL_INPUT}, {228, TIER1_NORMAL_INPUT}});
      steps.push_back(sim.process_immediate(v));
    }
    Vector bridgeTick = sim.perceptual_space().vector();
    zero_region(bridgeTick, 40, 4);  zero_region(bridgeTick, 84, 4);
    zero_region(bridgeTick, 184, 4); zero_region(bridgeTick, 228, 4);
    steps.push_back(sim.process_immediate(bridgeTick));
    return steps;
  };
  auto a = run();
  auto b = run();
  EXPECT(a.size() == b.size(), "determinism: run length mismatch");
  for (size_t t = 0; t < a.size() && t < b.size(); ++t) {
    EXPECT(a[t].mergeBatch.size() == b[t].mergeBatch.size(),
           "determinism: stage " + std::to_string(t) + " mergeBatch size mismatch");
    for (size_t i = 0; i < a[t].mergeBatch.size() && i < b[t].mergeBatch.size(); ++i) {
      EXPECT(a[t].mergeBatch[i].machineId  == b[t].mergeBatch[i].machineId,
             "determinism: stage " + std::to_string(t) + " machineId differs at "  + std::to_string(i));
      EXPECT(a[t].mergeBatch[i].sequenceId == b[t].mergeBatch[i].sequenceId,
             "determinism: stage " + std::to_string(t) + " sequenceId differs at " + std::to_string(i));
      EXPECT(a[t].mergeBatch[i].values     == b[t].mergeBatch[i].values,
             "determinism: stage " + std::to_string(t) + " values differ at "      + std::to_string(i));
    }
  }
}

} // namespace

int main(int argc, char** argv) {
  AI_MACHINES = argc > 1 ? argv[1] : "../RealityEngine_AI/examples/machines";
  if (!std::filesystem::exists(AI_MACHINES)) {
    auto alt = std::filesystem::path("..") / AI_MACHINES;
    if (std::filesystem::exists(alt)) AI_MACHINES = alt;
  }
  if (!std::filesystem::exists(AI_MACHINES)) {
    std::cerr << "Skipping e2e_yuma_localai_cascade — corpus not found at " << AI_MACHINES << "\n";
    return 0;
  }

  test_full_cascade();
  test_facility_stable_path();
  test_cascade_determinism();

  std::cout << "E2E Yuma → localAIStack cascade summary\n"
            << "  expectations passed:  " << passed << "\n"
            << "  expectations failed:  " << failed << "\n";
  if (failed > 0) {
    std::cerr << "\nFailures:\n";
    for (const auto& m : failureMsgs) std::cerr << "  - " << m << "\n";
    std::cerr << "RealityEngine_CPP Yuma cascade tests FAILED\n";
    return 1;
  }
  std::cout << "RealityEngine_CPP Yuma cascade tests passed\n";
  return 0;
}
