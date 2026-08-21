#pragma once
// vector_aggregator.hpp — PE machine output aggregator
//
// Merges gated machine CES output vectors from RE's SimulationStep.machineResults
// into the base perceptual space vector to produce the next InputSpaceVector.
//
// Gating:      only machines whose transitionResult.arbiterMetadata.shouldOutput
//              is true contribute to the merge.
// Merge order: deterministic — std::map<machineId,...> iteration is already
//              lexicographically sorted, so no explicit sort is required.
//
// This module is intentionally a thin, stateless function so the aggregation
// restriction (all machine outputs must be present before the next input vector
// is assembled) can be relaxed in the future without changing the call sites.

#include "reality/json.hpp"
#include <algorithm>
#include <string>
#include <vector>

namespace reality::aggregator {

// Merge gated machine CES output vectors into a copy of `base` and return the
// merged nextInputSpaceVector.  `machine_results` is the `machineResults` field
// from RE's SimulationStep JSON response.
inline std::vector<double> aggregate_machine_outputs(
    std::vector<double>       base,
    const reality::json::Value& machine_results)
{
    if (!machine_results.is_object()) return base;

    // Merge order must be identical on every runtime, not merely stable here.
    // Object keys are machineIds and std::map keeps them sorted, which made this
    // deterministic within one process and different between processes: the
    // corpus declares no id, so each runtime mints its own. Where two machines'
    // output regions overlap the write below is last-writer-wins, so the winner
    // was decided by an id that differs per engine and the merged vector — the
    // next InputSpaceVector — diverged. Seen on AgHarvestReadinessAssessor,
    // whose output [3967:3971] overlaps AGX055's [3959:3971]: ISRE cell 3968
    // read 1.0 here and 0.0 on Scala while every OREV agreed
    // (RealityEngine_CI corpus parity sweep, 2026-08-19).
    //
    // machineName is corpus-declared and globally unique across the corpus, so
    // ordering by it is the same everywhere. Results are collected first, then
    // sorted by that name, rather than relying on the container's key order.
    struct MergeRecord {
        std::string sort_key;
        int offset;
        int write_len;
        std::vector<double> vec;
    };
    std::vector<MergeRecord> records;

    for (const auto& [machine_id, result] : machine_results.object()) {
        // Gate: transitionResult.arbiterMetadata.shouldOutput must be true
        const auto& transition = result.at("transitionResult");
        if (!transition.is_object()) continue;
        const auto& arb = transition.at("arbiterMetadata");
        if (!arb.is_object() || !arb.at("shouldOutput").as_bool(false)) continue;

        // outputRegion: { offset, length }
        const auto& region = result.at("outputRegion");
        if (!region.is_object()) continue;
        const int offset = static_cast<int>(region.at("offset").as_number(-1));
        const int length = static_cast<int>(region.at("length").as_number(0));
        if (offset < 0 || length <= 0) continue;

        // Prefer mergedOutputVector: the machine's collection of potential
        // outputs folded under its own outputMergeTransformation. Presenting
        // that fold is the Reality Engine's job and the last thing it does in
        // the step, so it is the machine's actual output.
        //
        // outputVector is one arbitrarily chosen member of that collection, and
        // which member differed per runtime — reading it here is what carried
        // the RE's disagreement into the perceptual space
        // (RealityEngine_CI#154). Falling back to it keeps this working against
        // a Reality Engine that has not yet been updated.
        const auto& merged_vec = result.at("mergedOutputVector");
        const auto& out_vec = merged_vec.is_array() ? merged_vec : result.at("outputVector");
        if (!out_vec.is_array()) continue;
        std::vector<double> vec = reality::json::to_numbers(out_vec);
        if (vec.empty()) continue;

        // Fall back to the id only when a result carries no name, which keeps a
        // malformed payload ordered rather than unordered.
        std::string name = result.at("machineName").as_string("");
        if (name.empty()) name = machine_id;

        records.push_back(MergeRecord{std::move(name), offset,
                                      std::min(static_cast<int>(vec.size()), length),
                                      std::move(vec)});
    }

    std::sort(records.begin(), records.end(),
              [](const MergeRecord& a, const MergeRecord& b) { return a.sort_key < b.sort_key; });

    for (const auto& rec : records) {
        // Grow base to accommodate the output region if needed
        const int needed = rec.offset + rec.write_len;
        if (needed > static_cast<int>(base.size()))
            base.resize(needed, 0.0);

        // Write into region — unconditional (zeros clear stale values)
        for (int i = 0; i < rec.write_len; ++i)
            base[rec.offset + i] = rec.vec[i];
    }
    return base;
}

} // namespace reality::aggregator
