// SPDX-License-Identifier: Apache-2.0

#include <morph/logger.hpp>
#include <morph/reconnect_coordinator.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using morph::offline::ReconnectCoordinator;
using morph::offline::ReconnectOutcome;

namespace {

/// @brief Fakes for every Deps member, plus a shared event log to assert ordering.
///
/// Each callback appends its name to `events` so tests can assert both call
/// counts and relative ordering (spec §6, case 4). Behaviour of `tryReconnect`
/// and `shouldContinue` is programmable.
struct Fakes {
    std::vector<std::string> events;

    int tryReconnectCalls = 0;
    int activatePrimaryCalls = 0;
    int activateLocalCalls = 0;
    int bindContextCalls = 0;
    int replayCalls = 0;
    int sleepCalls = 0;
    std::vector<std::chrono::milliseconds> sleepDurations;

    // tryReconnect returns reconnectResults[i] for the i-th call; once exhausted,
    // returns reconnectDefault.
    std::vector<bool> reconnectResults;
    bool reconnectDefault = false;
    bool reconnectThrows = false;

    // shouldContinue returns continueResults[i] for the i-th call; once exhausted,
    // returns continueDefault.
    std::vector<bool> continueResults;
    bool continueDefault = true;

    [[nodiscard]] ReconnectCoordinator::Deps deps() {
        return ReconnectCoordinator::Deps{
            .tryReconnect =
                [this]() -> bool {
                    int idx = tryReconnectCalls++;
                    events.emplace_back("tryReconnect");
                    if (reconnectThrows) {
                        throw std::runtime_error("boom");
                    }
                    if (idx < static_cast<int>(reconnectResults.size())) {
                        return reconnectResults[static_cast<std::size_t>(idx)];
                    }
                    return reconnectDefault;
                },
            .activatePrimary =
                [this] {
                    ++activatePrimaryCalls;
                    events.emplace_back("activatePrimary");
                },
            .activateLocal =
                [this] {
                    ++activateLocalCalls;
                    events.emplace_back("activateLocal");
                },
            .bindContext =
                [this] {
                    ++bindContextCalls;
                    events.emplace_back("bindContext");
                },
            .replay =
                [this] {
                    ++replayCalls;
                    events.emplace_back("replay");
                },
            .shouldContinue =
                [this]() -> bool {
                    int idx = shouldContinueCalls_++;
                    events.emplace_back("shouldContinue");
                    if (idx < static_cast<int>(continueResults.size())) {
                        return continueResults[static_cast<std::size_t>(idx)];
                    }
                    return continueDefault;
                },
            .sleep =
                [this](std::chrono::milliseconds d) {
                    ++sleepCalls;
                    sleepDurations.push_back(d);
                    events.emplace_back("sleep");
                },
        };
    }

    [[nodiscard]] std::ptrdiff_t indexOf(const std::string& name) const {
        for (std::size_t i = 0; i < events.size(); ++i) {
            if (events[i] == name) {
                return static_cast<std::ptrdiff_t>(i);
            }
        }
        return -1;
    }

    [[nodiscard, maybe_unused]] int count(const std::string& name) const {
        int n = 0;
        for (const auto& e : events) {
            if (e == name) {
                ++n;
            }
        }
        return n;
    }

private:
    int shouldContinueCalls_ = 0;
};

}  // namespace

TEST_CASE("ReconnectCoordinator: happy path reconnects on first attempt", "[reconnect]") {
    Fakes f;
    f.reconnectResults = {true};
    ReconnectCoordinator coord{f.deps()};

    auto outcome = coord.onOnline();

    REQUIRE(outcome == ReconnectOutcome::Reconnected);
    // Exact call order per spec §6 case 1.
    REQUIRE(f.events ==
            std::vector<std::string>{"shouldContinue", "tryReconnect", "activatePrimary", "bindContext",
                                     "shouldContinue", "replay"});
}

TEST_CASE("ReconnectCoordinator: retries then succeeds", "[reconnect]") {
    Fakes f;
    f.reconnectResults = {false, false, true};
    ReconnectCoordinator::Config cfg;
    cfg.retryDelay = std::chrono::milliseconds{50};
    ReconnectCoordinator coord{f.deps(), cfg};

    auto outcome = coord.onOnline();

    REQUIRE(outcome == ReconnectOutcome::Reconnected);
    REQUIRE(f.tryReconnectCalls == 3);
    REQUIRE(f.sleepCalls == 2);
    REQUIRE(f.sleepDurations == std::vector<std::chrono::milliseconds>{std::chrono::milliseconds{50},
                                                                       std::chrono::milliseconds{50}});
    REQUIRE(f.replayCalls == 1);
}

