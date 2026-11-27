#pragma once
// K-lines: episodes captured at their corpus boundaries while the engine steps,
// kept in a persistent store per engine instance (RealityEngine_CI#603,
// RealityEngine_CPP#178).
//
// The specification is RealityEngine_CI SURFACE_SPEC.md: "Boundaries: where a
// K-line starts and ends", "Paths that dwell, fork or merge", "Scope",
// "Identity, clock and parity", "Managing K-lines during operation", "The
// K-line store" and "Record". This is the RE half: the episode, its active
// lists, OSRE, OSRE(m) and scope. The PE records the ISRE composition.
//
// The tracker observes committed steps; it never re-derives the transition
// rule. Each step it reads, per sequence, which events matched and which were
// activated (SequenceResult) and which are active afterwards (the running
// machine), and combines that with its own record of which K-line holds which
// event.

#include "reality/json.hpp"
#include "reality/reality.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace reality::kline {

// RFC 8785 (JCS) serialization of a JSON value: keys in code-point order,
// numbers in ECMAScript form, the minimal escapes.
std::string jcs(const Json& value);
// Lower-case hex SHA-256.
std::string sha256_hex(const std::string& bytes);
// The corpus fingerprint `sha256/resident-declared-v1` over (name, declared
// machine object) pairs, SURFACE_SPEC "Corpus fingerprint".
std::string corpus_fingerprint(const std::vector<std::pair<std::string, Json>>& declared);

struct Stamp {
  std::string instance;
  long long lamport = 0;
  long long commit = 0;
  long step = 0;
  Json json() const;
  static Stamp from(const Json& j);
};

struct Controls {
  bool capture = false;
  int feedbackHops = 3;
  long long storeMaxBytes = 2LL * 1024 * 1024 * 1024;
};

// One machine's operation in a step's mergeBatch, by corpus name.
struct Op {
  std::string machine;
  RegionMapping region;
  Vector values;
  std::vector<std::string> sequenceIds;
};

struct ActiveEntry {
  std::string machine;
  std::string sequence;
  std::string event;
  bool justMatched = false;
  bool operator==(const ActiveEntry& o) const {
    return machine == o.machine && sequence == o.sequence && event == o.event && justMatched == o.justMatched;
  }
};

// What the tracker reads at a step's completion point.
struct StepView {
  long step = 0;
  long long commit = 0;
  const SimulationStep* simulation = nullptr;
  const std::map<std::string, Machine>* running = nullptr;
  const TrajectoryEntry* isre = nullptr;
  const TrajectoryEntry* osre = nullptr;
  std::string fingerprint;
};

// The append-only store under <INSTANCE_KLINE_DIR>/<uuid>/.
class Store {
public:
  Store() = default;
  // `dir` empty: an in-memory store (an instance whose UUID was minted, or a
  // test). Throws when an on-disk store cannot be opened.
  explicit Store(std::filesystem::path dir);

  bool persistent() const { return !dir_.empty(); }
  const std::filesystem::path& dir() const { return dir_; }

  // Appends the step's lines (records and events, in order) and makes them
  // durable before returning. `fingerprint` seals the segment when it changes.
  void append(const std::vector<Json>& lines, const std::string& fingerprint, long long commit);
  void checkpoint(const Json& open);
  std::optional<Json> read_checkpoint() const;

  struct Meta {
    std::string id;
    std::string machine;
    std::string sequence;
    std::string outcome;
    long long closeCommit = 0;
    long closeStep = 0;
    long openStep = 0;
    size_t line = 0;  // index into lines_ (in-memory) or the line offset on disk
    std::string segment;
    std::uint64_t offset = 0;
  };
  const std::vector<Meta>& records() const { return metas_; }
  std::optional<Json> record(const std::string& id) const;
  Json record_at(const Meta& m) const;
  // The stored line of a record or event, byte for byte (bundles carry it).
  std::string raw_at(const Meta& m) const;
  std::vector<std::string> raw_events(const std::string& id) const;

  Json stats(const Controls& controls) const;
  std::vector<std::string> warnings(const Controls& controls) const;
  void mark_exported(const std::set<std::string>& segments);
  void enforce_budget(const Controls& controls);

private:
  struct Segment {
    std::string name;
    std::uint64_t bytes = 0;
    bool sealed = false;
    bool exported = false;
    std::string sha256;
    std::string fingerprint;
    long long firstCommit = 0;
    long long lastCommit = 0;
    bool archived = false;
  };
  std::filesystem::path dir_;
  std::vector<Segment> segments_;
  std::vector<Meta> metas_;
  std::map<std::string, size_t> byId_;
  // sustain-end events per K-line: (through, raw line, at-commit)
  std::map<std::string, std::vector<std::pair<long, std::string>>> events_;
  std::vector<std::string> lines_;  // in-memory store only
  static constexpr std::uint64_t kSealBytes = 64ULL * 1024 * 1024;

