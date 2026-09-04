// STA checker implementation — see include/reality/sta_checker.hpp.

#include "reality/sta_checker.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace reality::sta {

namespace {

constexpr double DEFAULT_THRESHOLD = 0.5;

int element_state(const json::Value& el) {
  if (!el.is_object()) return 0;
  const auto& v = el.at("value");
  double value = v.is_number() ? v.as_number() : 0.0;
  const auto& t = el.at("threshold");
  double threshold = t.is_number() ? t.as_number() : DEFAULT_THRESHOLD;
  return value >= threshold ? 1 : 0;
}

std::vector<int> vector_state(const json::Value& vec) {
  std::vector<int> out;
  const auto& elements = vec.at("elements");
  if (!elements.is_array()) return out;
  out.reserve(elements.array().size());
  for (const auto& el : elements.array()) out.push_back(element_state(el));
  return out;
}

std::optional<int> hamming_distance(const std::vector<int>& a, const std::vector<int>& b) {
  if (a.size() != b.size()) return std::nullopt;
  int n = 0;
  for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) ++n;
  return n;
}

const json::Value& unwrap_machine(const json::Value& root) {
  // Accept the canonical wrapped form {"machine": {...}} or a bare machine.
  const auto& wrapped = root.at("machine");
  return wrapped.is_object() ? wrapped : root;
}

}  // namespace

