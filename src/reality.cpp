#include "reality/reality.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <functional>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace reality {

long long now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string make_id(const std::string& prefix) {
  static std::mt19937_64 rng{std::random_device{}()};
  static std::mutex rngMutex;
  std::lock_guard<std::mutex> lock(rngMutex);
  return prefix + "-" + std::to_string(now_ms()) + "-" + std::to_string(rng() % 1000000000ULL);
}

static std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

class DomainWorkerPool {
public:
  DomainWorkerPool() {
    size_t n = static_cast<size_t>(std::max(2u, std::thread::hardware_concurrency()));
    workers.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      workers.emplace_back([this]() { run(); });
    }
  }

  ~DomainWorkerPool() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
    }
    cv.notify_all();
    for (auto& worker : workers) if (worker.joinable()) worker.join();
  }

  template <typename Fn>
  auto submit(Fn&& fn) -> std::future<decltype(fn())> {
    using R = decltype(fn());
    auto task = std::make_shared<std::packaged_task<R()>>(std::forward<Fn>(fn));
    auto future = task->get_future();
    {
      std::lock_guard<std::mutex> lock(mutex);
      jobs.push_back([task]() { (*task)(); });
    }
    cv.notify_one();
    return future;
  }

private:
  void run() {
    while (true) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this]() { return stopping || !jobs.empty(); });
        if (stopping && jobs.empty()) return;
        job = std::move(jobs.front());
        jobs.pop_front();
      }
      job();
    }
  }

  std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::function<void()>> jobs;
  std::vector<std::thread> workers;
  bool stopping = false;
};

static DomainWorkerPool& domain_workers() {
  static DomainWorkerPool pool;
  return pool;
}

std::string to_string(ComparatorType t) {
  switch (t) {
    case ComparatorType::Equals: return "equals";
    case ComparatorType::Threshold: return "threshold";
    case ComparatorType::Pattern: return "pattern";
    case ComparatorType::Custom: return "custom";
    case ComparatorType::Gte: return "gte";
  }
  return "gte";
}

std::string to_string(ArbiterRule r) {
  switch (r) {
    case ArbiterRule::And: return "and";
    case ArbiterRule::Or: return "or";
    case ArbiterRule::Passthrough: return "passthrough";
  }
  return "passthrough";
}

std::string to_string(SimPattern p) {
  switch (p) {
    case SimPattern::Sine: return "sine";
    case SimPattern::Sawtooth: return "sawtooth";
    case SimPattern::Square: return "square";
    case SimPattern::LinearRamp: return "linear-ramp";
    case SimPattern::RandomWalk: return "random-walk";
    case SimPattern::Constant: return "constant";
    case SimPattern::GaussianNoise: return "gaussian-noise";
    case SimPattern::Binary: return "binary";
  }
  return "constant";
}

std::string to_string(MatchAlgorithm a) {
  return a == MatchAlgorithm::Equals ? "equals" : "gte";
}

ComparatorType comparator_from_string(const std::string& s) {
  const auto v = lower(s);
  if (v == "equals") return ComparatorType::Equals;
  if (v == "threshold") return ComparatorType::Threshold;
  if (v == "pattern") return ComparatorType::Pattern;
  if (v == "custom") return ComparatorType::Custom;
  if (v == "gte") return ComparatorType::Gte;
  throw std::invalid_argument("Unknown comparator type: " + s);
}

ArbiterRule arbiter_from_string(const std::string& s) {
  const auto v = lower(s);
  if (v == "and") return ArbiterRule::And;
  if (v == "or") return ArbiterRule::Or;
  if (v == "passthrough") return ArbiterRule::Passthrough;
  throw std::invalid_argument("Unknown arbiter rule: " + s);
}

SimPattern sim_pattern_from_string(const std::string& s) {
  if (s == "sine") return SimPattern::Sine;
  if (s == "sawtooth") return SimPattern::Sawtooth;
  if (s == "square") return SimPattern::Square;
  if (s == "linear-ramp") return SimPattern::LinearRamp;
  if (s == "random-walk") return SimPattern::RandomWalk;
  if (s == "constant") return SimPattern::Constant;
  if (s == "gaussian-noise") return SimPattern::GaussianNoise;
  if (s == "binary") return SimPattern::Binary;
  throw std::invalid_argument("Unknown sim pattern: " + s);
}

MatchAlgorithm match_algorithm_from_string(const std::string& s) {
  if (s == "equals") return MatchAlgorithm::Equals;
  if (s == "gte") return MatchAlgorithm::Gte;
  throw std::invalid_argument("Unknown match algorithm: " + s);
}

RealityVector::RealityVector(std::vector<VectorElement> elems, bool initial, std::string vectorId)
  : id(std::move(vectorId)), elements(std::move(elems)), isInitial(initial),
    state(initial ? VectorState::Active : VectorState::Inactive) {}

bool RealityVector::is_active() const { return state == VectorState::Active; }
void RealityVector::set_active() { state = VectorState::Active; }
void RealityVector::clear_active() { if (!isInitial) state = VectorState::Inactive; }
bool RealityVector::was_just_matched() const { return justMatched; }
void RealityVector::set_was_just_matched() { justMatched = true; }
void RealityVector::clear_was_just_matched() { justMatched = false; }
void RealityVector::add_next_vector(const std::string& vectorId) {
  if (std::find(nextVectorIds.begin(), nextVectorIds.end(), vectorId) == nextVectorIds.end()) nextVectorIds.push_back(vectorId);
}
void RealityVector::add_output_vector(OutputVector ov) { outputVectors.push_back(std::move(ov)); }
const std::vector<std::string>& RealityVector::next_vector_ids() const { return nextVectorIds; }
const std::vector<OutputVector>& RealityVector::output_vectors() const { return outputVectors; }

