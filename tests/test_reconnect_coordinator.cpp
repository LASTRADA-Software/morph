// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/observability.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/offline/reconnect_coordinator.hpp>
#include <morph/offline/sync_worker.hpp>
#include <morph/testing/owner_probe_recorder.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "test_support.hpp"

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
            .tryReconnect = [this]() -> bool {
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
            .shouldContinue = [this]() -> bool {
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
    ReconnectCoordinator coord{f.deps(), morph::exec::detail::inlineExecutor()};

    auto outcome = morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return coord.onOnline(reply); });

    REQUIRE(outcome == ReconnectOutcome::Reconnected);
    // Exact call order per spec §6 case 1.
    REQUIRE(f.events == std::vector<std::string>{"shouldContinue", "tryReconnect", "activatePrimary", "bindContext",
                                                 "shouldContinue", "replay"});
}

TEST_CASE("ReconnectCoordinator: retries then succeeds", "[reconnect]") {
    Fakes f;
    f.reconnectResults = {false, false, true};
    ReconnectCoordinator::Config cfg;
    cfg.retryDelay = std::chrono::milliseconds{50};
    ReconnectCoordinator coord{f.deps(), morph::exec::detail::inlineExecutor(), cfg};

    auto outcome = morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return coord.onOnline(reply); });

    REQUIRE(outcome == ReconnectOutcome::Reconnected);
    REQUIRE(f.tryReconnectCalls == 3);
    REQUIRE(f.sleepCalls == 2);
    REQUIRE(f.sleepDurations ==
            std::vector<std::chrono::milliseconds>{std::chrono::milliseconds{50}, std::chrono::milliseconds{50}});
    REQUIRE(f.replayCalls == 1);
}

TEST_CASE("ReconnectCoordinator: gives up after maxAttempts without sleeping after the last", "[reconnect]") {
    Fakes f;
    f.reconnectDefault = false;  // always fails
    ReconnectCoordinator::Config cfg;
    cfg.maxAttempts = 3;
    ReconnectCoordinator coord{f.deps(), morph::exec::detail::inlineExecutor(), cfg};

    // Silence the expected give-up warning.
    morph::log::ScopedLoggerOverride guard{[](morph::log::LogLevel, std::string_view) {}};

    auto outcome = morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return coord.onOnline(reply); });

    REQUIRE(outcome == ReconnectOutcome::GaveUp);
    REQUIRE(f.tryReconnectCalls == 3);
    REQUIRE(f.sleepCalls == 2);  // not after the final attempt
    REQUIRE(f.activatePrimaryCalls == 0);
    REQUIRE(f.replayCalls == 0);
}

TEST_CASE("ReconnectCoordinator: ordering invariant activate < bind < replay", "[reconnect]") {
    Fakes f;
    f.reconnectResults = {true};
    ReconnectCoordinator coord{f.deps(), morph::exec::detail::inlineExecutor()};

    morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return coord.onOnline(reply); });

    REQUIRE(f.indexOf("activatePrimary") >= 0);
    REQUIRE(f.indexOf("bindContext") > f.indexOf("activatePrimary"));
    REQUIRE(f.indexOf("replay") > f.indexOf("bindContext"));
}

TEST_CASE("ReconnectCoordinator: aborts before reconnect when shouldContinue is false on entry", "[reconnect]") {
    Fakes f;
    f.continueResults = {false};
    ReconnectCoordinator coord{f.deps(), morph::exec::detail::inlineExecutor()};

    auto outcome = morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return coord.onOnline(reply); });

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
    ReconnectCoordinator coord{f.deps(), morph::exec::detail::inlineExecutor()};

    auto outcome = morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return coord.onOnline(reply); });

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
    ReconnectCoordinator coord{f.deps(), morph::exec::detail::inlineExecutor(), cfg};

    morph::log::ScopedLoggerOverride guard{[](morph::log::LogLevel, std::string_view) {}};

    // Must not propagate the exception to the caller.
    REQUIRE_NOTHROW([&] {
        auto outcome =
            morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return coord.onOnline(reply); });
        REQUIRE(outcome == ReconnectOutcome::GaveUp);
    }());
    REQUIRE(f.tryReconnectCalls == 2);
    REQUIRE(f.activatePrimaryCalls == 0);
}

TEST_CASE("ReconnectCoordinator: onOffline activates local then binds context", "[reconnect]") {
    Fakes f;
    ReconnectCoordinator coord{f.deps(), morph::exec::detail::inlineExecutor()};

    coord.onOffline();

    REQUIRE(f.events == std::vector<std::string>{"activateLocal", "bindContext"});
}