StaReport compute_sta(const json::Value& root) {
  const json::Value& m = unwrap_machine(root);
  StaReport report;

  const auto& id = m.at("id");
  const auto& name = m.at("name");
  if (id.is_string())   report.machineId   = id.as_string();
  if (name.is_string()) report.machineName = name.as_string();

  const auto& md = m.at("metadata");
  const auto& severity = md.at("severity");
  report.lifeSafety = severity.is_string() && severity.as_string() == "life-safety";

  const auto& declared = md.at("singleTransitionAssumption");
  if (declared.is_object()) report.declared = declared;

  // Build a machine-wide index from vector id → (sequenceId, vector json).
  // Used to classify transitions as inter-sequence when the target id is not
  // in the current sequence's local index.
  struct IndexEntry { std::string sequenceId; const json::Value* vector; };
  std::unordered_map<std::string, IndexEntry> indexByVector;
  const auto& sequences = m.at("sequences");
  if (sequences.is_array()) {
    for (const auto& seq : sequences.array()) {
      std::string seqId = seq.at("id").as_string();
      const auto& vectors = seq.at("events");
      if (!vectors.is_array()) continue;
      for (const auto& v : vectors.array()) {
        std::string vid = v.at("id").as_string();
        if (!vid.empty()) indexByVector.emplace(vid, IndexEntry{seqId, &v});
      }
    }
  }

  int intraViolations = 0;
  int interJumps = 0;

  if (sequences.is_array()) {
    for (const auto& seq : sequences.array()) {
      StaSequenceReport seqReport;
      seqReport.id   = seq.at("id").as_string();
      seqReport.name = seq.at("name").as_string();

      // Local index — vectors visible to this sequence.
      std::unordered_map<std::string, const json::Value*> localByVector;
      const auto& vectors = seq.at("events");
      if (vectors.is_array()) {
        for (const auto& v : vectors.array()) {
          std::string vid = v.at("id").as_string();
          if (!vid.empty()) localByVector.emplace(vid, &v);
        }

        for (const auto& v : vectors.array()) {
          auto fromState = vector_state(v);
          std::string fromId = v.at("id").as_string();
          const auto& next = v.at("nextEventIds");
          if (!next.is_array()) continue;
          for (const auto& nid : next.array()) {
            if (!nid.is_string()) continue;
            std::string nextId = nid.as_string();

            auto localIt = localByVector.find(nextId);
            if (localIt != localByVector.end()) {
              auto toState = vector_state(*localIt->second);
              auto hd = hamming_distance(fromState, toState);
              if (!hd) {
                StaTransition t;
                t.from = fromId; t.to = nextId; t.kind = "intra";
                t.error = "element-count-mismatch"; t.violation = true;
                seqReport.transitions.push_back(std::move(t));
                seqReport.anyViolation = true;
                ++intraViolations;
                continue;
              }
              seqReport.maxIntraHD = std::max(seqReport.maxIntraHD, *hd);
              StaTransition t;
              t.from = fromId; t.to = nextId; t.kind = "intra";
              t.hd = *hd;
              t.fromState = fromState;
              t.toState = std::move(toState);
              if (*hd > 1) {
                t.violation = true;
                seqReport.anyViolation = true;
                ++intraViolations;
              }
              seqReport.transitions.push_back(std::move(t));
              continue;
            }

            auto globalIt = indexByVector.find(nextId);
            if (globalIt != indexByVector.end()) {
              auto toState = vector_state(*globalIt->second.vector);
              auto hd = hamming_distance(fromState, toState);
              StaTransition t;
              t.from = fromId; t.to = nextId; t.kind = "inter-sequence";
              if (hd) t.hd = *hd;
              t.targetSequenceId = globalIt->second.sequenceId;
              seqReport.transitions.push_back(std::move(t));
              ++interJumps;
              continue;
            }

            // Dangling reference — the named successor does not exist anywhere
            // in the machine.  This is unambiguously a structural defect.
            StaTransition t;
            t.from = fromId; t.to = nextId; t.kind = "dangling";
            t.error = "next vector id not found in machine";
            t.violation = true;
            seqReport.transitions.push_back(std::move(t));
            seqReport.anyViolation = true;
            ++intraViolations;
          }
        }
      }
      report.sequences.push_back(std::move(seqReport));
    }
  }

  report.intraViolations = intraViolations;
  report.interJumps      = interJumps;

  // Drift detection — when metadata.singleTransitionAssumption declares
  // `compliant` / `maxHammingDistanceIntraSequence`, compare against the
  // computed values.  Mirrors the AI logic verbatim.
  if (report.declared) {
    const auto& d = *report.declared;
    std::ostringstream drift;
    bool any = false;
    const auto& compliantJ = d.at("compliant");
    if (compliantJ.is_bool()) {
      bool declaredCompliant = compliantJ.as_bool();
      bool computedCompliant = intraViolations == 0;
      if (declaredCompliant != computedCompliant) {
        drift << "declared compliant=" << (declaredCompliant ? "true" : "false")
              << " but computed compliant=" << (computedCompliant ? "true" : "false");
        any = true;
      }
    }
    const auto& mhd = d.at("maxHammingDistanceIntraSequence");
    if (mhd.is_number()) {
      int declaredMax = static_cast<int>(mhd.as_number());
      int computedMax = 0;
      for (const auto& s : report.sequences) computedMax = std::max(computedMax, s.maxIntraHD);
      if (declaredMax != computedMax) {
        if (any) drift << "; ";
        drift << "declared maxHammingDistanceIntraSequence=" << declaredMax
              << " but computed=" << computedMax;
        any = true;
      }
    }
    if (any) report.drift = drift.str();
  }

  return report;
}

StaReport compute_sta(const std::string& raw_json) {
  return compute_sta(json::parse(raw_json));
}

StaViolationError::StaViolationError(StaReport r)
    : std::runtime_error([&] {
        std::ostringstream msg;
        msg << "STA violation in life-safety machine \""
            << (r.machineName.empty() ? r.machineId : r.machineName)
            << "\": " << r.intraViolations
            << " intra-sequence transition(s) with HD>1.";
        for (const auto& seq : r.sequences) {
          for (const auto& t : seq.transitions) {
            if (t.violation || !t.error.empty()) {
              msg << "\n  - " << seq.id << ": " << t.from << " -> " << t.to
                  << " (HD=" << (t.hd ? std::to_string(*t.hd) : std::string("null"));
              if (!t.error.empty()) msg << ", " << t.error;
              msg << ")";
            }
          }
        }
        return msg.str();
      }()),
      report(std::move(r)) {}

StaReport assert_sta_for_life_safety(const json::Value& root) {
  auto report = compute_sta(root);
  if (report.lifeSafety && report.intraViolations > 0) {
    throw StaViolationError(report);
  }
  return report;
}

StaReport assert_sta_for_life_safety(const std::string& raw_json) {
  return assert_sta_for_life_safety(json::parse(raw_json));
}

}  // namespace reality::sta
