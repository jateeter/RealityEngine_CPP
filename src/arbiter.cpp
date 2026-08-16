#include "reality/arbiter.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <future>
#include <sstream>
#include <sys/stat.h>
#include <thread>

#include "reality/json.hpp"

namespace reality {

namespace {

const std::map<std::string, Determinism>& provider_classes() {
  static const std::map<std::string, Determinism> m = {
      {"machine", Determinism::Deterministic},
      {"sensor", Determinism::Measured},   {"mqtt", Determinism::Measured},
      {"healthkit", Determinism::Measured}, {"stream", Determinism::Measured},
      {"ui", Determinism::Measured},        {"synthetic", Determinism::Measured},
      {"acp", Determinism::Generated},      {"mcp", Determinism::Generated},
      {"localai", Determinism::Generated},
  };
  return m;
}

bool file_exists(const std::string& p) {
  struct stat st{};
  return !p.empty() && ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Select the contributions maximising (or minimising) a score. Ties are kept so
// the caller can apply the next stage; keeping every tied contribution is what
// makes the composite rules associative.
template <typename Score>
std::vector<Contribution> pick(const std::vector<Contribution>& cs, Score score, bool wantMax) {
  double best = score(cs.front());
  for (const auto& c : cs) {
    double s = score(c);
    if (wantMax ? (s > best) : (s < best)) best = s;
  }
  std::vector<Contribution> out;
  for (const auto& c : cs)
    if (score(c) == best) out.push_back(c);
  return out;
}

}  // namespace

Determinism determinism_of(const std::string& provider) {
  auto it = provider_classes().find(provider);
  return it == provider_classes().end() ? Determinism::Generated : it->second;
}

int determinism_rank(Determinism d) {
  switch (d) {
    case Determinism::Deterministic: return 3;
    case Determinism::Measured:      return 2;
    default:                         return 1;
  }
}

int severity_rank(const std::string& rag, bool lifeSafety) {
  if (lifeSafety) return 3;
  if (rag == "RED") return 2;
  if (rag == "AMBER") return 1;
  return 0;  // GREEN or absent
}

ArbitrationRegistry& ArbitrationRegistry::instance() {
  static ArbitrationRegistry r;
  return r;
}

void ArbitrationRegistry::load(const std::string& machinesDir) {
  std::vector<std::string> candidates;
  if (const char* env = std::getenv("ARBITRATION_REGISTRY")) candidates.emplace_back(env);
  if (!machinesDir.empty()) {
    candidates.push_back(machinesDir + "/../domains/arbitration-registry.json");
    candidates.push_back(machinesDir + "/domains/arbitration-registry.json");
  }
  candidates.push_back("../RealityEngine_Machines/domains/arbitration-registry.json");

  std::string found;
  for (const auto& c : candidates)
    if (file_exists(c)) { found = c; break; }

  if (found.empty()) {
    std::fprintf(stderr, "[arbiter] no arbitration registry found; contended cells fall back to PRECEDENCE\n");
    return;
  }

  std::ifstream in(found);
  std::stringstream ss;
  ss << in.rdbuf();
  try {
    auto doc = json::parse(ss.str());
    const auto& entriesNode = doc.at("entries");
    if (entriesNode.is_array()) {
      for (const auto& e : entriesNode.array()) {
        ArbitrationEntry entry;
        entry.cell       = static_cast<int>(e.at("cell").as_number());
        entry.rule       = e.at("rule").as_string("PRECEDENCE");
        entry.withinRank = e.at("withinRank").as_string("");
        const auto& ranks = e.at("providerRanks");
        if (ranks.is_object())
          for (const auto& [k, v] : ranks.object())
            entry.providerRanks[k] = static_cast<int>(v.as_number());
        entries_[entry.cell] = std::move(entry);
      }
    }
    source_ = found;
    std::fprintf(stderr, "[arbiter] loaded %zu contended cell declaration(s) from %s\n",
                 entries_.size(), found.c_str());
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "[arbiter] failed to read %s: %s\n", found.c_str(), ex.what());
  }
}

const ArbitrationEntry* ArbitrationRegistry::entry_for(int cell) const {
  auto it = entries_.find(cell);
  return it == entries_.end() ? nullptr : &it->second;
}

double resolve_cell(int cell, int instant,
                    const std::vector<Contribution>& contributions,
                    const ArbitrationEntry* entry,
                    std::optional<ArbitrationRecord>& record) {
  record.reset();
  if (contributions.empty()) return 0.0;
  // A single contributor resolves to itself regardless of declared rule
  // (contract 4.5) and needs no record.
  if (contributions.size() == 1) return contributions.front().value;

  const std::string rule = entry ? entry->rule : "PRECEDENCE";
  std::vector<Contribution> winners;

  if (rule == "OR" || rule == "MAX") {
    winners = pick(contributions, [](const Contribution& c) { return c.value; }, true);
  } else if (rule == "AND" || rule == "MIN") {
    winners = pick(contributions, [](const Contribution& c) { return c.value; }, false);
  } else if (rule == "SEVERITY") {
    auto top = pick(contributions,
                    [](const Contribution& c) { return static_cast<double>(severity_rank(c.ragStatusCode, c.lifeSafety)); },
                    true);
    winners = pick(top, [](const Contribution& c) { return c.value; }, true);
  } else if (rule == "MEAN") {
    // Floating-point addition is not associative, so a parallel MEAN would not
    // be order-independent. The canonical order makes it deterministic; the sum
    // is serial within a cell, and cells stay independent.
    std::vector<Contribution> ordered = contributions;
    std::sort(ordered.begin(), ordered.end(), [](const Contribution& a, const Contribution& b) {
      if (a.originId != b.originId) return a.originId < b.originId;
      if (a.cesId != b.cesId) return a.cesId < b.cesId;
      return a.outputVectorId < b.outputVectorId;
    });
    double sum = 0.0;
    for (const auto& c : ordered) sum += c.value;
    double mean = sum / static_cast<double>(ordered.size());
    record = ArbitrationRecord{instant, cell, rule, mean, ordered, {}};
    return mean;
  } else {
    // PRECEDENCE — rank by determinism class rather than provider identity. A
    // deterministic contribution is derivable from the corpus and IS(k) alone; a
    // generated one is not derivable from anything, and letting the
    // irreproducible term win makes IS(k+1) irreproducible with it.
    auto rank_of = [&](const Contribution& c) {
      if (entry) {
        auto it = entry->providerRanks.find(c.provider);
        if (it != entry->providerRanks.end()) return static_cast<double>(it->second);
      }
      return static_cast<double>(determinism_rank(determinism_of(c.provider)));
    };
    auto atTop = pick(contributions, rank_of, true);
    const std::string within = entry ? entry->withinRank : "";
    if (within == "SEVERITY") {
      auto top = pick(atTop,
                      [](const Contribution& c) { return static_cast<double>(severity_rank(c.ragStatusCode, c.lifeSafety)); },
                      true);
      winners = pick(top, [](const Contribution& c) { return c.value; }, true);
    } else if (within == "MIN" || within == "AND") {
      winners = pick(atTop, [](const Contribution& c) { return c.value; }, false);
    } else {
      winners = pick(atTop, [](const Contribution& c) { return c.value; }, true);
    }
  }

  const double resolved = winners.front().value;
  std::vector<Contribution> suppressed;
  for (const auto& c : contributions) {
    bool won = std::any_of(winners.begin(), winners.end(), [&](const Contribution& w) {
      return w.originId == c.originId && w.cesId == c.cesId &&
             w.outputVectorId == c.outputVectorId && w.provider == c.provider && w.value == c.value;
    });
    if (!won) suppressed.push_back(c);
  }
  record = ArbitrationRecord{instant, cell, rule, resolved, contributions, suppressed};
  return resolved;
}

const char* determinism_name(Determinism d) {
  switch (d) {
    case Determinism::Deterministic: return "deterministic";
    case Determinism::Measured:      return "measured";
    case Determinism::Generated:     return "generated";
  }
  return "generated";
}

unsigned arbiter_shards() {
  const char* shardEnv = std::getenv("ARBITER_SHARDS");
  unsigned hw = std::thread::hardware_concurrency();
  unsigned shards = shardEnv ? static_cast<unsigned>(std::atoi(shardEnv)) : (hw ? hw : 1u);
  return shards == 0 ? 1u : shards;
}

std::vector<std::pair<int, double>> resolve_all(
    const std::map<int, std::vector<Contribution>>& byCell,
    int instant,
    std::vector<ArbitrationRecord>& records) {
  std::vector<std::pair<int, double>> out;
  out.reserve(byCell.size());
  records.clear();
  if (byCell.empty()) return out;

  // Cells never interact, so the resolve parallelises by cell range with no
  // shared mutable state. Correctness does not depend on the shard count —
  // every rule is a commutative monoid, which is exactly what makes any
  // partitioning safe (acceptance criterion 3).
  const unsigned shards = arbiter_shards();

  std::vector<std::pair<int, const std::vector<Contribution>*>> flat;
  flat.reserve(byCell.size());
  for (const auto& [cell, cs] : byCell) flat.emplace_back(cell, &cs);

  // Below this size the futures cost more than the work they parallelise.
  constexpr size_t kParallelThreshold = 256;
  if (shards <= 1 || flat.size() < kParallelThreshold) {
    for (const auto& [cell, cs] : flat) {
      std::optional<ArbitrationRecord> rec;
      double v = resolve_cell(cell, instant, *cs, ArbitrationRegistry::instance().entry_for(cell), rec);
      out.emplace_back(cell, v);
      if (rec) records.push_back(std::move(*rec));
    }
    return out;
  }

  const size_t chunk = (flat.size() + shards - 1) / shards;
  using Shard = std::pair<std::vector<std::pair<int, double>>, std::vector<ArbitrationRecord>>;
  std::vector<std::future<Shard>> futures;
  for (size_t begin = 0; begin < flat.size(); begin += chunk) {
    const size_t end = std::min(begin + chunk, flat.size());
    futures.push_back(std::async(std::launch::async, [&flat, begin, end, instant]() {
      Shard shard;
      for (size_t i = begin; i < end; ++i) {
        std::optional<ArbitrationRecord> rec;
        double v = resolve_cell(flat[i].first, instant, *flat[i].second,
                                ArbitrationRegistry::instance().entry_for(flat[i].first), rec);
        shard.first.emplace_back(flat[i].first, v);
        if (rec) shard.second.push_back(std::move(*rec));
      }
      return shard;
    }));
  }
  for (auto& f : futures) {
    Shard shard = f.get();
    out.insert(out.end(), shard.first.begin(), shard.first.end());
    records.insert(records.end(), shard.second.begin(), shard.second.end());
  }
  // Cell order is restored so the commit is byte-stable regardless of how the
  // work was partitioned or which shard finished first.
  std::sort(out.begin(), out.end());
  std::sort(records.begin(), records.end(),
            [](const ArbitrationRecord& a, const ArbitrationRecord& b) { return a.cell < b.cell; });
  return out;
}

}  // namespace reality
