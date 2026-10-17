// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <functional>
#include <vector>

/// @file
/// The N-client convergence check `examples/TESTING.md`'s "Multi-client
/// stress harness" section names: several clients' fingerprints, compared
/// until they agree or the attempt budget runs out.

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
