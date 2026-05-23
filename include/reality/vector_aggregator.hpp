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

    // Object keys (machineId strings) are stored in std::map — already sorted.
    for (const auto& [machine_id, result] : machine_results.object()) {
        (void)machine_id;

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

        // outputVector
        const auto& out_vec = result.at("outputVector");
        if (!out_vec.is_array()) continue;
        const std::vector<double> vec = reality::json::to_numbers(out_vec);
        if (vec.empty()) continue;

        // Grow base to accommodate the output region if needed
        const int write_len = std::min(static_cast<int>(vec.size()), length);
        const int needed    = offset + write_len;
        if (needed > static_cast<int>(base.size()))
            base.resize(needed, 0.0);

        // Write into region — unconditional (zeros clear stale values)
        for (int i = 0; i < write_len; ++i)
            base[offset + i] = vec[i];
    }
    return base;
}

} // namespace reality::aggregator
