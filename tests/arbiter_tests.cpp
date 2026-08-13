// Conformance checks for RealityEngine_Machines docs/ARBITER_CONTRACT.md.
//
// The properties here are the ones no live probe can establish: that resolution
// does not depend on the order contributions arrive in, nor on how the resolve
// phase is sharded. Those are the entire basis for parallelising the arbiter,
// and a violation would be invisible in any single run.

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "reality/arbiter.hpp"

using namespace reality;

namespace {

int failures = 0;

void check(const char* name, bool cond, const std::string& detail = "") {
  if (cond) {
    std::printf("  ok   %s\n", name);
  } else {
    ++failures;
    std::printf("  FAIL %s  %s\n", name, detail.c_str());
  }
}

Contribution c(double value, const std::string& provider, const std::string& origin,
               const std::string& rag = "") {
  Contribution x;
  x.cell = 1;
  x.value = value;
  x.provider = provider;
  x.originId = origin;
  x.cesId = "seq";
  x.outputVectorId = "ov";
  x.ragStatusCode = rag;
  return x;
}

double resolve(const std::vector<Contribution>& cs, const ArbitrationEntry* e,
               std::optional<ArbitrationRecord>* out = nullptr) {
  std::optional<ArbitrationRecord> rec;
  double v = resolve_cell(1, 0, cs, e, rec);
  if (out) *out = rec;
  return v;
}

}  // namespace

int main() {
  std::printf("Arbiter conformance (ARBITER_CONTRACT.md)\n");

  // An unregistered surface must not outrank a reading by default.
  check("unregistered provider classifies as generated",
        determinism_of("some-future-surface") == Determinism::Generated);
  check("machine is deterministic", determinism_of("machine") == Determinism::Deterministic);
  check("mqtt is measured", determinism_of("mqtt") == Determinism::Measured);
  check("life-safety outranks RED", severity_rank("GREEN", true) > severity_rank("RED"));

  {
    // The generated value is larger, so a MAX-based merge would take it.
    ArbitrationEntry e; e.cell = 1; e.rule = "PRECEDENCE";
    std::optional<ArbitrationRecord> rec;
    double v = resolve({c(0.0, "machine", "m1"), c(1.0, "acp", "a1")}, &e, &rec);
    check("PRECEDENCE: generated never overrides deterministic", v == 0.0,
          "got " + std::to_string(v));
    check("PRECEDENCE: the generated contribution is recorded as suppressed",
          rec && rec->suppressed.size() == 1 && rec->suppressed.front().provider == "acp");
  }

  {
    // Two machine determinations plus an agent: RED asserts 0, AMBER asserts 1.
    // MAX would take 1; SEVERITY within the winning class must take 0.
    ArbitrationEntry e; e.cell = 1; e.rule = "PRECEDENCE"; e.withinRank = "SEVERITY";
    double v = resolve({c(1.0, "machine", "m-amber", "AMBER"),
                        c(0.0, "machine", "m-red", "RED"),
                        c(1.0, "acp", "a1")}, &e);
    check("withinRank SEVERITY is applied rather than falling back to MAX", v == 0.0,
          "got " + std::to_string(v));
  }

  {
    ArbitrationEntry e; e.cell = 1; e.rule = "SEVERITY";
    double v = resolve({c(1.0, "machine", "a", "AMBER"), c(0.0, "machine", "b", "RED")}, &e);
    check("SEVERITY resolves by RAG rank before value", v == 0.0, "got " + std::to_string(v));
  }

  {
    // Acceptance criteria 2 and 4: the externally visible form of the
    // commutative-monoid requirement, and the property most likely to break once
    // sources arrive asynchronously.
    ArbitrationEntry e; e.cell = 1; e.rule = "PRECEDENCE"; e.withinRank = "SEVERITY";
    std::vector<Contribution> base = {c(1.0, "machine", "m-amber", "AMBER"),
                                      c(0.0, "machine", "m-red", "RED"),
                                      c(0.7, "acp", "a1"),
                                      c(0.3, "mqtt", "s1")};
    std::sort(base.begin(), base.end(),
              [](const Contribution& a, const Contribution& b) { return a.originId < b.originId; });
    std::vector<double> results;
    do {
      results.push_back(resolve(base, &e));
    } while (std::next_permutation(base.begin(), base.end(),
                                   [](const Contribution& a, const Contribution& b) {
                                     return a.originId < b.originId;
                                   }));
    bool allSame = std::all_of(results.begin(), results.end(),
                               [&](double v) { return v == results.front(); });
    check("resolution is invariant under contribution order", allSame,
          std::to_string(results.size()) + " permutations");
  }

  {
    std::optional<ArbitrationRecord> rec;
    double v = resolve({c(0.42, "acp", "a1")}, nullptr, &rec);
    check("a single contributor resolves to itself", v == 0.42);
    check("a single contributor emits no record", !rec.has_value());
  }

  {
    // contributors u suppressed must equal the full set — a discarded agent
    // answer has to stay attributable.
    ArbitrationEntry e; e.cell = 1; e.rule = "PRECEDENCE";
    std::optional<ArbitrationRecord> rec;
    resolve({c(0.0, "machine", "m1"), c(1.0, "acp", "a1"), c(0.5, "mcp", "a2")}, &e, &rec);
    check("the record accounts for every contribution",
          rec && rec->contributors.size() == 3 && rec->suppressed.size() == 2);
  }

  {
    // Floating-point addition is not associative, so MEAN is admissible only
    // under the contract's canonical contributor ordering.
    ArbitrationEntry e; e.cell = 1; e.rule = "MEAN";
    double a = resolve({c(0.1, "machine", "c"), c(0.2, "machine", "a"), c(0.7, "machine", "b")}, &e);
    double b = resolve({c(0.7, "machine", "b"), c(0.1, "machine", "c"), c(0.2, "machine", "a")}, &e);
    check("MEAN is order-independent under its canonical ordering", a == b);
  }

  {
    // Acceptance criterion 3: varying the partitioning changes nothing. Run the
    // same cells through resolve_all twice with different shard counts.
    std::map<int, std::vector<Contribution>> byCell;
    for (int cell = 0; cell < 600; ++cell) {
      byCell[cell] = {c(0.0, "machine", "m" + std::to_string(cell)),
                      c(1.0, "acp", "a" + std::to_string(cell))};
      for (auto& x : byCell[cell]) x.cell = cell;
    }
    std::vector<ArbitrationRecord> r1, r2;
    setenv("ARBITER_SHARDS", "1", 1);
    auto one = resolve_all(byCell, 0, r1);
    setenv("ARBITER_SHARDS", "8", 1);
    auto many = resolve_all(byCell, 0, r2);
    check("shard count does not change the resolved values", one == many,
          std::to_string(one.size()) + " cells");
    check("shard count does not change the record count", r1.size() == r2.size());
  }

  if (failures == 0) {
    std::printf("Arbiter conformance tests: OK\n");
    return 0;
  }
  std::printf("Arbiter conformance tests: %d FAILED\n", failures);
  return 1;
}
