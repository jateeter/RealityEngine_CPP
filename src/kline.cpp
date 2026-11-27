#include "reality/kline.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <fcntl.h>
#include <unistd.h>

namespace reality::kline {
namespace fs = std::filesystem;

// ── JCS, SHA-256, the corpus fingerprint ────────────────────────────────────

namespace {

void append_jcs_string(std::string& out, const std::string& s) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

// JCS orders keys by UTF-16 code units. UTF-8 byte order agrees except where a
// supplementary character (4 bytes) meets U+E000..U+FFFF (3 bytes, lead byte
// 0xEE/0xEF): UTF-16 puts the surrogate pair (0xD800..) first.
std::u16string utf16(const std::string& s) {
  std::u16string out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    char32_t cp;
    size_t n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c >> 5) == 0x6) { cp = c & 0x1F; n = 2; }
    else if ((c >> 4) == 0xE) { cp = c & 0x0F; n = 3; }
    else { cp = c & 0x07; n = 4; }
    for (size_t k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    i += n;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
  }
  return out;
}

void append_jcs(std::string& out, const Json& v) {
  if (v.is_null()) { out += "null"; return; }
  if (v.is_bool()) { out += v.as_bool() ? "true" : "false"; return; }
  if (v.is_number()) { out += json::format_double(v.as_number()); return; }
  if (v.is_string()) { append_jcs_string(out, v.as_string()); return; }
  if (v.is_array()) {
    out.push_back('[');
    bool first = true;
    for (const auto& item : v.array()) {
      if (!first) out.push_back(',');
      first = false;
      append_jcs(out, item);
    }
    out.push_back(']');
    return;
  }
  std::vector<const std::pair<const std::string, Json>*> entries;
  for (const auto& kv : v.object()) entries.push_back(&kv);
  std::sort(entries.begin(), entries.end(), [](auto* a, auto* b) { return utf16(a->first) < utf16(b->first); });
  out.push_back('{');
  bool first = true;
  for (auto* kv : entries) {
    if (!first) out.push_back(',');
    first = false;
    append_jcs_string(out, kv->first);
    out.push_back(':');
    append_jcs(out, kv->second);
  }
  out.push_back('}');
}

}  // namespace

std::string jcs(const Json& value) {
  std::string out;
  append_jcs(out, value);
  return out;
}

std::string sha256_hex(const std::string& bytes) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int n = 0;
  if (EVP_Digest(bytes.data(), bytes.size(), md, &n, EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("SHA-256 failed");
  std::ostringstream hex;
  for (unsigned int i = 0; i < n; ++i) hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(md[i]);
  return hex.str();
}

std::string corpus_fingerprint(const std::vector<std::pair<std::string, Json>>& declared) {
  std::vector<std::pair<std::string, std::string>> lines;
  lines.reserve(declared.size());
  for (const auto& [name, machine] : declared) lines.emplace_back(name, sha256_hex(jcs(machine)));
  std::sort(lines.begin(), lines.end());  // by name bytes, then hex
  std::string all;
  for (const auto& [name, hex] : lines) all += name + "\t" + hex + "\n";
  return sha256_hex(all);
}

Json Stamp::json() const {
  return Json::Object{{"instance", instance}, {"lamport", static_cast<double>(lamport)},
                      {"commit", static_cast<double>(commit)}, {"step", static_cast<double>(step)}};
}

Stamp Stamp::from(const Json& j) {
  Stamp s;
  s.instance = j.at("instance").as_string();
  s.lamport = static_cast<long long>(j.at("lamport").as_number());
  s.commit = static_cast<long long>(j.at("commit").as_number());
  s.step = static_cast<long>(j.at("step").as_number());
  return s;
}

// ── The store ───────────────────────────────────────────────────────────────

namespace {

void write_durable(const fs::path& path, const std::string& bytes) {
  const fs::path tmp = path.string() + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) throw std::runtime_error("cannot write " + tmp.string());
  const char* p = bytes.data();
  size_t left = bytes.size();
  while (left > 0) {
    ssize_t w = ::write(fd, p, left);
    if (w < 0) { ::close(fd); throw std::runtime_error("write failed: " + tmp.string()); }
    p += w;
    left -= static_cast<size_t>(w);
  }
  ::fsync(fd);
  ::close(fd);
  fs::rename(tmp, path);
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

long long segment_commit(const std::string& name) {
  // segment-<commit>.jsonl
  const auto dash = name.find('-');
  const auto dot = name.rfind('.');
  if (dash == std::string::npos || dot == std::string::npos || dot <= dash) return -1;
  try { return std::stoll(name.substr(dash + 1, dot - dash - 1)); } catch (...) { return -1; }
}

}  // namespace

Store::Store(fs::path dir) : dir_(std::move(dir)) {
  if (dir_.empty()) return;
  fs::create_directories(dir_);
  load();
}

void Store::load() {
  std::map<std::string, Segment> known;
  if (fs::exists(dir_ / "manifest.json")) {
    Json m = json::parse(read_file(dir_ / "manifest.json"));
    for (const auto& s : m.at("segments").array()) {
      Segment seg;
      seg.name = s.at("segment").as_string();
      seg.sealed = s.at("sealed").as_bool();
      seg.exported = s.at("exported").as_bool();
      seg.sha256 = s.at("sha256").as_string();
      seg.fingerprint = s.at("fingerprint").as_string();
      seg.archived = s.at("archived").as_bool();
      known[seg.name] = seg;
    }
  }
  std::vector<std::string> names;
  for (const auto& e : fs::directory_iterator(dir_)) {
    const auto n = e.path().filename().string();
    if (n.rfind("segment-", 0) == 0 && e.path().extension() == ".jsonl" && segment_commit(n) >= 0) names.push_back(n);
  }
  std::sort(names.begin(), names.end(), [](const auto& a, const auto& b) { return segment_commit(a) < segment_commit(b); });
  for (size_t i = 0; i < names.size(); ++i) {
    const fs::path path = dir_ / names[i];
    std::string bytes = read_file(path);
    // A half-written last record is truncated: everything after the last
    // newline, or a final line that does not parse.
    size_t good = bytes.rfind('\n');
    good = good == std::string::npos ? 0 : good + 1;
    if (good != bytes.size()) {
      fs::resize_file(path, good);
      bytes.resize(good);
    }
    Segment seg = known.count(names[i]) ? known[names[i]] : Segment{};
    seg.name = names[i];
    seg.firstCommit = segment_commit(names[i]);
    seg.bytes = bytes.size();
    std::uint64_t offset = 0;
    std::istringstream lines(bytes);
    std::string line;
    while (std::getline(lines, line)) {
      try {
        index_line(line, seg.name, offset, 0);
      } catch (...) {
        // An unparseable line inside a segment: leave it, it is not a record.
      }
      offset += line.size() + 1;
    }
    segments_.push_back(seg);
  }
  write_index();
}

void Store::index_line(const std::string& raw, const std::string& segment, std::uint64_t offset, size_t line) {
  Json j = json::parse(raw);
  const std::string format = j.at("format").as_string();
  if (format == "re-kline/1") {
    Meta m;
    m.id = j.at("id").as_string();
    m.machine = j.at("focal").at("machine").as_string();
    m.sequence = j.at("focal").at("sequence").as_string();
    m.outcome = j.at("outcome").as_string();
    m.closeCommit = static_cast<long long>(j.at("close").at("commit").as_number());
    m.closeStep = static_cast<long>(j.at("close").at("step").as_number());
    m.openStep = static_cast<long>(j.at("open").at("step").as_number());
    m.segment = segment;
    m.offset = offset;
    m.line = line;
    byId_[m.id] = metas_.size();
    metas_.push_back(std::move(m));
  } else if (format == "re-kline-event/1") {
    events_[j.at("kline").as_string()].emplace_back(static_cast<long>(j.at("through").as_number()), raw);
  }
}

Store::Segment& Store::active_segment(const std::string& fingerprint, long long commit) {
  if (!segments_.empty()) {
    Segment& last = segments_.back();
    if (!last.sealed) {
      const bool otherCorpus = !last.fingerprint.empty() && last.fingerprint != fingerprint && last.bytes > 0;
      if (!otherCorpus && last.bytes < kSealBytes) {
        if (last.fingerprint.empty()) last.fingerprint = fingerprint;
        return last;
      }
      seal(last);
    }
  }
  Segment s;
  s.name = "segment-" + std::to_string(commit) + ".jsonl";
  s.firstCommit = commit;
  s.fingerprint = fingerprint;
  segments_.push_back(s);
  write_manifest();
  return segments_.back();
}

void Store::seal(Segment& s) {
  s.sealed = true;
  if (persistent()) s.sha256 = sha256_hex(read_file(dir_ / s.name));
  write_manifest();
}

void Store::append(const std::vector<Json>& lines, const std::string& fingerprint, long long commit) {
  if (lines.empty()) return;
  Segment& seg = active_segment(fingerprint, commit);
  std::string bytes;
  std::vector<std::pair<std::string, std::uint64_t>> raws;
  std::uint64_t offset = seg.bytes;
  for (const auto& l : lines) {
    std::string raw = jcs(l);
    raws.emplace_back(raw, offset);
    offset += raw.size() + 1;
    bytes += raw;
    bytes.push_back('\n');
  }
  if (persistent()) {
    const fs::path path = dir_ / seg.name;
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) throw std::runtime_error("cannot append to " + path.string());
    const char* p = bytes.data();
    size_t left = bytes.size();
    while (left > 0) {
      ssize_t w = ::write(fd, p, left);
      if (w < 0) { ::close(fd); throw std::runtime_error("append failed: " + path.string()); }
      p += w;
      left -= static_cast<size_t>(w);
    }
    ::fsync(fd);  // durable before the step's completion is published
    ::close(fd);
  }
  for (auto& [raw, off] : raws) {
    if (!persistent()) lines_.push_back(raw);
    index_line(raw, seg.name, off, persistent() ? 0 : lines_.size() - 1);
  }
  seg.bytes += bytes.size();
  seg.lastCommit = commit;
  // index.json is a cache of what the segments hold, rebuilt at boot; it is
  // rewritten when a segment seals and every 64 appends, not on every step.
  static thread_local unsigned appends = 0;
  if (++appends % 64 == 0) write_index();
}

