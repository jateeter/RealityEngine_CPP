// CES composition / meta-CES — C++ side of step 5.
//
// Mirrors the active Scala replacement composition coverage: same
// fixture (CommunityCommandAgent.json), same producer triggers, same
// expected event-bus writes and same terminal mergeBatch entry.
//
// Validates the wire-level shape (step.eventBus list, latched bits across
// process_immediate calls, conjunction-fires-only-when-all-three) so any
// future drift between runtimes shows up on every push.

#include "reality/reality.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
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

Vector dense(int length, std::initializer_list<std::pair<int, double>> writes) {
  Vector v(static_cast<size_t>(length), 0.0);
  for (const auto& [off, val] : writes) v[static_cast<size_t>(off)] = val;
  return v;
}

// Build a single-state producer machine programmatically.  Mirrors the
// helper in CesgenComposition.test.ts so producer ids/sequence ids match
// the agent's compose subscriptions without needing fixture JSON files
// for trivial sensor stubs.
Machine producer(const std::string& id, const std::string& sequenceId, int inputBit, int outputOffset) {
  Machine m("producer-" + id, "meta-CES composition producer",
            ArbiterRule::Passthrough,
            PerceptualMapping{{inputBit, 1}, {outputOffset, 1}},
            id);
  CriticalEventSequence seq(sequenceId + " sequence", sequenceId);
  RealityVector v({VectorElement{1.0, ComparatorType::Gte, 0.5}}, true, sequenceId + "-fire");
  v.add_output_vector({sequenceId + "-out", {1.0}, {}, now_ms(), {}});
  seq.add_vector(v);
  m.add_sequence(seq);
  return m;
}

int passed = 0;
int failed = 0;
std::vector<std::string> failureMsgs;

#define EXPECT(cond, msg) do { \
  if (!(cond)) { ++failed; failureMsgs.push_back(std::string(msg) + " at " __FILE__ ":" + std::to_string(__LINE__)); } \
  else { ++passed; } \
} while (0)

void test_subscriptions_registered(const std::filesystem::path& machinesDir) {
  Machine agent = load_machine_from_json_string(
      read_file(find_machine_file(machinesDir, "CommunityCommandAgent.json")), "machine-community-command-agent");
  PerceptualSpaceSimulator sim(0);
  sim.add_machine(agent);
  EXPECT(sim.event_bus_subscription_count() == 3, "expected 3 subscriptions");
  EXPECT(sim.dimension() >= 5503, "PE should grow to cover subscription bits");
}

void test_single_producer_does_not_complete(const std::filesystem::path& machinesDir) {
  PerceptualSpaceSimulator sim(0);
  sim.add_machine(load_machine_from_json_string(
      read_file(find_machine_file(machinesDir, "CommunityCommandAgent.json")), "machine-community-command-agent"));
  sim.add_machine(producer("machine-housing-placement", "housing-place", 6002, 6012));

  auto step = sim.process_immediate(dense(7000, {{6002, 1}}));

  // Exactly one event-bus write — for bit 5502.
  EXPECT(step.eventBus.size() == 1, "expected 1 event-bus write");
  if (step.eventBus.size() == 1) {
    EXPECT(step.eventBus[0].bitOffset == 5502, "bit offset should be 5502");
    EXPECT(step.eventBus[0].producerMachineId == "machine-housing-placement", "producer machineId");
    EXPECT(step.eventBus[0].subscriberMachineId == "machine-community-command-agent", "subscriber machineId");
  }

  // The agent's referral-completion does NOT fire (only 1 of 3 milestones).
  bool referralComplete = false;
  for (const auto& op : step.mergeBatch) {
    if (op.machineId == "machine-community-command-agent"
        && op.sequenceId == "referral-completion") { referralComplete = true; break; }
  }
  EXPECT(!referralComplete, "referral-completion must not fire with only 1 milestone");
}

