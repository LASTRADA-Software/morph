// SPDX-License-Identifier: Apache-2.0
#include "testkit/convergence.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

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

TEST_CASE("pollUntilConverged retries until fingerprints agree, then gives up after maxAttempts", "[testkit][convergence]") {
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
