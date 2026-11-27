#include "reality/isre.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace reality::isre {
namespace fs = std::filesystem;

std::filesystem::path Taxonomy::dir_for(const std::string& machinesDir) {
  return fs::absolute(machinesDir.empty() ? fs::path("../RealityEngine_Machines/machines") : fs::path(machinesDir))
             .parent_path() / "schemas" / "source-kinds";
}

std::optional<Taxonomy> Taxonomy::load(const fs::path& dir, int version) {
  const fs::path file = dir / ("re-source-kinds-" + std::to_string(version) + ".json");
  std::ifstream in(file);
  if (!in) return std::nullopt;
  std::stringstream ss;
  ss << in.rdbuf();
  Json doc;
  try { doc = json::parse(ss.str()); } catch (...) { return std::nullopt; }
  if (doc.at("format").as_string() != "re-source-kinds" ||
      static_cast<int>(doc.at("version").as_number(-1)) != version)
    return std::nullopt;
  Taxonomy t;
  t.version_ = version;
  if (doc.at("providerMapping").is_object())
    for (const auto& [k, v] : doc.at("providerMapping").object()) t.providerMapping_[k] = v.as_string();
  for (const auto& k : doc.at("kinds").array()) {
    const std::string kind = k.at("kind").as_string();
    if (k.at("providers").is_array()) {
      for (const auto& p : k.at("providers").array())
        t.providerClass_[kind][p.at("provider").as_string()] = p.at("class").as_string();
    } else if (k.at("class").is_string()) {
      t.kindClass_[kind] = k.at("class").as_string();
    }
  }
  t.unknownProvider_ = doc.at("unknownProvider").at("provider").as_string("unattributed");
  t.unknownClass_ = doc.at("unknownProvider").at("class").as_string("generated");
  return t;
}

Entry Taxonomy::source_entry(const std::string& kind, const std::string& origin) const {
  auto k = kindClass_.find(kind);
  if (k != kindClass_.end()) return Entry{kind, std::nullopt, k->second};
  // A kind with providers (sensor), or one the version does not list, which is
  // read as a sensor of its origin.
  const std::string asKind = providerClass_.count(kind) ? kind : "sensor";
  std::string provider = origin.substr(0, origin.find('.'));
  std::transform(provider.begin(), provider.end(), provider.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (provider.empty()) provider = "signal";
  if (auto m = providerMapping_.find(provider); m != providerMapping_.end()) provider = m->second;
  auto kp = providerClass_.find(asKind);
  if (kp != providerClass_.end()) {
    auto c = kp->second.find(provider);
    if (c != kp->second.end()) return Entry{asKind, provider, c->second};
  }
  return Entry{asKind, unknownProvider_, unknownClass_};
}

Entry Taxonomy::feedback_entry() const {
  auto k = kindClass_.find("feedback");
  return Entry{"feedback", std::nullopt, k != kindClass_.end() ? k->second : "deterministic"};
}

Json sources_json(const std::vector<Winner>& cells, const Taxonomy& taxonomy) {
  Json::Array runs;
  struct Run { int first, last; Entry e; std::string id; Json causedBy; };
  std::optional<Run> cur;
  auto flush = [&] {
    if (!cur) return;
    Json::Object o{{"cells", Json::Array{static_cast<double>(cur->first), static_cast<double>(cur->last)}},
                   {"kind", cur->e.kind}, {"class", cur->e.cls}, {"causedBy", cur->causedBy}};
    if (cur->e.provider) o["provider"] = *cur->e.provider;
    if (!cur->id.empty()) o["id"] = cur->id;
    runs.push_back(o);
    cur.reset();
  };
  std::vector<Winner> sorted = cells;
  std::sort(sorted.begin(), sorted.end(), [](const Winner& a, const Winner& b) { return a.cell < b.cell; });
  for (const auto& w : sorted) {
    Entry e = w.feedback ? taxonomy.feedback_entry() : taxonomy.source_entry(w.kind, w.origin);
    const std::string id = w.feedback ? "" : w.id;
    const Json causedBy = w.feedback ? Json(nullptr) : w.causedBy;
    const bool same = cur && cur->last + 1 == w.cell && cur->e.kind == e.kind && cur->e.provider == e.provider &&
                      cur->e.cls == e.cls && cur->id == id && json::stringify(cur->causedBy) == json::stringify(causedBy);
    if (same) { cur->last = w.cell; continue; }
    flush();
    cur = Run{w.cell, w.cell, e, id, causedBy};
  }
  flush();
  return runs;
}

void Recorder::record(const Json& re, const Json& pe, const std::vector<Winner>& cells, const Json& folds) {
  if (!controls_.capture || !taxonomy_) return;
  if (re.at("instance").is_string()) reInstance_ = re.at("instance").as_string();
  Json rec = Json::Object{{"format", "re-isre-composition/1"}, {"taxonomy", taxonomy_->name()},
                          {"re", re}, {"pe", pe}, {"sources", sources_json(cells, *taxonomy_)},
                          {"folds", folds.is_array() ? folds : Json(Json::Array{})}, {"delta", false}};
  const long long commit = static_cast<long long>(re.at("commit").as_number());
  store_.append({rec}, taxonomy_->name(), commit);
  kline::Controls kc;
  kc.storeMaxBytes = controls_.storeMaxBytes;
  store_.enforce_budget(kc);
}

Json Recorder::list(long long from, long long to, int limit) const {
  limit = std::clamp(limit <= 0 ? 100 : limit, 1, 1000);
  Json::Array out;
  Json next = nullptr;
  for (const auto& m : store_.records()) {
    if (m.closeCommit < from || (to >= 0 && m.closeCommit > to)) continue;
    if (static_cast<int>(out.size()) >= limit) { next = static_cast<double>(m.closeCommit); break; }
    out.push_back(json::parse(store_.raw_at(m)));
  }
  return Json::Object{{"records", out}, {"next", next}};
}

Json Recorder::stats() const {
  kline::Controls kc;
  kc.storeMaxBytes = controls_.storeMaxBytes;
  Json s = store_.stats(kc);
  Json::Object o;
  for (const auto& key : {"segments", "bytes", "budget", "warnings"}) o[key] = s.at(key);
  o["records"] = static_cast<double>(store_.records().size());
  o["isreCapture"] = controls_.capture;
  o["taxonomy"] = taxonomy_ ? Json(taxonomy_->name()) : Json(nullptr);
  return o;
}

std::string Recorder::export_bundle(long long from, long long to) {
  std::string body;
  size_t count = 0;
  std::set<std::string> segments;
  for (const auto& m : store_.records()) {
    if (m.closeCommit < from || (to >= 0 && m.closeCommit > to)) continue;
    body += store_.raw_at(m) + "\n";
    ++count;
    segments.insert(m.segment);
  }
  Json header = Json::Object{
    {"format", "re-isre-bundle/1"},
    {"source", Json::Object{{"instance", instance_}}},
    {"re", Json::Object{{"instance", reInstance_}}},
    {"taxonomy", taxonomy_ ? Json(taxonomy_->name()) : Json(nullptr)},
    {"count", static_cast<double>(count)},
    {"sha256", kline::sha256_hex(body)}};
  store_.mark_exported(segments);
  return kline::jcs(header) + "\n" + body;
}

}  // namespace reality::isre
