#include "reality/json.hpp"
#include "reality/reality.hpp"

#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>

using namespace reality;

namespace {

constexpr int kCasesPerDomain = 10;

struct DomainCase {
  std::string category;
  std::filesystem::path file;
  Json sequence;
  std::string rawMachine;
};

struct CaseResult {
  std::string machineFile;
  std::string sequenceName;
  int steps = 0;
  int outputs = 0;
};

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("Failed to open " + path.string());
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool approx_equal(const Vector& a, const Vector& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::abs(a[i] - b[i]) > 1e-9) return false;
  }
  return true;
}

std::vector<Vector> parse_vector_literals(const std::string& text) {
  std::vector<Vector> out;
  std::regex rx(R"(\[[^\[\]]+\])");
  for (std::sregex_iterator it(text.begin(), text.end(), rx), end; it != end; ++it) {
    try {
      auto v = json::to_numbers(json::parse(it->str()));
      if (!v.empty()) out.push_back(v);
    } catch (...) {
    }
  }
  return out;
}

int exact_expected_count_from_metadata(const Json& meta) {
  if (!meta.is_object()) return -1;
  if (meta.at("expectedOutputCount").is_number()) return static_cast<int>(meta.at("expectedOutputCount").as_number());
  return -1;
}

int minimum_expected_count_from_metadata(const Json& meta) {
  if (!meta.is_object()) return -1;
  if (meta.at("expectedOutputs").is_number()) return static_cast<int>(meta.at("expectedOutputs").as_number());
  return -1;
}

std::vector<Vector> expected_vectors_from_metadata(const Json& meta) {
  std::vector<Vector> expected;
  if (!meta.is_object()) return expected;
  if (meta.at("expectedOutputVector").is_string()) {
    auto parsed = parse_vector_literals(meta.at("expectedOutputVector").as_string());
    expected.insert(expected.end(), parsed.begin(), parsed.end());
  }
  if (meta.at("expectedOutputSequence").is_string()) {
    auto parsed = parse_vector_literals(meta.at("expectedOutputSequence").as_string());
    expected.insert(expected.end(), parsed.begin(), parsed.end());
  }
  return expected;
}

bool contains_ordered(const std::vector<Vector>& actual, const std::vector<Vector>& expected) {
  size_t pos = 0;
  for (const auto& e : expected) {
    bool found = false;
    while (pos < actual.size()) {
      if (approx_equal(actual[pos], e)) {
        found = true;
        ++pos;
        break;
      }
      ++pos;
    }
    if (!found) return false;
  }
  return true;
}

std::string vector_to_string(const Vector& v) {
  std::ostringstream out;
  out << "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out << ",";
    out << v[i];
  }
  out << "]";
  return out.str();
}

std::string vectors_to_string(const std::vector<Vector>& vectors) {
  std::ostringstream out;
  for (size_t i = 0; i < vectors.size(); ++i) {
    if (i) out << ", ";
    out << vector_to_string(vectors[i]);
  }
  return out.str();
}

int simulator_dimension_for(const Machine& machine) {
  int dimension = 768;
  if (machine.perceptualMapping) {
    const auto& mapping = *machine.perceptualMapping;
    dimension = std::max(dimension, mapping.input.offset + mapping.input.length);
    dimension = std::max(dimension, mapping.output.offset + mapping.output.length);
  }
  return dimension;
}

CaseResult run_domain_case(const DomainCase& dc, int caseIndex) {
  std::string stem = dc.file.stem().string();
  Machine machine = load_machine_from_json_string(dc.rawMachine, "machine-" + stem + "-domain-" + std::to_string(caseIndex));
  if (!machine.perceptualMapping) {
    throw std::runtime_error(dc.file.filename().string() + ": machine has no perceptual mapping for domain e2e");
  }

  PerceptualSpaceRuntime spaceRuntime(simulator_dimension_for(machine));
  std::string machineId = machine.id;
  spaceRuntime.add_machine(machine);

  CaseResult result;
  result.machineFile = dc.file.filename().string();
  result.sequenceName = dc.sequence.at("name").as_string(dc.sequence.at("id").as_string("unnamed"));

  std::vector<Vector> actualOutputs;
  const auto& vectorsJson = dc.sequence.at_either("events", "vectors");
  if (!vectorsJson.is_array()) {
    throw std::runtime_error(result.machineFile + " / " + result.sequenceName + ": input sequence has no vectors array");
  }

  for (const auto& vj : vectorsJson.array()) {
    Vector input = json::to_numbers(vj);
    if (input.empty()) throw std::runtime_error(result.machineFile + " / " + result.sequenceName + ": empty input vector");

    Vector universal(simulator_dimension_for(machine), 0.0);
    const auto& inputRegion = machine.perceptualMapping->input;
    if (static_cast<int>(input.size()) != inputRegion.length) {
      throw std::runtime_error(result.machineFile + " / " + result.sequenceName +
        ": input vector length " + std::to_string(input.size()) +
        " does not match mapped input length " + std::to_string(inputRegion.length));
    }
    for (size_t i = 0; i < input.size(); ++i) universal[inputRegion.offset + static_cast<int>(i)] = input[i];

    auto step = spaceRuntime.process_immediate(universal);
    auto it = step.machineResults.find(machineId);
    if (it == step.machineResults.end()) {
      throw std::runtime_error(result.machineFile + " / " + result.sequenceName + ": spaceRuntime did not process machine");
    }
    if (it->second.transitionResult.machineOutput) {
      actualOutputs.push_back(it->second.transitionResult.machineOutput->vector);
    }
    ++result.steps;
  }
  result.outputs = static_cast<int>(actualOutputs.size());

  const Json& meta = dc.sequence.at("metadata");
  int exactExpectedCount = exact_expected_count_from_metadata(meta);
  if (exactExpectedCount >= 0 && exactExpectedCount != result.outputs) {
    throw std::runtime_error(result.machineFile + " / " + result.sequenceName +
      ": expected exactly " + std::to_string(exactExpectedCount) + " output(s), got " + std::to_string(result.outputs) +
      " actual=" + vectors_to_string(actualOutputs));
  }

  int minimumExpectedCount = minimum_expected_count_from_metadata(meta);
  if (minimumExpectedCount >= 0 && result.outputs < minimumExpectedCount) {
    throw std::runtime_error(result.machineFile + " / " + result.sequenceName +
      ": expected at least " + std::to_string(minimumExpectedCount) + " output(s), got " + std::to_string(result.outputs) +
      " actual=" + vectors_to_string(actualOutputs));
  }

  auto expectedVectors = expected_vectors_from_metadata(meta);
  if (!expectedVectors.empty() && !contains_ordered(actualOutputs, expectedVectors)) {
    throw std::runtime_error(result.machineFile + " / " + result.sequenceName +
      ": expected output vector sequence " + vectors_to_string(expectedVectors) +
      " not found in actual " + vectors_to_string(actualOutputs));
  }

  return result;
}

} // namespace

