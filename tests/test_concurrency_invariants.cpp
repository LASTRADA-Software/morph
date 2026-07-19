// SPDX-License-Identifier: Apache-2.0

// Concurrency invariants applied from patterns observed in Bloomberg/quantum's
// test suite. These cover existing morph primitives under conditions previous
// tests did not reach: multi-producer ordering, pool saturation, key-map churn,
// callback executor identity, atomic-once delivery, orphan logging, atomic
// backend swap under load, monitor reentrancy, and mid-replay cancellation.

#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/offline/network_monitor.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/strand.hpp>
#include <morph/offline/sync_worker.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "test_support.hpp"

using namespace std::chrono_literals;

namespace {

using InlineExec = morph::testing::InlineExecutor;
using morph::testing::waitUntil;
using LogGuard = morph::log::ScopedLoggerOverride;

}  // namespace

// ── Strand: per-producer FIFO under multi-thread contention ───────────────────

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("morph::exec::detail::StrandExecutor: per-producer FIFO preserved across many concurrent producers",
          "[strand][concurrency][quantum-parity]") {
    morph::exec::ThreadPoolExecutor pool{4};
    morph::exec::detail::StrandExecutor strand{pool};
    morph::exec::detail::ModelId key{42};

    constexpr int numProducers = 8;
    constexpr int perProducer = 50;
    constexpr int totalTasks = numProducers * perProducer;

    std::vector<std::pair<int, int>> observed;
    std::mutex obsMtx;
    std::atomic<int> done{0};

    std::vector<std::thread> producers;
    producers.reserve(numProducers);
    for (int producerIdx = 0; producerIdx < numProducers; ++producerIdx) {
        producers.emplace_back([&, producerIdx] {
            for (int seq = 0; seq < perProducer; ++seq) {
                strand.post(key, [&, producerIdx, seq] {
                    {
                        std::scoped_lock lock{obsMtx};
                        observed.emplace_back(producerIdx, seq);
                    }
                    done.fetch_add(1);
                });
            }
        });
    }
    for (auto& thr : producers) {
        thr.join();
    }
    REQUIRE(waitUntil([&] { return done.load() == totalTasks; }));

    REQUIRE(observed.size() == static_cast<std::size_t>(totalTasks));
    std::vector<int> nextExpected(static_cast<std::size_t>(numProducers), 0);
    for (auto& [producerIdx, seq] : observed) {
        REQUIRE(seq == nextExpected[static_cast<std::size_t>(producerIdx)]);
        ++nextExpected[static_cast<std::size_t>(producerIdx)];
    }
    for (int count : nextExpected) {
        REQUIRE(count == perProducer);
    }
}

// ── Strand: cross-key concurrency saturates pool ──────────────────────────────

TEST_CASE("morph::exec::detail::StrandExecutor: cross-key concurrency saturates pool size and never exceeds it",
          "[strand][concurrency][quantum-parity]") {
    constexpr std::size_t poolSize = 4;
    morph::exec::ThreadPoolExecutor pool{poolSize};
    morph::exec::detail::StrandExecutor strand{pool};

    constexpr int numKeys = 16;
    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    std::atomic<int> done{0};

    for (int key = 1; key <= numKeys; ++key) {
        strand.post(morph::exec::detail::ModelId{static_cast<uint64_t>(key)}, [&] {
            int now = active.fetch_add(1) + 1;
            int prev = peak.load();
            while (now > prev && !peak.compare_exchange_weak(prev, now)) {
            }
            std::this_thread::sleep_for(40ms);
            active.fetch_sub(1);
            done.fetch_add(1);
        });
    }
    REQUIRE(waitUntil([&] { return done.load() == numKeys; }, 5s));

    REQUIRE(peak.load() <= static_cast<int>(poolSize));
    REQUIRE(peak.load() == static_cast<int>(poolSize));
}

// ── Strand: thousands of distinct keys exercise cleanup path ──────────────────

TEST_CASE("morph::exec::detail::StrandExecutor: churn across thousands of distinct keys completes without deadlock",
          "[strand][churn][quantum-parity]") {
    morph::exec::ThreadPoolExecutor pool{4};
    morph::exec::detail::StrandExecutor strand{pool};

    constexpr int numKeys = 3000;
    std::atomic<int> done{0};

    for (int key = 1; key <= numKeys; ++key) {
        strand.post(morph::exec::detail::ModelId{static_cast<uint64_t>(key)}, [&] { done.fetch_add(1); });
    }
    REQUIRE(waitUntil([&] { return done.load() == numKeys; }, 10s));

    // Post once more after the churn — confirms the cleanup path left the
    // strand executor in a usable state (regression target for map corruption).
    std::atomic<bool> after{false};
    strand.post(morph::exec::detail::ModelId{static_cast<uint64_t>(numKeys + 1)}, [&] { after.store(true); });
    REQUIRE(waitUntil([&] { return after.load(); }));
}

