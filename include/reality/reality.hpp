#pragma once

#include "reality/json.hpp"

#include <chrono>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace reality {

using Vector = std::vector<double>;
using Json = json::Value;

long long now_ms();
std::string make_id(const std::string& prefix);

enum class ComparatorType { Equals, Threshold, Pattern, Custom, Gte };
enum class VectorState { Active, Inactive };
enum class ArbiterRule { And, Or, Passthrough };
enum class SimPattern { Sine, Sawtooth, Square, LinearRamp, RandomWalk, Constant, GaussianNoise, Binary };
enum class MatchAlgorithm { Gte, Equals };

std::string to_string(ComparatorType t);
std::string to_string(ArbiterRule r);
std::string to_string(SimPattern p);
std::string to_string(MatchAlgorithm a);
ComparatorType comparator_from_string(const std::string& s);
ArbiterRule arbiter_from_string(const std::string& s);
SimPattern sim_pattern_from_string(const std::string& s);
MatchAlgorithm match_algorithm_from_string(const std::string& s);

struct RegionMapping {
  int offset = 0;
  int length = 0;
};

struct PerceptualMapping {
  RegionMapping input;
  RegionMapping output;
  // Option A1 narrow-cell declaration.  Internal engine cells remain
  // double-valued; API compact mode uses this for packed wire payloads.
  std::optional<int> bitsPerElement = std::nullopt;
};

struct StorageFootprint {
  size_t float64Bytes = 0;
  size_t packedBytes = 0;
  double shrinkFactor = 0.0;
};

struct VectorElement {
  double value = 0.0;
  std::optional<ComparatorType> comparatorType;
  std::optional<double> threshold;
};

struct MatchResult {
  bool matched = false;
  double score = 0.0;
  std::map<std::string, Json> metadata;
};

struct OutputVector {
  std::string id;
  Vector vector;
  std::map<std::string, Json> metadata;
  long long timestamp = 0;
  // Ordered vector IDs whose matches led to this output — populated by the
  // engine, not the source JSON.  Mirrors `OutputVector.provenance` in the
  // TypeScript runtime so listeners get the same evidence chain on both
  // sides.  Empty when the asserter was an isInitial vector emitting on
  // first match (chain is just [vector.id]).
  std::vector<std::string> provenance;
};

struct SequenceResult {
  std::vector<std::string> matchedVectors;
  std::vector<std::string> activatedVectors;
  std::vector<OutputVector> assertedOutputs;
};

struct ArbiterMetadata {
  std::string rule;
  int totalInputs = 0;
  int sequencesWithOutput = 0;
  bool shouldOutput = false;
};

struct MachineTransitionResult {
  Vector inputVector;
  long long timestamp = 0;
  std::map<std::string, SequenceResult> sequenceResults;
  std::optional<OutputVector> machineOutput;
  ArbiterMetadata arbiterMetadata;
};

class RealityVector {
public:
  std::string id;
  std::vector<VectorElement> elements;
  bool isInitial = false;
  ComparatorType matchAlgorithm = ComparatorType::Gte;
  std::map<std::string, Json> metadata;

  RealityVector() = default;
  RealityVector(std::vector<VectorElement> elems, bool initial, std::string vectorId = make_id("vector"));

  bool is_active() const;
  // Activate with an optional predecessor chain so downstream outputs can
  // carry the full evidence trail.  Empty chain = activated as an initial
  // vector (provenance starts at this.id).
  void set_active(const std::vector<std::string>& predecessorChain = {});
  void clear_active();
  // Full ordered chain that would justify this vector's next emitted output:
  // predecessorChain + [this.id].  Used by CriticalEventSequence to thread
  // provenance into successor activations.
  std::vector<std::string> provenance_chain() const;
  bool was_just_matched() const;
  void set_was_just_matched();
  void clear_was_just_matched();
  void add_next_vector(const std::string& vectorId);
  void add_output_vector(OutputVector ov);
  const std::vector<std::string>& next_vector_ids() const;
  const std::vector<OutputVector>& output_vectors() const;
  MatchResult match(const Vector& input, std::optional<ComparatorType> overrideType = std::nullopt) const;
  struct Transition {
    bool matched = false;
    std::vector<std::string> nextVectorIds;
    std::vector<OutputVector> outputVectors;
    MatchResult matchResult;
    // Full evidence chain through this match — predecessor chain + this.id.
    // Forwarded by CriticalEventSequence::transition to every successor it
    // activates so the chain extends until the terminal output fires.
    std::vector<std::string> provenanceChain;
  };
  Transition transition(const Vector& input, std::optional<ComparatorType> overrideType = std::nullopt);
  Json to_json() const;

private:
  VectorState state = VectorState::Inactive;
  std::vector<std::string> nextVectorIds;
  std::vector<OutputVector> outputVectors;
  bool justMatched = false;
  std::vector<std::string> predecessorChain;
};