void test_all_three_fire_completes_workflow(const std::filesystem::path& machinesDir) {
  PerceptualSpaceSimulator sim(0);
  sim.add_machine(load_machine_from_json_string(
      read_file(find_machine_file(machinesDir, "CommunityCommandAgent.json")), "machine-community-command-agent"));
  sim.add_machine(producer("machine-benefits-eligibility", "bel-finalize",    6000, 6010));
  sim.add_machine(producer("machine-intake-triage",        "intake-finalize", 6001, 6011));
  sim.add_machine(producer("machine-housing-placement",    "housing-place",   6002, 6012));

  auto tick = [&](std::initializer_list<int> triggers) {
    Vector v(7000, 0.0);
    for (int o : triggers) v[static_cast<size_t>(o)] = 1.0;
    return sim.process_immediate(v);
  };

  // Step 1: fire benefits.  eventBus = [5500].
  auto step = tick({6000});
  std::vector<int> bits1;
  for (const auto& w : step.eventBus) bits1.push_back(w.bitOffset);
  EXPECT(bits1 == std::vector<int>{5500}, "step 1 eventBus = [5500]");

  // Step 2: fire intake.  eventBus = [5501]; bit 5500 still latched on PE.
  step = tick({6001});
  std::vector<int> bits2;
  for (const auto& w : step.eventBus) bits2.push_back(w.bitOffset);
  EXPECT(bits2 == std::vector<int>{5501}, "step 2 eventBus = [5501]");

  // Step 3: fire housing.  eventBus = [5502].
  step = tick({6002});
  std::vector<int> bits3;
  for (const auto& w : step.eventBus) bits3.push_back(w.bitOffset);
  EXPECT(bits3 == std::vector<int>{5502}, "step 3 eventBus = [5502]");

  // Step 4: idle — meta snapshot reads [1,1,1] and emits REFERRAL_COMPLETE.
  step = tick({});
  bool found = false;
  for (const auto& op : step.mergeBatch) {
    if (op.machineId == "machine-community-command-agent"
        && op.sequenceId == "referral-completion") {
      found = true;
      EXPECT((op.values == Vector{1.0, 0.0, 0.0, 0.0}), "REFERRAL_COMPLETE values");
      EXPECT(op.region.offset == 5503 && op.region.length == 4, "agent output region");
      EXPECT(op.provenance.size() == 1 && op.provenance[0] == "ccr-all-milestones-reached",
             "agent provenance chain");
      break;
    }
  }
  EXPECT(found, "REFERRAL_COMPLETE mergeBatch entry");
}

void test_event_bus_sort_order(const std::filesystem::path& machinesDir) {
  PerceptualSpaceSimulator sim(0);
  sim.add_machine(load_machine_from_json_string(
      read_file(find_machine_file(machinesDir, "CommunityCommandAgent.json")), "machine-community-command-agent"));
  // Add producers in reverse offset order to confirm the sort isn't a
  // side effect of add order.
  sim.add_machine(producer("machine-housing-placement",    "housing-place",   6002, 6012));
  sim.add_machine(producer("machine-benefits-eligibility", "bel-finalize",    6000, 6010));
  sim.add_machine(producer("machine-intake-triage",        "intake-finalize", 6001, 6011));

  Vector v(7000, 0.0);
  v[6000] = v[6001] = v[6002] = 1.0;
  auto step = sim.process_immediate(v);

  std::vector<int> bits;
  for (const auto& w : step.eventBus) bits.push_back(w.bitOffset);
  EXPECT(bits == (std::vector<int>{5500, 5501, 5502}), "eventBus sorted by (subscriber, bitOffset)");
}

} // namespace

int main(int argc, char** argv) {
  std::filesystem::path machinesDir = argc > 1 ? argv[1] : "../RealityEngine_Machines/machines";

  if (!std::filesystem::exists(find_machine_file(machinesDir, "CommunityCommandAgent.json"))) {
    std::cerr << "Skipping cesgen_composition — CommunityCommandAgent.json not found in "
              << machinesDir << "\n";
    return 0;
  }

  test_subscriptions_registered(machinesDir);
  test_single_producer_does_not_complete(machinesDir);
  test_all_three_fire_completes_workflow(machinesDir);
  test_event_bus_sort_order(machinesDir);

  std::cout << "cesgen_composition summary\n"
            << "  passed: " << passed << "\n"
            << "  failed: " << failed << "\n";
  if (failed > 0) {
    std::cerr << "FAILURES:\n";
    for (const auto& m : failureMsgs) std::cerr << "  " << m << "\n";
    std::cerr << "cesgen_composition FAILED\n";
    return 1;
  }
  std::cout << "cesgen_composition passed\n";
  return 0;
}