// ── morph::async::Completion: callback runs on cbExec, not the setValue caller ──────────────

TEST_CASE("morph::async::Completion: callback runs on cbExec thread, not the setValue thread",
          "[completion][concurrency][quantum-parity]") {
    morph::exec::ThreadPoolExecutor cbPool{1};
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &cbPool};

    std::atomic<bool> fired{false};
    std::thread::id cbThread{};
    std::mutex idMtx;
    comp.then([&](int) {
        {
            std::scoped_lock lock{idMtx};
            cbThread = std::this_thread::get_id();
        }
        fired.store(true);
    });

    auto producerId = std::this_thread::get_id();
    state->setValue(42);

    REQUIRE(waitUntil([&] { return fired.load(); }));
    std::scoped_lock lock{idMtx};
    REQUIRE(cbThread != producerId);
}

// ── morph::async::Completion: concurrent setValue fires success callback at most once ───────

TEST_CASE("morph::async::Completion: concurrent setValue calls fire the success callback exactly once",
          "[completion][concurrency][quantum-parity]") {
    InlineExec exec;
    auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
    morph::async::Completion<int> comp{state, &exec};

    std::atomic<int> fireCount{0};
    comp.then([&](int) { fireCount.fetch_add(1); });

    constexpr int numSetters = 16;
    std::vector<std::thread> setters;
    setters.reserve(numSetters);
    for (int idx = 0; idx < numSetters; ++idx) {
        setters.emplace_back([state, idx] { state->setValue(idx); });
    }
    for (auto& thr : setters) {
        thr.join();
    }
    // Callbacks are inline → already fired by setValue. Sleep is unnecessary.
    REQUIRE(fireCount.load() == 1);
}

// ── morph::async::Completion: orphan exception emits via the configured logger sink ─────────

TEST_CASE("morph::async::Completion: unhandled exception emits orphan log via the configured sink",
          "[completion][orphan][quantum-parity]") {
    LogGuard guard;
    std::atomic<int> orphanCount{0};
    morph::log::setLogger([&](morph::log::LogLevel lvl, std::string_view msg) {
        if (lvl == morph::log::LogLevel::error && msg.contains("[orphan]")) {
            orphanCount.fetch_add(1);
        }
    });

    {
        auto state = std::make_shared<morph::async::detail::CompletionState<int>>();
        state->setException(std::make_exception_ptr(std::runtime_error{"unhandled"}));
        // No onError attached; destructor of state must log an orphan error.
    }

    REQUIRE(orphanCount.load() == 1);
}

// ── morph::bridge::Bridge: concurrent executeVia + switchBackend ─────────────────────────────

namespace {

struct LoadCountAction {
    int delta = 0;
};

struct LoadCountModel {
    int value = 0;
    int execute(const LoadCountAction& act) {
        value += act.delta;
        return value;
    }
};

}  // namespace

template <>
struct morph::model::ModelTraits<LoadCountModel> {
    static constexpr std::string_view typeId() { return "QPI_LoadCountModel"; }
};

template <>
struct morph::model::ActionTraits<LoadCountAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "QPI_LoadCountAction"; }
    static std::string toJson(const LoadCountAction& act) { return std::to_string(act.delta); }
    static LoadCountAction fromJson(std::string_view str) { return LoadCountAction{std::stoi(std::string{str})}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view str) { return std::stoi(std::string{str}); }
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("morph::bridge::Bridge: concurrent executeVia under repeated switchBackend resolves every morph::async::Completion",
          "[bridge][concurrency][quantum-parity]") {
    morph::exec::ThreadPoolExecutor poolA{2};
    morph::exec::ThreadPoolExecutor poolB{2};
    InlineExec cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(poolA)};
    morph::bridge::BridgeHandler<LoadCountModel> handler{bridge, &cbExec};

    constexpr int numProducers = 4;
    constexpr int perProducer = 50;
    constexpr int totalActions = numProducers * perProducer;

    std::atomic<int> resolved{0};
    std::atomic<int> succeeded{0};
    std::atomic<int> failed{0};

    std::atomic<bool> stopSwitch{false};
    std::thread switcher([&] {
        bool flip = false;
        while (!stopSwitch.load()) {
            morph::exec::ThreadPoolExecutor& target = flip ? poolA : poolB;
            bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(target));
            flip = !flip;
            std::this_thread::sleep_for(1ms);
        }
    });

    std::vector<std::thread> producers;
    producers.reserve(numProducers);
    for (int prodIdx = 0; prodIdx < numProducers; ++prodIdx) {
        producers.emplace_back([&] {
            for (int idx = 0; idx < perProducer; ++idx) {
                handler.execute(LoadCountAction{1})
                    .then([&](int) {
                        succeeded.fetch_add(1);
                        resolved.fetch_add(1);
                    })
                    .onError([&](const std::exception_ptr&) {
                        failed.fetch_add(1);
                        resolved.fetch_add(1);
                    });
            }
        });
    }
    for (auto& thr : producers) {
        thr.join();
    }

    REQUIRE(waitUntil([&] { return resolved.load() == totalActions; }, 10s));
    stopSwitch.store(true);
    switcher.join();

    REQUIRE(resolved.load() == totalActions);
    REQUIRE(succeeded.load() + failed.load() == totalActions);
    // Some actions must succeed — if every single one failed something is
    // structurally broken (the snapshot-and-dispatch path never resolved).
    REQUIRE(succeeded.load() > 0);
}