void Store::checkpoint(const Json& open) {
  if (!persistent()) return;
  write_durable(dir_ / "open.json", jcs(open) + "\n");
}

std::optional<Json> Store::read_checkpoint() const {
  if (!persistent() || !fs::exists(dir_ / "open.json")) return std::nullopt;
  try {
    return json::parse(read_file(dir_ / "open.json"));
  } catch (...) {
    return std::nullopt;
  }
}

std::string Store::raw_at(const Meta& m) const {
  if (!persistent()) return lines_.at(m.line);
  std::ifstream in(dir_ / m.segment, std::ios::binary);
  in.seekg(static_cast<std::streamoff>(m.offset));
  std::string line;
  std::getline(in, line);
  return line;
}

std::vector<std::string> Store::raw_events(const std::string& id) const {
  std::vector<std::string> out;
  auto it = events_.find(id);
  if (it != events_.end())
    for (const auto& [_, raw] : it->second) out.push_back(raw);
  return out;
}

Json Store::record_at(const Meta& m) const {
  Json j = json::parse(raw_at(m));
  // A sustain-end appended later belongs on the record as served.
  auto it = events_.find(m.id);
  if (it != events_.end() && !it->second.empty()) {
    j.object()["sustain"] = Json::Object{{"through", static_cast<double>(it->second.back().first)}, {"ended", true}};
  }
  return j;
}

std::optional<Json> Store::record(const std::string& id) const {
  auto it = byId_.find(id);
  if (it == byId_.end()) return std::nullopt;
  return record_at(metas_[it->second]);
}

Json Store::stats(const Controls& controls) const {
  Json::Object byOutcome, byMachine;
  for (const auto& m : metas_) {
    byOutcome[m.outcome] = byOutcome[m.outcome].as_number() + 1;
    Json& per = byMachine[m.machine];
    if (!per.is_object()) per = Json::Object{};
    per.object()[m.outcome] = per.object()[m.outcome].as_number() + 1;
  }
  std::uint64_t bytes = 0;
  size_t live = 0;
  for (const auto& s : segments_) if (!s.archived) { bytes += s.bytes; ++live; }
  Json::Array warns;
  for (const auto& w : warnings(controls)) warns.emplace_back(w);
  return Json::Object{{"byOutcome", byOutcome}, {"byMachine", byMachine},
                      {"segments", static_cast<double>(live)}, {"bytes", static_cast<double>(bytes)},
                      {"budget", static_cast<double>(controls.storeMaxBytes)}, {"warnings", warns}};
}

