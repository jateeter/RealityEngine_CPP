// CES governance — C++ parity for the paging contract.
//
// Mirrors src/__tests__/CesGovernance.test.ts in the AI repo:
//   - rule-with-override picks the tier-1 team + 30s SLA for fall-confirmed RED
//   - rule-only falls back to the machine's sla.warning for the AMBER tiers
//   - machines without governance metadata resolve to "unrouted"
//   - the engine stamps the resolved decision onto every mergeBatch entry
//     whose values match a triggerConfig rule

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

void test_rule_with_override(const std::filesystem::path& machinesDir) {
  Machine m = load_machine_from_json_string(read_file(machinesDir / "FallDetection.json"), "gov-fd");
  auto decision = resolve_governance(m, "fall-confirmed", {4.0, 3.0});
  EXPECT(decision.has_value(), "fall-confirmed [4,3] should resolve to a decision");
  if (decision) {
    EXPECT(decision->ragStatusCode == "RED",              "ragStatusCode RED");
    EXPECT(decision->processStatus == "error",            "processStatus error");
    EXPECT(decision->ownerTeam     == "patient-safety-on-call-tier-1", "ownerTeam tier-1");
    EXPECT(decision->slaSeconds.has_value() && *decision->slaSeconds == 30, "slaSeconds 30");
    EXPECT(decision->runbook       == "https://runbooks.example.org/patient-safety/fall-detection#confirmed-red", "runbook deep link");
    EXPECT(decision->escalationPolicy == "pagerduty:patient-safety-tier-1", "escalationPolicy inherits machine default");
    EXPECT(decision->source        == "rule-with-override", "source = rule-with-override");
    EXPECT(decision->hasMachineGovernance, "hasMachineGovernance true");
  }
}

void test_partial_override(const std::filesystem::path& machinesDir) {
  Machine m = load_machine_from_json_string(read_file(machinesDir / "FallDetection.json"), "gov-fd-2");
  auto decision = resolve_governance(m, "fall-slow-collapse", {4.0, 2.0});
  EXPECT(decision.has_value(), "fall-slow-collapse [4,2] should resolve");
  if (decision) {
    EXPECT(decision->slaSeconds.has_value() && *decision->slaSeconds == 45, "slaSeconds 45");
    EXPECT(decision->runbook == "https://runbooks.example.org/patient-safety/fall-detection#slow-collapse", "deep-link runbook");
    // ownerTeam isn't overridden in the rule — should come from machine.
    EXPECT(decision->ownerTeam == "patient-safety-on-call", "machine-level ownerTeam preserved");
  }
}

void test_machine_default_sla_by_status(const std::filesystem::path& machinesDir) {
  Machine m = load_machine_from_json_string(read_file(machinesDir / "FallDetection.json"), "gov-fd-3");
  auto decision = resolve_governance(m, "fall-sustained-instability", {2.0, 2.0});
  EXPECT(decision.has_value(), "fall-sustained-instability should resolve");
  if (decision) {
    EXPECT(decision->processStatus == "warning", "processStatus warning");
    EXPECT(decision->slaSeconds.has_value() && *decision->slaSeconds == 1800, "slaSeconds 1800 from machine sla.warning");
    EXPECT(decision->source == "rule-only", "source = rule-only");
  }
}

void test_no_rule_matches(const std::filesystem::path& machinesDir) {
  Machine m = load_machine_from_json_string(read_file(machinesDir / "FallDetection.json"), "gov-fd-4");
  auto decision = resolve_governance(m, "fall-confirmed", {9.0, 9.0});
  EXPECT(!decision.has_value(), "unmatched values should resolve to nullopt");
  auto decision2 = resolve_governance(m, "no-such-sequence", {0.0});
  EXPECT(!decision2.has_value(), "unknown sequenceId should resolve to nullopt");
}

