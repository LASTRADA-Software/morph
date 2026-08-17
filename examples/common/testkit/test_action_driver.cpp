// SPDX-License-Identifier: Apache-2.0
#include "testkit/action_driver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_CASE("SeededScript generates the requested count and calls the invariant hook after every burst",
          "[testkit][action_driver]") {
    using morph::ladder::testkit::SeededScript;

    int invariantCalls = 0;
    std::vector<int> generated;

    SeededScript<int> script{
        /*seed=*/12345,
        /*generators=*/{{1, [] { return 1; }}, {1, [] { return 2; }}},
        /*burstSize=*/5,
        /*onBurst=*/[&](const std::vector<int>& burst) {
            ++invariantCalls;
            CHECK(burst.size() == 5);
        }};

    for (int i = 0; i < 15; ++i) {
        generated.push_back(script.next());
    }
    script.flushBurst();

    CHECK(generated.size() == 15);
    CHECK(invariantCalls == 3);
    for (int v : generated) {
        CHECK((v == 1 || v == 2));
    }
}

TEST_CASE("SeededScript is deterministic for a fixed seed", "[testkit][action_driver]") {
    using morph::ladder::testkit::SeededScript;
    auto make = [] {
        return SeededScript<int>{
            /*seed=*/999, /*generators=*/{{1, [] { return 10; }}, {2, [] { return 20; }}}, /*burstSize=*/3,
            /*onBurst=*/[](const std::vector<int>&) {}};
    };
    auto a = make();
    auto b = make();
    std::vector<int> seqA, seqB;
    for (int i = 0; i < 9; ++i) {
        seqA.push_back(a.next());
        seqB.push_back(b.next());
    }
    CHECK(seqA == seqB);
}
