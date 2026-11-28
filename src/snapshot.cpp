#include "reality/snapshot.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace reality::snapshot {
namespace fs = std::filesystem;

namespace {

Json entry_json(const TrajectoryEntry& e, const std::vector<RegionMapping>* keep) {
  Json::Array nz;
  for (const auto& c : e.nonZero) {
    bool in = keep == nullptr;
    if (keep)
      for (const auto& r : *keep)
        if (c.index >= r.offset && c.index < r.offset + r.length) { in = true; break; }
    if (in) nz.push_back(Json::Object{{"index", static_cast<double>(c.index)}, {"value", c.value}});
  }
  return Json::Object{{"stepNumber", static_cast<double>(e.stepNumber)}, {"length", static_cast<double>(e.length)},
                      {"nonZero", nz}};
}

Json clock_json(const std::string& instance, const Retained& r) {
  return Json::Object{{"instance", instance}, {"lamport", static_cast<double>(r.lamport)},
                      {"step", static_cast<double>(r.step)}};
}

std::vector<kline::ActiveEntry> only(const std::vector<kline::ActiveEntry>& list,
                                     const std::optional<std::set<std::string>>& scope) {
  if (!scope) return list;
  std::vector<kline::ActiveEntry> out;
  for (const auto& e : list) if (scope->count(e.machine)) out.push_back(e);
  return out;
}

}  // namespace

std::vector<kline::ActiveEntry> actives(const std::map<std::string, Machine>& running) {
  std::vector<kline::ActiveEntry> out;
  for (const auto& [_, m] : running)
    for (const auto& [sid, seq] : m.sequence_map())
      for (const auto& [eid, ev] : seq.event_map())
        if (ev.is_active()) out.push_back({m.name, sid, eid, ev.was_just_matched()});
  return out;
}

Json actives_json(std::vector<kline::ActiveEntry> list) {
  // std::string compares bytes: the declared UTF-8 collation. Duplicates are kept.
  std::stable_sort(list.begin(), list.end(), [](const kline::ActiveEntry& a, const kline::ActiveEntry& b) {
    if (a.machine != b.machine) return a.machine < b.machine;
    if (a.sequence != b.sequence) return a.sequence < b.sequence;
    return a.event < b.event;
  });
  Json::Array out;
  for (const auto& e : list)
    out.push_back(Json::Object{{"machine", e.machine}, {"sequence", e.sequence}, {"event", e.event},
                               {"justMatched", e.justMatched}});
  return out;
}

void Retention::on_reset(const std::map<std::string, Machine>&) {
  // The reset hook runs before the machines reset, so the base is taken at the
  // first step after it: every machine at its initial events.
  base_.clear();
  baseSet_ = false;
  steps_.clear();
}

void Retention::on_step(long step, long long lamport, const TrajectoryEntry* isre, const TrajectoryEntry* osre,
                        const SimulationStep& sim, const std::map<std::string, Machine>& running, size_t limit) {
  if (steps_.empty() && !baseSet_) {
    base_.clear();
    for (const auto& [_, m] : running)
      for (const auto& [sid, seq] : m.sequence_map())
        for (const auto& [eid, ev] : seq.event_map())
          if (ev.isInitial) base_.push_back({m.name, sid, eid, false});
    baseSet_ = true;
  }
  Retained r;
  r.step = step;
  r.lamport = lamport;
  if (isre) r.isre = *isre;
  if (osre) r.osre = *osre;
  for (const auto& op : sim.mergeBatch) r.ops.push_back({op.machineName, op.region, op.values, op.sequenceIds});
  r.after = actives(running);
  steps_.push_back(std::move(r));
  // Keep historyLimit steps; the newest one leaving becomes the base for the next.
  while (steps_.size() > std::max<size_t>(1, limit)) {
    base_ = steps_.front().after;
    steps_.pop_front();
  }
}

void Retention::restore(std::vector<kline::ActiveEntry> base) {
  base_ = std::move(base);
  baseSet_ = true;
  steps_.clear();
}

const Retained* Retention::at(long step) const {
  for (const auto& r : steps_) if (r.step == step) return &r;
  return nullptr;
}