std::vector<std::string> Store::warnings(const Controls& controls) const {
  std::uint64_t sealed = 0;
  size_t unexported = 0;
  for (const auto& s : segments_) {
    if (s.archived || !s.sealed) continue;
    sealed += s.bytes;
    if (!s.exported) ++unexported;
  }
  std::vector<std::string> out;
  if (static_cast<long long>(sealed) > controls.storeMaxBytes && unexported > 0)
    out.push_back("klineStoreMaxBytes reached: " + std::to_string(unexported) +
                  " unexported sealed segment(s) are kept; export them to free space");
  return out;
}

void Store::mark_exported(const std::set<std::string>& names) {
  for (auto& s : segments_) if (names.count(s.name)) s.exported = true;
  write_manifest();
}

void Store::enforce_budget(const Controls& controls) {
  auto sealedBytes = [&] {
    std::uint64_t b = 0;
    for (const auto& s : segments_) if (s.sealed && !s.archived) b += s.bytes;
    return b;
  };
  bool moved = false;
  while (static_cast<long long>(sealedBytes()) > controls.storeMaxBytes) {
    auto it = std::find_if(segments_.begin(), segments_.end(),
                           [](const Segment& s) { return s.sealed && s.exported && !s.archived; });
    if (it == segments_.end()) break;  // never evict an unexported segment
    if (persistent()) {
      fs::create_directories(dir_ / "archive");
      fs::rename(dir_ / it->name, dir_ / "archive" / it->name);
    }
    it->archived = true;
    moved = true;
  }
  if (!moved) return;
  // Archived records leave the live population.
  std::vector<Meta> kept;
  byId_.clear();
  std::set<std::string> gone;
  for (const auto& s : segments_) if (s.archived) gone.insert(s.name);
  for (auto& m : metas_) {
    if (gone.count(m.segment)) continue;
    byId_[m.id] = kept.size();
    kept.push_back(std::move(m));
  }
  metas_ = std::move(kept);
  write_manifest();
  write_index();
}

void Store::write_manifest() const {
  if (!persistent()) return;
  Json::Array segs;
  for (const auto& s : segments_)
    segs.push_back(Json::Object{{"segment", s.name}, {"sealed", s.sealed}, {"exported", s.exported},
                                {"sha256", s.sha256}, {"fingerprint", s.fingerprint}, {"archived", s.archived},
                                {"bytes", static_cast<double>(s.bytes)}});
  write_durable(dir_ / "manifest.json", jcs(Json::Object{{"segments", segs}}) + "\n");
}

void Store::write_index() const {
  if (!persistent()) return;
  Json::Object ids, counts;
  for (const auto& m : metas_) {
    ids[m.id] = Json::Object{{"segment", m.segment}, {"offset", static_cast<double>(m.offset)}};
    const std::string key = m.machine + "\t" + m.sequence;
    Json& c = counts[key];
    if (!c.is_object()) c = Json::Object{};
    c.object()[m.outcome] = c.object()[m.outcome].as_number() + 1;
  }
  write_durable(dir_ / "index.json", jcs(Json::Object{{"ids", ids}, {"counts", counts}}) + "\n");
}

// ── The tracker ─────────────────────────────────────────────────────────────

