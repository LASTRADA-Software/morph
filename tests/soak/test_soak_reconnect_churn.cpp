// SPDX-License-Identifier: Apache-2.0

// Soak test: drives morph::offline::NetworkMonitor -> ReconnectCoordinator ->
// SyncWorker through many offline/online flaps, exactly the wiring
// docs/spec/offline/offline.md's "End-to-end integration" shows, and checks
// that the offline queue always fully drains and every onOnline() reconnects
// on its first attempt (this test's tryReconnect never fails) -- a stuck
// SyncWorker, a growing offline queue, or a coordinator that stops attempting
// reconnects would all show up as an assertion failure or a timeout here.
//
// Single-threaded worker executor by design: it serialises every posted
// onOnline()/onOffline() task (and therefore every use of `coordinator`/`sync`/
// `queue` from the worker side) so there is never a task from a stale flap
// still running when the next one is posted, and the local variables' normal
// reverse-declaration-order destruction (monitor first, stopping its probe
// thread; worker/queue last) is safe with no extra synchronization.
//
// Opt-in: built only under -DMORPH_BUILD_LOAD_TESTS=ON. Default cycle count is
// CI-sized; scale up via MORPH_SOAK_FLAP_CYCLES for a real soak run.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/offline/network_monitor.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/offline/reconnect_coordinator.hpp>
#include <morph/offline/sync_worker.hpp>
#include <string>

#include "test_support.hpp"

using namespace std::chrono_literals;

namespace {
int envIntOr(const char* name, int def) {
    const char* raw = std::getenv(name);
    if (!raw || *raw == '\0') {
        return def;
    }
    try {
        int parsed = std::stoi(raw);
        return parsed > 0 ? parsed : def;
    } catch (const std::exception&) {
        return def;
    }
}
}  // namespace

namespace {

/// The queue's depth, asked on its owner from this thread.
std::size_t pendingIn(morph::offline::InMemoryOfflineQueue& queue) {
    return morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) { return queue.size(reply); });
}

}  // namespace

TEST_CASE("soak: NetworkMonitor/ReconnectCoordinator/SyncWorker offline-online flap churn", "[soak][reconnect]") {
    const int flapCycles = envIntOr("MORPH_SOAK_FLAP_CYCLES", 150);

    morph::exec::ThreadPoolExecutor worker{1};

    std::atomic<int> tryReconnectCalls{0};
    std::atomic<int> replayCalls{0};
    std::atomic<int> activatePrimaryCalls{0};
    std::atomic<int> activateLocalCalls{0};
    std::atomic<bool> netOnline{true};

    // Drains on the coordinator's strand, so it is built once the coordinator
    // is; the queue belongs to that strand too. Both outlive the coordinator.
    std::unique_ptr<morph::offline::InMemoryOfflineQueue> queue;
    std::unique_ptr<morph::offline::SyncWorker> sync;

    morph::offline::ReconnectCoordinator coordinator{
        {.tryReconnect =
             [&] {
                 tryReconnectCalls.fetch_add(1, std::memory_order_relaxed);
                 return true;
             },
         .activatePrimary = [&] { activatePrimaryCalls.fetch_add(1, std::memory_order_relaxed); },
         .activateLocal = [&] { activateLocalCalls.fetch_add(1, std::memory_order_relaxed); },
         .bindContext = [] {},
         .replay =
             [&] {
                 replayCalls.fetch_add(1, std::memory_order_relaxed);
                 (void)sync->run(coordinator.strand());
             },
         .shouldContinue = [&] { return netOnline.load(); },
         .sleep = [](std::chrono::milliseconds) {}},
        worker};
    // This thread enqueues into the queue through its completion form.
    queue = std::make_unique<morph::offline::InMemoryOfflineQueue>(coordinator.strand());
    sync = std::make_unique<morph::offline::SyncWorker>(coordinator.strand(), *queue,
                                                        [](const std::string&) { return true; });

    // Fast flaps: 1ms probes, single-sample thresholds, so each online/offline
    // transition is observed on the very next probe tick instead of waiting
    // out the (much larger) production defaults.
    morph::offline::NetworkMonitor monitor{
        [&] { return netOnline.load(); }, [&] { coordinator.onOffline(); },
        [&] { (void)coordinator.onOnline(coordinator.strand()); },
        morph::offline::NetworkMonitorConfig{.probeInterval = 1ms, .failureThreshold = 1, .onlineThreshold = 1}};

    for (int cycle = 0; cycle < flapCycles; ++cycle) {
        (void)morph::testing::awaitAnswer([&](morph::exec::IExecutor& reply) {
            return queue->enqueue(reply, "{\"cycle\":" + std::to_string(cycle) + "}");
        });
        netOnline.store(false);
        REQUIRE(morph::testing::waitUntil([&] { return activateLocalCalls.load() > cycle; }));
        netOnline.store(true);
        REQUIRE(morph::testing::waitUntil([&] { return activatePrimaryCalls.load() > cycle; }));
        REQUIRE(morph::testing::waitUntil([&] { return pendingIn(*queue) == 0; }));
    }

    CHECK(tryReconnectCalls.load() == flapCycles);
    CHECK(replayCalls.load() == flapCycles);
    CHECK(activatePrimaryCalls.load() == flapCycles);
    CHECK(activateLocalCalls.load() == flapCycles);
    CHECK(pendingIn(*queue) == 0);
}