MatchResult RealityVector::match(const Vector& input, std::optional<ComparatorType> overrideType) const {
  if (input.size() != elements.size()) {
    MatchResult r;
    r.metadata["error"] = "Vector dimension mismatch";
    return r;
  }
  double total = 0.0;
  for (size_t i = 0; i < elements.size(); ++i) {
    const auto& elem = elements[i];
    double in = input[i];
    ComparatorType type = overrideType.value_or(elem.comparatorType.value_or(matchAlgorithm));
    double score = 0.0;
    bool ok = false;
    switch (type) {
      case ComparatorType::Equals:
      case ComparatorType::Custom:
        ok = elem.value == in;
        score = ok ? 1.0 : 0.0;
        break;
      case ComparatorType::Threshold: {
        double t = elem.threshold.value_or(0.1);
        double diff = std::abs(elem.value - in);
        ok = diff <= t;
        score = ok && t != 0.0 ? 1.0 - diff / t : (ok ? 1.0 : 0.0);
        break;
      }
      case ComparatorType::Pattern: {
        score = 1.0 - std::abs(elem.value - in);
        ok = score >= elem.threshold.value_or(0.5);
        break;
      }
      case ComparatorType::Gte: {
        double t = elem.threshold.value_or(0.5);
        bool inputHigh = in >= t;
        bool valueHigh = elem.value >= t;
        ok = inputHigh == valueHigh;
        if (ok) {
          score = inputHigh
            ? (t < 1.0 ? (in - t) / (1.0 - t) : 1.0)
            : (t > 0.0 ? (t - in) / t : 1.0);
          score = std::clamp(score, 0.0, 1.0);
        }
        break;
      }
    }
    if (!ok) {
      MatchResult r;
      r.score = elements.empty() ? 0.0 : total / static_cast<double>(elements.size());
      r.metadata["failedAtIndex"] = static_cast<double>(i);
      return r;
    }
    total += score;
  }
  return MatchResult{true, elements.empty() ? 0.0 : total / static_cast<double>(elements.size()), {}};
}

RealityVector::Transition RealityVector::transition(const Vector& input, std::optional<ComparatorType> overrideType) {
  auto mr = match(input, overrideType);
  if (!mr.matched) {
    if (!isInitial) clear_active();
    return {false, {}, {}, mr};
  }
  bool isFinal = !outputVectors.empty();
  bool isTransitional = !isInitial && !isFinal;
  if (isTransitional && !nextVectorIds.empty()) clear_active();
  return {true, nextVectorIds, outputVectors, mr};
}

Json RealityVector::to_json() const {
  Json::Array elems;
  for (const auto& e : elements) {
    Json::Object o{{"value", e.value}};
    if (e.comparatorType) o["comparatorType"] = to_string(*e.comparatorType);
    if (e.threshold) o["threshold"] = *e.threshold;
    elems.emplace_back(o);
  }
  Json::Array next;
  for (const auto& n : nextVectorIds) next.emplace_back(n);
  Json::Array outs;
  for (const auto& o : outputVectors) outs.push_back(reality::to_json(o));
  return Json::Object{
    {"id", id}, {"matchAlgorithm", to_string(matchAlgorithm)}, {"elements", elems},
    {"state", is_active() ? "active" : "inactive"}, {"isActive", is_active()},
    {"nextVectorIds", next}, {"outputVectors", outs}, {"isInitial", isInitial},
    {"wasJustMatched", justMatched}, {"metadata", metadata}
  };
}

CriticalEventSequence::CriticalEventSequence(std::string sequenceName, std::string sequenceId)
  : id(std::move(sequenceId)), name(std::move(sequenceName)) {}

