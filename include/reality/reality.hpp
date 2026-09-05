#pragma once
#include "reality/arbiter.hpp"

#include "reality/json.hpp"

#include <algorithm>
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
// How a machine folds its collection of potential outputs into the single
// output presented to the Perception Engine.
//
// At the completion boundary of the atomic matching action a machine holds one
// potential output per completed Reality Event. Presenting that collection is
// the Reality Engine's job and the last thing it does in the step; this names
// the deterministic elementwise transformation used to combine them.
//
// Declared per machine (`outputMergeTransformation`, default "or") so it is
// available when the machine is interned, and mutable between steps at runtime.
// It is a training variable, which is why it belongs to the machine rather than
// to a deployment.
//
// The first five are Boolean n-input gates over {0,1}. The remaining five are
// multi-valued: they operate on an ordered chain {0..k} and are the fold a
// machine whose cells carry a severity ladder actually needs. Folding such a
// machine with a Boolean gate destroys the ladder — FallDetection ranges over
// {0,1,2,3,4} and every Boolean gate collapses that to 0 or 1
// (RealityEngine_CI#158).
//
// Meet/Join are the lattice operations, StrongDisjunction/StrongConjunction the
// Łukasiewicz ⊕/⊙, and DiscreteMedian the majority-consensus filter. All five
// are closed over the chain, symmetric, and deterministic; Meet, Join and
// DiscreteMedian additionally return one of their contributors and are
// idempotent. The Łukasiewicz pair is deliberately NOT idempotent — x ⊕ x
// saturates and x ⊙ x extinguishes, which is the point of them. Verified by
// exhaustion in RealityEngine_CI/scripts/experiment-mv-transforms.py, which is
// the specification all three runtimes implement against.
enum class OutputMergeTransformation {
  Or, And, Xor, Nor, Nand,
  Meet, Join, StrongConjunction, StrongDisjunction, DiscreteMedian
};
enum class SimPattern { Sine, Sawtooth, Square, LinearRamp, RandomWalk, Constant, GaussianNoise, Binary };
enum class MatchAlgorithm { Gte, Equals };

std::string to_string(ComparatorType t);
std::string to_string(ArbiterRule r);
std::string to_string(SimPattern p);
std::string to_string(MatchAlgorithm a);
ComparatorType comparator_from_string(const std::string& s);
ArbiterRule arbiter_from_string(const std::string& s);
std::string to_string(OutputMergeTransformation t);
OutputMergeTransformation output_merge_from_string(const std::string& s);
// There is no safe default, so there is no default: the two Łukasiewicz
// transformations REFUSE to fold when k is absent, presenting nothing.
//
// A Boolean-chain fallback (k = 1) was tried first and is unsound in both
// directions. ⊕ is min(k, Σx), which clamps: FallDetection's severity ladder
// [0,1,2,3,4,4,0] folds to 1, silently reintroducing the flattening this whole
// vocabulary exists to prevent — through the parameter instead of the gate.
// ⊙ is max(0, Σx − k(n−1)), which does NOT clamp: its closure bound holds only
// while every x_i ≤ k, so the same ladder at k = 1 yields 8, outside the chain
// {0,1} and outside the machine's own alphabet {0..4}. Smallest witness:
// k = 1 over [2,2] gives 3.
//
// So a degenerate fallback either clamps its inputs first, destroying the
// alphabet silently, or does not, violating the closure the ontology asserts.
// Refusing is the only option that neither lies nor fabricates.
//
// This is defence in depth, not the primary guard. A machine should be stopped
// from selecting ⊕/⊙ without a declared alphabet top at the point it is
// configured — the loader and PUT /api/machines/:id/output-merge — so the fold
// never meets the case. It is handled here because a fold that silently did
// something plausible would be undiscoverable.
inline constexpr bool kRefuseChainFoldWithoutTop = true;

// Fold a machine's collection of potential outputs elementwise under `t`.
// Empty collection yields nullopt: a machine that completed no Reality Event
// presents no output, which is different from presenting a zero vector.
//
// `chainTop` is the k of the multi-valued chain {0..k}. It is an explicit
// parameter because k belongs to the machine's alphabet, not to the fold:
//   - Or/And/Xor/Nor/Nand and Meet/DiscreteMedian ignore it entirely;
//   - Join uses it only for an early exit, and is total without it;
//   - StrongConjunction/StrongDisjunction genuinely need it, and REFUSE —
//     returning nullopt, presenting nothing — when it is absent. See
//     kRefuseChainFoldWithoutTop for why no fallback value is sound.
std::optional<Vector> fold_outputs(const std::vector<Vector>& outputs,
                                   OutputMergeTransformation t,
                                   std::optional<int> chainTop = std::nullopt);
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
  // The top of the machine's output alphabet — k, the chain the multi-valued
  // transformations fold over. Distinct from bitsPerElement, which states the
  // representable range and not the alphabet: FallDetection ranges over {0..4}
  // while declaring 4 bits, so k derived from bitsPerElement would be 15 and put
  // strong-disjunction at 14, outside the alphabet entirely
  // (RealityEngine_CI#158). strong-conjunction and strong-disjunction are
  // undefined without it and refuse rather than guess; every other
  // transformation ignores it.
  std::optional<int> outputAlphabetTop = std::nullopt;
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

