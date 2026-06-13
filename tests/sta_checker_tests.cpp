// STA checker tests — verifies parity with the active Scala replacement.
// and exercises the strictSta load gate on load_machine_from_json_string.

#include "reality/reality.hpp"
#include "reality/sta_checker.hpp"

#include <iostream>
#include <sstream>
#include <string>

using namespace reality;

namespace {

int failures = 0;

#define EXPECT(cond, label)                                              \
  do {                                                                   \
    if (!(cond)) {                                                       \
      std::cerr << "FAIL: " << label << "  (" << __FILE__ << ":"        \
                << __LINE__ << ")\n";                                    \
      ++failures;                                                        \
    }                                                                    \
  } while (0)

// Minimal life-safety machine with two intra transitions: a clean
// (HD=1) walk and a violating (HD>1) jump.  Sequence "good" → 1 violation
// when the second vector flips both bits; sequence "clean" → 0 violations.
std::string life_safety_with_violation_json() {
  return R"({
    "version": "1.0.0",
    "machine": {
      "id": "machine-test-life-safety",
      "name": "TestLifeSafety",
      "description": "fixture",
      "arbiterRule": "PASSTHROUGH",
      "metadata": { "severity": "life-safety" },
      "perceptualMapping": { "input": {"offset":0,"length":4}, "output": {"offset":4,"length":2} },
      "sequences": [
        {
          "id": "seq-violation",
          "name": "violation",
          "vectors": [
            { "id": "v1", "isInitial": true,  "elements": [{"value":0},{"value":0}], "nextVectorIds": ["v2"] },
            { "id": "v2", "isInitial": false, "elements": [{"value":1},{"value":1}], "nextVectorIds": [],
              "outputVectors": [{"id":"o1","vector":[1,0]}] }
          ]
        }
      ]
    }
  })";
}

// Clean life-safety machine — every intra transition flips at most one bit.
std::string life_safety_clean_json() {
  return R"({
    "version": "1.0.0",
    "machine": {
      "id": "machine-test-life-safety-clean",
      "name": "TestLifeSafetyClean",
      "description": "fixture",
      "arbiterRule": "PASSTHROUGH",
      "metadata": { "severity": "life-safety" },
      "perceptualMapping": { "input": {"offset":0,"length":4}, "output": {"offset":4,"length":2} },
      "sequences": [
        {
          "id": "seq-ok",
          "name": "ok",
          "vectors": [
            { "id": "v1", "isInitial": true,  "elements": [{"value":0},{"value":0}], "nextVectorIds": ["v2"] },
            { "id": "v2", "isInitial": false, "elements": [{"value":1},{"value":0}], "nextVectorIds": [],
              "outputVectors": [{"id":"o1","vector":[1,0]}] }
          ]
        }
      ]
    }
  })";
}

// Same shape as the violation fixture, but without the life-safety tag.
// compute_sta still reports the violation; assert_sta_for_life_safety
// must NOT throw because the gate is severity-scoped.
std::string non_life_safety_with_violation_json() {
  return R"({
    "version": "1.0.0",
    "machine": {
      "id": "machine-test-permissive",
      "name": "TestPermissive",
      "description": "fixture",
      "arbiterRule": "PASSTHROUGH",
      "metadata": { },
      "perceptualMapping": { "input": {"offset":0,"length":4}, "output": {"offset":4,"length":2} },
      "sequences": [
        {
          "id": "seq-violation",
          "name": "violation",
          "vectors": [
            { "id": "v1", "isInitial": true,  "elements": [{"value":0},{"value":0}], "nextVectorIds": ["v2"] },
            { "id": "v2", "isInitial": false, "elements": [{"value":1},{"value":1}], "nextVectorIds": [],
              "outputVectors": [{"id":"o1","vector":[1,0]}] }
          ]
        }
      ]
    }
  })";
}

