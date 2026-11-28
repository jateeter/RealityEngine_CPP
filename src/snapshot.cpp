#include "reality/snapshot.hpp"

#include "reality/http.hpp"

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

// ── a snapshot's state, and nearest (SURFACE_SPEC "Nearest", #900) ──────────

State state_of(const Json& body) {
  State out;
  const Json& kline = body.at("kline");
  const double to = kline.at("span").at("to").as_number(-1);
  const Json& pairs = kline.at("pairs");
  if (!pairs.is_array()) return out;
  for (const auto& p : pairs.array()) {
    if (p.at("stepNumber").as_number(-2) != to) continue;
    const Json& nz = p.at("isre").at("nonZero");
    if (nz.is_array())
      for (const auto& c : nz.array()) out[static_cast<long>(c.at("index").as_number())] = c.at("value").as_number();
  }
  return out;
}

std::optional<State> parse_state(const Json& isre) {
  if (!isre.is_object() || !isre.at("nonZero").is_array()) return std::nullopt;
  State out;
  for (const auto& c : isre.at("nonZero").array()) {
    if (!c.at("index").is_number() || !c.at("value").is_number() || c.at("index").as_number() < 0)
      return std::nullopt;
    out[static_cast<long>(c.at("index").as_number())] = c.at("value").as_number();
  }
  return out;
}

double score(const State& query, const State& state) {
  double sum = 0;
  for (const auto& [cell, value] : query) {
    auto it = state.find(cell);
    if (it != state.end()) sum += value * it->second;
  }
  return sum;
}

namespace {

Json summary(const Json& b) {
  return Json::Object{{"id", id_of(b)}, {"digest", b.at("digest")}, {"scope", b.at("kline").at("scope")},
                      {"span", b.at("kline").at("span")}, {"machines", b.at("corpus").at("machines")}};
}

double lamport_of(const Json& b) { return b.at("kline").at("clocks").at("to").at("lamport").as_number(); }

// Oldest first by clocks.to.lamport, then id: every store lists alike.
void order_by_clock(std::vector<Json>& bodies) {
  std::sort(bodies.begin(), bodies.end(), [](const Json& a, const Json& b) {
    if (lamport_of(a) != lamport_of(b)) return lamport_of(a) < lamport_of(b);
    return id_of(a) < id_of(b);
  });
}

Json ranked(std::vector<std::pair<double, Json>> scored, std::size_t limit) {
  std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first > b.first;
    return id_of(a.second) < id_of(b.second);
  });
  Json::Array out;
  for (const auto& [sc, b] : scored) {
    if (out.size() >= limit) break;
    Json e = summary(b);
    e.object()["score"] = sc;
    out.push_back(e);
  }
  return Json::Object{{"snapshots", out}};
}

Json sparse(const State& state) {
  Json::Array indices, values;
  for (const auto& [cell, value] : state) {
    indices.push_back(static_cast<double>(cell));
    values.push_back(value);
  }
  return Json::Object{{"indices", indices}, {"values", values}};
}

}  // namespace

// ── the store ────────────────────────────────────────────────────────────────

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
  } else if (mode.rfind("qdrant:", 0) == 0) {
    std::string url = mode.substr(7);
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.rfind("https://", 0) != 0) {
      // Never a plaintext link (ENGINEERING_CONTRACT, data in motion).
      s.mode_ = Mode::Off;
      s.offReason_ = "RE_SNAPSHOT_STORE=qdrant: needs an https URL; the snapshot store is off";
    } else {
      s.mode_ = Mode::Qdrant;
      s.url_ = url;
      const char* c = std::getenv("RE_SNAPSHOT_COLLECTION");
      s.collection_ = c && *c ? c : "re-snapshots";
      const char* i = std::getenv("INSTANCE_UUID");
      s.instance_ = i && *i ? i : "unidentified";
    }
  } else {
    s.mode_ = Mode::Off;
    s.offReason_ = "RE_SNAPSHOT_STORE=" + mode + " is not memory, file:<dir>, qdrant:<url> or off; the snapshot store is off";
  }
  return s;
}

std::string Store::mode() const {
  switch (mode_) {
    case Mode::Memory: return "memory";
    case Mode::File: return "file";
    case Mode::Qdrant: return "qdrant";
    default: return "off";
  }
}

// One Qdrant call: its `result`, or StoreUnavailable. The RE stamps no
// outbound call (no client clock), so this never moves the engine's clock.
Json Store::qdrant(const std::string& method, const std::string& path, const Json& body) const {
  std::string raw;
  try {
    raw = http::request_json(method, url_ + path, body.is_null() ? "" : json::stringify(body));
  } catch (const std::exception& e) {
    throw StoreUnavailable(std::string("Qdrant: ") + e.what());
  }
  Json res;
  try {
    res = json::parse(raw);
  } catch (const std::exception&) {
    throw StoreUnavailable("Qdrant answered something that is not JSON");
  }
  if (res.at("status").as_string() != "ok") {
    std::string why = res.at("status").at("error").as_string();
    throw StoreUnavailable("Qdrant: " + (why.empty() ? std::string("an error") : why));
  }
  return res.at("result");
}