namespace {

bool overlaps(const RegionMapping& a, const RegionMapping& b) {
  return a.length > 0 && b.length > 0 && a.offset < b.offset + b.length && b.offset < a.offset + a.length;
}

Json entry_json(const TrajectoryEntry& e, const std::vector<RegionMapping>& keep) {
  Json::Array nz;
  for (const auto& c : e.nonZero) {
    bool in = keep.empty();
    for (const auto& r : keep)
      if (c.index >= r.offset && c.index < r.offset + r.length) { in = true; break; }
    if (in) nz.push_back(Json::Object{{"index", static_cast<double>(c.index)}, {"value", c.value}});
  }
  return Json::Object{{"stepNumber", static_cast<double>(e.stepNumber)}, {"length", static_cast<double>(e.length)},
                      {"nonZero", nz}};
}

Json vector_json(const Vector& v) {
  Json::Array a;
  for (double x : v) a.emplace_back(x);
  return a;
}

Json actives_json(std::vector<ActiveEntry> list) {
  std::sort(list.begin(), list.end(), [](const ActiveEntry& a, const ActiveEntry& b) {
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

std::string comparator_name(ComparatorType t) {
  return t == ComparatorType::Custom ? "equals" : to_string(t);
}

Json threshold_of(ComparatorType t, const VectorElement& e) {
  switch (t) {
    case ComparatorType::Equals:
    case ComparatorType::Custom: return nullptr;
    case ComparatorType::Threshold: return e.threshold.value_or(0.1);
    case ComparatorType::Pattern: return e.threshold.value_or(0.5);
    case ComparatorType::Gte: return e.threshold.value_or(0.5);
  }
  return nullptr;
}

// The first comparison that failed for `event` against `input`.
Json difference_of(const RealityEvent& event, const Vector& input, const RegionMapping& region) {
  MatchResult mr = event.match(input);
  size_t idx = 0;
  if (auto it = mr.metadata.find("failedAtIndex"); it != mr.metadata.end()) idx = static_cast<size_t>(it->second.as_number());
  if (idx >= event.elements.size()) idx = 0;
  const VectorElement* elem = event.elements.empty() ? nullptr : &event.elements[idx];
  ComparatorType t = elem ? elem->comparatorType.value_or(event.matchAlgorithm) : event.matchAlgorithm;
  return Json::Object{
    {"expected", Json::Array{event.id}},
    {"elementIndex", static_cast<double>(idx)},
    {"cell", static_cast<double>(region.offset + static_cast<int>(idx))},
    {"value", idx < input.size() ? input[idx] : 0.0},
    {"comparator", comparator_name(t)},
    {"threshold", elem ? threshold_of(t, *elem) : Json(nullptr)},
    {"cells", Json::Array{static_cast<double>(region.offset), static_cast<double>(region.offset + region.length - 1)}}};
}

std::vector<ActiveEntry> initial_actives(const Machine& m) {
  std::vector<ActiveEntry> out;
  for (const auto& [sid, seq] : m.sequence_map())
    for (const auto& [eid, ev] : seq.event_map())
      if (ev.isInitial) out.push_back({m.name, sid, eid, false});
  return out;
}

}  // namespace

Tracker::Tracker(std::string instance, Store store, Tick tick)
    : instance_(std::move(instance)), store_(std::move(store)), tick_(std::move(tick)) {}

Stamp Tracker::stamp(long long lamport, long step, long long commit) const {
  return Stamp{instance_, lamport, commit, step};
}

std::vector<ActiveEntry> Tracker::actives_of(const Machine& m) {
  std::vector<ActiveEntry> out;
  for (const auto& [sid, seq] : m.sequence_map())
    for (const auto& [eid, ev] : seq.event_map())
      if (ev.is_active()) out.push_back({m.name, sid, eid, ev.was_just_matched()});
  return out;
}

const Tracker::StepRecord* Tracker::record_for(long step) const {
  for (const auto& r : ring_) if (r.step == step) return &r;
  return nullptr;
}

std::vector<ActiveEntry> Tracker::active_at(const std::string& machine, long step) const {
  // The newest change at or before `step`, else the state before the ring.
  for (auto it = ring_.rbegin(); it != ring_.rend(); ++it) {
    if (it->step > step) continue;
    auto c = it->changed.find(machine);
    if (c != it->changed.end()) return c->second;
  }
  auto b = baseline_.find(machine);
  return b == baseline_.end() ? std::vector<ActiveEntry>{} : b->second;
}

std::vector<std::string> Tracker::open_ids() const {
  std::vector<std::string> out;
  for (const auto& k : open_) out.push_back(k.id);
  for (const auto& s : sustain_) out.push_back(s.id);
  return out;
}

void Tracker::on_step(const StepView& v) {
  if (!controls_.capture || !v.simulation || !v.running) return;
  const SimulationStep& sim = *v.simulation;
  const auto& running = *v.running;
  const long k = v.step;
  fingerprint_ = v.fingerprint;

  // Machines by corpus name, and their step results.
  std::map<std::string, const Machine*> byName;
  for (const auto& [id, m] : running) byName[m.name] = &m;
  std::map<std::string, const MachineStepResult*> results;
  for (const auto& [id, r] : sim.machineResults) {
    auto it = running.find(id);
    if (it != running.end()) results[it->second.name] = &r;
  }
  if (ring_.empty() && current_.empty()) {
    for (const auto& [name, m] : byName) baseline_[name] = initial_actives(*m);
  }

  // ── The step record (pushed now: closes this step read it) ──
  StepRecord rec;
  rec.step = k;
  rec.commit = v.commit;
  if (v.isre) rec.isre = *v.isre;
  if (v.osre) rec.osre = *v.osre;
  for (const auto& op : sim.mergeBatch) rec.ops.push_back({op.machineName, op.region, op.values, op.sequenceIds});
  for (const auto& [name, m] : byName) {
    if (m->perceptualMapping)
      rec.regions[name] = {m->perceptualMapping->input, std::optional<RegionMapping>(m->perceptualMapping->output)};
    auto now = actives_of(*m);
    auto prev = current_.find(name);
    const std::vector<ActiveEntry>& before = prev != current_.end() ? prev->second
                                             : (baseline_.count(name) ? baseline_[name] : initial_actives(*m));
    if (!(now == before)) rec.changed[name] = now;
    current_[name] = std::move(now);
  }

  // ── Boundaries, per sequence ──
  struct Touch {
    size_t openIndex = SIZE_MAX;  // into open_ (existing or new)
    bool isNew = false;
    int milestones = 0;
    std::string closeOutcome;     // empty: still open
    Json difference = nullptr;
    std::string subsumedBy;
    std::string sustainEndOf;     // a sustain that ended (Sustain id)
    long sustainThrough = 0;
    std::string machine, sequence;
    long openStep = 0;
  };
  std::vector<Touch> touches;
  std::vector<Open> opened;

  // Who held each event before this step: (machine, sequence, event) -> id.
  std::map<std::tuple<std::string, std::string, std::string>, std::string> holder;
  for (const auto& kl : open_)
    for (const auto& e : kl.held) holder[{kl.machine, kl.sequence, e}] = kl.id;
  for (const auto& s : sustain_) holder[{s.machine, s.sequence, s.event}] = s.id;

  std::vector<bool> stillOpen(open_.size(), true);

  for (const auto& [name, m] : byName) {
    auto rit = results.find(name);
    if (rit == results.end()) continue;
    const auto& seqResults = rit->second->transitionResult.sequenceResults;
    const Vector& input = rit->second->transitionResult.inputVector;
    const RegionMapping inRegion = m->perceptualMapping ? m->perceptualMapping->input : RegionMapping{};
    for (const auto& [sid, seq] : m->sequence_map()) {
      auto sr = seqResults.find(sid);
      if (sr == seqResults.end()) continue;
      const std::set<std::string> matched(sr->second.matchedVectors.begin(), sr->second.matchedVectors.end());
      const std::set<std::string> activated(sr->second.activatedVectors.begin(), sr->second.activatedVectors.end());
      const auto& events = seq.event_map();
      auto isActive = [&](const std::string& e) { auto it = events.find(e); return it != events.end() && it->second.is_active(); };
      std::map<std::string, std::string> claimed;  // successor -> K-line id (or "#new")

      auto pathEntry = [&](const RealityEvent& ev) {
        PathEntry p;
        p.event = ev.id;
        p.step = k;
        p.milestone = !ev.output_vectors().empty() && !ev.next_vector_ids().empty();
        Json::Array outs;
        for (const auto& o : ev.output_vectors()) outs.push_back(vector_json(o.vector));
        p.outputs = outs;
        return p;
      };

      // Existing K-lines of this sequence, older first.
      std::vector<size_t> mine;
      for (size_t i = 0; i < open_.size(); ++i)
        if (open_[i].machine == name && open_[i].sequence == sid) mine.push_back(i);
      std::sort(mine.begin(), mine.end(), [&](size_t a, size_t b) {
        if (open_[a].open.step != open_[b].open.step) return open_[a].open.step < open_[b].open.step;
        return open_[a].activator < open_[b].activator;
      });
      for (size_t i : mine) {
        Open& kl = open_[i];
        Touch t;
        t.openIndex = i;
        t.machine = name;
        t.sequence = sid;
        t.openStep = kl.open.step;
        std::set<std::string> next;
        std::string completedBy;
        std::string firstFailed;
        for (const auto& e : kl.held) {
          auto eit = events.find(e);
          if (eit == events.end()) continue;
          const RealityEvent& ev = eit->second;
          if (matched.count(e)) {
            kl.path.push_back(pathEntry(ev));
            if (kl.path.back().milestone) ++t.milestones;
            if (ev.next_vector_ids().empty()) {
              if (completedBy.empty()) completedBy = e;
              continue;
            }
            for (const auto& n : ev.next_vector_ids()) {
              if (n == e) {  // a dwell: re-activated, still this K-line's
                if (isActive(n)) next.insert(n);
                continue;
              }
              if (activated.count(n)) {
                auto c = claimed.find(n);
                if (c == claimed.end()) { claimed[n] = kl.id; next.insert(n); }
                else if (kl.refusedBy.empty()) kl.refusedBy = c->second;
              } else {
                auto h = holder.find({name, sid, n});
                if (h != holder.end() && h->second != kl.id) {
                  if (kl.refusedBy.empty()) kl.refusedBy = h->second;
                } else if (h != holder.end()) {
                  next.insert(n);  // already this K-line's
                }
              }
            }
            // An event with outputs stays active after it matches (a milestone).
            if (!ev.output_vectors().empty() && isActive(e)) next.insert(e);
          } else if (firstFailed.empty()) {
            firstFailed = e;
          }
        }
        if (!completedBy.empty()) {
          t.closeOutcome = "completed";
          sustain_.push_back({"", name, sid, completedBy, k});  // id filled when ticked
          t.sustainThrough = k;
        } else if (next.empty()) {
          if (!kl.refusedBy.empty()) {
            t.closeOutcome = "subsumed";
            t.subsumedBy = kl.refusedBy;
          } else {
            t.closeOutcome = "abandoned";
            const std::string e = firstFailed.empty() ? *kl.held.begin() : firstFailed;
            t.difference = difference_of(events.at(e), input, inRegion);
          }
        }
        kl.held = std::move(next);
        if (!t.closeOutcome.empty()) stillOpen[i] = false;
        touches.push_back(std::move(t));
      }

      // Sustains of this sequence.
      for (auto& s : sustain_) {
        if (s.machine != name || s.sequence != sid || s.id.empty()) continue;
        if (matched.count(s.event)) {
          s.through = k;
        } else {
          Touch t;
          t.machine = name;
          t.sequence = sid;
          t.sustainEndOf = s.id;
          t.sustainThrough = s.through;
          auto c = closed_.find(s.id);
          t.openStep = c != closed_.end() ? c->second.openStep : s.through;
          touches.push_back(std::move(t));
          s.through = -1;  // removed below
        }
      }

      // A departure: the initial event matched.
      for (const auto& [eid, ev] : events) {
        if (!ev.isInitial || !matched.count(eid)) continue;
        bool sustaining = false;
        for (const auto& s : sustain_)
          if (s.machine == name && s.sequence == sid && s.event == eid && s.through == k) sustaining = true;
        if (sustaining) continue;
        Open kl;
        kl.machine = name;
        kl.sequence = sid;
        kl.activator = eid;
        kl.open = stamp(0, k, v.commit);
        kl.input = inRegion;
        if (m->perceptualMapping) kl.output = m->perceptualMapping->output;
        kl.path.push_back(pathEntry(ev));
        Touch t;
        t.isNew = true;
        t.machine = name;
        t.sequence = sid;
        t.openStep = k;
        if (kl.path.back().milestone) ++t.milestones;
        if (ev.next_vector_ids().empty()) {
          t.closeOutcome = "completed";  // a one-event sequence opens and completes
          sustain_.push_back({"", name, sid, eid, k});
          t.sustainThrough = k;
        } else {
          for (const auto& n : ev.next_vector_ids()) {
            if (n == eid) continue;  // an initial event that lists itself
            if (activated.count(n) && !claimed.count(n)) {
              claimed[n] = "#new";
              kl.held.insert(n);
            }
          }
          if (kl.held.empty()) continue;  // no successor activated: no departure
        }
        // Its focal start state is the list after k-1.
        kl.focalActiveBefore = actives_json(active_at(name, k - 1));
        t.openIndex = opened.size();
        opened.push_back(std::move(kl));
        touches.push_back(std::move(t));
      }
    }
  }
  sustain_.erase(std::remove_if(sustain_.begin(), sustain_.end(), [](const Sustain& s) { return s.through < 0; }),
                 sustain_.end());

  ring_.push_back(std::move(rec));
  StepRecord& now = ring_.back();

  // ── Ticks, in record order ──
  std::sort(touches.begin(), touches.end(), [](const Touch& a, const Touch& b) {
    if (a.machine != b.machine) return a.machine < b.machine;
    if (a.sequence != b.sequence) return a.sequence < b.sequence;
    if (a.openStep != b.openStep) return a.openStep < b.openStep;
    return a.isNew < b.isNew;
  });
  std::vector<Json> lines;
  auto refOpen = [&](const Touch& t) -> Open& { return t.isNew ? opened[t.openIndex] : open_[t.openIndex]; };
  for (auto& t : touches) {
    if (!t.sustainEndOf.empty()) {
      const long long l = tick_();
      lines.push_back(Json::Object{{"format", "re-kline-event/1"}, {"kline", t.sustainEndOf}, {"event", "sustain-end"},
                                   {"through", static_cast<double>(t.sustainThrough)},
                                   {"at", stamp(l, k, v.commit).json()}});
      continue;
    }
    Open& kl = refOpen(t);
    if (t.isNew) {
      kl.open.lamport = tick_();
      kl.id = instance_ + ":" + std::to_string(kl.open.lamport);
    }
    for (int i = 0; i < t.milestones; ++i) tick_();
    // The pair of this step, projected onto the focal machine (shutdown record).
    {
      Json::Array contribs;
      for (const auto& op : now.ops)
        if (op.machine == kl.machine)
          contribs.push_back(Json::Object{{"machine", op.machine},
                                          {"region", Json::Object{{"offset", static_cast<double>(op.region.offset)},
                                                                  {"length", static_cast<double>(op.region.length)}}},
                                          {"values", vector_json(op.values)}});
      kl.focalPairs.array().push_back(Json::Object{{"stepNumber", static_cast<double>(k)},
                                                   {"isre", entry_json(now.isre, {kl.input})},
                                                   {"osre", entry_json(now.osre, {kl.output})},
                                                   {"contributions", contribs}});
    }
    if (!t.closeOutcome.empty()) {
      const Stamp close = stamp(tick_(), k, v.commit);
      if (t.closeOutcome == "completed") {
        for (auto& s : sustain_)
          if (s.id.empty() && s.machine == kl.machine && s.sequence == kl.sequence) { s.id = kl.id; break; }
      }
      closed_[kl.id] = {kl.machine, kl.sequence, kl.open.step, k};
      lines.push_back(build_record(kl, t.closeOutcome, nullptr, close, t.difference, t.subsumedBy,
                                   t.sustainThrough, true));
    }
  }
  // What asserted this step: contributor searches at later steps read it.
  for (auto& t : touches) {
    if (!t.sustainEndOf.empty()) continue;
    Open& kl = refOpen(t);
    if (t.milestones > 0 || t.closeOutcome == "completed") now.asserted[{kl.machine, kl.sequence}] = kl.id;
  }
  for (const auto& s : sustain_)
    if (s.through == k && !s.id.empty()) now.asserted[{s.machine, s.sequence}] = s.id;

  // Keep what is still open.
  std::vector<Open> keep;
  for (size_t i = 0; i < open_.size(); ++i) if (stillOpen[i]) keep.push_back(std::move(open_[i]));
  for (auto& t : touches)
    if (t.isNew && t.closeOutcome.empty()) keep.push_back(std::move(opened[t.openIndex]));
  open_ = std::move(keep);

  lastStep_ = k;
  lastCommit_ = v.commit;
  persist_step(lines);
  trim_ring();
}

Json Tracker::build_record(const Open& k, const std::string& outcome, const Json& reason, const Stamp& close,
                           const Json& difference, const std::string& subsumedBy, long sustainThrough,
                           bool ringAvailable) const {
  std::vector<std::string> scope{k.machine};
  Json::Array contributors;
  bool truncated = false;
  std::vector<RegionMapping> inputs{k.input}, outputs{k.output};
  Json pairs = k.focalPairs;
  Json activeBefore = k.focalActiveBefore;
  Json activeAfter = Json::Array{};
  Json::Array shared;
  Json out = Json::Object{{"outputVector", nullptr}, {"mergedOutputVector", nullptr}};

  if (ringAvailable) {
    // Contributors through the recorded OSRE(m), hop by hop.
    struct Frontier { std::string machine; long from, to; };
    std::vector<Frontier> frontier{{k.machine, k.open.step, close.step}};
    std::set<std::string> seen{k.id};
    std::vector<std::tuple<int, std::string, long, std::string>> found;  // hops, machine, openStep, id
    for (int hop = 1; !frontier.empty(); ++hop) {
      std::vector<Frontier> next;
      for (const auto& f : frontier) {
        const StepRecord* any = record_for(f.to);
        RegionMapping region = f.machine == k.machine ? k.input : RegionMapping{};
        if (f.machine != k.machine && any) {
          auto r = any->regions.find(f.machine);
          if (r != any->regions.end()) region = r->second.first;
        }
        for (long s = f.from; s <= f.to; ++s) {
          const StepRecord* prev = record_for(s - 1);
          if (!prev) {
            if (s - 1 >= 0) truncated = true;  // the ring no longer holds it
            continue;
          }
          for (const auto& op : prev->ops) {
            if (op.machine == f.machine || !overlaps(op.region, region)) continue;
            for (const auto& sq : op.sequenceIds) {
              auto a = prev->asserted.find({op.machine, sq});
              if (a == prev->asserted.end() || seen.count(a->second)) continue;
              if (hop > controls_.feedbackHops) { truncated = true; continue; }
              seen.insert(a->second);
              long from = s - 1, to = s - 1;
              if (auto c = closed_.find(a->second); c != closed_.end()) { from = c->second.openStep; to = c->second.closeStep; }
              found.emplace_back(hop, op.machine, from, a->second);
              next.push_back({op.machine, from, to});
            }
          }
        }
      }
      frontier = std::move(next);
    }
    std::sort(found.begin(), found.end());
    for (const auto& [hops, machine, openStep, id] : found) {
      contributors.push_back(Json::Object{{"kline", id}, {"machine", machine}, {"hops", static_cast<double>(hops)}});
      scope.push_back(machine);
    }
    std::sort(scope.begin(), scope.end());
    scope.erase(std::unique(scope.begin(), scope.end()), scope.end());

    // Regions of the scope, as they stood at close.
    const StepRecord* last = record_for(close.step);
    inputs.clear();
    outputs.clear();
    std::set<std::string> inScope(scope.begin(), scope.end());
    if (last) {
      for (const auto& name : scope) {
        auto r = last->regions.find(name);
        if (r == last->regions.end()) continue;
        inputs.push_back(r->second.first);
        if (r->second.second) outputs.push_back(*r->second.second);
      }
      // Output cells shared with a machine outside the scope.
      std::set<int> cells;
      for (const auto& o : outputs)
        for (const auto& [name, rg] : last->regions) {
          if (inScope.count(name) || !rg.second) continue;
          const RegionMapping& x = *rg.second;
          for (int c = std::max(o.offset, x.offset); c < std::min(o.offset + o.length, x.offset + x.length); ++c) cells.insert(c);
        }
      for (int c : cells) shared.emplace_back(static_cast<double>(c));
    }
    Json::Array ps;
    for (long s = k.open.step; s <= close.step; ++s) {
      const StepRecord* r = record_for(s);
      if (!r) continue;
      std::vector<const Op*> ops;
      for (const auto& op : r->ops) if (inScope.count(op.machine)) ops.push_back(&op);
      std::sort(ops.begin(), ops.end(), [](const Op* a, const Op* b) {
        if (a->region.offset != b->region.offset) return a->region.offset < b->region.offset;
        return a->machine < b->machine;
      });
      Json::Array contribs;
      for (const Op* op : ops)
        contribs.push_back(Json::Object{{"machine", op->machine},
                                        {"region", Json::Object{{"offset", static_cast<double>(op->region.offset)},
                                                                {"length", static_cast<double>(op->region.length)}}},
                                        {"values", vector_json(op->values)}});
      ps.push_back(Json::Object{{"stepNumber", static_cast<double>(s)}, {"isre", entry_json(r->isre, inputs)},
                                {"osre", entry_json(r->osre, outputs)}, {"contributions", contribs}});
    }
    if (!ps.empty()) pairs = ps;
    std::vector<ActiveEntry> before, after;
    for (const auto& name : scope) {
      auto b = active_at(name, k.open.step - 1);
      before.insert(before.end(), b.begin(), b.end());
      auto a = active_at(name, close.step);
      after.insert(after.end(), a.begin(), a.end());
    }
    activeBefore = actives_json(before);
    activeAfter = actives_json(after);
  } else {
    truncated = true;  // the ring did not survive: scope is the focal machine alone
  }

  Json::Array path;
  for (const auto& p : k.path)
    path.push_back(Json::Object{{"event", p.event}, {"step", static_cast<double>(p.step)},
                                {"milestone", p.milestone}, {"outputs", p.outputs}});
  Json::Array scopeJson;
  for (const auto& s : scope) scopeJson.emplace_back(s);
  const bool completed = outcome == "completed";
  return Json::Object{
    {"format", "re-kline/1"},
    {"id", k.id},
    {"corpus", Json::Object{{"algorithm", "sha256/resident-declared-v1"}, {"fingerprint", fingerprint_}}},
    {"focal", Json::Object{{"machine", k.machine}, {"sequence", k.sequence}}},
    {"outcome", outcome},
    {"reason", reason},
    {"open", k.open.json()},
    {"close", close.json()},
    {"path", path},
    {"difference", difference},
    {"subsumedBy", subsumedBy.empty() ? Json(nullptr) : Json(subsumedBy)},
    {"sustain", Json::Object{{"through", static_cast<double>(completed ? sustainThrough : close.step)}, {"ended", !completed}}},
    {"scope", scopeJson},
    {"contributors", contributors},
    {"feedbackTruncated", truncated},
    {"sharedCells", shared},
    {"activeBefore", activeBefore},
    {"activeAfter", activeAfter},
    {"pairs", pairs},
    {"outputs", out}};
}

void Tracker::on_reset() {
  if (!controls_.capture) return;
  std::vector<Json> lines;
  std::vector<std::pair<std::tuple<std::string, std::string, long>, size_t>> order;
  for (size_t i = 0; i < open_.size(); ++i) order.push_back({{open_[i].machine, open_[i].sequence, open_[i].open.step}, i});
  std::sort(order.begin(), order.end());
  for (const auto& [_, i] : order) {
    const Stamp close = stamp(tick_(), lastStep_ < 0 ? 0 : lastStep_, lastCommit_);
    closed_[open_[i].id] = {open_[i].machine, open_[i].sequence, open_[i].open.step, close.step};
    lines.push_back(build_record(open_[i], "interrupted", "reset", close, nullptr, "", close.step, true));
  }
  std::sort(sustain_.begin(), sustain_.end(), [](const Sustain& a, const Sustain& b) {
    return std::tie(a.machine, a.sequence, a.id) < std::tie(b.machine, b.sequence, b.id);
  });
  for (const auto& s : sustain_)
    lines.push_back(Json::Object{{"format", "re-kline-event/1"}, {"kline", s.id}, {"event", "sustain-end"},
                                 {"through", static_cast<double>(s.through)},
                                 {"at", stamp(tick_(), lastStep_ < 0 ? 0 : lastStep_, lastCommit_).json()}});
  open_.clear();
  sustain_.clear();
  persist_step(lines);
  ring_.clear();
  baseline_.clear();
  current_.clear();
  lastStep_ = -1;
}

void Tracker::on_machine_removed(const std::string& machineName) {
  if (!controls_.capture) return;
  std::vector<Json> lines;
  std::vector<Open> keep;
  std::vector<Open> gone;
  for (auto& kl : open_) (kl.machine == machineName ? gone : keep).push_back(std::move(kl));
  std::sort(gone.begin(), gone.end(), [](const Open& a, const Open& b) {
    return std::tie(a.sequence, a.open.step) < std::tie(b.sequence, b.open.step);
  });
  for (const auto& kl : gone) {
    const Stamp close = stamp(tick_(), lastStep_ < 0 ? 0 : lastStep_, lastCommit_);
    closed_[kl.id] = {kl.machine, kl.sequence, kl.open.step, close.step};
    lines.push_back(build_record(kl, "interrupted", "machine-removed", close, nullptr, "", close.step, true));
  }
  open_ = std::move(keep);
  std::vector<Sustain> sk;
  for (const auto& s : sustain_) {
    if (s.machine != machineName) { sk.push_back(s); continue; }
    lines.push_back(Json::Object{{"format", "re-kline-event/1"}, {"kline", s.id}, {"event", "sustain-end"},
                                 {"through", static_cast<double>(s.through)},
                                 {"at", stamp(tick_(), lastStep_ < 0 ? 0 : lastStep_, lastCommit_).json()}});
  }
  sustain_ = std::move(sk);
  current_.erase(machineName);
  persist_step(lines);
}

Json Tracker::checkpoint_json() const {
  Json::Array open;
  for (const auto& kl : open_) {
    Json::Array held, path;
    for (const auto& e : kl.held) held.emplace_back(e);
    for (const auto& p : kl.path)
      path.push_back(Json::Object{{"event", p.event}, {"step", static_cast<double>(p.step)},
                                  {"milestone", p.milestone}, {"outputs", p.outputs}});
    open.push_back(Json::Object{
      {"id", kl.id}, {"machine", kl.machine}, {"sequence", kl.sequence}, {"activator", kl.activator},
      {"open", kl.open.json()}, {"held", held}, {"path", path}, {"refusedBy", kl.refusedBy},
      {"focalPairs", kl.focalPairs}, {"focalActiveBefore", kl.focalActiveBefore},
      {"focalActiveAfter", actives_json(current_.count(kl.machine) ? current_.at(kl.machine) : std::vector<ActiveEntry>{})},
      {"input", Json::Object{{"offset", static_cast<double>(kl.input.offset)}, {"length", static_cast<double>(kl.input.length)}}},
      {"output", Json::Object{{"offset", static_cast<double>(kl.output.offset)}, {"length", static_cast<double>(kl.output.length)}}}});
  }
  Json::Array sustains;
  for (const auto& s : sustain_)
    sustains.push_back(Json::Object{{"id", s.id}, {"machine", s.machine}, {"sequence", s.sequence},
                                    {"event", s.event}, {"through", static_cast<double>(s.through)}});
  return Json::Object{{"format", "re-kline-open/1"}, {"step", static_cast<double>(lastStep_)},
                      {"commit", static_cast<double>(lastCommit_)}, {"fingerprint", fingerprint_},
                      {"open", open}, {"sustain", sustains}};
}

void Tracker::persist_step(std::vector<Json>& lines) {
  store_.append(lines, fingerprint_, lastCommit_);
  store_.checkpoint(checkpoint_json());
  store_.enforce_budget(controls_);
}

void Tracker::recover() {
  auto cp = store_.read_checkpoint();
  if (!cp || !cp->is_object()) return;
  const long step = static_cast<long>(cp->at("step").as_number());
  const long long commit = static_cast<long long>(cp->at("commit").as_number());
  fingerprint_ = cp->at("fingerprint").as_string();
  lastStep_ = step;
  lastCommit_ = commit;
  std::vector<Json> lines;
  for (const auto& o : cp->at("open").array()) {
    Open kl;
    kl.id = o.at("id").as_string();
    kl.machine = o.at("machine").as_string();
    kl.sequence = o.at("sequence").as_string();
    kl.activator = o.at("activator").as_string();
    kl.open = Stamp::from(o.at("open"));
    for (const auto& p : o.at("path").array())
      kl.path.push_back({p.at("event").as_string(), static_cast<long>(p.at("step").as_number()),
                         p.at("milestone").as_bool(), p.at("outputs")});
    kl.focalPairs = o.at("focalPairs");
    kl.focalActiveBefore = o.at("focalActiveBefore");
    const Stamp close = stamp(tick_(), step, commit);
    Json rec = build_record(kl, "interrupted", "shutdown", close, nullptr, "", step, false);
    rec.object()["activeAfter"] = o.at("focalActiveAfter");
    lines.push_back(rec);
  }
  for (const auto& s : cp->at("sustain").array()) {
    if (s.at("id").as_string().empty()) continue;
    lines.push_back(Json::Object{{"format", "re-kline-event/1"}, {"kline", s.at("id").as_string()},
                                 {"event", "sustain-end"}, {"through", s.at("through")},
                                 {"at", stamp(tick_(), step, commit).json()}});
  }
  open_.clear();
  sustain_.clear();
  persist_step(lines);
  lastStep_ = -1;  // the engine's step count starts over
}

void Tracker::start_capture() {
  open_.clear();
  sustain_.clear();
  closed_.clear();
  ring_.clear();
  baseline_.clear();
  current_.clear();
  lastStep_ = -1;
  controls_.capture = true;
  recover();
}

void Tracker::trim_ring() {
  long keepFrom = lastStep_ - std::max(1, ring_depth());
  for (const auto& kl : open_) keepFrom = std::min(keepFrom, kl.open.step - 1);
  while (!ring_.empty() && ring_.front().step < keepFrom) {
    for (auto& [name, list] : ring_.front().changed) baseline_[name] = std::move(list);
    ring_.erase(ring_.begin());
  }
  // Closed K-lines are needed while their span is in the ring.
  for (auto it = closed_.begin(); it != closed_.end();) {
    if (it->second.closeStep < keepFrom - 1) it = closed_.erase(it);
    else ++it;
  }
}

// ── Routes' bodies ──────────────────────────────────────────────────────────

Json Tracker::open_json() const {
  std::vector<const Open*> list;
  for (const auto& kl : open_) list.push_back(&kl);
  std::sort(list.begin(), list.end(), [](const Open* a, const Open* b) {
    return std::tie(a->machine, a->sequence, a->open.step) < std::tie(b->machine, b->sequence, b->open.step);
  });
  Json::Array out;
  for (const Open* kl : list) {
    Json::Array path, held;
    for (const auto& p : kl->path)
      path.push_back(Json::Object{{"event", p.event}, {"step", static_cast<double>(p.step)},
                                  {"milestone", p.milestone}, {"outputs", p.outputs}});
    for (const auto& e : kl->held) held.emplace_back(e);
    out.push_back(Json::Object{{"id", kl->id}, {"focal", Json::Object{{"machine", kl->machine}, {"sequence", kl->sequence}}},
                               {"open", kl->open.json()}, {"path", path}, {"held", held}});
  }
  return Json::Object{{"commit", static_cast<double>(lastCommit_)}, {"open", out}};
}

Json Tracker::list(const std::optional<std::string>& outcome, const std::optional<std::string>& machine,
                   const std::optional<std::string>& sequence, long long since, int limit) const {
  limit = std::clamp(limit <= 0 ? 100 : limit, 1, 1000);
  std::vector<const Store::Meta*> sel;
  for (const auto& m : store_.records()) {
    if (m.closeCommit < since) continue;
    if (outcome && m.outcome != *outcome) continue;
    if (machine && m.machine != *machine) continue;
    if (sequence && m.sequence != *sequence) continue;
    sel.push_back(&m);
  }
  // A page holds whole commits: it stops before the commit that would cross
  // `limit`, unless that commit is the page's first.
  Json::Array out;
  Json next = nullptr;
  size_t i = 0;
  while (i < sel.size()) {
    size_t j = i;
    while (j < sel.size() && sel[j]->closeCommit == sel[i]->closeCommit) ++j;
    if (!out.empty() && out.size() + (j - i) > static_cast<size_t>(limit)) {
      next = static_cast<double>(sel[i]->closeCommit);
      break;
    }
    for (size_t x = i; x < j; ++x) out.push_back(store_.record_at(*sel[x]));
    i = j;
    if (out.size() >= static_cast<size_t>(limit) && i < sel.size()) {
      next = static_cast<double>(sel[i]->closeCommit);
      break;
    }
  }
  return Json::Object{{"klines", out}, {"next", next}};
}

std::optional<Json> Tracker::get(const std::string& id) const { return store_.record(id); }

Json Tracker::stats() const { return store_.stats(controls_); }

std::string Tracker::export_bundle(const std::set<std::string>& outcomes, long long since,
                                   const std::set<std::string>& machines) {
  std::string body;
  size_t count = 0;
  std::set<std::string> segments;
  for (const auto& m : store_.records()) {
    if (m.closeCommit < since) continue;
    if (!outcomes.empty() && !outcomes.count(m.outcome)) continue;
    if (!machines.empty() && !machines.count(m.machine)) continue;
    body += store_.raw_at(m) + "\n";
    ++count;
    for (const auto& e : store_.raw_events(m.id)) { body += e + "\n"; ++count; }
    segments.insert(m.segment);
  }
  Json header = Json::Object{
    {"format", "re-kline-bundle/1"},
    {"source", Json::Object{{"instance", instance_}}},
    {"corpus", Json::Object{{"algorithm", "sha256/resident-declared-v1"}, {"fingerprint", fingerprint_}}},
    {"controls", Json::Object{{"klineCapture", controls_.capture},
                              {"klineFeedbackHops", static_cast<double>(controls_.feedbackHops)},
                              {"klineStoreMaxBytes", static_cast<double>(controls_.storeMaxBytes)}}},
    {"count", static_cast<double>(count)},
    {"sha256", sha256_hex(body)}};
  store_.mark_exported(segments);
  return jcs(header) + "\n" + body;
}

}  // namespace reality::kline
