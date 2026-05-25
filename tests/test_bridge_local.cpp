// SPDX-License-Identifier: Apache-2.0

#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/registry.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <thread>

#include "test_support.hpp"

using SyncExecutor = morph::testing::InlineExecutor;

// Test-local model — only used in this translation unit
struct PingAction {
    int value = 0;
};
struct PingFailAction {};

struct PingModel {
    int execute(PingAction action) { return action.value * 2; }
    int execute(PingFailAction) { throw std::runtime_error("ping failed"); }
};

BRIDGE_REGISTER_MODEL(PingModel, "Test_PingModel")
BRIDGE_REGISTER_ACTION(PingModel, PingAction, "Test_PingAction")
BRIDGE_REGISTER_ACTION(PingModel, PingFailAction, "Test_PingFailAction")

TEST_CASE("morph::backend::LocalBackend: action result delivered via then", "[bridge][local]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<PingModel> handler{bridge, &cbExec};

    std::atomic<int> result{-1};
    handler.execute(PingAction{21}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {
    });

    // Wait for the worker to complete and the callback to fire
    for (int idx = 0; idx < 50 && result.load() == -1; ++idx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(result.load() == 42);
}

TEST_CASE("morph::backend::LocalBackend: exception delivered via on_error", "[bridge][local]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<PingModel> handler{bridge, &cbExec};

    std::atomic<bool> errorFired{false};
    handler.execute(PingFailAction{}).then([](int) {}).onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::runtime_error&) {
            errorFired.store(true);
        }
    });

    for (int idx = 0; idx < 50 && !errorFired.load(); ++idx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(errorFired.load());
}

TEST_CASE("morph::backend::LocalBackend: multiple sequential actions on same handler", "[bridge][local]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<PingModel> handler{bridge, &cbExec};

    std::atomic<int> sum{0};
    std::atomic<int> count{0};
    constexpr int numActions = 5;

    for (int idx = 1; idx <= numActions; ++idx) {
        handler.execute(PingAction{idx})
            .then([&](int val) {
                sum.fetch_add(val);
                count.fetch_add(1);
            })
            .onError([](const std::exception_ptr&) {});
    }

    for (int idx = 0; idx < 100 && count.load() < numActions; ++idx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(count.load() == numActions);
    // PingModel::Execute returns value*2, so sum = 2*(1+2+3+4+5) = 30
    REQUIRE(sum.load() == 30);
}
