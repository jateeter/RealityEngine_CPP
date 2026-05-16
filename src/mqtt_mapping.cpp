// MQTT mapping registry implementation — see include/reality/mqtt_mapping.hpp.

#include "reality/mqtt_mapping.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace reality::mqtt {

namespace {

// Split a topic / filter into level segments (slash-delimited).
std::vector<std::string> split_topic(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == '/') { out.push_back(cur); cur.clear(); }
    else cur.push_back(c);
  }
  out.push_back(cur);
  return out;
}

ExtractType parse_extract_type(const std::string& s) {
  if (s == "json")         return ExtractType::Json;
  if (s == "csv-float")    return ExtractType::CsvFloat;
  if (s == "raw")          return ExtractType::Raw;
  if (s == "single-float") return ExtractType::SingleFloat;
  throw std::runtime_error("unknown extract.type: " + s);
}

NormalizeMode parse_normalize_mode(const std::string& s) {
  if (s == "minmax")      return NormalizeMode::MinMax;
  if (s == "linear")      return NormalizeMode::Linear;
  if (s == "passthrough") return NormalizeMode::Passthrough;
  if (s.empty())          return NormalizeMode::Passthrough;
  throw std::runtime_error("unknown normalize.mode: " + s);
}

PushMode parse_push_mode(const std::string& s) {
  if (s == "debounced") return PushMode::Debounced;
  if (s == "manual")    return PushMode::Manual;
  if (s == "immediate") return PushMode::Immediate;
  if (s.empty())        return PushMode::Debounced;
  throw std::runtime_error("unknown pushMode: " + s);
}

// Parse comma/whitespace-separated ASCII floats.  Empty entries → 0.0.
Vector parse_csv_floats(const std::string& s) {
  Vector out;
  std::string current;
  auto flush = [&]() {
    if (current.empty()) { out.push_back(0.0); current.clear(); return; }
    try { out.push_back(std::stod(current)); }
    catch (const std::exception&) { out.push_back(std::numeric_limits<double>::quiet_NaN()); }
    current.clear();
  };
  for (char c : s) {
    if (c == ',' || c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      if (!current.empty()) flush();
    } else current.push_back(c);
  }
  if (!current.empty()) flush();
  return out;
}

// Walk a JSON pointer (RFC 6901 lite — no escape handling, which is fine
// for our simple field-name corpus).  "/value" → root["value"];
// "/sensor/0/reading" → root["sensor"][0]["reading"].
const json::Value* navigate_pointer(const json::Value& root, const std::string& pointer) {
  if (pointer.empty() || pointer == "/") return &root;
  if (pointer[0] != '/') return nullptr;
  const json::Value* cur = &root;
  size_t pos = 1;
  while (pos <= pointer.size() && cur != nullptr) {
    size_t next = pointer.find('/', pos);
    std::string token = pointer.substr(pos, next - pos);
    pos = (next == std::string::npos) ? pointer.size() + 1 : next + 1;
    if (cur->is_object()) {
      const auto& obj = cur->object();
      auto it = obj.find(token);
      cur = (it == obj.end()) ? nullptr : &it->second;
    } else if (cur->is_array()) {
      try {
        int idx = std::stoi(token);
        if (idx < 0 || idx >= static_cast<int>(cur->array().size())) cur = nullptr;
        else cur = &cur->array()[idx];
      } catch (...) { cur = nullptr; }
    } else {
      cur = nullptr;
    }
  }
  return cur;
}

double clamp_unit(double v) {
  if (!std::isfinite(v)) return std::numeric_limits<double>::quiet_NaN();
  if (v < 0.0) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

}  // namespace

// ── Parse / load ────────────────────────────────────────────────────────────

