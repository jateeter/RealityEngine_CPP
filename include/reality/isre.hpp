#pragma once
// The PE half of a K-line: one ISRE composition record per committed step
// (re-isre-composition/1), kept in a persistent store per PE instance
// (RealityEngine_CI#603; SURFACE_SPEC "The PE half: ISRE composition").
//
// Which source won each input cell, of what kind, under the versioned
// source-kind taxonomy the corpus carries
// (<MACHINES_DIR>/../schemas/source-kinds/re-source-kinds-<N>.json).

#include "reality/kline.hpp"
#include "reality/reality.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace reality::isre {

// The taxonomy version the PEs record (owner, 2026-11-27).
constexpr int kTaxonomyVersion = 2;

struct Entry {
  std::string kind;
  std::optional<std::string> provider;
  std::string cls;
};

class Taxonomy {
public:
  // Loads re-source-kinds-<version>.json from `dir`; nullopt when it is not
  // there or does not parse.
  static std::optional<Taxonomy> load(const std::filesystem::path& dir, int version = kTaxonomyVersion);
  // <MACHINES_DIR>/../schemas/source-kinds
  static std::filesystem::path dir_for(const std::string& machinesDir);

  int version() const { return version_; }
  std::string name() const { return "re-source-kinds/" + std::to_string(version_); }
  // A source's entry: its kind, a sensor's provider from its origin (first
  // `.` segment, lowercased, through providerMapping; empty is `signal`;
  // unlisted is the unknown provider), and the version's class.
  Entry source_entry(const std::string& kind, const std::string& origin) const;
  Entry feedback_entry() const;

private:
  int version_ = 0;
  std::map<std::string, std::string> providerMapping_;
  std::map<std::string, std::string> kindClass_;                      // kind -> class (no providers)
  std::map<std::string, std::map<std::string, std::string>> providerClass_;  // kind -> provider -> class
  std::string unknownProvider_ = "unattributed";
  std::string unknownClass_ = "generated";
};

// One cell's winner at a step: a source, or the feedback term.
struct Winner {
  int cell = 0;
  bool feedback = false;
  std::string id, kind, origin;
  Json causedBy = nullptr;
};

// The step's `sources`: runs of consecutive cells with one winner, ascending.
Json sources_json(const std::vector<Winner>& cells, const Taxonomy& taxonomy);

struct Controls {
  bool capture = false;
  long long storeMaxBytes = 2LL * 1024 * 1024 * 1024;
};

class Recorder {
public:
  Recorder(std::string instance, kline::Store store, std::optional<Taxonomy> taxonomy)
      : instance_(std::move(instance)), store_(std::move(store)), taxonomy_(std::move(taxonomy)) {}

  Controls& controls() { return controls_; }
  const Controls& controls() const { return controls_; }
  const std::optional<Taxonomy>& taxonomy() const { return taxonomy_; }

  // One committed step: `re` is the RE step response's clock, `pe` the PE's
  // tick for the record, `folds` the step's fold records.
  void record(const Json& re, const Json& pe, const std::vector<Winner>& cells, const Json& folds);

  Json list(long long from, long long to, int limit) const;
  Json stats() const;
  std::string export_bundle(long long from, long long to);

private:
  std::string instance_;
  kline::Store store_;
  std::optional<Taxonomy> taxonomy_;
  Controls controls_;
  std::string reInstance_;
};

}  // namespace reality::isre
