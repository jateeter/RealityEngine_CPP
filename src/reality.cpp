#include "reality/reality.hpp"
#include "reality/sta_checker.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace reality {

namespace {
constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

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
    const char* workersEnv = std::getenv("DOMAIN_WORKERS");
    const char* capacityEnv = std::getenv("DOMAIN_QUEUE_CAPACITY");
    size_t n = workersEnv ? static_cast<size_t>(std::max(1, std::atoi(workersEnv))) : static_cast<size_t>(std::max(2u, std::thread::hardware_concurrency()));
    queueCapacity = capacityEnv ? static_cast<size_t>(std::max(1, std::atoi(capacityEnv))) : n * 256;
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
  auto submit(Fn&& fn) -> std::optional<std::future<decltype(fn())>> {
    using R = decltype(fn());
    auto task = std::make_shared<std::packaged_task<R()>>(std::forward<Fn>(fn));
    auto future = task->get_future();
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (jobs.size() >= queueCapacity) {
        rejected.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
      }
      jobs.push_back([task]() { (*task)(); });
    }
    cv.notify_one();
    return future;
  }

  bool try_reserve(size_t count) {
    std::lock_guard<std::mutex> lock(mutex);
    if (jobs.size() + reserved + count > queueCapacity) {
      rejected.fetch_add(count, std::memory_order_relaxed);
      return false;
    }
    reserved += count;
    return true;
  }

  void release_reservation(size_t count) {
    std::lock_guard<std::mutex> lock(mutex);
    reserved = count > reserved ? 0 : reserved - count;
  }

  template <typename Fn>
  auto submit_reserved(Fn&& fn) -> std::future<decltype(fn())> {
    using R = decltype(fn());
    auto task = std::make_shared<std::packaged_task<R()>>(std::forward<Fn>(fn));
    auto future = task->get_future();
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (reserved == 0) throw std::runtime_error("Domain worker reservation exhausted");
      --reserved;
      jobs.push_back([task]() { (*task)(); });
    }
    cv.notify_one();
    return future;
  }

  WorkerPoolMetrics metrics() const {
    std::lock_guard<std::mutex> lock(mutex);
    return {
      workers.size(),
      jobs.size() + reserved,
      active.load(std::memory_order_relaxed),
      completed.load(std::memory_order_relaxed),
      rejected.load(std::memory_order_relaxed),
      queueCapacity
    };
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
      active.fetch_add(1, std::memory_order_relaxed);
      job();
      active.fetch_sub(1, std::memory_order_relaxed);
      completed.fetch_add(1, std::memory_order_relaxed);
    }
  }

  mutable std::mutex mutex;
  std::condition_variable cv;
  std::deque<std::function<void()>> jobs;
  std::vector<std::thread> workers;
  size_t queueCapacity = 0;
  size_t reserved = 0;
  std::atomic_size_t active{0};
  std::atomic_size_t completed{0};
  std::atomic_size_t rejected{0};
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
void RealityVector::set_active(const std::vector<std::string>& predecessor) {
  state = VectorState::Active;
  predecessorChain = predecessor;
}
void RealityVector::clear_active() {
  if (!isInitial) {
    state = VectorState::Inactive;
    predecessorChain.clear();
  }
}
std::vector<std::string> RealityVector::provenance_chain() const {
  std::vector<std::string> chain = predecessorChain;
  chain.push_back(id);
  return chain;
}
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
    return {false, {}, {}, mr, {}};
  }
  // Stamp every emitted output with the full evidence chain that led here
  // (predecessor chain + this.id).  Listeners read this off the
  // mergeBatch payload as the audit trail behind the assertion.
  std::vector<std::string> chain = provenance_chain();
  std::vector<OutputVector> stamped = outputVectors;
  for (auto& o : stamped) o.provenance = chain;

  bool isFinal = !outputVectors.empty();
  bool isTransitional = !isInitial && !isFinal;
  if (isTransitional && !nextVectorIds.empty()) clear_active();
  return {true, nextVectorIds, std::move(stamped), mr, std::move(chain)};
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