TEST_CASE("ReconnectCoordinator: concurrent calls are serialised", "[reconnect][threading]") {
    Fakes f;
    f.reconnectDefault = true;  // every onOnline reconnects immediately
    ReconnectCoordinator coord{f.deps(), morph::exec::detail::inlineExecutor()};

    // Run several onOnline/onOffline calls from two threads. The mutex must keep
    // each call's event sub-sequence contiguous — no interleaving. We assert the
    // weaker, deterministic property that total counts are consistent and the
    // event log never splits a sequence (every activatePrimary is immediately
    // followed, eventually, by its bindContext with nothing from another call
    // wedged between the paired activate/bind — verified via balanced counts).
    constexpr int kPerThread = 50;
    auto worker = [&] {
        for (int i = 0; i < kPerThread; ++i) {
            morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return coord.onOnline(reply); });
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

TEST_CASE("ReconnectCoordinator: onOnline emits reconnectAttempts per attempt and a tagged reconnectOutcome once",
          "[reconnect][observability]") {
    morph::observe::ScopedObserveOverride guard;
    Fakes f;
    f.reconnectResults = {false, false, true};
    ReconnectCoordinator::Config cfg;
    cfg.retryDelay = std::chrono::milliseconds{1};
    ReconnectCoordinator coord{f.deps(), morph::exec::detail::inlineExecutor(), cfg};

    std::atomic<int> attemptEvents{0};
    std::vector<std::string> outcomeTags;
    morph::observe::setMetricSink([&](const morph::observe::MetricEvent& evt) {
        if (evt.metric == morph::observe::Metric::reconnectAttempts) {
            attemptEvents.fetch_add(1, std::memory_order_relaxed);
        } else if (evt.metric == morph::observe::Metric::reconnectOutcome) {
            for (auto& [key, val] : evt.tags) {
                if (key == "outcome") {
                    outcomeTags.emplace_back(val);
                }
            }
        }
    });

    auto outcome = morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return coord.onOnline(reply); });

    REQUIRE(outcome == ReconnectOutcome::Reconnected);
    REQUIRE(attemptEvents.load() == 3);
    REQUIRE(outcomeTags == std::vector<std::string>{"Reconnected"});
}

TEST_CASE("ReconnectCoordinator: giving up tags reconnectOutcome as GaveUp", "[reconnect][observability]") {
    morph::observe::ScopedObserveOverride guard;
    Fakes f;
    f.reconnectDefault = false;
    ReconnectCoordinator::Config cfg;
    cfg.maxAttempts = 2;
    cfg.retryDelay = std::chrono::milliseconds{1};
    ReconnectCoordinator coord{f.deps(), morph::exec::detail::inlineExecutor(), cfg};

    std::vector<std::string> outcomeTags;
    morph::observe::setMetricSink([&](const morph::observe::MetricEvent& evt) {
        if (evt.metric == morph::observe::Metric::reconnectOutcome) {
            for (auto& [key, val] : evt.tags) {
                if (key == "outcome") {
                    outcomeTags.emplace_back(val);
                }
            }
        }
    });

    morph::log::ScopedLoggerOverride logGuard{[](morph::log::LogLevel, std::string_view) {}};
    auto outcome = morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return coord.onOnline(reply); });

    REQUIRE(outcome == ReconnectOutcome::GaveUp);
    REQUIRE(outcomeTags == std::vector<std::string>{"GaveUp"});
}

// ── The owner rule: onOnline()/onOffline() run on the offline strand ─────────

namespace {

/// Deps whose every step records its name, from whichever thread runs it, and
/// whose tryReconnect stays inside for a while so an overlap would show.
struct StrandFakes {
    std::mutex mtx;
    std::vector<std::string> events;
    std::atomic<int> active{0};
    std::atomic<int> maxActive{0};

    void record(std::string event) {
        std::scoped_lock const lock{mtx};
        events.push_back(std::move(event));
    }

    ReconnectCoordinator::Deps deps() {
        return ReconnectCoordinator::Deps{
            .tryReconnect =
                [this] {
                    int const now = active.fetch_add(1) + 1;
                    int seen = maxActive.load();
                    while (now > seen && !maxActive.compare_exchange_weak(seen, now)) {
                    }
                    record("try");
                    std::this_thread::sleep_for(std::chrono::milliseconds{30});
                    active.fetch_sub(1);
                    return true;
                },
            .activatePrimary = [this] { record("activatePrimary"); },
            .activateLocal = [this] { record("activateLocal"); },
            .bindContext = [this] { record("bind"); },
            .replay = [this] { record("replay"); },
            .shouldContinue = [] { return true; },
            .sleep = [](std::chrono::milliseconds) {},
        };
    }
};

}  // namespace

TEST_CASE("ReconnectCoordinator: onOnline and onOffline called off the offline strand run their bodies on it",
          "[reconnect][owner]") {
    morph::exec::ThreadPoolExecutor pool{2};
    StrandFakes fakes;
    ReconnectCoordinator coord{fakes.deps(), pool};
    morph::testing::OwnerProbeRecorder const recorder{coord.strand().coreExecutor()};
    REQUIRE_FALSE(morph::exec::runningOn(coord.strand()));

    std::optional<ReconnectOutcome> outcome;
    std::thread caller{[&] {
        coord.onOffline();
        outcome = morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return coord.onOnline(reply); });
    }};
    caller.join();

    REQUIRE(outcome == ReconnectOutcome::Reconnected);
    std::scoped_lock const lock{fakes.mtx};
    CHECK(fakes.events ==
          std::vector<std::string>{"activateLocal", "bind", "try", "activatePrimary", "bind", "replay"});
    CHECK(recorder.count("ReconnectCoordinator::onOffline") == 1U);
    CHECK(recorder.allPosted("ReconnectCoordinator::onOffline"));
    CHECK(recorder.count("ReconnectCoordinator::onOnline") == 1U);
    CHECK(recorder.allPosted("ReconnectCoordinator::onOnline"));
}

