#pragma once
// Cloneable machines and the clone space (RealityEngine_CI SURFACE_SPEC.md,
// "Cloneable machines"; RealityEngine_CI#713, localAssetStorageStack#8).
//
// Recognition is decided here, at load, by the specification's nine-rule
// table; nothing in a machine file marks a machine cloneable. The clone space is
// a band of 64-cell blocks in the nest layout, allocated lowest free first, each
// with a generation that the recycle route must present.

#include "reality/reality.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace reality::clones {

// Fixed by the specification ("The clone space"); RealityEngine_Machines
// domains/clone-space.json records the same values.
constexpr int kBandOffset = 20480;
constexpr int kBandLength = 65536;
constexpr int kBlockCells = 64;
constexpr int kQuarter = 16;
constexpr int kBlocks = kBandLength / kBlockCells;  // 1024
constexpr int kBusCells = 4;
constexpr int kMaxActiveDefault = 8;

struct Verdict {
  bool cloneable = false;
  // The first rule the machine fails, as the specification's reason code; empty
  // when cloneable.
  std::string reason;
  // Folded fanout cells + bus cells, present whenever rule 9 was evaluated.
  std::optional<int> n;
  // Folded fanout cells alone (rule 9), when evaluated.
  int folded = 0;
};

// The file names other machines' published-domain-bus-producer edges target.
std::set<std::string> bus_target_files(const std::map<std::string, Machine>& machines);

// The nine-rule table, in order. `busTargets` is bus_target_files over every
// registered machine (rule 3 needs the whole set: the loader's second pass).
Verdict assess(const Machine& machine, const std::set<std::string>& busTargets);

// GET /api/machines/cloneable: every registered machine except clones, sorted
// by id, with its verdict.
Json listing(const std::map<std::string, Machine>& machines);

struct Clone {
  std::string cloneId;
  std::string machineId;
  std::string engineInstance;
  std::string envelopeId;
  std::string assetId;
  int block = 0;
  long generation = 0;
  int inputLength = 0;
  int outputLength = 0;
  int fanoutLength = 0;

  int offset() const { return kBandOffset + kBlockCells * block; }
  RegionMapping input() const { return {offset(), inputLength}; }
  RegionMapping output() const { return {offset() + kQuarter, outputLength}; }
  RegionMapping fanout() const { return {offset() + 2 * kQuarter, fanoutLength}; }
  RegionMapping bus() const { return {offset() + 3 * kQuarter, kBusCells}; }
  // The 201 body, and each entry of GET /api/engine/clones.
  Json to_json() const;
};

// The live clones and every block's generation. Not thread-safe: the server
// guards it with its machine registry lock.
class Space {
public:
  Space() : generations(kBlocks, 0) {}

  std::optional<int> lowest_free_block() const;
  int live_of(const std::string& machineId) const;
  const Clone* by_id(const std::string& cloneId) const;
  const Clone* by_envelope(const std::string& envelopeId) const;
  long generation(int block) const { return generations.at(static_cast<size_t>(block)); }
  bool is_clone(const std::string& machineId) const { return by_id(machineId) != nullptr; }
  void add(const Clone& clone);
  // Frees the clone's block and increments its generation; returns the new one.
  long recycle(const std::string& cloneId);
  // Live clones in block order.
  std::vector<Clone> live() const;

private:
  std::vector<long> generations;
  std::map<int, Clone> byBlock;
};

}  // namespace reality::clones