class CriticalEventSequence {
public:
  std::string id;
  std::string name;
  std::map<std::string, Json> metadata;
  // Lifecycle metadata — schemaVersion/deprecatedAt/replacedBy mirror the
  // AI runtime fields parsed by MachineLoader from the top level of the
  // sequence JSON.  Listeners read deprecatedAt to surface stale CESs.
  std::string schemaVersion;
  std::string deprecatedAt;
  std::string replacedBy;

  bool is_deprecated() const { return !deprecatedAt.empty(); }
  // Days elapsed since the sequence was deprecated (0 when not set or unparseable).
  long days_since_deprecation() const;

  explicit CriticalEventSequence(std::string sequenceName = "unnamed", std::string sequenceId = make_id("sequence"));
  void add_vector(const RealityVector& vector);
  std::optional<RealityVector*> get_vector(const std::string& vectorId);
  std::vector<RealityVector*> active_vectors();
  std::vector<RealityVector> all_vectors() const;
  std::pair<bool, std::vector<std::string>> validate() const;
  SequenceResult transition(const Vector& input, std::optional<ComparatorType> overrideType = std::nullopt);
  void reset();
  Json to_json() const;

private:
  std::map<std::string, RealityVector> vectors;
};

class OutputArbiter {
public:
  explicit OutputArbiter(ArbiterRule rule = ArbiterRule::Passthrough);
  ArbiterRule get_rule() const;
  void set_rule(ArbiterRule rule);
  struct Decision {
    bool shouldOutput = false;
    std::optional<OutputVector> machineOutput;
    ArbiterRule rule = ArbiterRule::Passthrough;
    int totalInputs = 0;
    int sequencesWithOutput = 0;
  };
  Decision arbitrate(const std::map<std::string, std::vector<OutputVector>>& sequenceOutputs, int totalSequences) const;

private:
  ArbiterRule rule;
};

class Machine {
public:
  std::string id;
  std::string name;
  std::string description;
  std::map<std::string, Json> metadata;
  std::optional<PerceptualMapping> perceptualMapping;
  ComparatorType matchAlgorithm = ComparatorType::Gte;

  Machine(std::string machineName = "unnamed",
          std::string machineDescription = "",
          ArbiterRule arbiterRule = ArbiterRule::Passthrough,
          std::optional<PerceptualMapping> mapping = std::nullopt,
          std::string machineId = make_id("machine"));

  void add_sequence(const CriticalEventSequence& sequence);
  void remove_sequence(const std::string& sequenceId);
  std::vector<CriticalEventSequence> all_sequences() const;
  std::vector<std::string> sequence_ids() const;
  int sequence_count() const;
  int total_vector_count() const;
  ArbiterRule arbiter_rule() const;
  MachineTransitionResult process_input(const Vector& input, std::optional<ComparatorType> overrideType = std::nullopt);
  void reset();
  Json to_json(bool full = false) const;

private:
  std::map<std::string, CriticalEventSequence> sequences;
  OutputArbiter arbiter;
};

// Canonical ordering for machine collections, shared by every runtime.
//
// Machines and sequences are stored in maps keyed by id, and ids are generated
// per runtime — so iteration order differed between C++, LSP and Scala and the
// same corpus serialized to different bytes.  Ordering by content rather than
// by identity is what makes the comparison meaningful.
//
// Machines sort by (metadata.domain, name, id).  The trailing id keeps the
// order total when two machines share a domain and name, and metadata.domain is
// absent on a handful of corpus machines, which sort first under an empty key.
std::string machine_domain(const Machine& m);
bool machine_order_less(const Machine& a, const Machine& b);
// Machines from a map, in canonical order.
std::vector<Machine> machines_in_canonical_order(const std::map<std::string, Machine>& machines);