// Machine with a dangling nextVectorId — structural defect, counted as
// an intra violation regardless of bit pattern.
std::string dangling_next_id_json() {
  return R"({
    "machine": {
      "id": "machine-test-dangling",
      "name": "TestDangling",
      "description": "fixture",
      "arbiterRule": "PASSTHROUGH",
      "metadata": { "severity": "life-safety" },
      "perceptualMapping": { "input": {"offset":0,"length":4}, "output": {"offset":4,"length":2} },
      "sequences": [
        {
          "id": "seq-dangling",
          "name": "dangling",
          "vectors": [
            { "id": "v1", "isInitial": true, "elements": [{"value":0}], "nextVectorIds": ["nope"] }
          ]
        }
      ]
    }
  })";
}

// Two sequences with an inter-sequence jump (s1.v1 → s2.v1).  Not an intra
// violation; counted on report.interJumps.
std::string inter_sequence_jump_json() {
  return R"({
    "machine": {
      "id": "machine-test-inter",
      "name": "TestInter",
      "description": "fixture",
      "arbiterRule": "PASSTHROUGH",
      "metadata": { "severity": "life-safety" },
      "perceptualMapping": { "input": {"offset":0,"length":4}, "output": {"offset":4,"length":2} },
      "sequences": [
        {
          "id": "s1", "name": "s1",
          "vectors": [
            { "id": "s1v1", "isInitial": true, "elements": [{"value":0}], "nextVectorIds": ["s2v1"] }
          ]
        },
        {
          "id": "s2", "name": "s2",
          "vectors": [
            { "id": "s2v1", "isInitial": false, "elements": [{"value":1}], "nextVectorIds": [],
              "outputVectors": [{"id":"o","vector":[1]}] }
          ]
        }
      ]
    }
  })";
}

// Machine that declares STA compliance but contains a violation — drift
// must be set by compute_sta.
std::string drift_declared_json() {
  return R"({
    "machine": {
      "id": "machine-test-drift",
      "name": "TestDrift",
      "description": "fixture",
      "arbiterRule": "PASSTHROUGH",
      "metadata": {
        "severity": "life-safety",
        "singleTransitionAssumption": { "compliant": true, "maxHammingDistanceIntraSequence": 1 }
      },
      "perceptualMapping": { "input": {"offset":0,"length":4}, "output": {"offset":4,"length":2} },
      "sequences": [
        {
          "id": "seq", "name": "seq",
          "vectors": [
            { "id": "v1", "isInitial": true,  "elements": [{"value":0},{"value":0}], "nextVectorIds": ["v2"] },
            { "id": "v2", "isInitial": false, "elements": [{"value":1},{"value":1}], "nextVectorIds": [],
              "outputVectors": [{"id":"o","vector":[1,0]}] }
          ]
        }
      ]
    }
  })";
}

