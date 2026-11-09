#include "reality/clone_space.hpp"

#include <stdexcept>

namespace reality::clones {
namespace {

const Json& meta(const Machine& m, const std::string& key) {
  static const Json null;
  auto it = m.metadata.find(key);
  return it == m.metadata.end() ? null : it->second;
}

const Json::Array& edges(const Machine& m) {
  static const Json::Array none;
  const Json& e = meta(m, "interconnections");
  return e.is_array() ? e.array() : none;
}

bool is_producer(const Json& edge) {
  return edge.is_object() && edge.at("type").is_string() &&
         edge.at("type").as_string() == "published-domain-bus-producer";
}

// The machine's file stem: its corpus file name without ".json". A machine
// registered from a request body has no file, so no handoff can name it.
std::string file_stem(const Machine& m) {
  const std::string& f = m.sourceFile;
  if (f.size() > 5 && f.compare(f.size() - 5, 5, ".json") == 0) return f.substr(0, f.size() - 5);
  return f;
}

bool is_outgoing_handoff(const Json& edge, const std::string& stem) {
  return !stem.empty() && edge.is_object() && edge.at("type").is_string() &&
         edge.at("type").as_string() == "document-workflow-handoff" &&
         edge.at("sourceMachine").is_string() && edge.at("sourceMachine").as_string() == stem;
}

Verdict fail(const char* reason) { return Verdict{false, reason, std::nullopt, 0}; }

Json region(const RegionMapping& r) {
  return Json::Object{{"offset", static_cast<double>(r.offset)}, {"length", static_cast<double>(r.length)}};
}

}  // namespace

std::set<std::string> bus_target_files(const std::map<std::string, Machine>& machines) {
  std::set<std::string> out;
  for (const auto& [_, m] : machines)
    for (const auto& e : edges(m))
      if (is_producer(e) && e.at("targetMachineFile").is_string() && !e.at("targetMachineFile").as_string().empty())
        out.insert(e.at("targetMachineFile").as_string());
  return out;
}

Verdict assess(const Machine& m, const std::set<std::string>& busTargets) {
  const std::string stem = file_stem(m);
  int producers = 0, handoffs = 0, folded = 0;
  for (const auto& e : edges(m)) {
    if (is_producer(e)) {
      ++producers;
      if (e.at("targetInputElements").is_array()) folded += static_cast<int>(e.at("targetInputElements").array().size());
    } else if (is_outgoing_handoff(e, stem)) {
      ++handoffs;
    }
  }
  const Json& binding = meta(m, "agentBinding");
  // 1. It dispatches an agentic action.
  if (binding.is_null() && handoffs == 0) return fail("no-agentic-action");
  // 2. It has fanout.
  if (producers == 0 && handoffs == 0) return fail("no-fanout");
  // 3. It is not a bus target.
  if (!m.sourceFile.empty() && busTargets.count(m.sourceFile)) return fail("bus-target");
  // 4. Not life-safety.
  const Json& severity = meta(m, "severity");
  if (severity.is_string() && severity.as_string() == "life-safety") return fail("life-safety");
  // 5. Not a fixture.
  const Json& tags = meta(m, "tags");
  if (tags.is_array())
    for (const auto& t : tags.array())
      if (t.is_string() && t.as_string().find("fixture") != std::string::npos) return fail("fixture");
  // 6. Not opted out.
  const Json& opt = meta(m, "cloneable");
  if (opt.is_bool() && !opt.as_bool()) return fail("opted-out");
  // 7, 8. The machine's own regions fit their quarters. A machine with no
  // perceptualMapping has no region to move, and fails rule 7.
  if (!m.perceptualMapping || m.perceptualMapping->input.length > kQuarter) return fail("input-too-long");
  if (m.perceptualMapping->output.length > kQuarter) return fail("output-too-long");
  // 9. The cells its fanout folds into fit the fanout quarter.
  Verdict v{true, "", folded + kBusCells, folded};
  if (folded > kQuarter) { v.cloneable = false; v.reason = "fanout-too-wide"; }
  return v;
}

Json listing(const std::map<std::string, Machine>& machines) {
  const auto targets = bus_target_files(machines);
  Json::Array out;
  // std::map iterates in id order: the specification's sort.
  for (const auto& [id, m] : machines) {
    if (m.cloneOf) continue;  // a clone is never itself cloneable, and is not listed
    const Verdict v = assess(m, targets);
    out.push_back(Json::Object{
      {"id", id},
      {"cloneable", v.cloneable},
      {"reason", v.cloneable ? Json(nullptr) : Json(v.reason)},
      {"n", v.n ? Json(static_cast<double>(*v.n)) : Json(nullptr)}});
  }
  return Json::Object{{"machines", out}};
}

Json Clone::to_json() const {
  return Json::Object{
    {"cloneId", cloneId},
    {"machineId", machineId},
    {"engineInstance", engineInstance},
    {"envelopeId", envelopeId},
    {"assetId", assetId},
    {"block", Json::Object{{"index", static_cast<double>(block)}, {"offset", static_cast<double>(offset())},
                           {"length", static_cast<double>(kBlockCells)}}},
    {"input", region(input())},
    {"output", region(output())},
    {"fanout", region(fanout())},
    {"bus", region(bus())},
    {"generation", static_cast<double>(generation)}};
}

std::optional<int> Space::lowest_free_block() const {
  for (int k = 0; k < kBlocks; ++k)
    if (!byBlock.count(k)) return k;
  return std::nullopt;
}

int Space::live_of(const std::string& machineId) const {
  int n = 0;
  for (const auto& [_, c] : byBlock) if (c.machineId == machineId) ++n;
  return n;
}

const Clone* Space::by_id(const std::string& cloneId) const {
  for (const auto& [_, c] : byBlock) if (c.cloneId == cloneId) return &c;
  return nullptr;
}

const Clone* Space::by_envelope(const std::string& envelopeId) const {
  for (const auto& [_, c] : byBlock) if (c.envelopeId == envelopeId) return &c;
  return nullptr;
}

void Space::add(const Clone& clone) {
  if (byBlock.count(clone.block)) throw std::logic_error("clone block already held");
  byBlock.emplace(clone.block, clone);
}

long Space::recycle(const std::string& cloneId) {
  for (auto it = byBlock.begin(); it != byBlock.end(); ++it) {
    if (it->second.cloneId != cloneId) continue;
    const int block = it->first;
    byBlock.erase(it);
    return ++generations.at(static_cast<size_t>(block));
  }
  throw std::out_of_range("no live clone " + cloneId);
}

std::vector<Clone> Space::live() const {
  std::vector<Clone> out;
  for (const auto& [_, c] : byBlock) out.push_back(c);
  return out;
}

}  // namespace reality::clones
