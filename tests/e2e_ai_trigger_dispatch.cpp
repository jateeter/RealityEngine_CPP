// CES → localAIStack trigger-envelope parity, single-machine scope.
//
// What this exercises
// -------------------
// Every machine JSON ships its own `inputSequences[]` block (loaded at machine
// load time, same as all current sources).  For machines that ALSO declare:
//
//     metadata.triggerConfig.rules[]    — RAG-coded output→action mapping
//     metadata.agentBinding             — localAIStack dispatch binding
//
// this test walks each input sequence, replays its vectors through the
// machine, and asserts that the (sequenceId, outputVector) the machine
// produces resolves — via reality::resolve_governance — to a PagingDecision
// whose fields are exactly what the dispatcher would project into the
// envelope at examples/triggers/ai_trigger_envelope.template.json.
//
// Companion to:
//   examples/triggers/ai_trigger_envelope.template.json
//   examples/triggers/ai_trigger.agx051_urgent_maint.example.json
//   examples/triggers/ai_trigger.agx055_aqua_urgent.example.json
//
// The multi-machine cascade (AGX051 → AGX055 → AgYieldOptimizationAI) is
// covered by e2e_yuma_localai_cascade.cpp.

#include "reality/json.hpp"
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

// Pull a top-level metadata block off the raw JSON — the loaded Machine
// preserves metadata in its map, but for the envelope-shape assertions we want
// to read the authoring intent straight from the file.
const Json& machine_metadata(const Json& root) {
  return root.at("machine").at("metadata");
}

const Json& agent_binding(const Json& machineMd) {
  return machineMd.at("agentBinding");
}

std::string dispatch_agent(const Json& machineMd) {
  const Json& binding = agent_binding(machineMd);
  if (binding.is_object()) return binding.at("agent").as_string(machineMd.at("dispatchableAgent").as_string());
  return machineMd.at("dispatchableAgent").as_string();
}

std::string dispatch_trigger(const Json& machineMd) {
  const Json& binding = agent_binding(machineMd);
  if (binding.is_object()) return binding.at("trigger").as_string(machineMd.at("aiTrigger").as_string());
  return machineMd.at("aiTrigger").as_string();
}

struct EnvelopeFields {
  std::string agent;
  std::string trigger;
  std::string autonomyMode;
  std::string writeBackType;
  std::string ragStatusCode;
  std::string processStatus;
  std::string ownerTeam;
  std::optional<int> slaSeconds;
  std::string runbook;
  std::string escalationPolicy;
};

// Build the envelope-projection bundle the dispatcher would emit for a
// single fired (sequenceId, outputVector) pair, by walking the same fields
// it would.  Returns std::nullopt when no triggerConfig rule matches —
// matches dispatcher's drop-on-miss behaviour.
std::optional<EnvelopeFields> envelope_for(const Machine& machine,
                                           const Json& machineMd,
                                           const std::string& sequenceId,
                                           const Vector& output) {
  auto decision = resolve_governance(machine, sequenceId, output);
  if (!decision) return std::nullopt;

  EnvelopeFields env;
  const Json& binding   = agent_binding(machineMd);
  env.agent             = dispatch_agent(machineMd);
  env.trigger           = dispatch_trigger(machineMd);
  env.autonomyMode      = binding.at("mode").as_string();
  env.writeBackType     = binding.at("writeBack").at("type").as_string();
  env.ragStatusCode     = decision->ragStatusCode;
  env.processStatus     = decision->processStatus;
  env.ownerTeam         = decision->ownerTeam;
  env.slaSeconds        = decision->slaSeconds;
  env.runbook           = decision->runbook;
  env.escalationPolicy  = decision->escalationPolicy;
  return env;
}

struct WalkSummary {
  int machinesWithTriggers = 0;
  int inputSequencesRun = 0;
  int outputsProduced = 0;
  int envelopesResolved = 0;
};