void CriticalEventSequence::add_vector(const RealityVector& vector) { vectors[vector.id] = vector; }
std::optional<RealityVector*> CriticalEventSequence::get_vector(const std::string& vectorId) {
  auto it = vectors.find(vectorId);
  if (it == vectors.end()) return std::nullopt;
  return &it->second;
}
std::vector<RealityVector*> CriticalEventSequence::active_vectors() {
  std::vector<RealityVector*> out;
  for (auto& [_, v] : vectors) if (v.is_active()) out.push_back(&v);
  return out;
}
std::vector<RealityVector> CriticalEventSequence::all_vectors() const {
  std::vector<RealityVector> out;
  for (const auto& [_, v] : vectors) out.push_back(v);
  return out;
}
std::pair<bool, std::vector<std::string>> CriticalEventSequence::validate() const {
  bool hasInitial = false, hasOutput = false;
  for (const auto& [_, v] : vectors) {
    hasInitial = hasInitial || v.isInitial;
    hasOutput = hasOutput || !v.output_vectors().empty();
  }
  std::vector<std::string> errors;
  if (!hasInitial) errors.push_back("CriticalEventSequence must have at least one initial vector");
  if (!hasOutput) errors.push_back("CriticalEventSequence must have at least one vector with output");
  return {errors.empty(), errors};
}
SequenceResult CriticalEventSequence::transition(const Vector& input, std::optional<ComparatorType> overrideType) {
  for (auto& [_, v] : vectors) v.clear_was_just_matched();
  SequenceResult result;
  std::set<std::string> pending;
  auto active = active_vectors();
  for (auto* v : active) {
    auto tr = v->transition(input, overrideType);
    if (!tr.matched) continue;
    result.matchedVectors.push_back(v->id);
    if (!v->output_vectors().empty()) v->set_was_just_matched();
    pending.insert(tr.nextVectorIds.begin(), tr.nextVectorIds.end());
    result.assertedOutputs.insert(result.assertedOutputs.end(), tr.outputVectors.begin(), tr.outputVectors.end());
  }
  for (const auto& id : pending) {
    auto it = vectors.find(id);
    if (it != vectors.end() && !it->second.is_active()) {
      it->second.set_active();
      result.activatedVectors.push_back(id);
    }
  }
  return result;
}
void CriticalEventSequence::reset() {
  for (auto& [_, v] : vectors) v.isInitial ? v.set_active() : v.clear_active();
}
Json CriticalEventSequence::to_json() const {
  Json::Array arr;
  Json::Array initials;
  Json::Array outputs;
  for (const auto& [id, v] : vectors) {
    arr.push_back(v.to_json());
    if (v.isInitial) initials.emplace_back(id);
    if (!v.output_vectors().empty()) outputs.emplace_back(id);
  }
  return Json::Object{{"id", id}, {"name", name}, {"vectors", arr}, {"initialVectorIds", initials}, {"outputVectorIds", outputs}, {"metadata", metadata}};
}

OutputArbiter::OutputArbiter(ArbiterRule r) : rule(r) {}
ArbiterRule OutputArbiter::get_rule() const { return rule; }
void OutputArbiter::set_rule(ArbiterRule r) { rule = r; }
OutputArbiter::Decision OutputArbiter::arbitrate(const std::map<std::string, std::vector<OutputVector>>& sequenceOutputs, int totalSequences) const {
  std::vector<OutputVector> all;
  int withOutput = 0;
  for (const auto& [_, outs] : sequenceOutputs) {
    if (!outs.empty()) ++withOutput;
    all.insert(all.end(), outs.begin(), outs.end());
  }
  bool should = false;
  if (rule == ArbiterRule::And) should = withOutput == totalSequences && totalSequences > 0;
  if (rule == ArbiterRule::Or) should = withOutput > 0;
  if (rule == ArbiterRule::Passthrough) should = !all.empty();
  std::optional<OutputVector> output;
  if (should && !all.empty()) {
    output = OutputVector{make_id("machine-output"), all.front().vector, {{"arbiter", true}, {"combinedFrom", static_cast<double>(all.size())}}, now_ms()};
  }
  return {should, output, rule, totalSequences, withOutput};
}

Machine::Machine(std::string machineName, std::string machineDescription, ArbiterRule arbiterRule, std::optional<PerceptualMapping> mapping, std::string machineId)
  : id(std::move(machineId)), name(std::move(machineName)), description(std::move(machineDescription)),
    perceptualMapping(std::move(mapping)), arbiter(arbiterRule) {}
void Machine::add_sequence(const CriticalEventSequence& sequence) { sequences[sequence.id] = sequence; }
void Machine::remove_sequence(const std::string& sequenceId) { sequences.erase(sequenceId); }
std::vector<CriticalEventSequence> Machine::all_sequences() const {
  std::vector<CriticalEventSequence> out;
  for (const auto& [_, s] : sequences) out.push_back(s);
  return out;
}
std::vector<std::string> Machine::sequence_ids() const {
  std::vector<std::string> out;
  for (const auto& [id, _] : sequences) out.push_back(id);
  return out;
}
int Machine::sequence_count() const { return static_cast<int>(sequences.size()); }
int Machine::total_vector_count() const {
  int total = 0;
  for (const auto& [_, s] : sequences) total += static_cast<int>(s.all_vectors().size());
  return total;
}
ArbiterRule Machine::arbiter_rule() const { return arbiter.get_rule(); }
MachineTransitionResult Machine::process_input(const Vector& input, std::optional<ComparatorType> overrideType) {
  std::map<std::string, SequenceResult> seqResults;
  std::map<std::string, std::vector<OutputVector>> seqOutputs;
  for (auto& [seqId, seq] : sequences) {
    auto result = seq.transition(input, overrideType);
    seqOutputs[seqId] = result.assertedOutputs;
    seqResults[seqId] = std::move(result);
  }
  auto decision = arbiter.arbitrate(seqOutputs, static_cast<int>(sequences.size()));
  return {input, now_ms(), seqResults, decision.machineOutput, {to_string(decision.rule), decision.totalInputs, decision.sequencesWithOutput, decision.shouldOutput}};
}
void Machine::reset() { for (auto& [_, s] : sequences) s.reset(); }
Json Machine::to_json(bool full) const {
  Json::Array seqIds;
  for (const auto& id : sequence_ids()) seqIds.emplace_back(id);
  Json::Array seqs;
  for (const auto& [_, s] : sequences) {
    seqs.push_back(full ? s.to_json() : Json::Object{{"id", s.id}, {"name", s.name}});
  }
  Json mapping = nullptr;
  if (perceptualMapping) mapping = reality::to_json(*perceptualMapping);
  return Json::Object{
    {"id", id}, {"name", name}, {"description", description}, {"matchAlgorithm", to_string(matchAlgorithm)},
    {"arbiterRule", to_string(arbiter.get_rule())}, {"sequenceCount", static_cast<double>(sequence_count())},
    {"totalVectors", static_cast<double>(total_vector_count())}, {"sequenceIds", seqIds}, {"sequences", seqs},
    {"metadata", metadata}, {"perceptualMapping", mapping}
  };
}

