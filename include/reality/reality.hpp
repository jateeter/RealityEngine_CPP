#pragma once

#include "reality/json.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <random>
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
  void set_active();
  void clear_active();
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
  };
  Transition transition(const Vector& input, std::optional<ComparatorType> overrideType = std::nullopt);
  Json to_json() const;

private:
  VectorState state = VectorState::Inactive;
  std::vector<std::string> nextVectorIds;
  std::vector<OutputVector> outputVectors;
  bool justMatched = false;
};

class CriticalEventSequence {
public:
  std::string id;
  std::string name;
  std::map<std::string, Json> metadata;

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

class PreceptionEngine {
public:
  explicit PreceptionEngine(int universalDimension = 256);
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

struct MergeOperation {
  RegionMapping region;
  std::string machineId;
  std::string sequenceId;
  size_t outputIndex = 0;
  Vector values;
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
};

struct WorkerPoolMetrics {
  size_t workers = 0;
  size_t queued = 0;
  size_t active = 0;
  size_t completed = 0;
  size_t rejected = 0;
  size_t capacity = 0;
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

private:
  Vector source_values(const SourceConfig& source) const;
  std::map<std::string, SourceConfig> sources;
  std::map<std::string, int> testStep;
  std::map<std::string, Vector> walkState;
  int dimension = 256;
  Vector persistentVector;
};

Machine load_machine_from_json_string(const std::string& raw, std::optional<std::string> id = std::nullopt);
std::vector<Machine> load_machines_from_directory(const std::string& directory);

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

} // namespace reality