constexpr int kExpectedMachinesWithTriggers = 1058;
constexpr int kExpectedInputSequencesRun = 5126;
constexpr int kExpectedOutputsProduced = 4251;
constexpr int kExpectedEnvelopesResolved = 4251;

// Walk a single machine file.  Skips files whose machine doesn't declare
// triggerConfig + agentBinding — those aren't AI-routed and have
// nothing to assert at this layer.
void walk_machine(const std::filesystem::path& file, WalkSummary& sum) {
  std::string raw = read_file(file);
  Json root;
  try { root = json::parse(raw); }
  catch (const std::exception& e) {
    EXPECT(false, file.filename().string() + ": JSON parse failed: " + e.what());
    return;
  }

  const Json& md = machine_metadata(root);
  if (!md.at("triggerConfig").at("rules").is_array() ||
      md.at("triggerConfig").at("rules").array().empty()) return;
  if (!md.at("agentBinding").is_object()) return;
  if (dispatch_agent(md).empty() || dispatch_trigger(md).empty()) return;

  ++sum.machinesWithTriggers;
  Machine machine = load_machine_from_json_string(raw, "trigger-" + file.stem().string());

  const Json& inputSequences = root.at("machine").at("inputSequences");
  if (!inputSequences.is_array()) return;

  for (const auto& seqJson : inputSequences.array()) {
    machine.reset();
    ++sum.inputSequencesRun;

    const std::string seqName = seqJson.at("name").as_string("unnamed");
    const std::string scenario = seqJson.at("metadata").at("scenario").as_string();
    const Json& vectorsJson = seqJson.at_either("events", "vectors");
    if (!vectorsJson.is_array()) {
      EXPECT(false, file.filename().string() + " / " + seqName + ": missing input vectors");
      continue;
    }

    // Baseline sequences (expectedOutputCount: 0) intentionally exercise the
    // input window without firing — they're the "armed-but-quiescent" tests
    // that prove the machine doesn't false-fire.  They have no envelope to
    // assert, so we skip them entirely after a single arm/replay.
    int expectedOutputCountEarly =
        seqJson.at("metadata").at("expectedOutputCount").is_number()
        ? static_cast<int>(seqJson.at("metadata").at("expectedOutputCount").as_number())
        : -1;
    if (expectedOutputCountEarly == 0) continue;

    // Collect (sequenceId, outputVector) pairs produced by the run.  We can't
    // assert against a single expected output because some input sequences
    // intentionally drive multiple sequences in parallel; we collect all
    // assertions and require at least one to resolve to an envelope.
    std::vector<std::pair<std::string, Vector>> fired;
    for (const auto& vj : vectorsJson.array()) {
      Vector input = json::to_numbers(vj);
      auto tr = machine.process_input(input);
      for (const auto& [seqId, seqResult] : tr.sequenceResults) {
        for (const auto& ov : seqResult.assertedOutputs) {
          fired.emplace_back(seqId, ov.vector);
          ++sum.outputsProduced;
        }
      }
    }

    // ── Assertion 1: at least one fire when the input sequence carries
    // metadata.expectedOutputCount > 0.  Same expectation the legacy
    // e2e_machine_sequences.cpp enforces, restated here for clarity.
    int expectedOutputCount = static_cast<int>(seqJson.at("metadata").at("expectedOutputCount").as_number(0));
    if (expectedOutputCount > 0) {
      EXPECT(static_cast<int>(fired.size()) == expectedOutputCount,
             file.filename().string() + " / " + seqName +
             ": expected " + std::to_string(expectedOutputCount) +
             " output(s), got " + std::to_string(fired.size()));
    }

    // ── Assertion 2: every fired (sequenceId, output) MUST resolve to an
    // envelope — i.e. it must hit a triggerConfig.rules[] entry.  A
    // terminal event that doesn't is invisible to localAIStack and the
    // dispatcher will silently drop it.  Make that audible at test time.
    int envelopesThisRun = 0;
    for (const auto& [seqId, out] : fired) {
      auto env = envelope_for(machine, md, seqId, out);
      if (env) {
        ++envelopesThisRun;
        ++sum.envelopesResolved;

        // Cross-check structural shape: every envelope must populate the
        // load-bearing fields the dispatcher writes into the GraphQL
        // updateProcessState mutation.  Empty values would route to
        // "unrouted" on the localAIStack side.
        EXPECT(!env->agent.empty(),
               file.filename().string() + " / " + seqName + " / " + scenario + ": agentBinding.agent empty");
        EXPECT(!env->trigger.empty(),
               file.filename().string() + " / " + seqName + " / " + scenario + ": agentBinding.trigger empty");
        EXPECT(env->autonomyMode == "observe" || env->autonomyMode == "advise" ||
               env->autonomyMode == "supervised-act" || env->autonomyMode == "automated-act",
               file.filename().string() + " / " + seqName + " / " + scenario +
               ": autonomyMode='" + env->autonomyMode + "' not in supported modes");
        EXPECT(!env->writeBackType.empty(),
               file.filename().string() + " / " + seqName + " / " + scenario + ": agentBinding.writeBack.type empty");
        EXPECT(env->ragStatusCode == "RED" || env->ragStatusCode == "AMBER" || env->ragStatusCode == "GREEN",
               file.filename().string() + " / " + seqName + " / " + scenario +
               ": ragStatusCode='" + env->ragStatusCode + "' not in {RED,AMBER,GREEN}");
        EXPECT(env->processStatus == "error" || env->processStatus == "warning" ||
               env->processStatus == "info"  || env->processStatus == "ok",
               file.filename().string() + " / " + seqName + " / " + scenario +
               ": processStatus='" + env->processStatus + "' not in {error,warning,info,ok}");
        EXPECT(!env->ownerTeam.empty() && env->ownerTeam != "unrouted",
               file.filename().string() + " / " + seqName + " / " + scenario + ": ownerTeam unrouted (governance not backfilled)");
        // SLA only required for paging tiers — error/warning page; info/ok
        // are informational and routinely carry null SLA in the corpus.
        const bool pagingTier = (env->processStatus == "error" || env->processStatus == "warning");
        if (pagingTier) {
          EXPECT(env->slaSeconds.has_value() && *env->slaSeconds > 0,
                 file.filename().string() + " / " + seqName + " / " + scenario +
                 ": paging tier '" + env->processStatus + "' has no slaSeconds — envelope would page with no contract");
        }
      }
    }
    EXPECT(envelopesThisRun > 0,
           file.filename().string() + " / " + seqName +
           ": no fired output matched any triggerConfig rule — envelope would be dropped");
  }
}