long CriticalEventSequence::days_since_deprecation() const {
  if (deprecatedAt.empty()) return 0;
  // Accept "YYYY-MM-DD" and "YYYY-MM-DDTHH:MM:SSZ".  Anything more exotic
  // returns 0 — same conservative fallback as the AI runtime.
  std::tm tm{};
  std::istringstream ss(deprecatedAt);
  ss >> std::get_time(&tm, "%Y-%m-%d");
  if (ss.fail()) return 0;
  std::time_t t = std::mktime(&tm);
  if (t == -1) return 0;
  auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  return static_cast<long>((now - t) / 86400);
}

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
  // Pair each pending successor id with the chain its activator carried —
  // mirrors the pendingActivations map in CriticalEventSequence.ts so the
  // newly-activated successor inherits the full evidence trail.
  std::map<std::string, std::vector<std::string>> pending;
  auto active = active_vectors();
  for (auto* v : active) {
    auto tr = v->transition(input, overrideType);
    if (!tr.matched) continue;
    result.matchedVectors.push_back(v->id);
    if (!v->output_vectors().empty()) v->set_was_just_matched();
    for (const auto& nid : tr.nextVectorIds) {
      if (!pending.count(nid)) pending[nid] = tr.provenanceChain;
    }
    result.assertedOutputs.insert(result.assertedOutputs.end(), tr.outputVectors.begin(), tr.outputVectors.end());
  }
  for (const auto& [id, chain] : pending) {
    auto it = vectors.find(id);
    if (it != vectors.end() && !it->second.is_active()) {
      it->second.set_active(chain);
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
  Json::Object out{{"id", id}, {"name", name}, {"vectors", arr}, {"initialVectorIds", initials}, {"outputVectorIds", outputs}, {"metadata", metadata}};
  if (!schemaVersion.empty()) out["schemaVersion"] = schemaVersion;
  if (!deprecatedAt.empty())  out["deprecatedAt"]  = deprecatedAt;
  if (!replacedBy.empty())    out["replacedBy"]    = replacedBy;
  return out;
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
    output = OutputVector{make_id("machine-output"), all.front().vector, {{"arbiter", true}, {"combinedFrom", static_cast<double>(all.size())}}, now_ms(), all.front().provenance};
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

PerceptualSpace::PerceptualSpace(int dim) : values(static_cast<size_t>(dim < 0 ? 0 : dim), 0.0) {}
int PerceptualSpace::dimension() const { return static_cast<int>(values.size()); }
const Vector& PerceptualSpace::vector() const { return values; }
void PerceptualSpace::set_vector(const Vector& v) {
  if (v.size() > values.size()) grow_to(static_cast<int>(v.size()));
  for (size_t i = 0; i < v.size(); ++i) values[i] = v[i];
  for (size_t i = v.size(); i < values.size(); ++i) values[i] = 0.0;
}
void PerceptualSpace::grow_to(int newDimension) {
  if (newDimension <= static_cast<int>(values.size())) return;
  values.resize(static_cast<size_t>(newDimension), 0.0);
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

PerceptionMapper::PerceptionMapper(int universalDimension) : dimension(universalDimension), space(universalDimension) {}
Vector PerceptionMapper::resolve_input_event_vector(const Vector& universalInputSpace, const PerceptualMapping& mapping) {
  space.set_vector(universalInputSpace);
  if (space.dimension() > dimension) dimension = space.dimension();
  return space.extract_machine_input(mapping);
}
Vector PerceptionMapper::resolve_input_event_vector_for_machine(const Vector& universalInputSpace, const Machine& machine) {
  if (!machine.perceptualMapping) throw std::invalid_argument("Machine has no perceptual mapping");
  return resolve_input_event_vector(universalInputSpace, *machine.perceptualMapping);
}
std::map<std::string, Vector> PerceptionMapper::resolve_inputs_for_machines(const Vector& universalInputSpace, const std::map<std::string, Machine>& machines) {
  space.set_vector(universalInputSpace);
  if (space.dimension() > dimension) dimension = space.dimension();
  std::map<std::string, Vector> out;
  for (const auto& [id, m] : machines) if (m.perceptualMapping) out[id] = space.extract_machine_input(*m.perceptualMapping);
  return out;
}
void PerceptionMapper::merge_output_into_perceptual_space(const Vector& outputVector, const PerceptualMapping& mapping) { space.merge_machine_output(outputVector, mapping); }
PerceptualSpace& PerceptionMapper::perceptual_space() { return space; }
const PerceptualSpace& PerceptionMapper::perceptual_space() const { return space; }
void PerceptionMapper::reset() { space.reset(); }
Json PerceptionMapper::diagnostic_mapping(const Vector& universalInputSpace, const std::map<std::string, Machine>& machines) {
  Json::Array nonzero;
  for (size_t i = 0; i < universalInputSpace.size(); ++i) if (universalInputSpace[i] != 0.0) nonzero.push_back(Json::Object{{"index", static_cast<double>(i)}, {"value", universalInputSpace[i]}});
  auto resolved = resolve_inputs_for_machines(universalInputSpace, machines);
  Json::Array mappings;
  for (const auto& [id, m] : machines) if (m.perceptualMapping) mappings.push_back(Json::Object{
    {"machineId", id}, {"machineName", m.name}, {"inputMapping", to_json(m.perceptualMapping->input)}, {"resolvedInput", json::numbers(resolved[id])}
  });
  return Json::Object{{"universalSpace", Json::Object{{"dimension", static_cast<double>(dimension)}, {"nonZeroValues", nonzero}}}, {"machineMappings", mappings}};
}

PerceptualSpaceSimulator::PerceptualSpaceSimulator(int dim) : initialDimension(dim < 0 ? 0 : dim), space(dim < 0 ? 0 : dim) {}
int PerceptualSpaceSimulator::dimension() const { return space.dimension(); }
int PerceptualSpaceSimulator::required_dimension() const {
  int req = 0;
  for (const auto& [_, m] : machines) {
    if (!m.perceptualMapping) continue;
    req = std::max(req, m.perceptualMapping->input.offset + m.perceptualMapping->input.length);
    req = std::max(req, m.perceptualMapping->output.offset + m.perceptualMapping->output.length);
  }
  return req;
}
long PerceptualSpaceSimulator::mapping_version() const { return mappingVersion; }
CesCoverageRegistry& PerceptualSpaceSimulator::ces_coverage() { return coverage; }
const CesCoverageRegistry& PerceptualSpaceSimulator::ces_coverage() const { return coverage; }
void PerceptualSpaceSimulator::add_machine(const Machine& machine) {
  if (!machine.perceptualMapping) throw std::invalid_argument("Machine has no perceptual mapping");
  const auto& mapping = *machine.perceptualMapping;
  int needed = std::max(mapping.input.offset + mapping.input.length,
                        mapping.output.offset + mapping.output.length);
  if (needed > space.dimension()) {
    space.grow_to(needed);
  }
  machines[machine.id] = machine;
  ++mappingVersion;
  edgesDirty = true;

  // Compose / meta-CES subscriptions — declared in
  // metadata.compose.subscriptions = [{ producerMachineId,
  // producerSequenceId, bitOffset }].  Mirrors the AI side; lets a meta
  // machine run as a normal CES over an event-bus region of PE.
  auto composeIt = machine.metadata.find("compose");
  if (composeIt != machine.metadata.end() && composeIt->second.is_object()) {
    const auto& compose = composeIt->second;
    auto subsField = compose.at("subscriptions");
    if (subsField.is_array()) {
      for (const auto& sub : subsField.array()) {
        if (!sub.is_object()) continue;
        std::string pm = sub.at("producerMachineId").as_string();
        std::string ps = sub.at("producerSequenceId").as_string();
        int bit = static_cast<int>(sub.at("bitOffset").as_number(-1));
        if (pm.empty() || ps.empty() || bit < 0) continue;
        eventBusSubscriptions[pm + "|" + ps].push_back({machine.id, bit, pm, ps});
        if (bit + 1 > space.dimension()) space.grow_to(bit + 1);
      }
    }
  }
}
bool PerceptualSpaceSimulator::remove_machine(const std::string& machineId) {
  bool removed = machines.erase(machineId) > 0;
  if (removed) { ++mappingVersion; edgesDirty = true; }
  // Drop subscriptions this machine had registered.
  for (auto it = eventBusSubscriptions.begin(); it != eventBusSubscriptions.end(); ) {
    auto& list = it->second;
    list.erase(std::remove_if(list.begin(), list.end(),
      [&](const ComposeSubscription& s) { return s.subscriberMachineId == machineId; }), list.end());
    if (list.empty()) it = eventBusSubscriptions.erase(it); else ++it;
  }
  return removed;
}
size_t PerceptualSpaceSimulator::event_bus_subscription_count() const {
  size_t n = 0;
  for (const auto& [_, list] : eventBusSubscriptions) n += list.size();
  return n;
}
std::vector<EventBusWrite> PerceptualSpaceSimulator::apply_event_bus(const std::vector<MergeOperation>& mergeBatch) {
  if (eventBusSubscriptions.empty()) return {};
  std::vector<EventBusWrite> writes;
  std::set<std::string> seen;
  for (const auto& op : mergeBatch) {
    auto it = eventBusSubscriptions.find(op.machineId + "|" + op.sequenceId);
    if (it == eventBusSubscriptions.end()) continue;
    for (const auto& sub : it->second) {
      std::string dedup = sub.subscriberMachineId + "|" + std::to_string(sub.bitOffset)
                        + "|" + op.machineId + "|" + op.sequenceId;
      if (!seen.insert(dedup).second) continue;
      writes.push_back({op.machineId, op.sequenceId, sub.subscriberMachineId, sub.bitOffset, 1.0, op.provenance});
      latchedEventBits.insert(sub.bitOffset);
    }
  }
  std::sort(writes.begin(), writes.end(), [](const EventBusWrite& a, const EventBusWrite& b) {
    if (a.subscriberMachineId != b.subscriberMachineId) return a.subscriberMachineId < b.subscriberMachineId;
    if (a.bitOffset != b.bitOffset) return a.bitOffset < b.bitOffset;
    if (a.producerMachineId != b.producerMachineId) return a.producerMachineId < b.producerMachineId;
    return a.producerSequenceId < b.producerSequenceId;
  });
  for (const auto& w : writes) {
    space.update_region(w.bitOffset, Vector{w.value});
  }
  return writes;
}
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
void PerceptualSpaceSimulator::reset() {
  running = false;
  space.reset();
  steps.clear();
  currentStep = 0;
  latchedEventBits.clear();
  for (auto& [_, m] : machines) m.reset();
}
std::optional<SimulationStep> PerceptualSpaceSimulator::step() {
  if (!configured) throw std::runtime_error("Simulation not configured");
  if (currentStep >= static_cast<int>(configuredInputSequence.size())) { stop(); return std::nullopt; }
  if (configuredMaxSteps && currentStep >= *configuredMaxSteps) { stop(); return std::nullopt; }
  space.update_region(configuredInputRegion.offset, configuredInputSequence[static_cast<size_t>(currentStep)]);
  auto result = run_phases(currentStep, std::nullopt);
  ++currentStep;
  steps.insert(steps.begin(), result);
  if (steps.size() > maxHistory) steps.resize(maxHistory);
  return result;
}
SimulationStep PerceptualSpaceSimulator::process_immediate(const Vector& vector, std::optional<ComparatorType> overrideType) {
  space.set_vector(vector);
  // Re-apply previously-latched event-bus bits so the caller's input
  // vector (which zero-fills past its length) doesn't clobber persistent
  // workflow milestones — same semantic as the AI runtime.
  for (int bit : latchedEventBits) {
    if (bit < space.dimension()) space.update_region(bit, Vector{1.0});
  }
  auto result = run_phases(immediateStepCount++, overrideType);
  steps.insert(steps.begin(), result);
  if (steps.size() > maxHistory) steps.resize(maxHistory);
  return result;
}
SimulationStep PerceptualSpaceSimulator::run_phases(int stepNumber, std::optional<ComparatorType> overrideType) {
  struct MachinePhaseJob {
    std::string id;
    Machine* machine = nullptr;
    Vector snapshot;
    PerceptualMapping mapping;
  };
  struct PendingOutput {
    std::string sequenceId;
    size_t outputIndex = 0;
    Vector values;
    std::vector<std::string> provenance;
  };
  struct MachinePhaseResult {
    std::string id;
    std::string name;
    Vector snapshot;
    PerceptualMapping mapping;
    MachineTransitionResult transition;
    std::vector<PendingOutput> pendingOutputs;
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
  auto& pool = domain_workers();
  if (!pool.try_reserve(jobs.size())) throw std::runtime_error("Domain worker queue is full");
  size_t unsubmitted = jobs.size();
  try {
    for (const auto& job : jobs) {
      auto future = pool.submit_reserved([job, overrideType]() mutable {
      auto transition = job.machine->process_input(job.snapshot, overrideType);
      std::vector<PendingOutput> pendingOutputs;
      if (transition.arbiterMetadata.shouldOutput) {
        for (const auto& [sequenceId, sr] : transition.sequenceResults) {
          for (size_t i = 0; i < sr.assertedOutputs.size(); ++i) {
            pendingOutputs.push_back({sequenceId, i, sr.assertedOutputs[i].vector, sr.assertedOutputs[i].provenance});
          }
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
      });
      --unsubmitted;
      futures.push_back(std::move(future));
    }
  } catch (...) {
    if (unsubmitted > 0) pool.release_reservation(unsubmitted);
    throw;
  }
  for (size_t i = 0; i < futures.size(); ++i) results[i] = futures[i].get();

  // Record CES coverage from every machine's transition.  Done after the
  // parallel phase joins so map writes are serialised by the main thread.
  for (const auto& result : results) {
    auto it = machines.find(result.id);
    if (it != machines.end()) coverage.record(it->second, result.transition);
  }

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

  // Canonical merge ordering — sort by (machineId, sequenceId, outputIndex)
  // so AI and C++ produce identical mergeBatch sequences for the same input.
  for (const auto& result : results) {
    auto machineIt = machines.find(result.id);
    for (const auto& po : result.pendingOutputs) {
      MergeOperation op{result.mapping.output, result.id, po.sequenceId, po.outputIndex, po.values, po.provenance, std::nullopt, std::nullopt};
      // Stamp the governance contract resolved from the machine's
      // triggerConfig + governance metadata.  Listeners read it from the
      // same record as the asserted values; the CES JSON is the sole
      // source of truth for paging.
      if (machineIt != machines.end()) {
        op.governance = resolve_governance(machineIt->second, po.sequenceId, po.values);
        if (op.governance) {
          coverage.record_paging_decision(
              op.governance->ownerTeam.empty()     ? "unrouted" : op.governance->ownerTeam,
              op.governance->processStatus.empty() ? "unknown"  : op.governance->processStatus,
              op.governance->ragStatusCode.empty() ? "unknown"  : op.governance->ragStatusCode,
              op.machineId);
        }
        // Deprecation stamp — find the firing sequence in the machine and
        // attach its lifecycle block when deprecated.  Mirrors the AI runtime.
        for (const auto& seq : machineIt->second.all_sequences()) {
          if (seq.id == po.sequenceId && seq.is_deprecated()) {
            DeprecationMark mark;
            mark.since      = seq.deprecatedAt;
            mark.replacedBy = seq.replacedBy;
            mark.ageDays    = seq.days_since_deprecation();
            op.deprecation  = std::move(mark);
            coverage.record_deprecated_fire(op.machineId, machineIt->second.name, po.sequenceId, seq.replacedBy);
            break;
          }
        }
      }
      step.mergeBatch.push_back(std::move(op));
    }
  }
  std::sort(step.mergeBatch.begin(), step.mergeBatch.end(), [](const MergeOperation& a, const MergeOperation& b) {
    if (a.machineId != b.machineId) return a.machineId < b.machineId;
    if (a.sequenceId != b.sequenceId) return a.sequenceId < b.sequenceId;
    return a.outputIndex < b.outputIndex;
  });
  for (const auto& merge : step.mergeBatch) {
    space.merge_machine_output(merge.values, PerceptualMapping{{0, 0}, merge.region});
  }
  // Phase 4 — apply compose/meta-CES event-bus subscriptions, latching
  // 1.0 bits at the offsets every subscriber asked for.  These writes
  // make producer "fired" signals visible to meta-machines on the next step.
  step.eventBus = apply_event_bus(step.mergeBatch);
  step.perceptualSpace = space.vector();
  for (const auto& [id, msr] : step.machineResults) {
    step.activeRegions.push_back({msr.inputRegion.offset, msr.inputRegion.length, id, "input"});
    if (msr.outputRegion) step.activeRegions.push_back({msr.outputRegion->offset, msr.outputRegion->length, id, "output"});
  }
  return step;
}
void PerceptualSpaceSimulator::rebuild_edge_cache() const {
  cachedEdges.clear();
  for (const auto& [sid, sm] : machines) {
    if (!sm.perceptualMapping) continue;
    const auto& so = sm.perceptualMapping->output;
    int send = so.offset + so.length;
    for (const auto& [tid, tm] : machines) {
      if (sid == tid || !tm.perceptualMapping) continue;
      const auto& ti = tm.perceptualMapping->input;
      int tend = ti.offset + ti.length;
      if (!(send <= ti.offset || so.offset >= tend))
        cachedEdges.push_back(Json::Object{{"source", sid}, {"target", tid},
            {"sourceRegion", to_json(so)}, {"targetRegion", to_json(ti)}, {"overlap", true}});
    }
  }
  edgesDirty = false;
}

Json PerceptualSpaceSimulator::machine_graph_data() const {
  Json::Array nodes;
  for (const auto& [id, m] : machines) {
    if (!m.perceptualMapping) continue;
    const auto& mapping = *m.perceptualMapping;
    nodes.push_back(Json::Object{{"id", id}, {"name", m.name}, {"description", m.description},
        {"inputMapping", to_json(mapping.input)}, {"outputMapping", to_json(mapping.output)},
        {"metadata", m.metadata}});
  }
  if (edgesDirty) rebuild_edge_cache();
  return Json::Object{{"nodes", nodes}, {"edges", cachedEdges},
      {"perceptualSpaceDimension", static_cast<double>(space.dimension())}};
}
Json PerceptualSpaceSimulator::state_json() const {
  Json::Array machineJson;
  for (const auto& [_, m] : machines) machineJson.push_back(m.to_json());
  return Json::Object{{"state", Json::Object{{"perceptualSpace", json::numbers(space.vector())}, {"currentStep", static_cast<double>(currentStep)}, {"isRunning", running}, {"machines", machineJson}}}};
}
std::vector<SimulationStep> PerceptualSpaceSimulator::history() const { return steps; }
void PerceptualSpaceSimulator::set_history_limit(size_t limit) {
  maxHistory = limit;
  if (steps.size() > maxHistory) steps.resize(maxHistory);
}
size_t PerceptualSpaceSimulator::history_limit() const { return maxHistory; }
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

Machine load_machine_from_json_string(const std::string& raw,
                                      std::optional<std::string> overrideId,
                                      const LoadOptions& opts) {
  Json root = json::parse(raw);
  // Version check — same validation Scala MachineLoader enforces.
  const auto& verField = root.at("version");
  if (!verField.is_string())
    throw std::runtime_error("Missing required field: version");
  {
    std::string ver = verField.as_string();
    int major = 0;
    auto dotPos = ver.find('.');
    if (dotPos != std::string::npos) {
      try { major = std::stoi(ver.substr(0, dotPos)); } catch (...) {}
    }
    if (major != 1)
      throw std::runtime_error("Incompatible machine JSON version: " + ver + " (current: 1.0.0)");
  }
  // STA strict-load gate — mirrors RealityEngine_AI's MachineLoader.loadFromJSON
  // option of the same name.  Runs before any RealityVector is constructed so
  // a violating life-safety machine cannot reach the engine.
  if (opts.strictSta) sta::assert_sta_for_life_safety(root);
  const Json& m = root.at("machine");
  std::string id = overrideId.value_or(make_id("machine"));
  std::string name = m.at("name").as_string("unnamed");
  std::string desc = m.at("description").as_string();
  ArbiterRule arbiter = ArbiterRule::Passthrough;
  if (!m.at("arbiterRule").is_null()) arbiter = arbiter_from_string(m.at("arbiterRule").as_string("PASSTHROUGH"));
  std::optional<PerceptualMapping> mapping;
  const auto& pm = m.at("perceptualMapping");
  if (pm.is_object()) {
    mapping = PerceptualMapping{parse_region(pm.at("input")), parse_region(pm.at("output")), std::nullopt};
    if (pm.at("bitsPerElement").is_number()) {
      int bits = static_cast<int>(pm.at("bitsPerElement").as_number());
      if (is_allowed_bits_per_element(bits)) mapping->bitsPerElement = bits;
    }
  }
  Machine machine(name, desc, arbiter, mapping, id);
  if (m.at("matchAlgorithm").is_string()) machine.matchAlgorithm = comparator_from_string(m.at("matchAlgorithm").as_string());
  if (m.at("metadata").is_object()) machine.metadata = m.at("metadata").object();
  if (m.at("inputSequences").is_array()) machine.metadata["inputSequences"] = m.at("inputSequences");
  for (const auto& sj : m.at("sequences").is_array() ? m.at("sequences").array() : Json::Array{}) {
    CriticalEventSequence seq(sj.at("name").as_string("unnamed"), sj.at("id").as_string(make_id("sequence")));
    if (sj.at("metadata").is_object()) seq.metadata = sj.at("metadata").object();
    // Lifecycle fields — same names as the AI runtime; absence is normal.
    if (sj.at("schemaVersion").is_string()) seq.schemaVersion = sj.at("schemaVersion").as_string();
    if (sj.at("deprecatedAt").is_string()) seq.deprecatedAt = sj.at("deprecatedAt").as_string();
    if (sj.at("replacedBy").is_string())   seq.replacedBy   = sj.at("replacedBy").as_string();
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
        rv.add_output_vector({oj.at("id").as_string(make_id("output")), json::to_numbers(oj.at("vector")), meta, now_ms(), {}});
      }
      seq.add_vector(rv);
    }
    machine.add_sequence(seq);
  }
  return machine;
}

std::vector<Machine> load_machines_from_directory(const std::string& directory, const LoadOptions& opts) {
  std::vector<Machine> out;
  namespace fs = std::filesystem;
  if (!fs::exists(directory)) return out;
  std::vector<fs::path> files;
  for (const auto& p : fs::recursive_directory_iterator(directory)) if (p.path().extension() == ".json") files.push_back(p.path());
  std::sort(files.begin(), files.end());
  for (const auto& file : files) {
    std::ifstream in(file);
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string stem = file.stem().string();
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return std::isalnum(c) ? static_cast<char>(std::tolower(c)) : '-'; });
    try { out.push_back(load_machine_from_json_string(raw, "machine-" + stem, opts)); }
    catch (const std::exception& e) { std::cerr << "Failed to load " << file << ": " << e.what() << "\n"; }
  }
  return out;
}

std::filesystem::path find_machine_file(const std::filesystem::path& directory, const std::string& filename) {
  namespace fs = std::filesystem;
  fs::path flat = directory / filename;
  if (fs::exists(flat) || !fs::exists(directory)) return flat;
  for (const auto& p : fs::recursive_directory_iterator(directory)) {
    if (p.path().filename().string() == filename) return p.path();
  }
  return flat;
}

Json to_json(const RegionMapping& r) { return Json::Object{{"offset", static_cast<double>(r.offset)}, {"length", static_cast<double>(r.length)}}; }
Json to_json(const PerceptualMapping& m) {
  Json::Object out{{"input", to_json(m.input)}, {"output", to_json(m.output)}};
  if (m.bitsPerElement) out["bitsPerElement"] = static_cast<double>(*m.bitsPerElement);
  return out;
}

bool is_allowed_bits_per_element(int bits) {
  return bits == 1 || bits == 2 || bits == 4 || bits == 8;
}

void validate_cell_range(const Vector& values, int bitsPerElement) {
  if (!is_allowed_bits_per_element(bitsPerElement)) {
    throw std::range_error("bitsPerElement must be one of 1, 2, 4, 8");
  }
  const int maxValue = (1 << bitsPerElement) - 1;
  for (size_t i = 0; i < values.size(); ++i) {
    double v = values[i];
    if (!std::isfinite(v) || v < 0.0 || v > maxValue || std::floor(v) != v) {
      std::ostringstream ss;
      ss << "cell[" << i << "]=" << v << " does not fit in "
         << bitsPerElement << "-bit cell (range 0.." << maxValue << ")";
      throw std::range_error(ss.str());
    }
  }
}

std::vector<unsigned char> pack_cells(const Vector& values, int bitsPerElement) {
  validate_cell_range(values, bitsPerElement);
  std::vector<unsigned char> bytes((values.size() * static_cast<size_t>(bitsPerElement) + 7) / 8, 0);
  if (bitsPerElement == 8) {
    for (size_t i = 0; i < values.size(); ++i) bytes[i] = static_cast<unsigned char>(values[i]);
    return bytes;
  }
  const unsigned int mask = static_cast<unsigned int>((1 << bitsPerElement) - 1);
  for (size_t i = 0; i < values.size(); ++i) {
    size_t bitIdx = i * static_cast<size_t>(bitsPerElement);
    size_t byteIdx = bitIdx >> 3;
    int shift = 8 - bitsPerElement - static_cast<int>(bitIdx & 7);
    bytes[byteIdx] |= static_cast<unsigned char>((static_cast<unsigned int>(values[i]) & mask) << shift);
  }
  return bytes;
}

Vector unpack_cells(const std::vector<unsigned char>& bytes, size_t length, int bitsPerElement) {
  if (!is_allowed_bits_per_element(bitsPerElement)) {
    throw std::range_error("bitsPerElement must be one of 1, 2, 4, 8");
  }
  size_t required = (length * static_cast<size_t>(bitsPerElement) + 7) / 8;
  if (bytes.size() < required) throw std::range_error("bytes too small for packed cell payload");
  Vector out(length, 0.0);
  if (bitsPerElement == 8) {
    for (size_t i = 0; i < length; ++i) out[i] = bytes[i];
    return out;
  }
  const unsigned int mask = static_cast<unsigned int>((1 << bitsPerElement) - 1);
  for (size_t i = 0; i < length; ++i) {
    size_t bitIdx = i * static_cast<size_t>(bitsPerElement);
    size_t byteIdx = bitIdx >> 3;
    int shift = 8 - bitsPerElement - static_cast<int>(bitIdx & 7);
    out[i] = static_cast<double>((bytes[byteIdx] >> shift) & mask);
  }
  return out;
}

std::string encode_packed_base64(const Vector& values, int bitsPerElement) {
  const auto bytes = pack_cells(values, bitsPerElement);
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  for (size_t i = 0; i < bytes.size(); i += 3) {
    unsigned int b0 = bytes[i];
    unsigned int b1 = (i + 1 < bytes.size()) ? bytes[i + 1] : 0;
    unsigned int b2 = (i + 2 < bytes.size()) ? bytes[i + 2] : 0;
    unsigned int triple = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(kBase64Alphabet[(triple >> 18) & 0x3F]);
    out.push_back(kBase64Alphabet[(triple >> 12) & 0x3F]);
    out.push_back(i + 1 < bytes.size() ? kBase64Alphabet[(triple >> 6) & 0x3F] : '=');
    out.push_back(i + 2 < bytes.size() ? kBase64Alphabet[triple & 0x3F] : '=');
  }
  return out;
}

StorageFootprint storage_footprint(size_t length, int bitsPerElement) {
  if (!is_allowed_bits_per_element(bitsPerElement)) {
    throw std::range_error("bitsPerElement must be one of 1, 2, 4, 8");
  }
  StorageFootprint out;
  out.float64Bytes = length * 8;
  out.packedBytes = (length * static_cast<size_t>(bitsPerElement) + 7) / 8;
  out.shrinkFactor = out.packedBytes == 0 ? 0.0 : static_cast<double>(out.float64Bytes) / static_cast<double>(out.packedBytes);
  return out;
}

Json to_json(const OutputVector& o) {
  Json::Array prov;
  for (const auto& vid : o.provenance) prov.emplace_back(vid);
  return Json::Object{
    {"id", o.id},
    {"vector", json::numbers(o.vector)},
    {"metadata", o.metadata},
    {"timestamp", static_cast<double>(o.timestamp)},
    {"provenance", prov}
  };
}
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
Json to_json(const SimulationStep& step, bool includeMachineResults, bool includePerceptualSpace) {
  Json::Object machineResults;
  if (includeMachineResults) {
    for (const auto& [id, mr] : step.machineResults) machineResults[id] = Json::Object{{"machineId", mr.machineId}, {"machineName", mr.machineName}, {"inputVector", json::numbers(mr.inputVector)}, {"outputVector", mr.outputVector ? Json(json::numbers(*mr.outputVector)) : Json(nullptr)}, {"inputRegion", to_json(mr.inputRegion)}, {"outputRegion", mr.outputRegion ? to_json(*mr.outputRegion) : Json(nullptr)}, {"transitionResult", to_json(mr.transitionResult)}};
  }
  Json::Array regions;
  for (const auto& r : step.activeRegions) regions.push_back(Json::Object{{"offset", static_cast<double>(r.offset)}, {"length", static_cast<double>(r.length)}, {"machineId", r.machineId}, {"type", r.type}});
  Json::Array mergeBatch;
  for (const auto& op : step.mergeBatch) {
    Json::Array provArr;
    for (const auto& vid : op.provenance) provArr.emplace_back(vid);
    Json::Object obj{
      {"region", to_json(op.region)},
      {"machineId", op.machineId},
      {"sequenceId", op.sequenceId},
      {"outputIndex", static_cast<double>(op.outputIndex)},
      {"values", json::numbers(op.values)},
      {"provenance", provArr}
    };
    if (op.governance) obj["governance"] = to_json(*op.governance);
    if (op.deprecation) {
      Json::Object d{
        {"since",   op.deprecation->since},
        {"ageDays", static_cast<double>(op.deprecation->ageDays)},
      };
      if (!op.deprecation->replacedBy.empty()) d["replacedBy"] = op.deprecation->replacedBy;
      obj["deprecation"] = std::move(d);
    }
    mergeBatch.push_back(std::move(obj));
  }
  // Event bus writes — emitted whenever a producer's fired sequence
  // matches a meta-machine's compose subscription.  Empty for runtimes
  // without meta-CES wiring.
  Json::Array eventBus;
  for (const auto& w : step.eventBus) {
    Json::Array prov;
    for (const auto& vid : w.provenance) prov.emplace_back(vid);
    eventBus.push_back(Json::Object{
      {"producerMachineId", w.producerMachineId},
      {"producerSequenceId", w.producerSequenceId},
      {"subscriberMachineId", w.subscriberMachineId},
      {"bitOffset", static_cast<double>(w.bitOffset)},
      {"value", w.value},
      {"provenance", prov}
    });
  }
  Json::Object out{{"stepNumber", static_cast<double>(step.stepNumber)}, {"timestamp", static_cast<double>(step.timestamp)}, {"activeRegions", regions}};
  // mergeBatch is the authoritative synchronization result — clients should
  // apply these region writes to stay in sync. The dense perceptualSpace
  // payload is a debug projection of the post-merge state and may be omitted
  // via includePerceptualSpace=false.
  out["mergeBatch"] = mergeBatch;
  out["eventBus"] = eventBus;
  if (includePerceptualSpace) {
    out["perceptualSpace"] = json::numbers(step.perceptualSpace);
    out["perceptualSpaceIsDebugProjection"] = true;
  }
  if (includeMachineResults) out["machineResults"] = machineResults;
  return out;
}
Json to_json(const SimulationStep& step, bool includeMachineResults) {
  return to_json(step, includeMachineResults, true);
}
Json to_json(const SimulationStep& step) {
  return to_json(step, true);
}
Json to_json(const SourceConfig& s) {
  Json::Object o{{"id", s.id}, {"name", s.name}, {"region", to_json(s.region)}, {"active", s.active}, {"type", s.kind}};
  if (s.kind == "test") {
    Json::Array inputs;
    for (const auto& v : s.inputs) inputs.push_back(json::numbers(v));
    o["machineId"] = s.machineId; o["machineName"] = s.machineName; o["sequenceName"] = s.sequenceName; o["inputs"] = inputs; o["loop"] = s.loop; o["metadata"] = s.sequenceMetadata; o["sequence"] = s.testSequence;
  } else if (s.kind == "sensor") {
    o["sensorId"] = s.sensorId; o["lastValue"] = json::numbers(s.lastValue); o["lastUpdated"] = s.lastUpdated ? Json(static_cast<double>(*s.lastUpdated)) : Json(nullptr); o["ttlMs"] = static_cast<double>(s.ttlMs);
    if (!s.origin.empty()) o["origin"] = s.origin;
  } else {
    o["pattern"] = to_string(s.pattern); o["frequency"] = s.frequency; o["amplitude"] = s.amplitude; o["dcOffset"] = s.dcOffset;
  }
  return o;
}

namespace {
std::string prom_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n";  break;
      default:   out.push_back(c);
    }
  }
  return out;
}