PerceptualSpace::PerceptualSpace(int dimension) : values(static_cast<size_t>(dimension), 0.0) {}
const Vector& PerceptualSpace::vector() const { return values; }
void PerceptualSpace::set_vector(const Vector& v) {
  if (v.size() != values.size()) throw std::invalid_argument("Perceptual space dimension mismatch");
  values = v;
}
void PerceptualSpace::reset() { std::fill(values.begin(), values.end(), 0.0); }
Vector PerceptualSpace::extract_machine_input(const PerceptualMapping& mapping) const {
  if (mapping.input.offset < 0 || mapping.input.length < 0 || mapping.input.offset + mapping.input.length > static_cast<int>(values.size()))
    throw std::out_of_range("Input mapping outside perceptual space");
  return Vector(values.begin() + mapping.input.offset, values.begin() + mapping.input.offset + mapping.input.length);
}
void PerceptualSpace::merge_machine_output(const Vector& output, const PerceptualMapping& mapping) {
  int n = std::min<int>(mapping.output.length, static_cast<int>(output.size()));
  if (mapping.output.offset < 0 || mapping.output.offset + n > static_cast<int>(values.size())) throw std::out_of_range("Output mapping outside perceptual space");
  for (int i = 0; i < n; ++i) values[static_cast<size_t>(mapping.output.offset + i)] = output[static_cast<size_t>(i)];
}
void PerceptualSpace::update_region(int offset, const Vector& regionValues) {
  if (offset < 0 || offset + static_cast<int>(regionValues.size()) > static_cast<int>(values.size())) throw std::out_of_range("Region outside perceptual space");
  std::copy(regionValues.begin(), regionValues.end(), values.begin() + offset);
}

PreceptionEngine::PreceptionEngine(int universalDimension) : dimension(universalDimension), space(universalDimension) {}
Vector PreceptionEngine::resolve_input_event_vector(const Vector& universalInputSpace, const PerceptualMapping& mapping) {
  if (static_cast<int>(universalInputSpace.size()) != dimension) throw std::invalid_argument("Universal input space dimension mismatch");
  space.set_vector(universalInputSpace);
  return space.extract_machine_input(mapping);
}
Vector PreceptionEngine::resolve_input_event_vector_for_machine(const Vector& universalInputSpace, const Machine& machine) {
  if (!machine.perceptualMapping) throw std::invalid_argument("Machine has no perceptual mapping");
  return resolve_input_event_vector(universalInputSpace, *machine.perceptualMapping);
}
std::map<std::string, Vector> PreceptionEngine::resolve_inputs_for_machines(const Vector& universalInputSpace, const std::map<std::string, Machine>& machines) {
  if (static_cast<int>(universalInputSpace.size()) != dimension) throw std::invalid_argument("Universal input space dimension mismatch");
  space.set_vector(universalInputSpace);
  std::map<std::string, Vector> out;
  for (const auto& [id, m] : machines) if (m.perceptualMapping) out[id] = space.extract_machine_input(*m.perceptualMapping);
  return out;
}
void PreceptionEngine::merge_output_into_perceptual_space(const Vector& outputVector, const PerceptualMapping& mapping) { space.merge_machine_output(outputVector, mapping); }
PerceptualSpace& PreceptionEngine::perceptual_space() { return space; }
const PerceptualSpace& PreceptionEngine::perceptual_space() const { return space; }
void PreceptionEngine::reset() { space.reset(); }
Json PreceptionEngine::diagnostic_mapping(const Vector& universalInputSpace, const std::map<std::string, Machine>& machines) {
  Json::Array nonzero;
  for (size_t i = 0; i < universalInputSpace.size(); ++i) if (universalInputSpace[i] != 0.0) nonzero.push_back(Json::Object{{"index", static_cast<double>(i)}, {"value", universalInputSpace[i]}});
  auto resolved = resolve_inputs_for_machines(universalInputSpace, machines);
  Json::Array mappings;
  for (const auto& [id, m] : machines) if (m.perceptualMapping) mappings.push_back(Json::Object{
    {"machineId", id}, {"machineName", m.name}, {"inputMapping", to_json(m.perceptualMapping->input)}, {"resolvedInput", json::numbers(resolved[id])}
  });
  return Json::Object{{"universalSpace", Json::Object{{"dimension", static_cast<double>(dimension)}, {"nonZeroValues", nonzero}}}, {"machineMappings", mappings}};
}