std::optional<std::vector<kline::ActiveEntry>> Retention::before(long step) const {
  if (const Retained* prev = at(step - 1)) return prev->after;
  if (!steps_.empty() && steps_.front().step == step) return base_;
  if (steps_.empty()) return base_;
  return std::nullopt;
}

std::optional<Json> capture(const Retention& r, const CaptureRequest& req, Error& error) {
  if (r.steps().empty()) {
    error = {410, Json::Object{{"error", "no step is retained"}}};
    return std::nullopt;
  }
  const long newest = r.steps().back().step;
  const long to = req.to.value_or(req.from.value_or(newest));
  const long from = req.from.value_or(to);
  if (from > to) {
    error = {422, Json::Object{{"error", "span.from is after span.to"}}};
    return std::nullopt;
  }
  for (long s : {from, to})
    if (!r.at(s)) {
      error = {410, Json::Object{{"error", "step " + std::to_string(s) + " is no longer retained"}}};
      return std::nullopt;
    }
  auto before = r.before(from);
  if (!before) {
    error = {410, Json::Object{{"error", "step " + std::to_string(from - 1) + " is no longer retained"}}};
    return std::nullopt;
  }

  // The scope's regions, and the output cells it shares with a machine outside it.
  std::vector<RegionMapping> inputs, outputs, others;
  Json scopeJson = "corpus";
  if (req.scope) {
    Json::Array names;
    for (const auto& n : *req.scope) names.emplace_back(n);  // std::set: byte order
    scopeJson = names;
    for (const auto& [_, m] : *req.running) {
      if (!m.perceptualMapping) continue;
      if (req.scope->count(m.name)) {
        inputs.push_back(m.perceptualMapping->input);
        outputs.push_back(m.perceptualMapping->output);
      } else {
        others.push_back(m.perceptualMapping->output);
      }
    }
  }
  std::set<int> shared;
  for (const auto& o : outputs)
    for (const auto& x : others)
      for (int c = std::max(o.offset, x.offset); c < std::min(o.offset + o.length, x.offset + x.length); ++c)
        shared.insert(c);

  Json::Array pairs;
  for (long k = from; k <= to; ++k) {
    const Retained* s = r.at(k);
    if (!s) {
      error = {410, Json::Object{{"error", "step " + std::to_string(k) + " is no longer retained"}}};
      return std::nullopt;
    }
    std::vector<kline::Op> ops;
    for (const auto& op : s->ops) if (!req.scope || req.scope->count(op.machine)) ops.push_back(op);
    std::stable_sort(ops.begin(), ops.end(), [](const kline::Op& a, const kline::Op& b) {
      if (a.region.offset != b.region.offset) return a.region.offset < b.region.offset;
      return a.machine < b.machine;
    });
    Json::Array contribs;
    for (const auto& op : ops) {
      Json::Array values;
      for (double v : op.values) values.emplace_back(v);
      contribs.push_back(Json::Object{{"machine", op.machine},
                                      {"region", Json::Object{{"offset", static_cast<double>(op.region.offset)},
                                                              {"length", static_cast<double>(op.region.length)}}},
                                      {"values", values}});
    }
    pairs.push_back(Json::Object{{"stepNumber", static_cast<double>(k)},
                                 {"isre", entry_json(s->isre, req.scope ? &inputs : nullptr)},
                                 {"osre", entry_json(s->osre, req.scope ? &outputs : nullptr)},
                                 {"contributions", contribs}});
  }
  Json::Array sharedJson;
  for (int c : shared) sharedJson.emplace_back(static_cast<double>(c));

  Json body = Json::Object{
    {"format", "re-snapshot/1"},
    {"corpus", Json::Object{{"algorithm", "sha256/resident-declared-v1"}, {"fingerprint", req.fingerprint},
                            {"machines", static_cast<double>(req.machines)}}},
    {"controls", req.controls},
    {"machineControls", req.machineControls},
    {"machineOverrides", req.machineOverrides},
    {"kline", Json::Object{
      {"scope", scopeJson},
      {"span", Json::Object{{"from", static_cast<double>(from)}, {"to", static_cast<double>(to)}}},
      {"clocks", Json::Object{{"from", clock_json(req.instance, *r.at(from))}, {"to", clock_json(req.instance, *r.at(to))}}},
      {"activeBefore", actives_json(only(*before, req.scope))},
      {"pairs", pairs},
      {"activeAfter", actives_json(only(r.at(to)->after, req.scope))},
      {"sharedCells", sharedJson}}},
    {"arbitration", req.arbitration}};
  body.object()["digest"] = digest_of(body);
  return body;
}