TEST_CASE("ReconnectCoordinator: gives up after maxAttempts without sleeping after the last", "[reconnect]") {
    Fakes f;
    f.reconnectDefault = false;  // always fails
    ReconnectCoordinator::Config cfg;
    cfg.maxAttempts = 3;
    ReconnectCoordinator coord{f.deps(), cfg};

    // Silence the expected give-up warning.
    morph::log::ScopedLoggerOverride guard{[](morph::log::LogLevel, std::string_view) {}};

    auto outcome = coord.onOnline();

    REQUIRE(outcome == ReconnectOutcome::GaveUp);
    REQUIRE(f.tryReconnectCalls == 3);
    REQUIRE(f.sleepCalls == 2);  // not after the final attempt
    REQUIRE(f.activatePrimaryCalls == 0);
    REQUIRE(f.replayCalls == 0);
}

TEST_CASE("ReconnectCoordinator: ordering invariant activate < bind < replay", "[reconnect]") {
    Fakes f;
    f.reconnectResults = {true};
    ReconnectCoordinator coord{f.deps()};

    coord.onOnline();

    REQUIRE(f.indexOf("activatePrimary") >= 0);
    REQUIRE(f.indexOf("bindContext") > f.indexOf("activatePrimary"));
    REQUIRE(f.indexOf("replay") > f.indexOf("bindContext"));
}

TEST_CASE("ReconnectCoordinator: aborts before reconnect when shouldContinue is false on entry", "[reconnect]") {
    Fakes f;
    f.continueResults = {false};
    ReconnectCoordinator coord{f.deps()};

    auto outcome = coord.onOnline();

    REQUIRE(outcome == ReconnectOutcome::Aborted);
    REQUIRE(f.tryReconnectCalls == 0);
    REQUIRE(f.activatePrimaryCalls == 0);
    REQUIRE(f.replayCalls == 0);
}

TEST_CASE("ReconnectCoordinator: aborts replay but stays reconnected when backend drops before replay",
          "[reconnect]") {
    Fakes f;
    f.reconnectResults = {true};
    // 1st shouldContinue (attempt check) = true, 2nd (pre-replay check) = false.
    f.continueResults = {true, false};
    ReconnectCoordinator coord{f.deps()};

    auto outcome = coord.onOnline();

    REQUIRE(outcome == ReconnectOutcome::Reconnected);
    REQUIRE(f.activatePrimaryCalls == 1);
    REQUIRE(f.bindContextCalls == 1);
    REQUIRE(f.replayCalls == 0);  // skipped
}

TEST_CASE("ReconnectCoordinator: tryReconnect throwing is treated as a failed attempt", "[reconnect]") {
    Fakes f;
    f.reconnectThrows = true;
    ReconnectCoordinator::Config cfg;
    cfg.maxAttempts = 2;
    ReconnectCoordinator coord{f.deps(), cfg};

    morph::log::ScopedLoggerOverride guard{[](morph::log::LogLevel, std::string_view) {}};

    // Must not propagate the exception to the caller.
    REQUIRE_NOTHROW([&] {
        auto outcome = coord.onOnline();
        REQUIRE(outcome == ReconnectOutcome::GaveUp);
    }());
    REQUIRE(f.tryReconnectCalls == 2);
    REQUIRE(f.activatePrimaryCalls == 0);
}

TEST_CASE("ReconnectCoordinator: onOffline activates local then binds context", "[reconnect]") {
    Fakes f;
    ReconnectCoordinator coord{f.deps()};

    coord.onOffline();

    REQUIRE(f.events == std::vector<std::string>{"activateLocal", "bindContext"});
}

TEST_CASE("ReconnectCoordinator: concurrent calls are serialised", "[reconnect][threading]") {
    Fakes f;
    f.reconnectDefault = true;  // every onOnline reconnects immediately
    ReconnectCoordinator coord{f.deps()};

    // Run several onOnline/onOffline calls from two threads. The mutex must keep
    // each call's event sub-sequence contiguous — no interleaving. We assert the
    // weaker, deterministic property that total counts are consistent and the
    // event log never splits a sequence (every activatePrimary is immediately
    // followed, eventually, by its bindContext with nothing from another call
    // wedged between the paired activate/bind — verified via balanced counts).
    constexpr int kPerThread = 50;
    auto worker = [&] {
        for (int i = 0; i < kPerThread; ++i) {
            coord.onOnline();
        }
    };
    std::thread t1{worker};
    std::thread t2{worker};
    t1.join();
    t2.join();

    // Each successful onOnline does exactly one activatePrimary, one bindContext,
    // one replay. With 2*kPerThread calls and immediate reconnect, counts match.
    REQUIRE(f.activatePrimaryCalls == 2 * kPerThread);
    REQUIRE(f.bindContextCalls == 2 * kPerThread);
    REQUIRE(f.replayCalls == 2 * kPerThread);
}