PerceptualSpaceSimulator::PerceptualSpaceSimulator(int dim) : dimension(dim), space(dim) {}
void PerceptualSpaceSimulator::add_machine(const Machine& machine) {
  if (!machine.perceptualMapping) throw std::invalid_argument("Machine has no perceptual mapping");
  machines[machine.id] = machine;
}
bool PerceptualSpaceSimulator::remove_machine(const std::string& machineId) { return machines.erase(machineId) > 0; }
void PerceptualSpaceSimulator::configure(std::vector<Vector> inputSequence, RegionMapping inputRegion, long delay, std::optional<int> maxSteps) {
  configuredInputSequence = std::move(inputSequence);
  configuredInputRegion = inputRegion;
  configuredStepDelayMs = delay;
  configuredMaxSteps = maxSteps;
  configured = true;
  reset();
  configured = true;
}
void PerceptualSpaceSimulator::start() { if (!configured) throw std::runtime_error("Simulation not configured"); running = true; }
void PerceptualSpaceSimulator::stop() { running = false; }
void PerceptualSpaceSimulator::reset() { running = false; space.reset(); steps.clear(); currentStep = 0; for (auto& [_, m] : machines) m.reset(); }
std::optional<SimulationStep> PerceptualSpaceSimulator::step() {
  if (!configured) throw std::runtime_error("Simulation not configured");
  if (currentStep >= static_cast<int>(configuredInputSequence.size())) { stop(); return std::nullopt; }
  if (configuredMaxSteps && currentStep >= *configuredMaxSteps) { stop(); return std::nullopt; }
  space.update_region(configuredInputRegion.offset, configuredInputSequence[static_cast<size_t>(currentStep)]);
  auto result = run_phases(currentStep, std::nullopt);
  ++currentStep;
  steps.insert(steps.begin(), result);
  return result;
}
SimulationStep PerceptualSpaceSimulator::process_immediate(const Vector& vector, std::optional<ComparatorType> overrideType) {
  space.set_vector(vector);
  auto result = run_phases(immediateStepCount++, overrideType);
  steps.insert(steps.begin(), result);
  return result;
}
SimulationStep PerceptualSpaceSimulator::run_phases(int stepNumber, std::optional<ComparatorType> overrideType) {
  struct MachinePhaseJob {
    std::string id;
    Machine* machine = nullptr;
    Vector snapshot;
    PerceptualMapping mapping;
  };
  struct MachinePhaseResult {
    std::string id;
    std::string name;
    Vector snapshot;
    PerceptualMapping mapping;
    MachineTransitionResult transition;
    std::vector<Vector> pendingOutputs;
  };
  struct PendingMerge {
    RegionMapping region;
    std::string machineId;
    size_t outputIndex = 0;
    Vector output;
  };

  std::vector<MachinePhaseJob> jobs;
  jobs.reserve(machines.size());
  for (auto& [id, m] : machines) {
    auto mapping = *m.perceptualMapping;
    jobs.push_back({id, &m, space.extract_machine_input(mapping), mapping});
  }

  std::vector<MachinePhaseResult> results(jobs.size());
  std::vector<std::future<MachinePhaseResult>> futures;
  futures.reserve(jobs.size());
  for (const auto& job : jobs) {
    futures.push_back(domain_workers().submit([job, overrideType]() mutable {
      auto transition = job.machine->process_input(job.snapshot, overrideType);
      std::vector<Vector> pendingOutputs;
      if (transition.arbiterMetadata.shouldOutput) {
        for (const auto& [_, sr] : transition.sequenceResults) {
          for (const auto& out : sr.assertedOutputs) pendingOutputs.push_back(out.vector);
        }
      }
      return MachinePhaseResult{
        job.id,
        job.machine->name,
        std::move(job.snapshot),
        job.mapping,
        std::move(transition),
        std::move(pendingOutputs)
      };
    }));
  }
  for (size_t i = 0; i < futures.size(); ++i) results[i] = futures[i].get();

  SimulationStep step;
  step.stepNumber = stepNumber;
  step.timestamp = now_ms();
  for (auto& result : results) {
    MachineStepResult msr{
      result.id,
      result.name,
      result.snapshot,
      result.transition.machineOutput ? std::optional<Vector>(result.transition.machineOutput->vector) : std::nullopt,
      result.mapping.input,
      std::nullopt,
      result.transition
    };
    if (msr.outputVector) msr.outputRegion = result.mapping.output;
    step.machineResults[result.id] = msr;
  }

  std::vector<PendingMerge> pendingMerges;
  for (const auto& result : results) {
    for (size_t i = 0; i < result.pendingOutputs.size(); ++i) {
      pendingMerges.push_back({result.mapping.output, result.id, i, result.pendingOutputs[i]});
    }
  }
  std::sort(pendingMerges.begin(), pendingMerges.end(), [](const PendingMerge& a, const PendingMerge& b) {
    if (a.region.offset != b.region.offset) return a.region.offset < b.region.offset;
    if (a.region.length != b.region.length) return a.region.length < b.region.length;
    if (a.machineId != b.machineId) return a.machineId < b.machineId;
    return a.outputIndex < b.outputIndex;
  });
  for (const auto& merge : pendingMerges) {
    space.merge_machine_output(merge.output, PerceptualMapping{{0, 0}, merge.region});
  }
  step.perceptualSpace = space.vector();
  for (const auto& [id, msr] : step.machineResults) {
    step.activeRegions.push_back({msr.inputRegion.offset, msr.inputRegion.length, id, "input"});
    if (msr.outputRegion) step.activeRegions.push_back({msr.outputRegion->offset, msr.outputRegion->length, id, "output"});
  }
  return step;
}
Json PerceptualSpaceSimulator::machine_graph_data() const {
  Json::Array nodes;
  Json::Array edges;
  for (const auto& [id, m] : machines) {
    auto mapping = *m.perceptualMapping;
    nodes.push_back(Json::Object{{"id", id}, {"name", m.name}, {"description", m.description}, {"inputMapping", to_json(mapping.input)}, {"outputMapping", to_json(mapping.output)}, {"metadata", m.metadata}});
  }
  for (const auto& [sid, sm] : machines) for (const auto& [tid, tm] : machines) if (sid != tid) {
    auto so = sm.perceptualMapping->output;
    auto ti = tm.perceptualMapping->input;
    int send = so.offset + so.length, tend = ti.offset + ti.length;
    if (!(send <= ti.offset || so.offset >= tend)) edges.push_back(Json::Object{{"source", sid}, {"target", tid}, {"sourceRegion", to_json(so)}, {"targetRegion", to_json(ti)}, {"overlap", true}});
  }
  return Json::Object{{"nodes", nodes}, {"edges", edges}, {"perceptualSpaceDimension", static_cast<double>(dimension)}};
}
Json PerceptualSpaceSimulator::state_json() const {
  Json::Array machineJson;
  for (const auto& [_, m] : machines) machineJson.push_back(m.to_json());
  return Json::Object{{"state", Json::Object{{"perceptualSpace", json::numbers(space.vector())}, {"currentStep", static_cast<double>(currentStep)}, {"isRunning", running}, {"machines", machineJson}}}};
}
std::vector<SimulationStep> PerceptualSpaceSimulator::history() const { return steps; }
PerceptualSpace& PerceptualSpaceSimulator::perceptual_space() { return space; }
int PerceptualSpaceSimulator::current_step() const { return currentStep; }
bool PerceptualSpaceSimulator::is_running() const { return running; }
long PerceptualSpaceSimulator::step_delay_ms() const { return configuredStepDelayMs; }