MappingRegistry MappingRegistry::from_json(const json::Value& root) {
  MappingRegistry reg;

  // Shared defaults — applied when an individual mapping omits the field.
  long defaultTtl = 30000;
  int  defaultQos = 0;
  bool defaultRetained = true;
  PushMode defaultPushMode = PushMode::Debounced;
  long defaultDebounce = 250;
  const auto& defaults = root.at("defaults");
  if (defaults.is_object()) {
    if (defaults.at("ttlMs").is_number())          defaultTtl = static_cast<long>(defaults.at("ttlMs").as_number());
    if (defaults.at("qos").is_number())            defaultQos = static_cast<int>(defaults.at("qos").as_number());
    if (defaults.at("acceptRetained").is_bool())   defaultRetained = defaults.at("acceptRetained").as_bool();
    if (defaults.at("pushMode").is_string())       defaultPushMode = parse_push_mode(defaults.at("pushMode").as_string());
    if (defaults.at("debounceMs").is_number())     defaultDebounce = static_cast<long>(defaults.at("debounceMs").as_number());
  }

  const auto& mappings = root.at("mappings");
  if (!mappings.is_array())
    throw std::runtime_error("mqtt-mappings: missing top-level \"mappings\" array");

  for (const auto& m : mappings.array()) {
    if (!m.is_object())
      throw std::runtime_error("mqtt-mappings: each mapping must be a JSON object");
    MappingRule rule;
    rule.id          = m.at("id").as_string();
    rule.topicFilter = m.at("topicFilter").as_string();
    rule.sensorIdTemplate = m.at("sensorIdTemplate").as_string();
    if (rule.id.empty() || rule.topicFilter.empty())
      throw std::runtime_error("mqtt-mappings: id and topicFilter are required");

    const auto& region = m.at("region");
    if (region.is_object()) {
      rule.offset = static_cast<int>(region.at("offset").as_number(0));
      rule.length = std::max(1, static_cast<int>(region.at("length").as_number(1)));
    }

    // Extract block — type + optional details.
    const auto& extract = m.at("extract");
    if (extract.is_object()) {
      rule.extract.type = parse_extract_type(extract.at("type").as_string("csv-float"));
      if (extract.at("pointer").is_string()) rule.extract.jsonPointer = extract.at("pointer").as_string();
      if (extract.at("index").is_number())   rule.extract.csvIndex   = static_cast<int>(extract.at("index").as_number());
    }

    // Normalize block — mode + scaling params.
    const auto& norm = m.at("normalize");
    if (norm.is_object()) {
      rule.normalize.mode  = parse_normalize_mode(norm.at("mode").as_string("passthrough"));
      if (norm.at("min").is_number())    rule.normalize.min    = norm.at("min").as_number();
      if (norm.at("max").is_number())    rule.normalize.max    = norm.at("max").as_number();
      if (norm.at("scale").is_number())  rule.normalize.scale  = norm.at("scale").as_number();
      if (norm.at("offset").is_number()) rule.normalize.offset = norm.at("offset").as_number();
      if (norm.at("clamp").is_bool())    rule.normalize.clamp  = norm.at("clamp").as_bool();
    }

    rule.ttlMs          = m.at("ttlMs").is_number()       ? static_cast<long>(m.at("ttlMs").as_number()) : defaultTtl;
    rule.qos            = m.at("qos").is_number()         ? static_cast<int>(m.at("qos").as_number())   : defaultQos;
    rule.acceptRetained = m.at("acceptRetained").is_bool() ? m.at("acceptRetained").as_bool()           : defaultRetained;
    rule.pushMode       = m.at("pushMode").is_string()    ? parse_push_mode(m.at("pushMode").as_string()) : defaultPushMode;
    rule.debounceMs     = m.at("debounceMs").is_number()  ? static_cast<long>(m.at("debounceMs").as_number()) : defaultDebounce;

    reg.rules_.push_back(std::move(rule));
    reg.metrics_.push_back(std::make_unique<MappingMetrics>());
  }
  return reg;
}

MappingRegistry MappingRegistry::from_file(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("mqtt-mappings: cannot open " + path);
  std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return from_json(json::parse(raw));
}

// ── Topic-filter matching (MQTT v3.1.1 §4.7) ────────────────────────────────

namespace {
// Shared topic-filter match logic — returns true + captures when `filter`
// matches `topic`, false otherwise.  Used by both match() and match_all().
bool match_one(const std::vector<std::string>& topicLevels,
               const std::vector<std::string>& filterLevels,
               std::vector<std::string>& captures) {
  captures.clear();
  size_t fi = 0, ti = 0;
  for (; fi < filterLevels.size(); ++fi) {
    const auto& f = filterLevels[fi];
    if (f == "#") {
      std::string tail;
      for (size_t k = ti; k < topicLevels.size(); ++k) {
        if (k > ti) tail += "/";
        tail += topicLevels[k];
      }
      captures.push_back(tail);
      ti = filterLevels.size();
      ++fi;
      return fi == filterLevels.size();
    }
    if (ti >= topicLevels.size()) return false;
    if (f == "+") {
      captures.push_back(topicLevels[ti]);
    } else if (f != topicLevels[ti]) {
      return false;
    }
    ++ti;
  }
  return fi == filterLevels.size() && ti == topicLevels.size();
}
}  // namespace

std::optional<MappingRegistry::Match> MappingRegistry::match(const std::string& topic) const {
  auto topicLevels = split_topic(topic);
  for (size_t i = 0; i < rules_.size(); ++i) {
    auto filterLevels = split_topic(rules_[i].topicFilter);
    Match m;
    m.ruleIndex = i;
    if (match_one(topicLevels, filterLevels, m.captures)) return m;
  }
  return std::nullopt;
}