class RealityEvent {
public:
  std::string id;
  std::vector<VectorElement> elements;
  bool isInitial = false;
  ComparatorType matchAlgorithm = ComparatorType::Gte;
  std::map<std::string, Json> metadata;

  RealityEvent() = default;
  RealityEvent(std::vector<VectorElement> elems, bool initial, std::string vectorId = make_id("vector"));

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
  void add_vector(const RealityEvent& vector);
  std::optional<RealityEvent*> get_vector(const std::string& vectorId);
  std::vector<RealityEvent*> active_vectors();
  std::vector<RealityEvent> all_vectors() const;
  std::pair<bool, std::vector<std::string>> validate() const;
  SequenceResult transition(const Vector& input, std::optional<ComparatorType> overrideType = std::nullopt);
  void reset();
  Json to_json() const;

private:
  std::map<std::string, RealityEvent> vectors;
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
  OutputMergeTransformation outputMergeTransformation = OutputMergeTransformation::Or;
  // Registry copies do not perceive reality.
  //
  // add_machine stores every machine twice — once in the server registry, once
  // in the spaceRuntime — and only the spaceRuntime's is stepped by the PE->RE->PE
  // path. Anything that stepped a registry copy advanced a machine nothing else
  // observes, forking the two permanently and silently: the endpoint reported a
  // transition that the running corpus never made.
  //
  // Set on the registry copy only. process_input() then refuses to transition,
  // so the inhibition holds at one choke point rather than depending on every
  // present and future call site reaching for the spaceRuntime instead. A consumer
  // that wants activity is thereby forced to observe the machines actually in
  // action.
  bool transitionsInhibited = false;
  // Interlock on the knob above. Initialised LOCKED: the transformation is a
  // training variable, and a run that retunes one by accident is a run whose
  // results mean nothing. Changing it requires unlocking first, deliberately
  // and as a separate act.
  //
  // Runtime state, not a corpus property — a machine's declared transformation
  // travels with it, but whether this deployment is currently allowed to change
  // it does not.
  bool outputMergeLocked = true;

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
  // Single-cell store. update_region is the region form and needs a Vector to
  // pass one double, which allocates once per cell — a real cost when 90% of
  // written cells hold exactly one contribution and need no fold at all
  // (ARBITER_CONTRACT.md 4.5). Same bounds contract as update_region.
  void set_cell(int cell, double value);

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

// One operation per machine per output region per step — the single
// contribution the machine presents into arbitration (FOLD_PLACEMENT.md 1).
//
// It used to be one operation per *asserted output*, so a machine whose seven
// CESs completed contributed seven values to the same cell and the cell arbiter
// resolved contention that belonged to the machine. FallDetection resolved to
// 2.0 on C++ and LSP and 0.0 on Scala that way — neither the maximum nor the
// minimum, because the answer depended on which subset fired and how each
// runtime broke ties among same-machine contributions (RealityEngine_CI#154).
// Folding first removes the contention rather than resolving it consistently.
struct MergeOperation {
  RegionMapping region;
  std::string machineId;
  // The CESs whose completed Reality Events folded into `values` — sorted and
  // deduplicated. Replaces the scalar sequenceId: one operation now covers the
  // whole machine, so there is no single firing to name, and collapsing the set
  // to one arbitrarily chosen member is what the fold exists to stop.
  std::vector<std::string> sequenceIds;
  // The collection of potential outputs folded under the machine's
  // outputMergeTransformation. Never a zero vector standing in for "nothing":
  // a machine that completed no Reality Event, or whose fold refused, emits no
  // operation at all.
  Vector values;
  // Same field as MergeOperation.provenance in the AI runtime — emitted in
  // mergeBatch JSON so listeners can render the evidence chain alongside
  // the asserted output. Union over the contributors, order-preserved and
  // deduplicated.
  std::vector<std::string> provenance;
  // Resolved paging contract, joined over the contributors by severity rank
  // (FOLD_PLACEMENT.md 3). std::nullopt when no contributor's fired output is
  // covered by a triggerConfig rule — paging is opt-in per (sequenceId, values).
  std::optional<PagingDecision> governance;
  // Populated when ANY contributing sequence carries deprecatedAt — listeners
  // and dashboards use this to surface stale CESs without re-deriving from JSON.
  std::optional<DeprecationMark> deprecation;
};

// The Reality Events a machine completed in one step, and the evidence behind
// them. Separate from MergeOperation on purpose: an operation exists only when
// the machine has a value to present, and WHICH CESs completed is a fact about
// the step that survives the fold declining to produce one.
//
// This is what drives the event bus. Driving it off mergeBatch instead made a
// fold refusal retract the firings along with the value, so a meta machine
// subscribed to a producer whose fold refused would never see it fire — the
// producer completed its Reality Event and the subscriber was told nothing.
struct MachineFirings {
  std::string machineId;
  std::vector<std::string> sequenceIds;   // sorted, deduplicated
  std::vector<std::string> provenance;    // union, order-preserved, deduped
};

// Did this CES contribute to the folded value? The replacement for the
// `op.sequenceId == id` test that the scalar field used to allow. Membership,
// not equality: an operation covers every Reality Event the machine completed
// this step, so asking whether one of them fired is a set question now.
inline bool contributed(const MergeOperation& op, const std::string& sequenceId) {
  return std::find(op.sequenceIds.begin(), op.sequenceIds.end(), sequenceId)
         != op.sequenceIds.end();
}

// Secondary write triggered by a primary merge — emitted when a fired
// (machineId, sequenceId) matches a subscription declared by another
// machine via metadata.compose.subscriptions[*].  Latches 1.0 at the
// subscriber's bit offset so the next step's snapshot sees the producer's
// "fired" signal as an input bit.  Lets meta-CES domain workflows run as
// ordinary CESs over an event-bus region of perceptual space.  Mirrors
// EventBusWrite in src/engine/PerceptualSpaceRuntime.ts.
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
  // A single member of the machine's collection of potential outputs, chosen by
  // the arbiter. Which member that is has differed per runtime. Kept as-is so
  // existing consumers are unaffected; new ones should read mergedOutputVector.
  std::optional<Vector> outputVector;
  // The collection folded under the machine's outputMergeTransformation — what
  // the Reality Engine presents to the Perception Engine. nullopt when the
  // machine completed no Reality Event, which is not the same as a zero vector.
  std::optional<Vector> mergedOutputVector;
  // The transformation in force for this machine on this step, reported so the
  // knob can be observed rather than inferred.
  std::string outputMergeTransformation = "or";
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
  // Arbitration records for this step — contributors, rule applied, resolved
  // value, and what was suppressed. A discarded contribution has to stay
  // attributable (ARBITER_CONTRACT.md 6).
  std::vector<ArbitrationRecord> arbitration;
};