PerceptionEngine::PerceptionEngine(int vectorDimension)
  : dimension(vectorDimension), persistentVector(static_cast<size_t>(vectorDimension), 0.0) {}

SourceConfig PerceptionEngine::add_source(SourceConfig source) {
  if (source.id.empty()) source.id = make_id("source");
  sources[source.id] = source;
  testStep[source.id] = 0;
  if (source.pattern == SimPattern::RandomWalk) walkState[source.id] = Vector(static_cast<size_t>(source.region.length), source.dcOffset);
  return source;
}
bool PerceptionEngine::remove_source(const std::string& id) { testStep.erase(id); walkState.erase(id); return sources.erase(id) > 0; }
std::optional<SourceConfig> PerceptionEngine::get_source(const std::string& id) const {
  auto it = sources.find(id);
  if (it == sources.end()) return std::nullopt;
  return it->second;
}
std::vector<SourceConfig> PerceptionEngine::get_sources() const {
  std::vector<SourceConfig> out;
  for (const auto& [_, s] : sources) out.push_back(s);
  return out;
}
bool PerceptionEngine::update_sensor_value(const std::string& sensorId, const Vector& values) {
  for (auto& [_, s] : sources) if (s.kind == "sensor" && s.sensorId == sensorId) {
    s.lastValue = values;
    s.lastUpdated = now_ms();
    return true;
  }
  return false;
}
Vector PerceptionEngine::assemble_vector() const {
  Vector out = persistentVector;
  for (const auto& [_, s] : sources) if (s.active) {
    auto vals = source_values(s);
    for (int i = 0; i < s.region.length && i < static_cast<int>(vals.size()) && s.region.offset + i < static_cast<int>(out.size()); ++i)
      out[static_cast<size_t>(s.region.offset + i)] = std::clamp(vals[static_cast<size_t>(i)], 0.0, 1.0);
  }
  return out;
}
void PerceptionEngine::update_from_perceptual_space(const Vector& values) {
  persistentVector.assign(static_cast<size_t>(dimension), 0.0);
  for (size_t i = 0; i < persistentVector.size() && i < values.size(); ++i) persistentVector[i] = values[i];
}
void PerceptionEngine::advance() {
  ++globalStep;
  for (auto& [id, s] : sources) {
    if (!s.active) continue;
    if (s.kind == "test") {
      int next = testStep[id] + 1;
      if (next >= static_cast<int>(s.inputs.size())) {
        if (s.loop) testStep[id] = 0;
        else { s.active = false; testStep[id] = 0; }
      } else testStep[id] = next;
    } else if (s.pattern == SimPattern::RandomWalk) {
      auto& walk = walkState[id];
      for (double& v : walk) v = std::clamp(v + ((std::rand() / static_cast<double>(RAND_MAX)) * 2.0 - 1.0) * 0.05, 0.0, 1.0);
    }
  }
}
void PerceptionEngine::reset() {
  globalStep = 0;
  persistentVector.assign(static_cast<size_t>(dimension), 0.0);
  for (auto& [id, s] : sources) {
    if (s.kind == "test") { testStep[id] = 0; s.active = true; }
    if (s.pattern == SimPattern::RandomWalk) walkState[id] = Vector(static_cast<size_t>(s.region.length), s.dcOffset);
  }
}
Vector PerceptionEngine::source_values(const SourceConfig& s) const {
  if (s.kind == "test") {
    auto it = testStep.find(s.id);
    int step = it == testStep.end() ? 0 : it->second;
    return step < static_cast<int>(s.inputs.size()) ? s.inputs[static_cast<size_t>(step)] : Vector(static_cast<size_t>(s.region.length), 0.0);
  }
  if (s.kind == "sensor") {
    if (!s.lastUpdated || now_ms() - *s.lastUpdated > s.ttlMs) return Vector(static_cast<size_t>(s.region.length), 0.0);
    Vector out(static_cast<size_t>(s.region.length), 0.0);
    for (size_t i = 0; i < out.size() && i < s.lastValue.size(); ++i) out[i] = s.lastValue[i];
    return out;
  }
  Vector out;
  for (int i = 0; i < s.region.length; ++i) {
    double t = static_cast<double>(globalStep) + i * 0.1;
    double period = s.frequency > 0 ? 1.0 / s.frequency : 1.0;
    double phase = std::fmod(t / period, 1.0);
    double v = s.dcOffset;
    switch (s.pattern) {
      case SimPattern::Sine: v += s.amplitude * std::sin(2 * M_PI * phase); break;
      case SimPattern::Sawtooth: v += s.amplitude * (2 * phase - 1); break;
      case SimPattern::Square: v += s.amplitude * (phase < 0.5 ? 1.0 : -1.0); break;
      case SimPattern::LinearRamp: v += s.amplitude * phase; break;
      case SimPattern::RandomWalk: {
        auto it = walkState.find(s.id);
        v = it != walkState.end() && !it->second.empty() ? it->second.front() : s.dcOffset;
        break;
      }
      case SimPattern::GaussianNoise: v += s.amplitude * (((std::rand() / static_cast<double>(RAND_MAX)) * 2.0) - 1.0); break;
      case SimPattern::Binary: v = phase < 0.5 ? 1.0 : 0.0; break;
      case SimPattern::Constant: v = s.dcOffset; break;
    }
    out.push_back(v);
  }
  return out;
}
Json PerceptionEngine::state_json(std::optional<long long> lastPush, bool autoRunning, long autoIntervalMs) const {
  Json::Array srcs;
  for (const auto& [_, s] : sources) srcs.push_back(to_json(s));
  return Json::Object{{"sources", srcs}, {"assembledVector", json::numbers(assemble_vector())}, {"globalStep", static_cast<double>(globalStep)}, {"auto", Json::Object{{"running", autoRunning}, {"intervalMs", static_cast<double>(autoIntervalMs)}}}, {"lastPush", lastPush ? Json(static_cast<double>(*lastPush)) : Json(nullptr)}, {"matchAlgorithm", to_string(matchAlgorithm)}, {"vectorSize", static_cast<double>(dimension)}};
}

