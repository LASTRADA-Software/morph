// SPDX-License-Identifier: Apache-2.0

// Integration test: morph::offline::NetworkMonitor → morph::offline::SyncWorker → morph::bridge::Bridge::switchBackend
//
// Scenario:
//   1. App starts with a local backend (bridge) — simulates offline mode.
//   2. Three serialised action payloads are enqueued in the offline queue
//      (representing writes that happened while the network was down).
//   3. morph::offline::NetworkMonitor is created with probe returning false (network is down).
//      Monitor detects offline after failureThreshold=1 probe; _online goes false.
//   4. networkOnline is set to true — probe recovers.
//   5. Monitor fires onOnline → morph::offline::SyncWorker replays the queue → bridge switches
//      to the "remote" pool.
//   6. All queue items were replayed and removed.
//   7. Execute on the handler still works, confirming the new backend is live.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/offline/network_monitor.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/offline/sync_worker.hpp>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "test_support.hpp"

using namespace std::chrono_literals;

// ── Test model ────────────────────────────────────────────────────────────────

struct OffAction {
    int x = 0;
};
struct OffModel {
    int execute(const OffAction& act) { return act.x * 10; }
    void onBackendChanged() {}
};

template <>
struct morph::model::ModelTraits<OffModel> {
    static constexpr std::string_view typeId() { return "OFF_OffModel"; }
};
template <>
struct morph::model::ActionTraits<OffAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "OFF_OffAction"; }
    static std::string toJson(const OffAction& act) { return R"({"x":)" + std::to_string(act.x) + "}"; }
    static OffAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view str) { return std::stoi(std::string{str}); }
};

using SyncExec = morph::testing::InlineExecutor;

// ── Integration test ──────────────────────────────────────────────────────────

TEST_CASE("Integration: offline queue replayed and backend switched on network recovery", "[integration]") {
    morph::exec::ThreadPoolExecutor localPool{2};
    morph::exec::ThreadPoolExecutor remotePool{2};
    SyncExec cbExec;
    morph::offline::InMemoryOfflineQueue queue;

    // Start in local (offline) mode.
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(localPool)};
    morph::bridge::BridgeHandler<OffModel> handler{bridge, &cbExec};

    // Enqueue payloads that were written while offline.
    (void)queue.enqueue("{\"x\":1}");
    (void)queue.enqueue("{\"x\":2}");
    (void)queue.enqueue("{\"x\":3}");

    // Probe starts returning false — network is down.
    std::atomic<bool> networkOnline{false};

    std::vector<std::string> replayed;
    std::mutex replayMtx;

    morph::offline::SyncWorker syncWorker{queue, [&](const std::string& payload) {
                                              std::scoped_lock lock{replayMtx};
                                              replayed.push_back(payload);
                                              return true;
                                          }};

    // Monitor: failureThreshold=1, onlineThreshold=1, probeInterval=30ms.
    // With networkOnline=false the monitor goes offline after ~30ms.
    // When networkOnline becomes true the monitor recovers after a further ~30ms.
    morph::offline::NetworkMonitor monitor{
        [&] { return networkOnline.load(); }, [] {},  // onOffline — not exercised here
        [&] {
            // onOnline fires on the probe thread — replay then switch backend.
            syncWorker.run();
            bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(remotePool));
        },
        morph::offline::NetworkMonitor::Config{.probeInterval = 30ms, .failureThreshold = 1, .onlineThreshold = 1}};

    // Wait for monitor to detect offline (one probe interval + margin).
    std::this_thread::sleep_for(80ms);
    REQUIRE_FALSE(monitor.isOnline());

    // Bring network back online.
    networkOnline.store(true);

    // Wait for monitor to detect recovery and fire onOnline (one probe interval + margin).
    std::this_thread::sleep_for(150ms);

    // All queued items must have been replayed.
    {
        std::scoped_lock lock{replayMtx};
        REQUIRE(replayed.size() == 3);
    }
    REQUIRE(queue.drain().empty());

    // morph::bridge::Bridge now routes to remotePool — execute still works.
    std::atomic<int> result{-1};
    handler.execute(OffAction{5}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {});
    REQUIRE(morph::testing::waitUntil([&] { return result.load() != -1; }));
    REQUIRE(result.load() == 50);
}