// Trajectory observation — the two histories the cross-engine trajectory proof
// reads (RealityEngine_CI#148).  Definitions, wire shape and ordering are
// specified in SURFACE_SPEC.md, "Trajectory histories".
//
//   OSRE(n)  the output reality event vector — the resolved output-cell writes
//            committed by the corpus at step n.  Observed at the commit, which
//            is the only moment the corpus's output for the step exists as a
//            single-valued vector.
//   ISRE(n)  the input space reality event vector — the perceptual space as
//            presented to the corpus at step n.  Observed immediately before
//            the machines' input snapshots are taken from it, so the recorded
//            vector and the vector the corpus read cannot differ.
//
// Carried sparsely: a cell absent from `nonZero` is zero.  The dense vector is
// 16k+ cells and a handful are ever non-zero, so sparse is the difference
// between a history that can be kept and one that cannot.  It is lossless —
// `length` and the (index, value) pairs reconstruct the dense vector exactly,
// which is what makes a first-divergent-index comparison possible.
struct TrajectoryCell {
  int index = 0;
  double value = 0.0;
};

struct TrajectoryEntry {
  int stepNumber = 0;
  int length = 0;                       // cells in the full input space
  std::vector<TrajectoryCell> nonZero;  // ascending index
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
  // Bump the paging-decisions counter.  Called by the spaceRuntime whenever
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

class PerceptualSpaceRuntime {
public:
  // Wall clock per phase of a step, for the serialised region that runs after
  // the machine futures join. The OSRE build is ~86% of a step
  // (RealityEngine_CI#256) and "the serialised build" is not a thing anyone can
  // act on — this says which part of it.
  //
  // Deliberately NOT on SimulationStep. That is the parity wire and byte
  // equivalence across the runtimes is the acceptance test; a timing field
  // would differ on every run of every engine by construction. Exposed through
  // /api/metrics instead, which is not compared.
  //
  // Plain integers, not atomics: every mutation happens inside run_phases and
  // every read happens in the /api/metrics handler, and both hold
  // spaceRuntimeMutex. Atomics here would also make the runtime non-copyable.
  struct PhaseTimings {
    std::uint64_t steps           = 0;  // denominator for all of the below
    std::uint64_t machineJoinNs   = 0;  // the barrier itself — waiting on the last machine
    std::uint64_t coverageNs      = 0;  // CES coverage + semantic audit record
    std::uint64_t mergeBuildNs    = 0;  // fold per machine, governance join, merge batch
    std::uint64_t mergeSortNs     = 0;  // canonical ordering of the batch
    std::uint64_t fanInNs         = 0;  // counting contributions per cell
    std::uint64_t gatherNs        = 0;  // building Contributions for contended cells
    std::uint64_t resolveNs       = 0;  // resolve_all
    std::uint64_t commitNs        = 0;  // writing resolved cells + OSRE nonZero
    std::uint64_t eventBusNs      = 0;  // apply_event_bus
    std::uint64_t spaceCopyNs     = 0;  // step.perceptualSpace = space.vector()
    std::uint64_t activeRegionsNs = 0;  // active region build + canonical sort
  };
  const PhaseTimings& phase_timings() const { return phaseTimings; }
  void reset_phase_timings() { phaseTimings = PhaseTimings{}; }