void Store::ensure_collection() const {
  if (collectionReady_) return;
  if (!qdrant("GET", "/collections/" + collection_ + "/exists", Json()).at("exists").as_bool()) {
    qdrant("PUT", "/collections/" + collection_,
           Json::Object{{"sparse_vectors", Json::Object{{"isre", Json::Object{}}}}});
    qdrant("PUT", "/collections/" + collection_ + "/index?wait=true",
           Json::Object{{"field_name", "instance"}, {"field_schema", "keyword"}});
  }
  collectionReady_ = true;
}

std::string Store::point_id(const std::string& id) const {
  const std::string h = kline::sha256_hex(instance_ + ":" + id);
  return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20, 12);
}

Json Store::instance_filter() const {
  return Json::Object{{"must", Json::Array{Json::Object{{"key", "instance"},
                                                        {"match", Json::Object{{"value", instance_}}}}}}};
}

void Store::put(const std::string& id, const Json& body) {
  if (mode_ == Mode::File) {
    const fs::path tmp = dir_ / (id + ".json.tmp");
    { std::ofstream out(tmp, std::ios::binary | std::ios::trunc); out << kline::jcs(body) << "\n"; }
    fs::rename(tmp, dir_ / (id + ".json"));
  } else if (mode_ == Mode::Qdrant) {
    ensure_collection();
    Json payload = Json::Object{{"instance", instance_}, {"id", id}, {"digest", body.at("digest")},
                                {"scope", body.at("kline").at("scope")}, {"span", body.at("kline").at("span")},
                                {"machines", body.at("corpus").at("machines")}, {"lamport", lamport_of(body)},
                                {"body", kline::jcs(body)}};
    Json point = Json::Object{{"id", point_id(id)}, {"vector", Json::Object{{"isre", sparse(state_of(body))}}},
                              {"payload", payload}};
    qdrant("PUT", "/collections/" + collection_ + "/points?wait=true", Json::Object{{"points", Json::Array{point}}});
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
  if (mode_ == Mode::Qdrant) {
    ensure_collection();
    Json res = qdrant("POST", "/collections/" + collection_ + "/points",
                      Json::Object{{"ids", Json::Array{point_id(id)}}, {"with_payload", true}});
    if (!res.is_array()) return std::nullopt;
    for (const auto& pt : res.array())
      if (pt.at("payload").at("instance").as_string() == instance_ && pt.at("payload").at("id").as_string() == id)
        return json::parse(pt.at("payload").at("body").as_string());
    return std::nullopt;
  }
  auto it = memory_.find(id);
  return it == memory_.end() ? std::nullopt : std::optional<Json>(it->second);
}

bool Store::remove(const std::string& id) {
  if (!get(id)) return false;
  if (mode_ == Mode::File) return fs::remove(dir_ / (id + ".json"));
  if (mode_ == Mode::Qdrant) {
    qdrant("POST", "/collections/" + collection_ + "/points/delete?wait=true",
           Json::Object{{"points", Json::Array{point_id(id)}}});
    return true;
  }
  return memory_.erase(id) > 0;
}

std::vector<Json> Store::bodies() const {
  std::vector<Json> out;
  if (mode_ == Mode::File) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir_, ec))
      if (e.path().extension() == ".json")
        if (auto b = get(e.path().stem().string())) out.push_back(*b);
  } else if (mode_ == Mode::Qdrant) {
    ensure_collection();
    Json offset;  // null: the first page
    for (;;) {
      Json req = Json::Object{{"filter", instance_filter()}, {"limit", 256.0}, {"with_payload", true},
                              {"with_vector", false}};
      if (!offset.is_null()) req.object()["offset"] = offset;
      Json res = qdrant("POST", "/collections/" + collection_ + "/points/scroll", req);
      if (res.at("points").is_array())
        for (const auto& pt : res.at("points").array()) out.push_back(json::parse(pt.at("payload").at("body").as_string()));
      offset = res.at("next_page_offset");
      if (offset.is_null()) break;
    }
  } else {
    for (const auto& [_, b] : memory_) out.push_back(b);
  }
  return out;
}

Json Store::list() const {
  std::vector<Json> all = bodies();
  order_by_clock(all);
  Json::Array out;
  for (const auto& b : all) out.push_back(summary(b));
  return Json::Object{{"snapshots", out}};
}

Json Store::nearest(const State& query, std::size_t limit) const {
  std::vector<std::pair<double, Json>> scored;
  if (query.empty()) return ranked(std::move(scored), limit);
  std::vector<Json> candidates;
  if (mode_ == Mode::Qdrant) {
    // Qdrant's candidates; the engine scores and orders them (ties at the
    // limit-th place are Qdrant's to break, SURFACE_SPEC "Nearest").
    ensure_collection();
    Json res = qdrant("POST", "/collections/" + collection_ + "/points/query",
                      Json::Object{{"query", sparse(query)}, {"using", "isre"}, {"filter", instance_filter()},
                                   {"limit", static_cast<double>(limit)}, {"with_payload", true}});
    if (res.at("points").is_array())
      for (const auto& pt : res.at("points").array())
        candidates.push_back(json::parse(pt.at("payload").at("body").as_string()));
  } else {
    candidates = bodies();
  }
  for (auto& b : candidates) {
    const double sc = score(query, state_of(b));
    if (sc > 0) scored.emplace_back(sc, std::move(b));
  }
  return ranked(std::move(scored), limit);
}

}  // namespace reality::snapshot