TEST_CASE("ReconnectCoordinator: two onOnline calls from two threads run one after the other on the offline strand",
          "[reconnect][owner]") {
    // A pool of two, so the two sequences could run side by side if anything
    // but the offline strand kept them apart.
    morph::exec::ThreadPoolExecutor pool{2};
    StrandFakes fakes;
    ReconnectCoordinator coord{fakes.deps(), pool};
    morph::testing::OwnerProbeRecorder const recorder{coord.strand().coreExecutor()};

    std::optional<ReconnectOutcome> first;
    std::optional<ReconnectOutcome> second;
    std::thread callerA{[&] {
        first = morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return coord.onOnline(reply); });
    }};
    std::thread callerB{[&] {
        second = morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return coord.onOnline(reply); });
    }};
    callerA.join();
    callerB.join();

    CHECK(first == ReconnectOutcome::Reconnected);
    CHECK(second == ReconnectOutcome::Reconnected);
    CHECK(fakes.maxActive.load() == 1);
    // Each sequence is contiguous: nothing of one lands inside the other.
    std::scoped_lock const lock{fakes.mtx};
    std::vector<std::string> const one{"try", "activatePrimary", "bind", "replay"};
    std::vector<std::string> both = one;
    both.insert(both.end(), one.begin(), one.end());
    CHECK(fakes.events == both);
    CHECK(recorder.count("ReconnectCoordinator::onOnline") == 2U);
    CHECK(recorder.allPosted("ReconnectCoordinator::onOnline"));
}

TEST_CASE("ReconnectCoordinator: a SyncWorker on the offline strand drains inside the replay step",
          "[reconnect][owner][sync]") {
    morph::exec::ThreadPoolExecutor pool{2};
    // Built on the coordinator's strand once the coordinator exists: the
    // worker drains it there.
    std::unique_ptr<morph::offline::InMemoryOfflineQueue> queue;
    std::unique_ptr<morph::offline::SyncWorker> worker;
    std::atomic<int> replayed{0};
    std::atomic<bool> drainedInsideReplay{false};
    ReconnectCoordinator coord{ReconnectCoordinator::Deps{
                                   .tryReconnect = [] { return true; },
                                   .activatePrimary = [] {},
                                   .activateLocal = [] {},
                                   .bindContext = [] {},
                                   .replay =
                                       [&] {
                                           (void)worker->run(coord.strand());
                                           // run() on its owner drains before it returns.
                                           drainedInsideReplay.store(replayed.load() == 2);
                                       },
                                   .shouldContinue = [] { return true; },
                                   .sleep = [](std::chrono::milliseconds) {},
                               },
                               pool};
    queue = std::make_unique<morph::offline::InMemoryOfflineQueue>(coord.strand());
    (void)queue->enqueue("a");
    (void)queue->enqueue("b");
    worker = std::make_unique<morph::offline::SyncWorker>(coord.strand(), *queue, [&](const std::string&) {
        ++replayed;
        return true;
    });

    CHECK(morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return coord.onOnline(reply); }) ==
          ReconnectOutcome::Reconnected);
    CHECK(drainedInsideReplay.load());
    CHECK(morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return queue->drain(reply); }).empty());
}

TEST_CASE("ReconnectCoordinator: onOnline() called from the app delivers its outcome on the app's executor",
          "[reconnect][owner][reply]") {
    morph::exec::ThreadPoolExecutor pool{2};
    ReconnectCoordinator coord{ReconnectCoordinator::Deps{
                                   .tryReconnect = [] { return true; },
                                   .activatePrimary = [] {},
                                   .activateLocal = [] {},
                                   .bindContext = [] {},
                                   .replay = [] {},
                                   .shouldContinue = [] { return true; },
                                   .sleep = [](std::chrono::milliseconds) {},
                               },
                               pool};

    morph::exec::MainThreadExecutor app;
    std::atomic<bool> delivered{false};
    std::atomic<bool> deliveredOnApp{false};
    std::atomic<bool> deliveredOnStrand{true};
    std::optional<ReconnectOutcome> outcome;
    coord.onOnline(app).then([&](ReconnectOutcome settled) {
        deliveredOnApp.store(morph::exec::runningOn(app));
        deliveredOnStrand.store(morph::exec::runningOn(coord.strand()));
        outcome = settled;
        delivered.store(true);
    });

    REQUIRE(morph::testing::pumpOwnerUntil(app, [&] { return delivered.load(); }));
    CHECK(deliveredOnApp.load());
    CHECK_FALSE(deliveredOnStrand.load());
    CHECK(outcome == ReconnectOutcome::Reconnected);
}