  explicit PerceptualSpaceRuntime(int dimension = 0);
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
  // The machine as it is *running*, Reality Event activation included.
  //
  // add_machine keeps two copies: one in the server's registry and one here,
  // and only this one is stepped. Serving machine detail from the registry
  // therefore reported every RE with its initial isActive no matter how far the
  // machine had advanced, so activation was unobservable from outside the
  // process (#37). nullptr when this spaceRuntime holds no such machine — a
  // machine without a perceptualMapping is never added here.
  const Machine* running_machine(const std::string& machineId) const;
  // The operational machine corpus — every machine this spaceRuntime is stepping.
  //
  // The server keeps a second registry of machines as declared, which is never
  // stepped. Anything answering "what is the engine running" must come from
  // here; the registry answers "what was loaded", which is a different question
  // and has repeatedly been served in place of this one (#37, #58).
  const std::map<std::string, Machine>& running_machines() const;
  // Retune the merge knob on the machine this spaceRuntime is stepping. The
  // registry holds a separate copy; both are set so a read of either agrees.
  bool set_output_merge_transformation(const std::string& machineId, OutputMergeTransformation t);
  bool set_output_merge_locked(const std::string& machineId, bool locked);
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
  // Ascending stepNumber — oldest first.  The step history above is newest
  // first because it is read as "what just happened"; these are read as
  // sequences to be compared element by element, and the index of the first
  // disagreement is the answer they exist to give.
  std::vector<TrajectoryEntry> osre_history() const;
  std::vector<TrajectoryEntry> isre_history() const;
  void set_trajectory_limit(size_t limit);
  size_t trajectory_limit() const;
  PerceptualSpace& perceptual_space();
  int current_step() const;
  bool is_running() const;
  long step_delay_ms() const;

private:
  SimulationStep run_phases(int stepNumber, std::optional<ComparatorType> overrideType);
  PhaseTimings phaseTimings;
  int initialDimension;
  PerceptualSpace space;
  std::map<std::string, Machine> machines;
  std::vector<SimulationStep> steps;
  std::vector<TrajectoryEntry> osreHistory;
  std::vector<TrajectoryEntry> isreHistory;
  size_t maxTrajectory = 1024;
  // Appends ISRE(n) and OSRE(n) together.  They are captured at their own
  // observation points inside the step and recorded in one action, so no
  // observer can see a step whose trajectories are half-written.
  void record_trajectory(TrajectoryEntry isre, TrajectoryEntry osre);
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
  // PerceptualSpaceRuntime.
  std::set<int> latchedEventBits;
  // Pre-computed edge list — rebuilt whenever machines are added or removed so
  // machine_graph_data() does not recompute an O(n²) overlap check on every request.
  mutable Json::Array cachedEdges;
  mutable bool edgesDirty = true;
  void rebuild_edge_cache() const;
  std::vector<EventBusWrite> apply_event_bus(const std::vector<MachineFirings>& firings);
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
  // Clear a source's stored active flag. Activation is earned through ingress;
  // deactivation is an operator action and is honoured directly (#43).
  bool deactivate_source(const std::string& id);
  bool remove_source(const std::string& id);
  std::optional<SourceConfig> get_source(const std::string& id) const;
  std::vector<SourceConfig> get_sources() const;
  // Active sources in canonical (name, id) order — see assemble_vector.
  std::vector<const SourceConfig*> active_sources_canonical() const;
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
Json to_json(const TrajectoryEntry& entry);
Json to_json(const SourceConfig& source);
// The reported `active` is stored AND validated at every read
// (RealityEngine_CI#175), so serializing a source needs a clock. Pass `now`
// when serializing more than one source, so a whole payload is validated
// against a single reading; the one-argument form takes its own.
Json to_json(const SourceConfig& source, long long now);
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