// Variant that accepts an arbitrary number of label pairs.  Used by the
// /api/metrics emitter to prepend baseLabels (e.g. runtime="cpp") onto
// every metric line without duplicating the call sites for the base case.
std::string prom_labels_v(const std::vector<std::pair<std::string, std::string>>& base,
                          std::initializer_list<std::pair<const char*, std::string>> extras) {
  std::string out;
  if (base.empty() && extras.size() == 0) return out;
  out += "{";
  bool first = true;
  for (const auto& [k, v] : base) {
    if (!first) out += ",";
    first = false;
    out += k;
    out += "=\"";
    out += prom_escape(v);
    out += "\"";
  }
  for (const auto& [k, v] : extras) {
    if (!first) out += ",";
    first = false;
    out += k;
    out += "=\"";
    out += prom_escape(v);
    out += "\"";
  }
  out += "}";
  return out;
}

// Split a tab-joined coverage map key back into its labels.
std::vector<std::string> split_key(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == '\t') { out.push_back(cur); cur.clear(); } else cur.push_back(c);
  }
  out.push_back(cur);
  return out;
}
} // anonymous namespace

void CesCoverageRegistry::record(const Machine& machine, const MachineTransitionResult& result) {
  std::string base = machine.id + "\t" + machine.name;
  steps[base] += 1;
  for (const auto& [sequenceId, sr] : result.sequenceResults) {
    for (const auto& vectorId : sr.matchedVectors) {
      matched[base + "\t" + sequenceId + "\t" + vectorId] += 1;
    }
    for (const auto& vectorId : sr.activatedVectors) {
      activated[base + "\t" + sequenceId + "\t" + vectorId] += 1;
    }
    if (result.arbiterMetadata.shouldOutput && !sr.assertedOutputs.empty()) {
      outputs[base + "\t" + sequenceId] += static_cast<long long>(sr.assertedOutputs.size());
    }
  }
}

