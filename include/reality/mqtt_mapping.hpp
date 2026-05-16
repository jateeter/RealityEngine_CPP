#pragma once

// MQTT mapping registry — the authoritative bridge between broker topics and
// Perception Engine perceptual-space regions.
//
// Design rule (per roadmap): MQTT topics describe the outside world; this
// registry decides how that world projects into perceptual space.  Topic
// strings never embed RE offsets; offsets live exclusively in mapping rules.
//
// One mapping rule = (topicFilter, sensorIdTemplate, region, extract,
// normalize, ttl, qos, retained-policy, push policy).  Topics with MQTT
// wildcards (+ and #) can fan out across many physical sensors; capture
// groups in the wildcard positions are interpolated into sensorIdTemplate
// to form a unique sensorId per concrete topic.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "reality/json.hpp"

namespace reality {
struct RegionMapping;  // forward decl — full type lives in reality.hpp
using Vector = std::vector<double>;
}

namespace reality::mqtt {

enum class ExtractType {
  Raw,          // entire payload, byte-for-byte, decoded as ASCII float (single)
  CsvFloat,     // split on commas / whitespace; either take an index or all
  Json,         // navigate via JSON pointer
  SingleFloat   // alias for Raw — kept for schema readability
};

enum class NormalizeMode {
  Passthrough,  // pass values through (with optional clamp)
  MinMax,       // (v - min) / (max - min)
  Linear        // v * scale + offset
};

enum class PushMode {
  Debounced,    // mark dirty, fire push N ms after last update on this mapping
  Manual,       // never auto-push; rely on POST /api/push
  Immediate     // push synchronously after every accepted message
};

struct ExtractRule {
  ExtractType type = ExtractType::CsvFloat;
  std::string jsonPointer;          // for ExtractType::Json — e.g. "/value"
  std::optional<int> csvIndex;      // for CsvFloat — take just this index; else all
};

struct NormalizeRule {
  NormalizeMode mode = NormalizeMode::Passthrough;
  double min   = 0.0;
  double max   = 1.0;
  double scale = 1.0;
  double offset = 0.0;
  bool   clamp = true;
};

struct MappingRule {
  std::string id;                   // logical name — used in /api/mqtt/mappings
  std::string topicFilter;          // MQTT wildcard pattern (+ or # allowed)
  std::string sensorIdTemplate;     // optional — "zone.{1}.temp"; {n} captures wildcard
  int         offset = 0;
  int         length = 1;
  ExtractRule extract;
  NormalizeRule normalize;
  long ttlMs = 30000;
  int  qos   = 0;
  bool acceptRetained = true;
  PushMode pushMode   = PushMode::Debounced;
  long debounceMs     = 250;
};

// Per-mapping counters surfaced via /api/mqtt/status.  Atomics so the read
// path on /status doesn't contend with the broker I/O thread.
struct MappingMetrics {
  std::atomic<uint64_t> received { 0 };
  std::atomic<uint64_t> mapped   { 0 };
  std::atomic<uint64_t> rejected { 0 };
  std::atomic<uint64_t> stale    { 0 };
  std::atomic<long long> lastMessageAtMs { 0 };
  std::atomic<long long> lastErrorAtMs   { 0 };
  // lastError is text — stored under a mutex to avoid std::string races.
  // Read via metrics_to_json().
  std::string lastError;
  mutable std::mutex lastErrorMutex;
};

// Parse / load the registry.  Throws std::runtime_error on schema errors.
class MappingRegistry {
public:
  MappingRegistry() = default;

  static MappingRegistry from_json(const json::Value& root);
  static MappingRegistry from_file(const std::string& path);

  struct Match {
    size_t ruleIndex = 0;
    std::vector<std::string> captures;  // values of + and # wildcards (in order)
  };
  // Find the first rule whose topicFilter matches `topic`.  Returns nullopt
  // when no rule matches.  Kept for callers that want to short-circuit; the
  // bridge dispatches via match_all so a single PUBLISH fans out to every
  // rule that shares a filter (e.g. five JSON-pointer extractions from one
  // multi-field sensor payload).
  std::optional<Match> match(const std::string& topic) const;
  // Find every rule whose topicFilter matches `topic`, in declaration order.
  // Empty when no rule matches.
  std::vector<Match> match_all(const std::string& topic) const;

  // Substitute capture groups into the sensorIdTemplate.  When the template
  // is empty, returns the topic itself.  {1}, {2}, ... are 1-indexed.
  std::string resolve_sensor_id(const MappingRule& rule,
                                const std::string& topic,
                                const std::vector<std::string>& captures) const;

  // Run extract + normalize + length-validate on a payload.  Returns the
  // transformed values when valid; otherwise `valid=false` with an `error`
  // string suitable for surfacing on /api/mqtt/status.
  struct Decode {
    Vector values;
    bool valid = false;
    std::string error;
  };
  Decode decode(const MappingRule& rule, const std::vector<uint8_t>& payload) const;

  // Returns a list of human-readable warnings describing overlapping
  // region declarations.  Empty when no overlaps (or when allowOverlap=true).
  std::vector<std::string> validate_overlaps(bool allowOverlap = false) const;

  const std::vector<MappingRule>& rules() const { return rules_; }
  // Metrics are heap-allocated unique_ptrs because MappingMetrics has a
  // non-movable mutex; we return references and the registry owns them.
  MappingMetrics& metrics(size_t i)             { return *metrics_[i]; }
  const MappingMetrics& metrics(size_t i) const { return *metrics_[i]; }

  // Render the registry as JSON for /api/mqtt/mappings.  Includes per-mapping
  // counters so a single GET shows both config and runtime state.
  json::Value to_json() const;

  size_t size() const { return rules_.size(); }

private:
  std::vector<MappingRule> rules_;
  std::vector<std::unique_ptr<MappingMetrics>> metrics_;
};

}  // namespace reality::mqtt
