// SPDX-License-Identifier: Apache-2.0

#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/registry.hpp>
#include <morph/remote.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <thread>


struct EchoAction {
    int value = 0;
};
struct EchoFail {};

struct EchoModel {
    int execute(EchoAction action) { return action.value; }
    int execute(EchoFail) { throw std::runtime_error("echo failed"); }
};

BRIDGE_REGISTER_MODEL(EchoModel, "TestR_EchoModel")
BRIDGE_REGISTER_ACTION(EchoModel, EchoAction, "TestR_EchoAction")
BRIDGE_REGISTER_ACTION(EchoModel, EchoFail, "TestR_EchoFail")

struct SyncExecutor : morph::exec::IExecutor {
    void post(std::function<void()> task) override { task(); }
};

TEST_CASE("morph::backend::SimulatedRemoteBackend: action result delivered via then", "[bridge][remote]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};
    morph::bridge::BridgeHandler<EchoModel> handler{bridge, &cbExec};

    std::atomic<int> result{-1};
    handler.execute(EchoAction{99}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {
    });

    for (int idx = 0; idx < 50 && result.load() == -1; ++idx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(result.load() == 99);
}

TEST_CASE("morph::backend::SimulatedRemoteBackend: exception delivered via onError", "[bridge][remote]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};
    morph::bridge::BridgeHandler<EchoModel> handler{bridge, &cbExec};

    std::atomic<bool> errorFired{false};
    handler.execute(EchoFail{}).then([](int) {}).onError([&](const std::exception_ptr& exc) {
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

TEST_CASE("morph::backend::SimulatedRemoteBackend: multiple actions on same handler", "[bridge][remote]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};
    morph::bridge::BridgeHandler<EchoModel> handler{bridge, &cbExec};

    std::atomic<int> sum{0};
    std::atomic<int> count{0};
    constexpr int numActions = 5;

    for (int idx = 1; idx <= numActions; ++idx) {
        handler.execute(EchoAction{idx})
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
    REQUIRE(sum.load() == 15);  // 1+2+3+4+5
}