void CesCoverageRegistry::record_paging_decision(const std::string& ownerTeam,
                                                  const std::string& processStatus,
                                                  const std::string& ragStatusCode,
                                                  const std::string& machineId) {
  pagingDecisions[ownerTeam + "\t" + processStatus + "\t" + ragStatusCode + "\t" + machineId] += 1;
}

void CesCoverageRegistry::record_deprecated_fire(const std::string& machineId,
                                                  const std::string& machineName,
                                                  const std::string& sequenceId,
                                                  const std::string& replacedBy) {
  deprecatedFires[machineId + "\t" + machineName + "\t" + sequenceId + "\t" + replacedBy] += 1;
}

void CesCoverageRegistry::reset() {
  matched.clear();
  activated.clear();
  outputs.clear();
  steps.clear();
  pagingDecisions.clear();
  deprecatedFires.clear();
  startedAtMs = now_ms();
}

std::string CesCoverageRegistry::to_prometheus_text(
    const std::map<std::string, Machine>& machines,
    const std::vector<std::pair<std::string, std::string>>& baseLabels) const {
  std::ostringstream out;
  size_t totalSequences = 0, totalVectors = 0;
  for (const auto& [_, m] : machines) {
    for (const auto& seq : m.all_sequences()) {
      ++totalSequences;
      totalVectors += seq.all_vectors().size();
    }
  }

  // bl() renders only the base labels (used for the unlabelled gauges so
  // they still carry the runtime tag).  An empty baseLabels produces an
  // empty string, preserving the historical `ces_machines_total N` form.
  auto bl = [&]() -> std::string { return prom_labels_v(baseLabels, {}); };

  out << "# HELP ces_machines_total Number of machines registered with the simulator.\n";
  out << "# TYPE ces_machines_total gauge\n";
  out << "ces_machines_total" << bl() << " " << machines.size() << "\n";

  out << "# HELP ces_sequences_total Number of sequences across all registered machines.\n";
  out << "# TYPE ces_sequences_total gauge\n";
  out << "ces_sequences_total" << bl() << " " << totalSequences << "\n";

  out << "# HELP ces_vectors_total Number of event vectors across all registered machines.\n";
  out << "# TYPE ces_vectors_total gauge\n";
  out << "ces_vectors_total" << bl() << " " << totalVectors << "\n";

  out << "# HELP ces_vector_matched_total Number of times a vector matched its input during a transition phase.\n";
  out << "# TYPE ces_vector_matched_total counter\n";
  for (const auto& [k, count] : matched) {
    auto parts = split_key(k);
    if (parts.size() == 4) {
      out << "ces_vector_matched_total"
          << prom_labels_v(baseLabels, {{"machine", parts[1]}, {"machine_id", parts[0]}, {"sequence", parts[2]}, {"vector", parts[3]}})
          << " " << count << "\n";
    }
  }

  out << "# HELP ces_vector_activated_total Number of times a vector was activated as a successor in a transition.\n";
  out << "# TYPE ces_vector_activated_total counter\n";
  for (const auto& [k, count] : activated) {
    auto parts = split_key(k);
    if (parts.size() == 4) {
      out << "ces_vector_activated_total"
          << prom_labels_v(baseLabels, {{"machine", parts[1]}, {"machine_id", parts[0]}, {"sequence", parts[2]}, {"vector", parts[3]}})
          << " " << count << "\n";
    }
  }

  out << "# HELP ces_sequence_outputs_total Number of asserted outputs emitted by a sequence.\n";
  out << "# TYPE ces_sequence_outputs_total counter\n";
  for (const auto& [k, count] : outputs) {
    auto parts = split_key(k);
    if (parts.size() == 3) {
      out << "ces_sequence_outputs_total"
          << prom_labels_v(baseLabels, {{"machine", parts[1]}, {"machine_id", parts[0]}, {"sequence", parts[2]}})
          << " " << count << "\n";
    }
  }

  out << "# HELP ces_machine_steps_total Number of process_input calls observed for this machine.\n";
  out << "# TYPE ces_machine_steps_total counter\n";
  for (const auto& [k, count] : steps) {
    auto parts = split_key(k);
    if (parts.size() == 2) {
      out << "ces_machine_steps_total"
          << prom_labels_v(baseLabels, {{"machine", parts[1]}, {"machine_id", parts[0]}})
          << " " << count << "\n";
    }
  }

  // Paging decisions resolved by the governance contract — same label
  // shape as the AI runtime so a single Prom scrape config covers both.
  // The `machine` (name) label is derived from machine_id so the dashboard
  // `machine=~"$machine"` filter resolves; the event hash key carries id
  // only because owner_team/process_status are the partitioning axes.
  out << "# HELP ces_paging_decisions_total Number of governance-resolved paging decisions issued by the engine.\n";
  out << "# TYPE ces_paging_decisions_total counter\n";
  for (const auto& [k, count] : pagingDecisions) {
    auto parts = split_key(k);
    if (parts.size() == 4) {
      auto mit = machines.find(parts[3]);
      std::string mname = (mit != machines.end()) ? mit->second.name : "";
      out << "ces_paging_decisions_total"
          << prom_labels_v(baseLabels, {
            {"owner_team",      parts[0]},
            {"process_status",  parts[1]},
            {"rag_status_code", parts[2]},
            {"machine_id",      parts[3]},
            {"machine",         mname},
          })
          << " " << count << "\n";
    }
  }

  // Deprecated-sequence fires — non-zero means a sequence past its
  // deprecation date is still emitting outputs in production.
  out << "# HELP ces_deprecated_fires_total Number of times a deprecated sequence emitted output.\n";
  out << "# TYPE ces_deprecated_fires_total counter\n";
  for (const auto& [k, count] : deprecatedFires) {
    auto parts = split_key(k);
    if (parts.size() == 4) {
      out << "ces_deprecated_fires_total"
          << prom_labels_v(baseLabels, {
            {"machine",     parts[1]},
            {"machine_id",  parts[0]},
            {"sequence",    parts[2]},
            {"replaced_by", parts[3]},
          })
          << " " << count << "\n";
    }
  }

  // Zero-baseline counter series — dashboards plot rate() / by(machine)
  // against these before any events fire.  Event-keyed series above carry
  // sequence/vector sub-labels so they live in a distinct Prom label set
  // and coexist without duplication; ces_machine_steps_total has the same
  // label shape as its baseline so we skip machines already seen in the
  // events hash.
  std::set<std::string> seenSteps;
  for (const auto& [k, _v] : steps) {
    auto parts = split_key(k);
    if (parts.size() == 2) seenSteps.insert(parts[0]);
  }
  for (const auto& [_id, m] : machines) {
    out << "ces_vector_matched_total"   << prom_labels_v(baseLabels, {{"machine", m.name}, {"machine_id", m.id}}) << " 0\n";
    out << "ces_vector_activated_total" << prom_labels_v(baseLabels, {{"machine", m.name}, {"machine_id", m.id}}) << " 0\n";
    out << "ces_sequence_outputs_total" << prom_labels_v(baseLabels, {{"machine", m.name}, {"machine_id", m.id}}) << " 0\n";
    out << "ces_deprecated_fires_total" << prom_labels_v(baseLabels, {{"machine", m.name}, {"machine_id", m.id}}) << " 0\n";
    if (!seenSteps.count(m.id)) {
      out << "ces_machine_steps_total"  << prom_labels_v(baseLabels, {{"machine", m.name}, {"machine_id", m.id}}) << " 0\n";
    }
  }

  // Per-machine "code-coverage at the operational-state level" gauges.
  out << "# HELP ces_unfired_sequences Number of sequences in this machine that have never emitted output.\n";
  out << "# TYPE ces_unfired_sequences gauge\n";
  for (const auto& [_, m] : machines) {
    int total = 0, fired = 0;
    for (const auto& seq : m.all_sequences()) {
      ++total;
      if (outputs.count(m.id + "\t" + m.name + "\t" + seq.id) > 0) ++fired;
    }
    out << "ces_unfired_sequences" << prom_labels_v(baseLabels, {{"machine", m.name}, {"machine_id", m.id}})
        << " " << (total - fired) << "\n";
  }

  out << "# HELP ces_unfired_vectors Number of vectors in this machine that have never matched or activated.\n";
  out << "# TYPE ces_unfired_vectors gauge\n";
  for (const auto& [_, m] : machines) {
    int total = 0, fired = 0;
    for (const auto& seq : m.all_sequences()) {
      for (const auto& v : seq.all_vectors()) {
        ++total;
        std::string key = m.id + "\t" + m.name + "\t" + seq.id + "\t" + v.id;
        if (matched.count(key) > 0 || activated.count(key) > 0) ++fired;
      }
    }
    out << "ces_unfired_vectors" << prom_labels_v(baseLabels, {{"machine", m.name}, {"machine_id", m.id}})
        << " " << (total - fired) << "\n";
  }

  out << "# HELP ces_machine_sequence_count Total sequences declared by this machine.\n";
  out << "# TYPE ces_machine_sequence_count gauge\n";
  for (const auto& [_, m] : machines) {
    out << "ces_machine_sequence_count" << prom_labels_v(baseLabels, {{"machine", m.name}, {"machine_id", m.id}})
        << " " << m.sequence_count() << "\n";
  }

  out << "# HELP ces_machine_vector_count Total vectors declared by this machine.\n";
  out << "# TYPE ces_machine_vector_count gauge\n";
  for (const auto& [_, m] : machines) {
    out << "ces_machine_vector_count" << prom_labels_v(baseLabels, {{"machine", m.name}, {"machine_id", m.id}})
        << " " << m.total_vector_count() << "\n";
  }

  long long uptimeMs = now_ms() - startedAtMs;
  out << "# HELP ces_registry_uptime_seconds Seconds since the coverage registry was instantiated.\n";
  out << "# TYPE ces_registry_uptime_seconds gauge\n";
  out << "ces_registry_uptime_seconds" << bl() << " " << (uptimeMs / 1000.0) << "\n";

  return out.str();
}

