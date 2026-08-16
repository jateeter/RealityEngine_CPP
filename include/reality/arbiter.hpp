#pragma once
//
// Output arbiter — RealityEngine_Machines docs/ARBITER_CONTRACT.md.
//
// The merge previously sorted mergeBatch canonically and then applied each
// operation with merge_machine_output, so on a contended cell the last operation
// in sort order won. The sort made that deterministic, which is better than
// Scala's list order but still not a resolution: a wrong value reproduces
// perfectly and reads as correct.
//
// Every admissible rule here is a commutative monoid (contract 4.1). That is the
// load-bearing property — it is what lets the resolve phase be sharded across
// cell ranges in any order, and what lets four independent runtimes agree on the
// same IS(k+1).
//
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace reality {

enum class Determinism { Deterministic, Measured, Generated };

// Provider is an open registry, not a closed enum — integration surfaces
// register here (contract 3). An unregistered surface classifies as Generated:
// misclassifying a generated source as measured would let irreproducible content
// outrank a reading, which is the failure 4.3a exists to prevent.
Determinism determinism_of(const std::string& provider);
int determinism_rank(Determinism d);
// The contract's own spelling of the class: "deterministic" | "measured" |
// "generated". Serialised by GET /api/arbitration, so it is part of the wire
// contract rather than a debug string.
const char* determinism_name(Determinism d);
int severity_rank(const std::string& ragStatusCode, bool lifeSafety = false);

struct Contribution {
  int         cell = 0;
  double      value = 0.0;
  std::string provider;        // "machine", "acp", "mqtt", ...
  std::string originId;        // machineId, or the PE source id
  std::string cesId;           // machine contributions only
  std::string outputVectorId;  // machine contributions only
  std::string ragStatusCode;   // from the trigger-rule join (contract 4.3.1)
  bool        lifeSafety = false;
};

struct ArbitrationRecord {
  int                       instant = 0;
  int                       cell = 0;
  std::string               rule;
  double                    resolved = 0.0;
  std::vector<Contribution> contributors;
  std::vector<Contribution> suppressed;
};

struct ArbitrationEntry {
  int                        cell = 0;
  std::string                rule = "PRECEDENCE";
  std::string                withinRank;  // empty means MAX (contract 4.3a default)
  std::map<std::string, int> providerRanks;
};

// Declares how each contended position resolves. Resolution is declared, not
// defaulted: an undeclared contended cell is a corpus error (contract 5). The
// engine still applies PRECEDENCE when it meets one, which at least keeps a
// generated contribution from overriding a deterministic one; the corpus gate is
// what prevents the situation arising.
class ArbitrationRegistry {
 public:
  static ArbitrationRegistry& instance();
  // Resolves ARBITRATION_REGISTRY, then paths beside machinesDir.
  void load(const std::string& machinesDir);
  const ArbitrationEntry* entry_for(int cell) const;
  size_t size() const { return entries_.size(); }
  const std::string& source() const { return source_; }

 private:
  std::map<int, ArbitrationEntry> entries_;
  std::string                     source_;
};

// The shard count resolve_all would use for the current environment.
//
// Reported by GET /api/arbitration so a parallelism difference between runtimes
// is visible rather than inferred. Correctness does not depend on it — every
// rule is a commutative monoid, which is what makes any partitioning safe
// (ARBITER_CONTRACT.md acceptance criterion 3) — but a run that cannot say how
// it partitioned cannot demonstrate that criterion either.
unsigned arbiter_shards();

// Resolve one cell. Returns the value; when the cell is contended, record is
// populated with contributors and what was suppressed — a discarded agent
// assessment has to stay attributable (contract 6).
double resolve_cell(int cell, int instant,
                    const std::vector<Contribution>& contributions,
                    const ArbitrationEntry* entry,
                    std::optional<ArbitrationRecord>& record);

// Resolve every cell. Sharded across cell ranges with std::async when the
// contended set is large enough to be worth it; correctness does not depend on
// the shard count, which is what acceptance criterion 3 asserts.
std::vector<std::pair<int, double>> resolve_all(
    const std::map<int, std::vector<Contribution>>& byCell,
    int instant,
    std::vector<ArbitrationRecord>& records);

}  // namespace reality