// ── morph::offline::NetworkMonitor: stop() called from onOnline does not deadlock ─────────────

TEST_CASE("morph::offline::NetworkMonitor: stop() called from onOnline callback does not deadlock",
          "[network_monitor][reentrancy][quantum-parity]") {
    std::atomic<int> probeCount{0};
    std::atomic<bool> onlineFired{false};

    std::atomic<morph::offline::NetworkMonitor*> monitorPtr{nullptr};
    auto monitor = std::make_shared<morph::offline::NetworkMonitor>(
        [&] {
            int now = probeCount.fetch_add(1) + 1;
            return now >= 3;  // 2 failures → offline; success on 3rd → online.
        },
        [] {},
        [&onlineFired, &monitorPtr] {
            onlineFired.store(true);
            if (auto* mon = monitorPtr.load()) {
                mon->stop();  // probe thread calling its own stop must not deadlock.
            }
        },
        morph::offline::NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 2, .onlineThreshold = 1});
    monitorPtr.store(monitor.get());

    REQUIRE(waitUntil([&] { return onlineFired.load(); }, 2s));
    REQUIRE_NOTHROW(monitor.reset());
}

// ── morph::offline::NetworkMonitor: probe calling isOnline() does not deadlock ────────────────

TEST_CASE("morph::offline::NetworkMonitor: probe that calls isOnline() does not deadlock",
          "[network_monitor][reentrancy][quantum-parity]") {
    std::atomic<morph::offline::NetworkMonitor*> monitorPtr{nullptr};
    std::atomic<int> probeCount{0};
    std::atomic<int> onlineSeen{0};

    auto monitor = std::make_shared<morph::offline::NetworkMonitor>(
        [&] {
            // isOnline() reads an atomic with no internal lock; calling it from
            // inside the probe thread must therefore never deadlock.
            if (auto* mon = monitorPtr.load(); mon != nullptr && mon->isOnline()) {
                onlineSeen.fetch_add(1);
            }
            probeCount.fetch_add(1);
            return true;
        },
        [] {}, [] {},
        morph::offline::NetworkMonitor::Config{.probeInterval = 10ms, .failureThreshold = 1, .onlineThreshold = 1});
    monitorPtr.store(monitor.get());

    REQUIRE(waitUntil([&] { return probeCount.load() >= 5; }, 2s));
    REQUIRE(onlineSeen.load() >= 1);
    REQUIRE_NOTHROW(monitor->stop());
}

// ── morph::offline::SyncWorker: stop() invoked mid-replay aborts before the next item ─────────

TEST_CASE("morph::offline::SyncWorker: stop() called mid-replay aborts before processing next item",
          "[sync][stop][quantum-parity]") {
    morph::offline::InMemoryOfflineQueue queue;
    constexpr int total = 10;
    for (int idx = 0; idx < total; ++idx) {
        queue.enqueue("item" + std::to_string(idx));
    }

    morph::offline::SyncWorker* workerPtr = nullptr;
    std::atomic<int> processed{0};
    morph::offline::SyncWorker worker{queue, [&](const std::string&) {
                          int now = processed.fetch_add(1) + 1;
                          if (now == 3) {
                              workerPtr->stop();
                          }
                          return true;
                      }};
    workerPtr = &worker;

    auto result = worker.run();
    // First three items processed and removed; on the fourth iteration the
    // loop sees _stopped == true and breaks before invoking the replay fn.
    REQUIRE(result.successful == 3);
    auto remaining = queue.drain();
    REQUIRE(remaining.size() == static_cast<std::size_t>(total - 3));
    REQUIRE(remaining[0].payload == "item3");
}
