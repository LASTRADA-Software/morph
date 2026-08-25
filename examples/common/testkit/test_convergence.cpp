// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "testkit/convergence.hpp"

TEST_CASE("assertConverged succeeds once every fingerprint agrees", "[testkit][convergence]") {
    std::vector<std::string> fingerprints{"a", "a", "a"};
    int calls = 0;
    auto poll = [&]() -> std::vector<std::string> {
        ++calls;
        return fingerprints;
    };
    CHECK(morph::ladder::testkit::pollUntilConverged(poll, /*maxAttempts=*/5));
    CHECK(calls == 1);
}

TEST_CASE("pollUntilConverged retries until fingerprints agree, then gives up after maxAttempts",
          "[testkit][convergence]") {
    int calls = 0;
    auto poll = [&]() -> std::vector<std::string> {
        ++calls;
        if (calls < 3) {
            return {"a", "b", "a"};  // disagreement
        }
        return {"a", "a", "a"};
    };
    CHECK(morph::ladder::testkit::pollUntilConverged(poll, /*maxAttempts=*/5));
    CHECK(calls == 3);

    int failCalls = 0;
    auto neverConverges = [&]() -> std::vector<std::string> {
        ++failCalls;
        return {"a", "b"};
    };
    CHECK_FALSE(morph::ladder::testkit::pollUntilConverged(neverConverges, /*maxAttempts=*/3));
    CHECK(failCalls == 3);
}

TEST_CASE("pollUntilConverged treats an empty fingerprint set as inconclusive and keeps polling",
          "[testkit][convergence]") {
    // A fetch function that returns no fingerprints at all (e.g. every
    // client has already deregistered, or the fetch raced ahead of any
    // client attaching) must not be treated as "converged" -- an empty set
    // vacuously satisfies std::all_of, so this branch exists specifically to
    // reject that false positive and keep polling instead.
    int calls = 0;
    auto emptyThenConverges = [&]() -> std::vector<std::string> {
        ++calls;
        if (calls < 3) {
            return {};
        }
        return {"a", "a"};
    };
    CHECK(morph::ladder::testkit::pollUntilConverged(emptyThenConverges, /*maxAttempts=*/5));
    CHECK(calls == 3);

    int alwaysEmptyCalls = 0;
    auto alwaysEmpty = [&]() -> std::vector<std::string> {
        ++alwaysEmptyCalls;
        return {};
    };
    CHECK_FALSE(morph::ladder::testkit::pollUntilConverged(alwaysEmpty, /*maxAttempts=*/3));
    CHECK(alwaysEmptyCalls == 3);
}
