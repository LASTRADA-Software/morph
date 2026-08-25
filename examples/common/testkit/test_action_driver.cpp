// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "testkit/action_driver.hpp"

namespace {

/// @brief Sets an environment variable for this scope, restoring whatever
///        was there before (or unsetting it, if it was previously unset) on
///        destruction. Cross-platform (`_putenv_s` on Windows, `setenv`/
///        `unsetenv` elsewhere) since `std::setenv` itself isn't portable.
class ScopedEnvVar {
public:
    ScopedEnvVar(std::string name, const std::string& value) : _name{std::move(name)} {
        if (const char* existing = std::getenv(_name.c_str()); existing != nullptr) {
            _previous = existing;
        }
        setEnv(value);
    }

    ~ScopedEnvVar() {
        if (_previous.has_value()) {
            setEnv(*_previous);
        } else {
            unsetEnv();
        }
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;
    ScopedEnvVar(ScopedEnvVar&&) = delete;
    ScopedEnvVar& operator=(ScopedEnvVar&&) = delete;

private:
    void setEnv(const std::string& value) const {
#ifdef _WIN32
        _putenv_s(_name.c_str(), value.c_str());
#else
        setenv(_name.c_str(), value.c_str(), /*overwrite=*/1);
#endif
    }

    void unsetEnv() const {
#ifdef _WIN32
        _putenv_s(_name.c_str(), "");
#else
        unsetenv(_name.c_str());
#endif
    }

    std::string _name;
    std::optional<std::string> _previous;
};

}  // namespace

TEST_CASE("SeededScript generates the requested count and calls the invariant hook after every burst",
          "[testkit][action_driver]") {
    using morph::ladder::testkit::SeededScript;

    int invariantCalls = 0;
    std::vector<int> generated;

    SeededScript<int> script{/*seed=*/12345,
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
        return SeededScript<int>{/*seed=*/999, /*generators=*/{{1, [] { return 10; }}, {2, [] { return 20; }}},
                                 /*burstSize=*/3,
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

TEST_CASE("SeededScript::flushBurst() invokes onBurst for a genuinely partial final burst",
          "[testkit][action_driver]") {
    // The prior TEST_CASE's 15-actions/burstSize-5 script never leaves a
    // remainder for flushBurst() to flush -- its onBurst already fired
    // exactly on every burstSize boundary, so flushBurst()'s own non-empty
    // branch (the one that matters: a real caller stopping mid-burst) was
    // never exercised. This picks a count that doesn't divide evenly.
    using morph::ladder::testkit::SeededScript;

    int invariantCalls = 0;
    std::vector<std::size_t> burstSizesSeen;
    std::vector<int> generated;

    SeededScript<int> script{/*seed=*/42,
                             /*generators=*/{{1, [] { return 7; }}},
                             /*burstSize=*/5,
                             /*onBurst=*/[&](const std::vector<int>& burst) {
                                 ++invariantCalls;
                                 burstSizesSeen.push_back(burst.size());
                             }};

    for (int i = 0; i < 12; ++i) {
        generated.push_back(script.next());
    }
    // 12 actions / burstSize 5 -> 2 full bursts already fired inside next();
    // 2 actions remain unflushed at this point.
    CHECK(invariantCalls == 2);

    script.flushBurst();
    REQUIRE(invariantCalls == 3);
    CHECK(burstSizesSeen.back() == 2);

    // A second flushBurst() with nothing pending must not fire onBurst again
    // -- confirms the "if (!_burst.empty())" guard, not just its true arm.
    script.flushBurst();
    CHECK(invariantCalls == 3);
}

TEST_CASE("SeededScript reads its seed from MORPH_STRESS_SEED when set, ignoring the caller's default",
          "[testkit][action_driver]") {
    using morph::ladder::testkit::SeededScript;

    const ScopedEnvVar envOverride{"MORPH_STRESS_SEED", "424242"};

    SeededScript<int> overridden{/*seed=*/1, /*generators=*/{{1, [] { return 0; }}}, /*burstSize=*/1,
                                 /*onBurst=*/[](const std::vector<int>&) {}};
    CHECK(overridden.seed() == 424242);
}

TEST_CASE("SeededScript falls back to the caller's default when MORPH_STRESS_SEED is set but empty",
          "[testkit][action_driver]") {
    // resolveSeed()'s guard is `env != nullptr && *env != '\0'`. The
    // TEST_CASE above covers a set, non-empty value; an unset variable is
    // covered by every other case in this file. Neither reaches the second
    // conjunct's false arm -- a variable that is *present but empty*, which
    // an `export MORPH_STRESS_SEED=` in a CI shell produces easily. Without
    // the emptiness check that value would reach std::stoull(""), which
    // throws std::invalid_argument rather than falling back.
    using morph::ladder::testkit::SeededScript;

    const ScopedEnvVar emptyOverride{"MORPH_STRESS_SEED", ""};

    SeededScript<int> script{/*seed=*/7'777, /*generators=*/{{1, [] { return 0; }}}, /*burstSize=*/1,
                             /*onBurst=*/[](const std::vector<int>&) {}};
    CHECK(script.seed() == 7'777);
}

TEST_CASE("SeededScript can pick the last generator, which absorbs the leftover weight", "[testkit][action_driver]") {
    // next() walks only the first N-1 weight ranges and lets the final
    // generator absorb the remainder, so "the loop ran to completion" is the
    // ordinary last-generator-won path. Weighting the last entry heavily
    // makes both outcomes -- an early break and a run to completion -- occur
    // within a single script, so neither arm of that loop goes unexercised.
    using morph::ladder::testkit::SeededScript;

    std::vector<int> generated;
    SeededScript<int> script{/*seed=*/2'024,
                             /*generators=*/{{1, [] { return 100; }}, {9, [] { return 200; }}},
                             /*burstSize=*/100,
                             /*onBurst=*/[](const std::vector<int>&) {}};

    for (int i = 0; i < 60; ++i) {
        generated.push_back(script.next());
    }

    // Both generators must actually have been selected, or this test would
    // silently stop covering one of the two paths it exists to cover.
    CHECK(std::count(generated.begin(), generated.end(), 100) > 0);
    CHECK(std::count(generated.begin(), generated.end(), 200) > 0);
    for (int v : generated) {
        CHECK((v == 100 || v == 200));
    }
}
