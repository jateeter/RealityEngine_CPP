#pragma once
// Runtime snapshots (RealityEngine_CI#287, phase 2; SURFACE_SPEC "K-lines and
// runtime snapshots": "The snapshot body", "Routes"). A snapshot is a K-line over
// retained steps plus what it takes to re-establish it on an engine holding the
// same corpus: re-snapshot/1, runtime-independent, digest over its JCS form.
//
// The retention here keeps, for every retained step (historyLimit), the active
// list after the step, its contributions and its clock, whether or not K-line
// capture is on (owner decision 4, 2026-11-24).

#include "reality/kline.hpp"
#include "reality/reality.hpp"

#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace reality::snapshot {

struct Retained {
  long step = 0;
  long long lamport = 0;
  TrajectoryEntry isre, osre;
  std::vector<kline::Op> ops;
  std::vector<kline::ActiveEntry> after;
};

// Every resident machine's active events, as active entries.
std::vector<kline::ActiveEntry> actives(const std::map<std::string, Machine>& running);
// The canonical active-list JSON: ordered by (machine, sequence, event) bytes.
Json actives_json(std::vector<kline::ActiveEntry> list);

class Retention {
public:
  // At boot and after a reset: the state the first retained step starts from.
  void on_reset(const std::map<std::string, Machine>& running);
  void on_step(long step, long long lamport, const TrajectoryEntry* isre, const TrajectoryEntry* osre,
               const SimulationStep& sim, const std::map<std::string, Machine>& running, size_t limit);
  // After a corpus restore: nothing retained, and the restored list is the base.
  void restore(std::vector<kline::ActiveEntry> base);
  const std::deque<Retained>& steps() const { return steps_; }
  const Retained* at(long step) const;
  // The list before `step`: after step-1, or the base when it is the first.
  std::optional<std::vector<kline::ActiveEntry>> before(long step) const;

private:
  std::vector<kline::ActiveEntry> base_;
  bool baseSet_ = false;  // a restore sets it; otherwise the initial events at the first step
  std::deque<Retained> steps_;
};

struct Error {
  int status = 422;
  Json body;
};

struct CaptureRequest {
  std::optional<std::set<std::string>> scope;  // nullopt: the corpus
  std::optional<long> from, to;
  std::string instance;
  std::string fingerprint;
  size_t machines = 0;
  Json controls, machineControls, machineOverrides, arbitration;
  const std::map<std::string, Machine>* running = nullptr;
};

// The snapshot body, or why there is none (410, 422).
std::optional<Json> capture(const Retention& r, const CaptureRequest& req, Error& error);
// "sha256:<hex>" over the body without digest and the clocks' instance and lamport.
std::string digest_of(const Json& body);
// The id a snapshot is stored under: its digest's hex.
std::string id_of(const Json& body);

class Store {
public:
  // RE_SNAPSHOT_STORE: memory (default), file:<dir>, off. qdrant:<url> is phase
  // 3 and is off until then, never a boot failure.
  static Store from_environment();
  bool enabled() const { return mode_ != Mode::Off; }
  std::string mode() const;
  void put(const std::string& id, const Json& body);
  std::optional<Json> get(const std::string& id) const;
  bool remove(const std::string& id);
  // {id, digest, scope, span, machines}, oldest first by clocks.to.lamport.
  Json list() const;

private:
  enum class Mode { Memory, File, Off };
  Mode mode_ = Mode::Memory;
  std::filesystem::path dir_;
  std::map<std::string, Json> memory_;
};

}  // namespace reality::snapshot