static RegionMapping parse_region(const Json& j) {
  return {static_cast<int>(j.at("offset").as_number()), static_cast<int>(j.at("length").as_number())};
}

Machine load_machine_from_json_string(const std::string& raw, std::optional<std::string> overrideId) {
  Json root = json::parse(raw);
  const Json& m = root.at("machine");
  std::string id = overrideId.value_or(make_id("machine"));
  std::string name = m.at("name").as_string("unnamed");
  std::string desc = m.at("description").as_string();
  ArbiterRule arbiter = ArbiterRule::Passthrough;
  if (!m.at("arbiterRule").is_null()) arbiter = arbiter_from_string(m.at("arbiterRule").as_string("PASSTHROUGH"));
  std::optional<PerceptualMapping> mapping;
  const auto& pm = m.at("perceptualMapping");
  if (pm.is_object()) mapping = PerceptualMapping{parse_region(pm.at("input")), parse_region(pm.at("output"))};
  Machine machine(name, desc, arbiter, mapping, id);
  if (m.at("matchAlgorithm").is_string()) machine.matchAlgorithm = comparator_from_string(m.at("matchAlgorithm").as_string());
  if (m.at("metadata").is_object()) machine.metadata = m.at("metadata").object();
  if (m.at("inputSequences").is_array()) machine.metadata["inputSequences"] = m.at("inputSequences");
  for (const auto& sj : m.at("sequences").is_array() ? m.at("sequences").array() : Json::Array{}) {
    CriticalEventSequence seq(sj.at("name").as_string("unnamed"), sj.at("id").as_string(make_id("sequence")));
    if (sj.at("metadata").is_object()) seq.metadata = sj.at("metadata").object();
    for (const auto& vj : sj.at("vectors").is_array() ? sj.at("vectors").array() : Json::Array{}) {
      std::vector<VectorElement> elems;
      for (const auto& ej : vj.at("elements").is_array() ? vj.at("elements").array() : Json::Array{}) {
        VectorElement e;
        e.value = ej.at("value").as_number();
        if (ej.at("comparatorType").is_string()) e.comparatorType = comparator_from_string(ej.at("comparatorType").as_string());
        if (ej.at("threshold").is_number()) e.threshold = ej.at("threshold").as_number();
        elems.push_back(e);
      }
      RealityVector rv(elems, vj.at("isInitial").as_bool(), vj.at("id").as_string(make_id("vector")));
      rv.matchAlgorithm = machine.matchAlgorithm;
      if (vj.at("metadata").is_object()) rv.metadata = vj.at("metadata").object();
      for (const auto& nid : vj.at("nextVectorIds").is_array() ? vj.at("nextVectorIds").array() : Json::Array{}) rv.add_next_vector(nid.as_string());
      for (const auto& oj : vj.at("outputVectors").is_array() ? vj.at("outputVectors").array() : Json::Array{}) {
        std::map<std::string, Json> meta;
        if (oj.at("metadata").is_object()) meta = oj.at("metadata").object();
        rv.add_output_vector({oj.at("id").as_string(make_id("output")), json::to_numbers(oj.at("vector")), meta, now_ms()});
      }
      seq.add_vector(rv);
    }
    machine.add_sequence(seq);
  }
  return machine;
}