std::vector<MappingRegistry::Match> MappingRegistry::match_all(const std::string& topic) const {
  std::vector<Match> out;
  auto topicLevels = split_topic(topic);
  for (size_t i = 0; i < rules_.size(); ++i) {
    auto filterLevels = split_topic(rules_[i].topicFilter);
    Match m;
    m.ruleIndex = i;
    if (match_one(topicLevels, filterLevels, m.captures)) {
      out.push_back(std::move(m));
    }
  }
  return out;
}

// ── Sensor-ID template substitution ─────────────────────────────────────────

std::string MappingRegistry::resolve_sensor_id(const MappingRule& rule,
                                                const std::string& topic,
                                                const std::vector<std::string>& captures) const {
  if (rule.sensorIdTemplate.empty()) return topic;
  std::string out;
  out.reserve(rule.sensorIdTemplate.size());
  for (size_t i = 0; i < rule.sensorIdTemplate.size(); ) {
    if (rule.sensorIdTemplate[i] == '{') {
      size_t close = rule.sensorIdTemplate.find('}', i + 1);
      if (close != std::string::npos) {
        std::string idxStr = rule.sensorIdTemplate.substr(i + 1, close - i - 1);
        try {
          int idx = std::stoi(idxStr);
          if (idx >= 1 && idx <= static_cast<int>(captures.size())) {
            out += captures[idx - 1];
          }
        } catch (...) { /* malformed placeholder — silently drop */ }
        i = close + 1;
        continue;
      }
    }
    out.push_back(rule.sensorIdTemplate[i]);
    ++i;
  }
  return out;
}

// ── Decode (extract + normalize + validate) ─────────────────────────────────

MappingRegistry::Decode MappingRegistry::decode(const MappingRule& rule,
                                                 const std::vector<uint8_t>& payload) const {
  Decode out;
  Vector raw;
  std::string text(payload.begin(), payload.end());

  switch (rule.extract.type) {
    case ExtractType::Raw:
    case ExtractType::SingleFloat: {
      try { raw.push_back(std::stod(text)); }
      catch (const std::exception&) { raw.push_back(std::numeric_limits<double>::quiet_NaN()); }
      break;
    }
    case ExtractType::CsvFloat: {
      auto all = parse_csv_floats(text);
      if (rule.extract.csvIndex) {
        int i = *rule.extract.csvIndex;
        if (i < 0 || i >= static_cast<int>(all.size())) {
          out.error = "csv index " + std::to_string(i) + " out of range (have " +
                      std::to_string(all.size()) + ")";
          return out;
        }
        raw.push_back(all[i]);
      } else {
        raw = std::move(all);
      }
      break;
    }
    case ExtractType::Json: {
      json::Value parsed;
      try { parsed = json::parse(text); }
      catch (const std::exception& e) { out.error = std::string("json parse: ") + e.what(); return out; }
      const json::Value* node = navigate_pointer(parsed, rule.extract.jsonPointer);
      if (!node) { out.error = "json pointer \"" + rule.extract.jsonPointer + "\" not found"; return out; }
      if (node->is_number()) {
        raw.push_back(node->as_number());
      } else if (node->is_array()) {
        for (const auto& v : node->array()) {
          if (v.is_number()) raw.push_back(v.as_number());
          else raw.push_back(std::numeric_limits<double>::quiet_NaN());
        }
      } else if (node->is_string()) {
        try { raw.push_back(std::stod(node->as_string())); }
        catch (const std::exception&) { raw.push_back(std::numeric_limits<double>::quiet_NaN()); }
      } else if (node->is_bool()) {
        raw.push_back(node->as_bool() ? 1.0 : 0.0);
      } else {
        out.error = "json pointer target is not a number / array / string / bool";
        return out;
      }
      break;
    }
  }

  // Normalize each value per the rule.
  Vector normalized;
  normalized.reserve(raw.size());
  for (double v : raw) {
    if (!std::isfinite(v)) { out.error = "value is not finite"; return out; }
    double n = v;
    switch (rule.normalize.mode) {
      case NormalizeMode::Passthrough: break;
      case NormalizeMode::MinMax: {
        double denom = rule.normalize.max - rule.normalize.min;
        if (denom == 0.0) { out.error = "normalize.min == normalize.max"; return out; }
        n = (v - rule.normalize.min) / denom;
        break;
      }
      case NormalizeMode::Linear:
        n = v * rule.normalize.scale + rule.normalize.offset;
        break;
    }
    if (rule.normalize.clamp) n = clamp_unit(n);
    if (!std::isfinite(n)) { out.error = "value not finite after normalize"; return out; }
    normalized.push_back(n);
  }

  // Length validation — must equal region.length exactly.  Per roadmap:
  // "Region length equals transformed value count."
  if (static_cast<int>(normalized.size()) != rule.length) {
    out.error = "transformed value count " + std::to_string(normalized.size()) +
                " != region.length " + std::to_string(rule.length);
    return out;
  }

  out.values = std::move(normalized);
  out.valid = true;
  return out;
}