class PerceptualSpace {
public:
  explicit PerceptualSpace(int dimension = 0);
  int dimension() const;
  const Vector& vector() const;
  // Tolerant write: input larger than current dimension triggers grow_to;
  // input shorter than current dimension zero-fills the tail. Mirrors
  // PerceptualSpace.setPerceptualVector in the AI runtime so cross-runtime
  // /api/perceive payloads behave identically.
  void set_vector(const Vector& values);
  // Expand to newDimension, padding new slots with zeros. Never shrinks.
  void grow_to(int newDimension);
  void reset();
  Vector extract_machine_input(const PerceptualMapping& mapping) const;
  void merge_machine_output(const Vector& output, const PerceptualMapping& mapping);
  void update_region(int offset, const Vector& values);

private:
  Vector values;
};

class PerceptionMapper {
public:
  explicit PerceptionMapper(int universalDimension = 256);
  Vector resolve_input_event_vector(const Vector& universalInputSpace, const PerceptualMapping& mapping);
  Vector resolve_input_event_vector_for_machine(const Vector& universalInputSpace, const Machine& machine);
  std::map<std::string, Vector> resolve_inputs_for_machines(const Vector& universalInputSpace, const std::map<std::string, Machine>& machines);
  void merge_output_into_perceptual_space(const Vector& outputVector, const PerceptualMapping& mapping);
  PerceptualSpace& perceptual_space();
  const PerceptualSpace& perceptual_space() const;
  void reset();
  Json diagnostic_mapping(const Vector& universalInputSpace, const std::map<std::string, Machine>& machines);

private:
  int dimension;
  PerceptualSpace space;
};

struct ActiveRegion {
  int offset = 0;
  int length = 0;
  std::string machineId;
  std::string type;
};

// Paging decision resolved by the governance contract — owner team, SLA,
// runbook URL, escalation policy — derived from the machine's metadata.
// Mirrors PagingDecision in src/services/GovernanceResolver.ts so the wire
// shape is byte-identical across runtimes.  `processStatus`, `ragStatusCode`,
// `escalationPolicy`, and `runbook` may be empty when the source machine
// hasn't declared the field; consumers should treat empty as "not set".
struct PagingDecision {
  std::string machineId;
  std::string machineName;
  std::string sequenceId;
  std::string ragStatusCode;      // "GREEN" | "AMBER" | "RED" | ""
  std::string processStatus;      // "ok" | "info" | "warning" | "error" | ""
  std::string ownerTeam;
  std::optional<int> slaSeconds;
  std::string runbook;
  std::string escalationPolicy;
  std::string contactPrimary;
  std::string contactSecondary;
  std::string description;
  std::string source;             // "rule-with-override" | "rule-only" | "machine-fallback"
  bool hasMachineGovernance = false;
};

struct DeprecationMark {
  std::string since;        // deprecatedAt
  std::string replacedBy;
  long ageDays = 0;
};

struct MergeOperation {
  RegionMapping region;
  std::string machineId;
  std::string sequenceId;
  size_t outputIndex = 0;
  Vector values;
  // Same field as MergeOperation.provenance in the AI runtime — emitted in
  // mergeBatch JSON so listeners can render the evidence chain alongside
  // the asserted output.
  std::vector<std::string> provenance;
  // Resolved paging contract.  std::nullopt when the fired output is not
  // covered by a triggerConfig rule — paging is opt-in per (sequenceId, values).
  std::optional<PagingDecision> governance;
  // Populated when the firing sequence carries deprecatedAt — listeners and
  // dashboards use this to surface stale CESs without re-deriving from JSON.
  std::optional<DeprecationMark> deprecation;
};

// Secondary write triggered by a primary merge — emitted when a fired
// (machineId, sequenceId) matches a subscription declared by another
// machine via metadata.compose.subscriptions[*].  Latches 1.0 at the
// subscriber's bit offset so the next step's snapshot sees the producer's
// "fired" signal as an input bit.  Lets meta-CES domain workflows run as
// ordinary CESs over an event-bus region of perceptual space.  Mirrors
// EventBusWrite in src/engine/PerceptualSpaceSimulator.ts.
struct EventBusWrite {
  std::string producerMachineId;
  std::string producerSequenceId;
  std::string subscriberMachineId;
  int bitOffset = 0;
  double value = 1.0;
  std::vector<std::string> provenance;
};

