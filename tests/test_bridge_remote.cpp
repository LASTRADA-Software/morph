// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "test_support.hpp"

using SyncExecutor = morph::testing::InlineExecutor;

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

TEST_CASE("morph::backend::SimulatedRemoteBackend: action result delivered via then", "[bridge][remote]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::exec::MainThreadExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server), cbExec};
    morph::bridge::BridgeHandler<EchoModel> handler{bridge, &cbExec};

    std::atomic<int> result{-1};
    handler.execute(EchoAction{99}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {
    });

    for (int idx = 0; idx < 50 && result.load() == -1; ++idx) {
        cbExec.runFor(std::chrono::milliseconds(10));
    }

    REQUIRE(result.load() == 99);
}

TEST_CASE("morph::backend::SimulatedRemoteBackend: exception delivered via onError", "[bridge][remote]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::exec::MainThreadExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server), cbExec};
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
        cbExec.runFor(std::chrono::milliseconds(10));
    }

    REQUIRE(errorFired.load());
}

TEST_CASE("morph::backend::SimulatedRemoteBackend: multiple actions on same handler", "[bridge][remote]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::exec::MainThreadExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server), cbExec};
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
        cbExec.runFor(std::chrono::milliseconds(10));
    }

    REQUIRE(count.load() == numActions);
    REQUIRE(sum.load() == 15);  // 1+2+3+4+5
}

TEST_CASE("morph::backend::SimulatedRemoteBackend: assignPrimary files a live instance under its key",
          "[bridge][remote]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto const server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::backend::SimulatedRemoteBackend backend{*server};

    auto const mid = backend.registerModel("TestR_EchoModel", {});
    REQUIRE(mid.v != 0U);
    REQUIRE(backend.listInstances("TestR_EchoModel").empty());

    backend.assignPrimary(mid, "TestR_EchoModel", "filed-key");
    CHECK(backend.listInstances("TestR_EchoModel") == std::vector<std::string>{"filed-key"});
}

TEST_CASE("morph::backend::SimulatedRemoteBackend: binding a private instance releases the one it replaces",
          "[bridge][remote]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto const server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::backend::SimulatedRemoteBackend backend{*server};
    auto const liveModels = [&server] {
        return morph::testing::awaitAnswer([&server](auto& owner) { return server->health(owner); }).liveModels;
    };

    auto const old = backend.registerModel("TestR_EchoModel", {});
    REQUIRE(liveModels() == 1U);

    morph::backend::detail::BindRequest request;
    request.typeId = "TestR_EchoModel";
    request.current = old;
    morph::exec::MainThreadExecutor cbExec;
    auto const bound = morph::testing::awaitValueOn(cbExec, backend.bindModel(std::move(request), cbExec));
    CHECK(bound.v != old.v);
    CHECK(liveModels() == 1U);
}

namespace {
std::atomic<bool>& remoteBlockRelease() {
    static std::atomic<bool> value{false};
    return value;
}
std::atomic<int>& remoteBlockStarted() {
    static std::atomic<int> value{0};
    return value;
}
}  // namespace

struct BlockAction {};
struct BlockModel {
    int execute(BlockAction /*action*/) {
        remoteBlockStarted().fetch_add(1);
        while (!remoteBlockRelease().load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 0;
    }
};

BRIDGE_REGISTER_MODEL(BlockModel, "TestR_BlockModel")
// The generated fromJson body declares a non-const local inside the macro.
// NOLINTNEXTLINE(misc-const-correctness)
BRIDGE_REGISTER_ACTION(BlockModel, BlockAction, "TestR_BlockAction")

TEST_CASE("morph::backend::SimulatedRemoteBackend: cancelPending fails every call still in flight",
          "[bridge][remote]") {
    remoteBlockRelease().store(false);
    remoteBlockStarted().store(0);
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto const server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::exec::MainThreadExecutor cbExec;
    auto owned = std::make_unique<morph::backend::SimulatedRemoteBackend>(*server);
    auto* const backend = owned.get();
    morph::bridge::Bridge bridge{std::move(owned), cbExec};
    morph::bridge::BridgeHandler<BlockModel> handler{bridge, &cbExec};

    // Three calls on one instance: the first runs and blocks, the other two
    // queue behind it, so all three are in flight together.
    constexpr int kCalls = 3;
    std::atomic<int> errors{0};
    for (int idx = 0; idx < kCalls; ++idx) {
        handler.execute(BlockAction{}).then([](int) {}).onError([&errors](const std::exception_ptr&) {
            errors.fetch_add(1);
        });
    }
    REQUIRE(morph::testing::waitUntil([] { return remoteBlockStarted().load() == 1; }));

    backend->cancelPending(std::make_exception_ptr(std::runtime_error("cancelled")));
    for (int idx = 0; idx < 100 && errors.load() < kCalls; ++idx) {
        cbExec.runFor(std::chrono::milliseconds(10));
    }
    CHECK(errors.load() == kCalls);

    remoteBlockRelease().store(true);
    REQUIRE(morph::testing::awaitAnswer(
        [&server](auto& owner) { return server->drainedWithin(std::chrono::milliseconds{5000}, owner); }));
}