void run() {
  // 1. compute_sta on the violation fixture reports the intra HD=2.
  {
    auto report = sta::compute_sta(life_safety_with_violation_json());
    EXPECT(report.lifeSafety, "violation fixture is flagged life-safety");
    EXPECT(report.intraViolations == 1, "exactly one intra violation reported");
    EXPECT(report.sequences.size() == 1, "one sequence in report");
    if (!report.sequences.empty()) {
      EXPECT(report.sequences[0].maxIntraHD == 2, "maxIntraHD == 2 for the violating sequence");
      EXPECT(report.sequences[0].anyViolation, "sequence flagged anyViolation");
    }
  }

  // 2. assert_sta_for_life_safety throws on the violation fixture.
  {
    bool threw = false;
    try {
      sta::assert_sta_for_life_safety(life_safety_with_violation_json());
    } catch (const sta::StaViolationError& e) {
      threw = true;
      std::string msg = e.what();
      EXPECT(msg.find("life-safety machine") != std::string::npos, "error message names the gate");
      EXPECT(msg.find("v1 -> v2") != std::string::npos, "error message lists offending transition");
    } catch (...) { /* swallow — failure is tracked by threw=false */ }
    EXPECT(threw, "assert_sta_for_life_safety throws on violation");
  }

  // 3. assert_sta_for_life_safety passes on a clean life-safety machine.
  {
    bool threw = false;
    try { sta::assert_sta_for_life_safety(life_safety_clean_json()); }
    catch (const std::exception&) { threw = true; }
    EXPECT(!threw, "clean life-safety machine passes the gate");
  }

  // 4. Non-life-safety machine with the same violation: compute_sta still
  //    reports it but assert_sta_for_life_safety must not throw.
  {
    auto report = sta::compute_sta(non_life_safety_with_violation_json());
    EXPECT(!report.lifeSafety, "non-life-safety machine flagged correctly");
    EXPECT(report.intraViolations == 1, "still reports the structural violation");
    bool threw = false;
    try { sta::assert_sta_for_life_safety(non_life_safety_with_violation_json()); }
    catch (const std::exception&) { threw = true; }
    EXPECT(!threw, "non-life-safety machine passes the gate even with violations");
  }

  // 5. Dangling nextVectorId counted as an intra violation.
  {
    auto report = sta::compute_sta(dangling_next_id_json());
    EXPECT(report.intraViolations == 1, "dangling id treated as intra violation");
    bool threw = false;
    try { sta::assert_sta_for_life_safety(dangling_next_id_json()); }
    catch (const sta::StaViolationError&) { threw = true; }
    EXPECT(threw, "dangling id trips the life-safety gate");
  }

  // 6. Inter-sequence jumps are recorded separately and do not trip the gate.
  {
    auto report = sta::compute_sta(inter_sequence_jump_json());
    EXPECT(report.intraViolations == 0, "inter-sequence jump is not an intra violation");
    EXPECT(report.interJumps == 1, "inter-sequence jump counted on interJumps");
    bool threw = false;
    try { sta::assert_sta_for_life_safety(inter_sequence_jump_json()); }
    catch (const std::exception&) { threw = true; }
    EXPECT(!threw, "inter-sequence jump does not trip the gate");
  }

  // 7. Drift detection — declared compliant=true vs actual violation.
  {
    auto report = sta::compute_sta(drift_declared_json());
    EXPECT(report.drift.has_value(), "drift detected on mismatched declaration");
    if (report.drift) {
      EXPECT(report.drift->find("compliant=true") != std::string::npos, "drift mentions declared compliant=true");
      EXPECT(report.drift->find("maxHamming") != std::string::npos, "drift mentions max-HD mismatch");
    }
  }

  // 8. Load gate via load_machine_from_json_string — strictSta=false loads
  //    cleanly even when the machine is violating; strictSta=true throws.
  {
    bool threwOff = false;
    try { (void)load_machine_from_json_string(life_safety_with_violation_json(), std::nullopt, {}); }
    catch (const std::exception&) { threwOff = true; }
    EXPECT(!threwOff, "strictSta=false admits a violating life-safety machine");

    bool threwOn = false;
    try { (void)load_machine_from_json_string(life_safety_with_violation_json(), std::nullopt, LoadOptions{true}); }
    catch (const sta::StaViolationError&) { threwOn = true; }
    EXPECT(threwOn, "strictSta=true rejects a violating life-safety machine");

    // Clean machine loads under strict mode.
    bool threwClean = false;
    try { (void)load_machine_from_json_string(life_safety_clean_json(), std::nullopt, LoadOptions{true}); }
    catch (const std::exception&) { threwClean = true; }
    EXPECT(!threwClean, "strictSta=true admits a clean life-safety machine");
  }
}

}  // namespace

int main() {
  run();
  if (failures > 0) {
    std::cerr << "\n" << failures << " STA checker assertion(s) failed.\n";
    return 1;
  }
  std::cout << "STA checker tests: OK\n";
  return 0;
}