struct MachineStepResult {
  std::string machineId;
  std::string machineName;
  Vector inputVector;
  std::optional<Vector> outputVector;
  RegionMapping inputRegion;
  std::optional<RegionMapping> outputRegion;
  MachineTransitionResult transitionResult;
};

struct SimulationStep {
  int stepNumber = 0;
  long long timestamp = 0;
  Vector perceptualSpace;
  std::map<std::string, MachineStepResult> machineResults;
  std::vector<ActiveRegion> activeRegions;
  std::vector<MergeOperation> mergeBatch;
  // Empty when no subscriptions are active.  Same wire shape as the AI
  // runtime's step.eventBus so cross-runtime parity covers composition too.
  std::vector<EventBusWrite> eventBus;
};

struct WorkerPoolMetrics {
  size_t workers = 0;
  size_t queued = 0;
  size_t active = 0;
  size_t completed = 0;
  size_t rejected = 0;
  size_t capacity = 0;
};

// Semantic audit trail — re:SequenceObservation records emitted while
// machines process input.  Shapes and IRI rules are specified in
// RealityEngine_Machines docs/SEMANTIC_AUDIT_CONTRACT.md (milestone M5); the
// server joins the corpus semantics manifest at read time to attach IRIs.
struct SequenceObservation {
  long long at = 0;
  std::string machineId;
  std::string machineName;
  std::string sequenceId;
  std::string stepId;
  bool completed = false;
  std::string determinationId;  // empty when the step emitted no output
  std::string actionCode;
  std::string ragStatus;
};

class SemanticAuditLog {
public:
  static constexpr size_t Capacity = 1000;
  // Append every matched step of every sequence in `result`.
  void record(const Machine& machine, const MachineTransitionResult& result);
  // Oldest-to-newest, at most `limit` most recent observations.
  std::vector<SequenceObservation> recent(size_t limit) const;
  size_t size() const;
  void clear();

private:
  mutable std::mutex mutex;
  std::deque<SequenceObservation> records;
};

// CES coverage telemetry — matched/activated/output counters keyed by
// (machineId, sequenceId, vectorId).  Mirrors src/services/CesCoverageRegistry.ts
// in the AI runtime so the /metrics endpoints in both engines share the same
// metric names, label sets, and unfired-* gauge derivations.
class CesCoverageRegistry {
public:
  void record(const Machine& machine, const MachineTransitionResult& result);
  // Bump the paging-decisions counter.  Called by the simulator whenever
  // a mergeBatch entry carries a resolved governance contract.
  void record_paging_decision(const std::string& ownerTeam,
                              const std::string& processStatus,
                              const std::string& ragStatusCode,
                              const std::string& machineId);
  // Bump the deprecated-fires counter — emitted as ces_deprecated_fires_total.
  void record_deprecated_fire(const std::string& machineId,
                              const std::string& machineName,
                              const std::string& sequenceId,
                              const std::string& replacedBy);
  void reset();
  // Render Prometheus text-format exposition.  Caller passes every registered
  // machine so we can derive the unfired-* gauges from the corpus, not just
  // the counters we've seen.  `baseLabels` is stamped onto every metric line
  // (used to attach a runtime="cpp" identifier so a single Prometheus scrape
  // config can drive a cross-runtime Grafana dashboard).
  std::string to_prometheus_text(const std::map<std::string, Machine>& machines,
                                 const std::vector<std::pair<std::string, std::string>>& baseLabels = {}) const;

private:
  // Tab-separated keys to keep a single std::map alloc instead of nested maps.
  std::map<std::string, long long> matched;          // mid \t mname \t sid \t vid
  std::map<std::string, long long> activated;        // mid \t mname \t sid \t vid
  std::map<std::string, long long> outputs;          // mid \t mname \t sid
  std::map<std::string, long long> steps;            // mid \t mname
  std::map<std::string, long long> pagingDecisions;  // team \t pstatus \t rag \t mid
  std::map<std::string, long long> deprecatedFires;  // mid \t mname \t sid \t replacedBy
  long long startedAtMs = now_ms();
};