// ── Overlap validation ──────────────────────────────────────────────────────

std::vector<std::string> MappingRegistry::validate_overlaps(bool allowOverlap) const {
  std::vector<std::string> warnings;
  if (allowOverlap) return warnings;
  for (size_t i = 0; i < rules_.size(); ++i) {
    int aStart = rules_[i].offset;
    int aEnd   = aStart + rules_[i].length;
    for (size_t j = i + 1; j < rules_.size(); ++j) {
      int bStart = rules_[j].offset;
      int bEnd   = bStart + rules_[j].length;
      if (aStart < bEnd && bStart < aEnd) {
        std::ostringstream w;
        w << "mappings \"" << rules_[i].id << "\" [" << aStart << "," << aEnd
          << ") and \"" << rules_[j].id << "\" [" << bStart << "," << bEnd
          << ") overlap";
        warnings.push_back(w.str());
      }
    }
  }
  return warnings;
}

// ── JSON serialization for /api/mqtt/mappings ───────────────────────────────

namespace {
std::string extract_type_str(ExtractType t) {
  switch (t) {
    case ExtractType::Raw:         return "raw";
    case ExtractType::CsvFloat:    return "csv-float";
    case ExtractType::Json:        return "json";
    case ExtractType::SingleFloat: return "single-float";
  }
  return "raw";
}
std::string normalize_mode_str(NormalizeMode m) {
  switch (m) {
    case NormalizeMode::Passthrough: return "passthrough";
    case NormalizeMode::MinMax:      return "minmax";
    case NormalizeMode::Linear:      return "linear";
  }
  return "passthrough";
}
std::string push_mode_str(PushMode p) {
  switch (p) {
    case PushMode::Debounced: return "debounced";
    case PushMode::Manual:    return "manual";
    case PushMode::Immediate: return "immediate";
  }
  return "debounced";
}
}  // namespace

json::Value MappingRegistry::to_json() const {
  json::Value::Array arr;
  for (size_t i = 0; i < rules_.size(); ++i) {
    const auto& r = rules_[i];
    const auto& m = *metrics_[i];
    json::Value::Object extract{
      {"type", extract_type_str(r.extract.type)},
    };
    if (!r.extract.jsonPointer.empty()) extract["pointer"] = r.extract.jsonPointer;
    if (r.extract.csvIndex) extract["index"] = static_cast<double>(*r.extract.csvIndex);

    json::Value::Object normalize{
      {"mode",   normalize_mode_str(r.normalize.mode)},
      {"min",    r.normalize.min},
      {"max",    r.normalize.max},
      {"scale",  r.normalize.scale},
      {"offset", r.normalize.offset},
      {"clamp",  r.normalize.clamp},
    };

    std::string lastError;
    {
      std::lock_guard<std::mutex> lock(m.lastErrorMutex);
      lastError = m.lastError;
    }
    json::Value::Object counters{
      {"received",        static_cast<double>(m.received.load())},
      {"mapped",          static_cast<double>(m.mapped.load())},
      {"rejected",        static_cast<double>(m.rejected.load())},
      {"stale",           static_cast<double>(m.stale.load())},
      {"lastMessageAtMs", static_cast<double>(m.lastMessageAtMs.load())},
      {"lastError",       lastError},
      {"lastErrorAtMs",   static_cast<double>(m.lastErrorAtMs.load())},
    };

    arr.push_back(json::Value::Object{
      {"id",               r.id},
      {"topicFilter",      r.topicFilter},
      {"sensorIdTemplate", r.sensorIdTemplate},
      {"region", json::Value::Object{
        {"offset", static_cast<double>(r.offset)},
        {"length", static_cast<double>(r.length)},
      }},
      {"extract", extract},
      {"normalize", normalize},
      {"ttlMs",          static_cast<double>(r.ttlMs)},
      {"qos",            static_cast<double>(r.qos)},
      {"acceptRetained", r.acceptRetained},
      {"pushMode",       push_mode_str(r.pushMode)},
      {"debounceMs",     static_cast<double>(r.debounceMs)},
      {"counters",       counters},
    });
  }
  return json::Value::Object{{"mappings", arr}};
}

}  // namespace reality::mqtt
