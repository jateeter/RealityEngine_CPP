#pragma once

// STA (Single Transition Assumption) checker — C++ twin of
// the historical services/StaChecker.ts.
//
// Operates on the raw machine JSON (not the loaded Machine class) so the
// analyzer can run before load_machine_from_json_string hands the object
// to the engine.  That ordering matters: the loader gates on STA when the
// machine is tagged `metadata.severity == "life-safety"` — a violating
// machine is refused before any RealityEvent instance exists.
//
// compute_sta() returns the same structured report the AI CLI emits in
// --json mode, so a regression in either side is byte-comparable.

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "reality/json.hpp"

namespace reality::sta {

struct StaTransition {
  std::string from;
  std::string to;
  std::string kind;                       // "intra" | "inter-sequence" | "dangling"
  std::optional<int> hd;                  // hamming distance; absent when element-count mismatch / dangling
  std::vector<int> fromState;             // binary projection of `from` vector
  std::vector<int> toState;               // binary projection of `to` vector
  std::string targetSequenceId;           // populated for "inter-sequence" kind
  bool violation = false;
  std::string error;                      // non-empty when the transition is structurally broken
};

struct StaSequenceReport {
  std::string id;
  std::string name;
  std::vector<StaTransition> transitions;
  int maxIntraHD = 0;
  bool anyViolation = false;
};

struct StaReport {
  std::string machineId;                  // empty if absent
  std::string machineName;
  bool lifeSafety = false;
  std::optional<json::Value> declared;    // metadata.singleTransitionAssumption
  std::vector<StaSequenceReport> sequences;
  int intraViolations = 0;
  int interJumps = 0;
  std::optional<std::string> drift;       // populated when declared != computed
};

// Accepts either the wrapped form {"machine": {...}} or a bare machine object.
StaReport compute_sta(const json::Value& root);
StaReport compute_sta(const std::string& raw_json);

// Thrown by assert_sta_for_life_safety when a life-safety machine has any
// intra-sequence HD>1 transition.  The message lists offending tuples.
class StaViolationError : public std::runtime_error {
public:
  StaReport report;
  explicit StaViolationError(StaReport r);
};

// Throws when the machine is tagged `severity: "life-safety"` AND has at
// least one intra-sequence violation.  No-op for non-life-safety machines.
StaReport assert_sta_for_life_safety(const json::Value& root);
StaReport assert_sta_for_life_safety(const std::string& raw_json);

}  // namespace reality::sta