class PerceptualSpaceSimulator {
public:
  explicit PerceptualSpaceSimulator(int dimension = 0);
  int dimension() const;
  // Required dimension across all currently registered machines —
  // max(offset + length) over every input and output mapping. Returned by
  // /api/runtime/vector-space so external clients can detect a stale PE.
  int required_dimension() const;
  // Bumped on every add_machine / remove_machine that changes the
  // dimension or the set of mappings. External clients can cache shape
  // assumptions keyed by this value.
  long mapping_version() const;
  // Coverage telemetry accumulated across every process_immediate / step call.
  CesCoverageRegistry& ces_coverage();
  const CesCoverageRegistry& ces_coverage() const;
  // Semantic audit trail accumulated across the same calls (milestone M5).
  SemanticAuditLog& semantic_audit();
  const SemanticAuditLog& semantic_audit() const;
  // Number of declared (subscriberMachine, producerMachineId, producerSequenceId)
  // subscriptions, summed across all registered meta-machines.
  size_t event_bus_subscription_count() const;
  void add_machine(const Machine& machine);
  bool remove_machine(const std::string& machineId);
  void configure(std::vector<Vector> inputSequence, RegionMapping inputRegion, long stepDelayMs, std::optional<int> maxSteps = std::nullopt);
  void start();
  void stop();
  void reset();
  std::optional<SimulationStep> step();
  SimulationStep process_immediate(const Vector& vector, std::optional<ComparatorType> overrideType = std::nullopt);
  Json machine_graph_data() const;
  Json state_json() const;
  std::vector<SimulationStep> history() const;
  void set_history_limit(size_t limit);
  size_t history_limit() const;
  PerceptualSpace& perceptual_space();
  int current_step() const;
  bool is_running() const;
  long step_delay_ms() const;

private:
  SimulationStep run_phases(int stepNumber, std::optional<ComparatorType> overrideType);
  int initialDimension;
  PerceptualSpace space;
  std::map<std::string, Machine> machines;
  std::vector<SimulationStep> steps;
  std::vector<Vector> configuredInputSequence;
  RegionMapping configuredInputRegion;
  long configuredStepDelayMs = 100;
  std::optional<int> configuredMaxSteps;
  size_t maxHistory = 256;
  int currentStep = 0;
  int immediateStepCount = 0;
  bool running = false;
  bool configured = false;
  long mappingVersion = 0;
  CesCoverageRegistry coverage;
  SemanticAuditLog semanticAudit;
  struct ComposeSubscription {
    std::string subscriberMachineId;
    int bitOffset = 0;
    std::string producerMachineId;
    std::string producerSequenceId;
  };
  // Keyed by `producerMachineId|producerSequenceId`.  Maps producer-fired
  // events to the list of subscribers that want a bit latched.
  std::map<std::string, std::vector<ComposeSubscription>> eventBusSubscriptions;
  // Bits that have fired at least once since reset.  Re-applied at the
  // top of every process_immediate call so a caller-provided input vector
  // (which zero-fills past its length) doesn't clobber persistent
  // workflow milestones.  Same semantic as latchedEventBits in the AI
  // PerceptualSpaceSimulator.
  std::set<int> latchedEventBits;
  // Pre-computed edge list — rebuilt whenever machines are added or removed so
  // machine_graph_data() does not recompute an O(n²) overlap check on every request.
  mutable Json::Array cachedEdges;
  mutable bool edgesDirty = true;
  void rebuild_edge_cache() const;
  std::vector<EventBusWrite> apply_event_bus(const std::vector<MergeOperation>& mergeBatch);
};

struct SourceConfig {
  std::string kind = "simulated";
  std::string id;
  std::string name;
  RegionMapping region;
  bool active = true;
  std::string machineId;
  std::string machineName;
  std::string sequenceName;
  std::vector<Vector> inputs;
  Json sequenceMetadata = Json::Object{};
  Json testSequence = Json::Object{};
  bool loop = true;
  SimPattern pattern = SimPattern::Constant;
  double frequency = 1.0;
  double amplitude = 1.0;
  double dcOffset = 0.0;
  std::string sensorId;
  Vector lastValue;
  std::optional<long long> lastUpdated;
  long ttlMs = 5000;
  // Provenance — which integration feeds this source ("mqtt", "openclaw",
  // "ollama", "healthkit", "carekit", "localai", "signal").  Empty for
  // manually created sources; omitted from JSON when unset.
  std::string origin;
};