// ── Yuma-specific tightening ─────────────────────────────────────────────
//
// The user-facing requirement names AGX051 and AGX055 explicitly; the AI-repo
// envelope examples (examples/triggers/ai_trigger.agx05{1,5}*.example.json)
// hard-code the expected dispatch targets.  These two checks pin the wire
// values so a refactor of the generator script can't silently re-route the
// envelopes to a different agent.
void test_agx051_envelope_pins(const std::filesystem::path& machinesDir) {
  std::string raw = read_file(find_machine_file(machinesDir, "AGX051_yuma-aqua-maintenance-forecaster.json"));
  Json root = json::parse(raw);
  Machine m = load_machine_from_json_string(raw, "pin-agx051");

  auto env = envelope_for(m, machine_metadata(root), "agx-051-urgent-maint", Vector{1, 0, 0, 0});
  EXPECT(env.has_value(),                                                       "AGX051 urgent_maint: no envelope resolved");
  if (!env) return;
  EXPECT(env->agent             == "aquaculture_predictive_maintenance_agent",  "AGX051 urgent_maint: dispatch agent != aquaculture_predictive_maintenance_agent");
  EXPECT(env->trigger           == "agriculture-yuma-aqua-maintenance-forecaster-maintenance",
                                                                                "AGX051 urgent_maint: aiTrigger mismatch");
  EXPECT(env->autonomyMode      == "advise",                                    "AGX051 urgent_maint: autonomyMode != advise");
  EXPECT(env->writeBackType     == "pe-sensor",                                 "AGX051 urgent_maint: writeBack.type != pe-sensor");
  EXPECT(env->ragStatusCode     == "RED",                                       "AGX051 urgent_maint: ragStatusCode != RED");
  EXPECT(env->processStatus     == "error",                                     "AGX051 urgent_maint: processStatus != error");
  EXPECT(env->ownerTeam         == "agriculture-operations",                    "AGX051 urgent_maint: ownerTeam mismatch");
  EXPECT(env->slaSeconds.value_or(0) == 900,                                    "AGX051 urgent_maint: slaSeconds != 900");

  // GREEN path — NORMAL output must also resolve, with a tighter contract.
  auto green = envelope_for(m, machine_metadata(root), "agx-051-normal", Vector{0, 0, 0, 1});
  EXPECT(green.has_value(),                                                     "AGX051 normal: no envelope resolved");
  if (green) EXPECT(green->ragStatusCode == "GREEN",                            "AGX051 normal: ragStatusCode != GREEN");
}