std::vector<Machine> load_machines_from_directory(const std::string& directory) {
  std::vector<Machine> out;
  namespace fs = std::filesystem;
  if (!fs::exists(directory)) return out;
  std::vector<fs::path> files;
  for (const auto& p : fs::directory_iterator(directory)) if (p.path().extension() == ".json") files.push_back(p.path());
  std::sort(files.begin(), files.end());
  for (const auto& file : files) {
    std::ifstream in(file);
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string stem = file.stem().string();
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return std::isalnum(c) ? static_cast<char>(std::tolower(c)) : '-'; });
    try { out.push_back(load_machine_from_json_string(raw, "machine-" + stem)); }
    catch (const std::exception& e) { std::cerr << "Failed to load " << file << ": " << e.what() << "\n"; }
  }
  return out;
}

Json to_json(const RegionMapping& r) { return Json::Object{{"offset", static_cast<double>(r.offset)}, {"length", static_cast<double>(r.length)}}; }
Json to_json(const PerceptualMapping& m) { return Json::Object{{"input", to_json(m.input)}, {"output", to_json(m.output)}}; }
Json to_json(const OutputVector& o) { return Json::Object{{"id", o.id}, {"vector", json::numbers(o.vector)}, {"metadata", o.metadata}, {"timestamp", static_cast<double>(o.timestamp)}}; }
Json to_json(const SequenceResult& r) {
  Json::Array matched, activated, outputs;
  for (const auto& id : r.matchedVectors) matched.emplace_back(id);
  for (const auto& id : r.activatedVectors) activated.emplace_back(id);
  for (const auto& o : r.assertedOutputs) outputs.push_back(to_json(o));
  return Json::Object{{"matchedVectors", matched}, {"activatedVectors", activated}, {"assertedOutputs", outputs}};
}
Json to_json(const MachineTransitionResult& r) {
  Json::Object seqs;
  for (const auto& [id, sr] : r.sequenceResults) seqs[id] = to_json(sr);
  Json output = r.machineOutput ? to_json(*r.machineOutput) : Json(nullptr);
  return Json::Object{{"inputVector", json::numbers(r.inputVector)}, {"timestamp", static_cast<double>(r.timestamp)}, {"sequenceResults", seqs}, {"machineOutput", output}, {"arbiterMetadata", Json::Object{{"rule", r.arbiterMetadata.rule}, {"totalInputs", static_cast<double>(r.arbiterMetadata.totalInputs)}, {"sequencesWithOutput", static_cast<double>(r.arbiterMetadata.sequencesWithOutput)}, {"shouldOutput", r.arbiterMetadata.shouldOutput}}}};
}
Json to_json(const SimulationStep& step, bool includeMachineResults) {
  Json::Object machineResults;
  if (includeMachineResults) {
    for (const auto& [id, mr] : step.machineResults) machineResults[id] = Json::Object{{"machineId", mr.machineId}, {"machineName", mr.machineName}, {"inputVector", json::numbers(mr.inputVector)}, {"outputVector", mr.outputVector ? Json(json::numbers(*mr.outputVector)) : Json(nullptr)}, {"inputRegion", to_json(mr.inputRegion)}, {"outputRegion", mr.outputRegion ? to_json(*mr.outputRegion) : Json(nullptr)}, {"transitionResult", to_json(mr.transitionResult)}};
  }
  Json::Array regions;
  for (const auto& r : step.activeRegions) regions.push_back(Json::Object{{"offset", static_cast<double>(r.offset)}, {"length", static_cast<double>(r.length)}, {"machineId", r.machineId}, {"type", r.type}});
  Json::Object out{{"stepNumber", static_cast<double>(step.stepNumber)}, {"timestamp", static_cast<double>(step.timestamp)}, {"perceptualSpace", json::numbers(step.perceptualSpace)}, {"activeRegions", regions}};
  if (includeMachineResults) out["machineResults"] = machineResults;
  return out;
}
Json to_json(const SimulationStep& step) {
  return to_json(step, true);
}
Json to_json(const SourceConfig& s) {
  Json::Object o{{"id", s.id}, {"name", s.name}, {"region", to_json(s.region)}, {"active", s.active}, {"type", s.kind}};
  if (s.kind == "test") {
    Json::Array inputs;
    for (const auto& v : s.inputs) inputs.push_back(json::numbers(v));
    o["machineId"] = s.machineId; o["machineName"] = s.machineName; o["sequenceName"] = s.sequenceName; o["inputs"] = inputs; o["loop"] = s.loop;
  } else if (s.kind == "sensor") {
    o["sensorId"] = s.sensorId; o["lastValue"] = json::numbers(s.lastValue); o["lastUpdated"] = s.lastUpdated ? Json(static_cast<double>(*s.lastUpdated)) : Json(nullptr); o["ttlMs"] = static_cast<double>(s.ttlMs);
  } else {
    o["pattern"] = to_string(s.pattern); o["frequency"] = s.frequency; o["amplitude"] = s.amplitude; o["dcOffset"] = s.dcOffset;
  }
  return o;
}

} // namespace reality