void test_no_governance_returns_unrouted(const std::filesystem::path& /*machinesDir*/) {
  // The corpus is now fully backfilled with governance metadata.  To exercise
  // the "rules but no governance" fallback we construct the legacy shape
  // inline — same approach as src/__tests__/CesGovernance.test.ts in the AI repo.
  const std::string raw = R"({
    "version": "1.0.0",
    "machine": {
      "id": "machine-legacy-rules-only",
      "name": "Legacy rules only",
      "arbiterRule": "PASSTHROUGH",
      "perceptualMapping": { "input": { "offset": 0, "length": 1 }, "output": { "offset": 1, "length": 1 } },
      "metadata": {
        "triggerConfig": {
          "rules": [{
            "sequenceId": "legacy-seq", "outputMatches": [1],
            "ragStatusCode": "GREEN", "processStatus": "info", "description": "probe"
          }]
        }
      },
      "sequences": [{
        "id": "legacy-seq", "name": "Legacy",
        "vectors": [{
          "id": "legacy-v", "isInitial": true,
          "elements": [{ "value": 1, "threshold": 0.5 }],
          "outputVectors": [{ "id": "legacy-out", "vector": [1], "metadata": {} }]
        }]
      }]
    }
  })";
  Machine m = load_machine_from_json_string(raw, "machine-legacy-rules-only");
  auto decision = resolve_governance(m, "legacy-seq", {1.0});
  EXPECT(decision.has_value(), "rule should resolve even without machine governance");
  if (decision) {
    EXPECT(!decision->hasMachineGovernance, "hasMachineGovernance must be false");
    EXPECT(decision->ownerTeam == "unrouted", "ownerTeam falls back to 'unrouted'");
    EXPECT(decision->source == "machine-fallback", "source = machine-fallback");
  }
}

void test_mergebatch_stamp(const std::filesystem::path& machinesDir) {
  // Drive the FallDetection nominal path and confirm the mergeBatch entry
  // carries the resolved governance decision (GREEN / ok / patient-safety-on-call).
  Machine m = load_machine_from_json_string(read_file(machinesDir / "FallDetection.json"), "gov-fd-stamp");
  PerceptualSpaceSimulator sim(0);
  sim.add_machine(m);
  int off = m.perceptualMapping->input.offset;
  Vector v(static_cast<size_t>(off) + 2, 0.0);
  auto step = sim.process_immediate(v);    // input [0,0] → fall-nominal fires

  const MergeOperation* nominal = nullptr;
  for (const auto& op : step.mergeBatch) {
    if (op.sequenceId == "fall-nominal") { nominal = &op; break; }
  }
  EXPECT(nominal != nullptr, "fall-nominal mergeBatch entry");
  if (nominal) {
    EXPECT(nominal->governance.has_value(), "governance stamped on mergeBatch");
    if (nominal->governance) {
      EXPECT(nominal->governance->ragStatusCode == "GREEN", "stamped ragStatusCode GREEN");
      EXPECT(nominal->governance->processStatus == "ok",    "stamped processStatus ok");
      EXPECT(!nominal->governance->slaSeconds.has_value(),  "ok severity → no SLA");
      EXPECT(nominal->governance->ownerTeam == "patient-safety-on-call", "machine-level ownerTeam");
    }
  }
}

void test_paging_decisions_metric(const std::filesystem::path& machinesDir) {
  Machine m = load_machine_from_json_string(read_file(machinesDir / "FallDetection.json"), "gov-fd-prom");
  std::map<std::string, Machine> machines{{m.id, m}};
  PerceptualSpaceSimulator sim(0);
  sim.add_machine(m);
  int off = m.perceptualMapping->input.offset;
  Vector v(static_cast<size_t>(off) + 2, 0.0);
  sim.process_immediate(v);    // bumps the GREEN/ok decision

  std::string prom = sim.ces_coverage().to_prometheus_text(machines);
  EXPECT(prom.find("# TYPE ces_paging_decisions_total counter") != std::string::npos,
         "TYPE line present");
  EXPECT(prom.find("owner_team=\"patient-safety-on-call\"") != std::string::npos,
         "owner_team label rendered");
  EXPECT(prom.find("rag_status_code=\"GREEN\"") != std::string::npos,
         "rag_status_code label rendered");
}

} // namespace

int main(int argc, char** argv) {
  std::filesystem::path machinesDir = argc > 1 ? argv[1] : "../RealityEngine_AI/examples/machines";

  if (!std::filesystem::exists(machinesDir / "FallDetection.json")) {
    std::cerr << "Skipping cesgen_governance — FallDetection.json not found in " << machinesDir << "\n";
    return 0;
  }

  test_rule_with_override(machinesDir);
  test_partial_override(machinesDir);
  test_machine_default_sla_by_status(machinesDir);
  test_no_rule_matches(machinesDir);
  test_no_governance_returns_unrouted(machinesDir);
  test_mergebatch_stamp(machinesDir);
  test_paging_decisions_metric(machinesDir);

  std::cout << "cesgen_governance summary\n"
            << "  passed: " << passed << "\n"
            << "  failed: " << failed << "\n";
  if (failed > 0) {
    std::cerr << "FAILURES:\n";
    for (const auto& m : failureMsgs) std::cerr << "  " << m << "\n";
    std::cerr << "cesgen_governance FAILED\n";
    return 1;
  }
  std::cout << "cesgen_governance passed\n";
  return 0;
}
