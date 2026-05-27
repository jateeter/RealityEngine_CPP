#include "reality/json.hpp"
#include "reality/reality.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

using namespace reality;

namespace {

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

CaseResult run_input_sequence(const std::filesystem::path& file, const Json& seqJson, const std::string& raw) {
  std::string stem = file.stem().string();
  Machine machine = load_machine_from_json_string(raw, "machine-" + stem);
  machine.reset();

  CaseResult result;
  result.machineFile = file.filename().string();
  result.sequenceName = seqJson.at("name").as_string(seqJson.at("id").as_string("unnamed"));

  std::vector<Vector> actualOutputs;
  const auto& vectorsJson = seqJson.at("vectors");
  if (!vectorsJson.is_array()) throw std::runtime_error(result.machineFile + " / " + result.sequenceName + ": input sequence has no vectors array");

  for (const auto& vj : vectorsJson.array()) {
    Vector input = json::to_numbers(vj);
    if (input.empty()) throw std::runtime_error(result.machineFile + " / " + result.sequenceName + ": empty input vector");
    auto transition = machine.process_input(input);
    if (transition.machineOutput) actualOutputs.push_back(transition.machineOutput->vector);
    ++result.steps;
  }
  result.outputs = static_cast<int>(actualOutputs.size());

  const Json& meta = seqJson.at("metadata");
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

  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(machinesDir)) {
    if (entry.path().extension() == ".json") files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());

  int machinesLoaded = 0;
  int sequencesRun = 0;
  int stepsRun = 0;
  int outputsObserved = 0;
  int metadataAssertions = 0;
  std::vector<std::string> failures;

  for (const auto& file : files) {
    try {
      std::string raw = read_file(file);
      (void)load_machine_from_json_string(raw, "machine-" + file.stem().string());
      ++machinesLoaded;

      Json root = json::parse(raw);
      const Json& inputSequences = root.at("machine").at("inputSequences");
      if (!inputSequences.is_array()) continue;

      for (const auto& seqJson : inputSequences.array()) {
        try {
          CaseResult r = run_input_sequence(file, seqJson, raw);
          ++sequencesRun;
          stepsRun += r.steps;
          outputsObserved += r.outputs;
          const Json& meta = seqJson.at("metadata");
          if (exact_expected_count_from_metadata(meta) >= 0 ||
              minimum_expected_count_from_metadata(meta) >= 0 ||
              !expected_vectors_from_metadata(meta).empty()) ++metadataAssertions;
        } catch (const std::exception& e) {
          failures.push_back(e.what());
        }
      }
    } catch (const std::exception& e) {
      failures.push_back(file.filename().string() + ": " + e.what());
    }
  }

  std::cout << "E2E machine sequence summary\n";
  std::cout << "  machines loaded:       " << machinesLoaded << "\n";
  std::cout << "  input sequences run:   " << sequencesRun << "\n";
  std::cout << "  input steps run:       " << stepsRun << "\n";
  std::cout << "  outputs observed:      " << outputsObserved << "\n";
  std::cout << "  metadata assertions:   " << metadataAssertions << "\n";

  if (!failures.empty()) {
    std::cerr << "\nFailures:\n";
    for (const auto& f : failures) std::cerr << "  - " << f << "\n";
    return 1;
  }

  if (machinesLoaded == 0 || sequencesRun == 0 || stepsRun == 0) {
    std::cerr << "E2E test did not exercise machines, sequences, and steps\n";
    return 1;
  }

  std::cout << "RealityEngine_CPP E2E machine sequence tests passed\n";
  return 0;
}