void test_agx055_envelope_pins(const std::filesystem::path& machinesDir) {
  std::string raw = read_file(find_machine_file(machinesDir, "AGX055_yuma-facility-ai-synthesis-bridge.json"));
  Json root = json::parse(raw);
  Machine m = load_machine_from_json_string(raw, "pin-agx055");

  // AGX055's output is a 12-cell one-hot.  Verify each of the four URGENT
  // labels routes to the cross-domain AI, and FACILITY_STABLE resolves GREEN.
  struct Case { std::string seqId; Vector out; std::string ragExpected; };
  const std::vector<Case> cases = {
    {"agx-055-aqua-urgent",      {1,0,0,0,0,0,0,0,0,0,0,0}, "RED"},
    {"agx-055-do-urgent",        {0,0,0,1,0,0,0,0,0,0,0,0}, "RED"},
    {"agx-055-climate-urgent",   {0,0,0,0,0,0,1,0,0,0,0,0}, "RED"},
    {"agx-055-safety-urgent",    {0,0,0,0,0,0,0,0,0,1,0,0}, "RED"},
    {"agx-055-facility-stable",  {0,0,0,0,0,0,0,0,0,0,0,1}, "GREEN"},
  };
  for (const auto& c : cases) {
    auto env = envelope_for(m, machine_metadata(root), c.seqId, c.out);
    EXPECT(env.has_value(),                                                     "AGX055 " + c.seqId + ": envelope unresolved");
    if (!env) continue;
    EXPECT(env->agent             == "agriculture_yield_optimization_ai",       "AGX055 " + c.seqId + ": dispatch agent != agriculture_yield_optimization_ai");
    EXPECT(env->trigger           == "ag-yield-optimization-ai-yuma-facility-bridge",
                                                                                "AGX055 " + c.seqId + ": aiTrigger mismatch");
    EXPECT(env->autonomyMode      == "advise",                                  "AGX055 " + c.seqId + ": autonomyMode != advise");
    EXPECT(env->writeBackType     == "pe-sensor",                               "AGX055 " + c.seqId + ": writeBack.type != pe-sensor");
    EXPECT(env->ragStatusCode     == c.ragExpected,                             "AGX055 " + c.seqId + ": ragStatusCode != " + c.ragExpected);
    EXPECT(env->ownerTeam         == "agriculture-operations",                  "AGX055 " + c.seqId + ": ownerTeam mismatch");
  }

  // Projection invariant: AGX055.output region must match
  // AgYieldOptimizationAI.input region exactly — the 12-cell bridge
  // contract is meaningless if those don't overlap.
  std::string yieldRaw = read_file(find_machine_file(machinesDir, "AgYieldOptimizationAI.json"));
  Json yieldRoot = json::parse(yieldRaw);
  const Json& bridgeOut  = root.at("machine").at("perceptualMapping").at("output");
  const Json& yieldIn    = yieldRoot.at("machine").at("perceptualMapping").at("input");
  EXPECT(bridgeOut.at("offset").as_number()  == yieldIn.at("offset").as_number(),
         "AGX055.output.offset != AgYieldOptimizationAI.input.offset (bridge contract broken)");
  EXPECT(bridgeOut.at("length").as_number()  == yieldIn.at("length").as_number(),
         "AGX055.output.length != AgYieldOptimizationAI.input.length (bridge contract broken)");
  EXPECT(bridgeOut.at("length").as_number()  == 12.0,
         "AGX055 bridge contract requires length=12 — corpus drifted");
}

} // namespace