// ── Governance resolver ─────────────────────────────────────────────────────

namespace {
bool values_match(const std::vector<double>& a, const Json& b) {
  if (!b.is_array()) return false;
  const auto& arr = b.array();
  if (arr.size() != a.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!arr[i].is_number()) return false;
    if (arr[i].as_number() != a[i]) return false;
  }
  return true;
}

std::string js(const Json& v, const std::string& fallback = "") {
  return v.is_string() ? v.as_string() : fallback;
}
} // namespace

std::optional<PagingDecision> resolve_governance(
    const Machine& machine, const std::string& sequenceId, const std::vector<double>& values) {
  auto tcIt = machine.metadata.find("triggerConfig");
  if (tcIt == machine.metadata.end() || !tcIt->second.is_object()) return std::nullopt;
  auto rulesField = tcIt->second.at("rules");
  if (!rulesField.is_array()) return std::nullopt;

  const Json* match = nullptr;
  for (const auto& rule : rulesField.array()) {
    if (!rule.is_object()) continue;
    if (js(rule.at("sequenceId")) != sequenceId) continue;
    if (!values_match(values, rule.at("outputMatches"))) continue;
    match = &rule;
    break;
  }
  if (!match) return std::nullopt;

  // Machine-level governance defaults.
  auto govIt = machine.metadata.find("governance");
  bool hasMachineGov = govIt != machine.metadata.end() && govIt->second.is_object();
  const Json& mGov = hasMachineGov ? govIt->second : Json(nullptr);

  // Rule-level overrides.
  Json ruleGov = match->at("governance");

  PagingDecision d;
  d.machineId      = machine.id;
  d.machineName    = machine.name;
  d.sequenceId     = sequenceId;
  d.ragStatusCode  = js(match->at("ragStatusCode"));
  d.processStatus  = js(match->at("processStatus"));
  d.description    = js(match->at("description"));

  // Precedence: rule override → machine defaults → empty.
  d.ownerTeam = js(ruleGov.at("ownerTeam"),
                   hasMachineGov ? js(mGov.at("ownerTeam"), "unrouted") : "unrouted");
  d.runbook = js(ruleGov.at("runbook"),
                 hasMachineGov ? js(mGov.at("runbook")) : "");
  d.escalationPolicy = js(ruleGov.at("escalationPolicy"),
                          hasMachineGov ? js(mGov.at("escalationPolicy")) : "");

  // SLA: rule.slaSeconds wins; else machine.sla[processStatus]; else nullopt.
  if (ruleGov.is_object() && ruleGov.at("slaSeconds").is_number()) {
    d.slaSeconds = static_cast<int>(ruleGov.at("slaSeconds").as_number());
  } else if (hasMachineGov && mGov.at("sla").is_object() && !d.processStatus.empty()) {
    const auto& slaObj = mGov.at("sla").object();
    auto it = slaObj.find(d.processStatus);
    if (it != slaObj.end() && it->second.is_number()) {
      d.slaSeconds = static_cast<int>(it->second.as_number());
    }
  }

  // Contact: rule.contact > machine.contact > empty.
  auto pickContact = [&](const std::string& field) -> std::string {
    if (ruleGov.is_object() && ruleGov.at("contact").is_object()) {
      auto v = ruleGov.at("contact").at(field);
      if (v.is_string()) return v.as_string();
    }
    if (hasMachineGov && mGov.at("contact").is_object()) {
      auto v = mGov.at("contact").at(field);
      if (v.is_string()) return v.as_string();
    }
    return "";
  };
  d.contactPrimary   = pickContact("primary");
  d.contactSecondary = pickContact("secondary");

  d.hasMachineGovernance = hasMachineGov;
  if (ruleGov.is_object())      d.source = "rule-with-override";
  else if (hasMachineGov)       d.source = "rule-only";
  else                          d.source = "machine-fallback";
  return d;
}