class PerceptionEngine {
public:
  explicit PerceptionEngine(int vectorDimension = 256);
  MatchAlgorithm matchAlgorithm = MatchAlgorithm::Gte;
  long long globalStep = 0;

  SourceConfig add_source(SourceConfig source);
  bool remove_source(const std::string& id);
  std::optional<SourceConfig> get_source(const std::string& id) const;
  std::vector<SourceConfig> get_sources() const;
  bool update_sensor_value(const std::string& sensorId, const Vector& values);
  Vector assemble_vector() const;
  void update_from_perceptual_space(const Vector& values);
  void advance();
  void reset();
  Json state_json(std::optional<long long> lastPush, bool autoRunning, long autoIntervalMs) const;
  // Number of elements this engine will actually read and write.  Grows as
  // sources are added and as the RE reports a larger perceptual space, so a
  // machine whose perceptualMapping extends past the configured default still
  // receives input.  Reported as perceptionDimension.
  int vector_dimension() const { return dimension; }

private:
  Vector source_values(const SourceConfig& source) const;
  // Expand persistentVector and dimension to cover [0, requiredEnd).
  // `context` names the machine/source that required the growth so the log
  // line is diagnosable.
  void ensure_capacity(int requiredEnd, const std::string& context);
  std::map<std::string, SourceConfig> sources;
  std::map<std::string, int> testStep;
  std::map<std::string, Vector> walkState;
  int dimension = 256;
  Vector persistentVector;
};

// Loader options — `strictSta=true` runs the Single Transition Assumption
// analyzer (see reality/sta_checker.hpp) on the raw JSON before construction
// and throws sta::StaViolationError when a machine tagged
// `metadata.severity == "life-safety"` carries any intra-sequence HD>1
// transition.  Non-life-safety machines are accepted regardless.  Default
// strictSta=false preserves the historical permissive behaviour.
struct LoadOptions {
  bool strictSta = false;
};

Machine load_machine_from_json_string(const std::string& raw,
                                      std::optional<std::string> id = std::nullopt,
                                      const LoadOptions& opts = {});
std::vector<Machine> load_machines_from_directory(const std::string& directory,
                                                  const LoadOptions& opts = {});
// Resolve a corpus filename under DIRECTORY: flat path first, then a
// recursive basename search (corpus files may live in machines/domains/<x>/;
// filenames are globally unique). Returns DIRECTORY/FILENAME when not found
// so callers keep their existing not-found handling.
std::filesystem::path find_machine_file(const std::filesystem::path& directory,
                                        const std::string& filename);

Json to_json(const RegionMapping& r);
Json to_json(const PerceptualMapping& m);
Json to_json(const OutputVector& o);
Json to_json(const SequenceResult& r);
Json to_json(const MachineTransitionResult& r);
Json to_json(const SimulationStep& step);
Json to_json(const SimulationStep& step, bool includeMachineResults);
Json to_json(const SimulationStep& step, bool includeMachineResults, bool includePerceptualSpace);
Json to_json(const SourceConfig& source);
Json worker_pool_metrics_json();
Json to_json(const PagingDecision& d);

bool is_allowed_bits_per_element(int bits);
void validate_cell_range(const Vector& values, int bitsPerElement);
std::vector<unsigned char> pack_cells(const Vector& values, int bitsPerElement);
Vector unpack_cells(const std::vector<unsigned char>& bytes, size_t length, int bitsPerElement);
std::string encode_packed_base64(const Vector& values, int bitsPerElement);
StorageFootprint storage_footprint(size_t length, int bitsPerElement);

// Resolve a paging contract for one fired output.  Walks
// machine.metadata.triggerConfig.rules looking for a (sequenceId, values)
// match, then merges in metadata.governance defaults.  Returns std::nullopt
// when no rule matches.  Same precedence as the AI runtime:
//   rule.governance.<field>  →  machine.metadata.governance.<field>  →  null.
std::optional<PagingDecision> resolve_governance(
    const Machine& machine, const std::string& sequenceId, const std::vector<double>& values);

} // namespace reality