int main(int argc, char** argv) {
  std::filesystem::path machinesDir = argc > 1 ? argv[1] : "../RealityEngine_Machines/machines";
  if (!std::filesystem::exists(machinesDir)) {
    std::cerr << "Machine directory not found: " << machinesDir << "\n";
    return 2;
  }

  // Recursive, because the corpus is path-aware. `machines/` holds only `core/`
  // and `domains/<name>/` — no top-level *.json at all — so a non-recursive
  // iterator found nothing here and the suite asserted against an empty corpus
  // (#42). Every engine loader already traverses recursively
  // (reality.cpp:1924, reality_engine_server.cpp:34); these tests did not
  // follow when the corpus moved into subdirectories.
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(machinesDir)) {
    if (entry.path().extension() == ".json") files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());

  // Discovery is asserted here rather than only through the exercise counts at
  // the end. Those catch a corpus that is entirely missing, which is how #42
  // surfaced; they do not catch a traversal that reaches some of it. Requiring
  // a find below the root fails a regression to a non-recursive iterator
  // directly, and says which of the two went wrong.
  if (files.empty()) {
    std::cerr << "No machine JSON found under " << machinesDir << "\n";
    return 2;
  }
  const bool foundBelowRoot = std::any_of(
      files.begin(), files.end(),
      [&](const std::filesystem::path& p) { return p.parent_path() != machinesDir; });
  if (!foundBelowRoot) {
    std::cerr << "Corpus traversal found no machines below " << machinesDir
              << " — the corpus is organised into domain subdirectories, so this"
                 " means the walk is not recursive\n";
    return 2;
  }

  std::map<std::string, std::vector<DomainCase>> casesByDomain;
  std::vector<std::string> failures;

  for (const auto& file : files) {
    try {
      std::string raw = read_file(file);
      Json root = json::parse(raw);
      const Json& machineJson = root.at("machine");
      std::string category = machineJson.at("metadata").at("category").as_string("uncategorized");
      const Json& inputSequences = machineJson.at("inputSequences");
      if (!inputSequences.is_array() || inputSequences.array().empty()) continue;
      for (const auto& sequence : inputSequences.array()) {
        casesByDomain[category].push_back({category, file, sequence, raw});
      }
    } catch (const std::exception& e) {
      failures.push_back(file.filename().string() + ": " + e.what());
    }
  }

  int domainsRun = 0;
  int casesRun = 0;
  int stepsRun = 0;
  int outputsObserved = 0;

  for (const auto& [domain, domainCases] : casesByDomain) {
    if (domainCases.empty()) {
      failures.push_back(domain + ": no runnable input sequences");
      continue;
    }

    ++domainsRun;
    for (int i = 0; i < kCasesPerDomain; ++i) {
      const auto& dc = domainCases[static_cast<size_t>(i) % domainCases.size()];
      try {
        CaseResult r = run_domain_case(dc, i);
        ++casesRun;
        stepsRun += r.steps;
        outputsObserved += r.outputs;
      } catch (const std::exception& e) {
        failures.push_back(domain + " case " + std::to_string(i + 1) + ": " + e.what());
      }
    }
  }

  std::cout << "E2E machine domain summary\n";
  std::cout << "  active domains:        " << domainsRun << "\n";
  std::cout << "  cases per domain:      " << kCasesPerDomain << "\n";
  std::cout << "  domain cases run:      " << casesRun << "\n";
  std::cout << "  input steps run:       " << stepsRun << "\n";
  std::cout << "  outputs observed:      " << outputsObserved << "\n";

  if (!failures.empty()) {
    std::cerr << "\nFailures:\n";
    for (const auto& f : failures) std::cerr << "  - " << f << "\n";
    return 1;
  }

  if (domainsRun == 0 || casesRun != domainsRun * kCasesPerDomain || stepsRun == 0) {
    std::cerr << "E2E domain test did not exercise 10 cases for each active domain\n";
    return 1;
  }

  std::cout << "RealityEngine_CPP domain e2e tests passed\n";
  return 0;
}