std::string digest_of(const Json& body) {
  Json copy = body;
  copy.object().erase("digest");
  if (copy.at("kline").is_object() && copy.at("kline").at("clocks").is_object()) {
    for (const char* end : {"from", "to"}) {
      Json& c = copy.object()["kline"].object()["clocks"].object()[end];
      if (c.is_object()) { c.object().erase("instance"); c.object().erase("lamport"); }
    }
  }
  return "sha256:" + kline::sha256_hex(kline::jcs(copy));
}

std::string id_of(const Json& body) {
  std::string d = body.at("digest").as_string();
  return d.rfind("sha256:", 0) == 0 ? d.substr(7) : d;
}

Store Store::from_environment() {
  Store s;
  const char* v = std::getenv("RE_SNAPSHOT_STORE");
  std::string mode = v ? v : "memory";
  if (mode.empty() || mode == "memory") {
    s.mode_ = Mode::Memory;
  } else if (mode == "off") {
    s.mode_ = Mode::Off;
  } else if (mode.rfind("file:", 0) == 0) {
    s.mode_ = Mode::File;
    s.dir_ = mode.substr(5);
    std::error_code ec;
    fs::create_directories(s.dir_, ec);
  } else {
    // qdrant:<url> is phase 3: off until it exists, and never a boot failure.
    s.mode_ = Mode::Off;
  }
  return s;
}

std::string Store::mode() const {
  return mode_ == Mode::Memory ? "memory" : mode_ == Mode::File ? "file" : "off";
}

void Store::put(const std::string& id, const Json& body) {
  if (mode_ == Mode::File) {
    const fs::path tmp = dir_ / (id + ".json.tmp");
    { std::ofstream out(tmp, std::ios::binary | std::ios::trunc); out << kline::jcs(body) << "\n"; }
    fs::rename(tmp, dir_ / (id + ".json"));
  } else {
    memory_[id] = body;
  }
}

std::optional<Json> Store::get(const std::string& id) const {
  if (id.empty() || id.find_first_not_of("0123456789abcdef") != std::string::npos) return std::nullopt;
  if (mode_ == Mode::File) {
    std::ifstream in(dir_ / (id + ".json"), std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
  }
  auto it = memory_.find(id);
  return it == memory_.end() ? std::nullopt : std::optional<Json>(it->second);
}

bool Store::remove(const std::string& id) {
  if (!get(id)) return false;
  if (mode_ == Mode::File) return fs::remove(dir_ / (id + ".json"));
  return memory_.erase(id) > 0;
}

Json Store::list() const {
  std::vector<Json> bodies;
  if (mode_ == Mode::File) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir_, ec))
      if (e.path().extension() == ".json")
        if (auto b = get(e.path().stem().string())) bodies.push_back(*b);
  } else {
    for (const auto& [_, b] : memory_) bodies.push_back(b);
  }
  std::stable_sort(bodies.begin(), bodies.end(), [](const Json& a, const Json& b) {
    return a.at("kline").at("clocks").at("to").at("lamport").as_number() <
           b.at("kline").at("clocks").at("to").at("lamport").as_number();
  });
  Json::Array out;
  for (const auto& b : bodies)
    out.push_back(Json::Object{{"id", id_of(b)}, {"digest", b.at("digest")}, {"scope", b.at("kline").at("scope")},
                               {"span", b.at("kline").at("span")}, {"machines", b.at("corpus").at("machines")}});
  return Json::Object{{"snapshots", out}};
}

}  // namespace reality::snapshot
