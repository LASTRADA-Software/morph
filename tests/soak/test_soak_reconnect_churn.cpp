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

TEST_CASE("soak: NetworkMonitor/ReconnectCoordinator/SyncWorker offline-online flap churn", "[soak][reconnect]") {
    const int flapCycles = envIntOr("MORPH_SOAK_FLAP_CYCLES", 150);

    morph::exec::ThreadPoolExecutor worker{1};
    morph::offline::InMemoryOfflineQueue queue;

    std::atomic<int> tryReconnectCalls{0};
    std::atomic<int> replayCalls{0};
    std::atomic<int> activatePrimaryCalls{0};
    std::atomic<int> activateLocalCalls{0};
    std::atomic<bool> netOnline{true};

    morph::offline::SyncWorker sync{queue, [](const std::string&) { return true; }};

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
                 sync.run();
             },
         .shouldContinue = [&] { return netOnline.load(); },
         .sleep = [](std::chrono::milliseconds) {}}};

    // Fast flaps: 1ms probes, single-sample thresholds, so each online/offline
    // transition is observed on the very next probe tick instead of waiting
    // out the (much larger) production defaults.
    morph::offline::NetworkMonitor monitor{
        [&] { return netOnline.load(); }, [&] { worker.post([&] { coordinator.onOffline(); }); },
        [&] { worker.post([&] { (void)coordinator.onOnline(); }); },
        morph::offline::NetworkMonitorConfig{.probeInterval = 1ms, .failureThreshold = 1, .onlineThreshold = 1}};

    for (int cycle = 0; cycle < flapCycles; ++cycle) {
        (void)queue.enqueue("{\"cycle\":" + std::to_string(cycle) + "}");
        netOnline.store(false);
        REQUIRE(morph::testing::waitUntil([&] { return activateLocalCalls.load() > cycle; }));
        netOnline.store(true);
        REQUIRE(morph::testing::waitUntil([&] { return activatePrimaryCalls.load() > cycle; }));
        REQUIRE(morph::testing::waitUntil([&] { return queue.drain().empty(); }));
    }

    CHECK(tryReconnectCalls.load() == flapCycles);
    CHECK(replayCalls.load() == flapCycles);
    CHECK(activatePrimaryCalls.load() == flapCycles);
    CHECK(activateLocalCalls.load() == flapCycles);
    CHECK(queue.drain().empty());
}
