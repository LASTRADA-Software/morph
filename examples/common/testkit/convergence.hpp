// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <functional>
#include <vector>

/// @file
/// The N-client convergence assertion `examples/TESTING.md` names as rung
/// 3's obligation but polls never built (design spec §6) -- absorbed into
/// rung 4's own scope, since kanban's "two clients' queues replaying
/// interleaved" DoD item needs it regardless of original ownership.

namespace morph::ladder::testkit {

/// @brief Polls @p fetchFingerprints up to @p maxAttempts times, returning
///        `true` as soon as every returned fingerprint is equal.
/// @param fetchFingerprints Called once per attempt; returns one
///        fingerprint string per client.
/// @param maxAttempts Number of attempts before giving up.
/// @return `true` if convergence was observed; `false` if `maxAttempts`
///         was exhausted without every fingerprint agreeing.
template <typename FetchFn>
[[nodiscard]] bool pollUntilConverged(FetchFn fetchFingerprints, int maxAttempts) {
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        auto fingerprints = fetchFingerprints();
        if (fingerprints.empty()) {
            continue;
        }
        const auto& first = fingerprints.front();
        if (std::all_of(fingerprints.begin(), fingerprints.end(), [&](const auto& f) { return f == first; })) {
            return true;
        }
    }
    return false;
}

}  // namespace morph::ladder::testkit