Json to_json(const PagingDecision& d) {
  Json::Object o;
  o["machineId"]            = d.machineId;
  o["machineName"]          = d.machineName;
  o["sequenceId"]           = d.sequenceId;
  o["ragStatusCode"]        = d.ragStatusCode.empty() ? Json(nullptr) : Json(d.ragStatusCode);
  o["processStatus"]        = d.processStatus.empty() ? Json(nullptr) : Json(d.processStatus);
  o["ownerTeam"]            = d.ownerTeam;
  o["slaSeconds"]           = d.slaSeconds ? Json(static_cast<double>(*d.slaSeconds)) : Json(nullptr);
  o["runbook"]              = d.runbook.empty() ? Json(nullptr) : Json(d.runbook);
  o["escalationPolicy"]     = d.escalationPolicy.empty() ? Json(nullptr) : Json(d.escalationPolicy);
  Json::Object contact;
  if (!d.contactPrimary.empty())   contact["primary"]   = d.contactPrimary;
  if (!d.contactSecondary.empty()) contact["secondary"] = d.contactSecondary;
  o["contact"]              = contact;
  if (!d.description.empty()) o["description"] = d.description;
  o["source"]               = d.source;
  o["hasMachineGovernance"] = d.hasMachineGovernance;
  return o;
}

Json worker_pool_metrics_json() {
  auto m = domain_workers().metrics();
  return Json::Object{
    {"workers", static_cast<double>(m.workers)},
    {"queued", static_cast<double>(m.queued)},
    {"active", static_cast<double>(m.active)},
    {"completed", static_cast<double>(m.completed)},
    {"rejected", static_cast<double>(m.rejected)},
    {"capacity", static_cast<double>(m.capacity)}
  };
}

} // namespace reality