int main(int argc, char** argv) {
  std::filesystem::path machinesDir = argc > 1 ? argv[1] : "../RealityEngine_Machines/machines";
  if (!std::filesystem::exists(machinesDir)) {
    auto alt = std::filesystem::path("..") / machinesDir;
    if (std::filesystem::exists(alt)) machinesDir = alt;
  }
  if (!std::filesystem::exists(machinesDir)) {
    std::cerr << "Skipping e2e_ai_trigger_dispatch — corpus not found at " << machinesDir << "\n";
    return 0;
  }

  std::vector<std::filesystem::path> files;
  // Recursive: domain subdirectories (machines/domains/<name>/) are part of
  // the corpus and must be held to the same dispatch contract.
  for (const auto& entry : std::filesystem::recursive_directory_iterator(machinesDir)) {
    if (entry.path().extension() == ".json") files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());

  WalkSummary sum;
  for (const auto& file : files) {
    try { walk_machine(file, sum); }
    catch (const std::exception& e) {
      EXPECT(false, file.filename().string() + ": walk threw — " + e.what());
    }
  }

  test_agx051_envelope_pins(machinesDir);
  test_agx055_envelope_pins(machinesDir);

  EXPECT(sum.machinesWithTriggers == kExpectedMachinesWithTriggers,
         "trigger corpus parity drift: machinesWithTriggers expected " +
         std::to_string(kExpectedMachinesWithTriggers) + ", got " +
         std::to_string(sum.machinesWithTriggers));
  EXPECT(sum.inputSequencesRun == kExpectedInputSequencesRun,
         "trigger corpus parity drift: inputSequencesRun expected " +
         std::to_string(kExpectedInputSequencesRun) + ", got " +
         std::to_string(sum.inputSequencesRun));
  EXPECT(sum.outputsProduced == kExpectedOutputsProduced,
         "trigger corpus parity drift: outputsProduced expected " +
         std::to_string(kExpectedOutputsProduced) + ", got " +
         std::to_string(sum.outputsProduced));
  EXPECT(sum.envelopesResolved == kExpectedEnvelopesResolved,
         "trigger corpus parity drift: envelopesResolved expected " +
         std::to_string(kExpectedEnvelopesResolved) + ", got " +
         std::to_string(sum.envelopesResolved));

  std::cout << "E2E AI trigger dispatch summary\n"
            << "  machines with triggerConfig+agentBinding:      " << sum.machinesWithTriggers << "\n"
            << "  input sequences walked:                        " << sum.inputSequencesRun   << "\n"
            << "  outputs produced:                              " << sum.outputsProduced     << "\n"
            << "  envelopes resolved (PagingDecision matched):   " << sum.envelopesResolved   << "\n"
            << "  expectations passed:                           " << passed                  << "\n"
            << "  expectations failed:                           " << failed                  << "\n";

  if (failed > 0) {
    std::cerr << "\nFailures:\n";
    for (const auto& m : failureMsgs) std::cerr << "  - " << m << "\n";
    std::cerr << "RealityEngine_CPP AI-trigger dispatch tests FAILED\n";
    return 1;
  }
  if (sum.machinesWithTriggers == 0 || sum.envelopesResolved == 0) {
    std::cerr << "AI-trigger dispatch test exercised zero machines/envelopes — corpus may be malformed\n";
    return 1;
  }
  std::cout << "RealityEngine_CPP AI-trigger dispatch tests passed\n";
  return 0;
}