  void load();
  void write_manifest() const;
  void write_index() const;
  Segment& active_segment(const std::string& fingerprint, long long commit);
  void seal(Segment& s);
  void index_line(const std::string& raw, const std::string& segment, std::uint64_t offset, size_t line);
};

class Tracker {
public:
  using Tick = std::function<long long()>;

  Tracker(std::string instance, Store store, Tick tick);

  Controls& controls() { return controls_; }
  const Controls& controls() const { return controls_; }
  Store& store() { return store_; }
  const Store& store() const { return store_; }

  // At boot: closes every K-line in the checkpoint `interrupted` ("shutdown")
  // at the step it was checkpointed, and ends every sustain there.
  void recover();
  // klineCapture turned on (at boot or at runtime): capture starts afresh,
  // first closing what the checkpoint holds, as at boot (SURFACE_SPEC
  // "Controls"). Turning it off is `controls().capture = false`, which leaves
  // the store and its checkpoint untouched.
  void start_capture();
  // A committed step (its commit tick already taken).
  void on_step(const StepView& view);
  // A reset: every open K-line closes `interrupted` ("reset") at `last`, the
  // last committed step, and every sustain ends there.
  void on_reset();
  // A machine removed at runtime: its open K-lines close `interrupted`
  // ("machine-removed") and its sustains end.
  void on_machine_removed(const std::string& machineName);

  Json open_json() const;
  Json list(const std::optional<std::string>& outcome, const std::optional<std::string>& machine,
            const std::optional<std::string>& sequence, long long since, int limit) const;
  std::optional<Json> get(const std::string& id) const;
  Json stats() const;
  // A re-kline-bundle/1: JSON Lines, header first. Marks the segments it
  // covered as exported.
  std::string export_bundle(const std::set<std::string>& outcomes, long long since,
                            const std::set<std::string>& machines);

  // The path length L of the longest initial-to-final path in the resident
  // corpus; W = L x (hops + 1) is the start-state ring's depth.
  void set_longest_path(int l) { longestPath_ = l; }
  int ring_depth() const { return longestPath_ * (controls_.feedbackHops + 1); }

  // Exposed for tests: the K-line ids open and sustaining now.
  std::vector<std::string> open_ids() const;

private:
  struct PathEntry {
    std::string event;
    long step = 0;
    bool milestone = false;
    Json outputs = Json::Array{};
  };
  struct Open {
    std::string id;          // assigned when its open boundary ticks
    std::string machine, sequence;
    std::string activator;   // the event id that activated what it holds first
    Stamp open;
    std::set<std::string> held;
    std::vector<PathEntry> path;
    std::string refusedBy;
    // Projected onto the focal machine, step by step: what a record carries
    // when the ring is gone (a shutdown).
    Json focalPairs = Json::Array{};
    Json focalActiveBefore = Json::Array{};
    RegionMapping input, output;
  };
  struct Sustain {
    std::string id, machine, sequence, event;
    long through = 0;
  };
  struct Closed {  // what a contributor search needs of a recently closed K-line
    std::string machine, sequence;
    long openStep = 0, closeStep = 0;
  };
  struct StepRecord {
    long step = 0;
    long long commit = 0;
    TrajectoryEntry isre, osre;
    std::vector<Op> ops;
    // Active lists of the machines whose list changed at this step (deltas).
    std::map<std::string, std::vector<ActiveEntry>> changed;
    // (machine, sequence) -> the K-line that asserted for it at this step.
    std::map<std::pair<std::string, std::string>, std::string> asserted;
    // machine -> (input, output) regions, as they stood
    std::map<std::string, std::pair<RegionMapping, std::optional<RegionMapping>>> regions;
    // machine name -> input vector and outputs at this step
    std::map<std::string, Vector> inputs;
    std::map<std::string, Json> outputs;
  };

  std::string instance_;
  Store store_;
  Tick tick_;
  Controls controls_;
  int longestPath_ = 7;
  std::string fingerprint_;
  long lastStep_ = -1;
  long long lastCommit_ = 0;

  std::vector<Open> open_;
  std::vector<Sustain> sustain_;
  std::map<std::string, Closed> closed_;
  std::vector<StepRecord> ring_;
  std::map<std::string, std::vector<ActiveEntry>> baseline_;  // before ring_.front()
  std::map<std::string, std::vector<ActiveEntry>> current_;   // after the last step

  Stamp stamp(long long lamport, long step, long long commit) const;
  std::vector<ActiveEntry> active_at(const std::string& machine, long step) const;
  const StepRecord* record_for(long step) const;
  Json build_record(const Open& k, const std::string& outcome, const Json& reason, const Stamp& close,
                    const Json& difference, const std::string& subsumedBy, long sustainThrough,
                    bool ringAvailable) const;
  void trim_ring();
  void persist_step(std::vector<Json>& lines);
  Json checkpoint_json() const;
  static std::vector<ActiveEntry> actives_of(const Machine& m);
};

}  // namespace reality::kline
