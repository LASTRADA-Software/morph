// SPDX-License-Identifier: Apache-2.0

// Guards in core that no other suite reaches: the ways a stop link, an owner
// check and a gated attach decline to do anything.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <core/async/StopToken.hpp>
#include <exception>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/timeout_scheduler.hpp>
#include <morph/testing/owner_probe_recorder.hpp>
#include <stdexcept>
#include <thread>

#include "bind_support.hpp"
#include "test_support.hpp"

// NOLINTBEGIN(misc-use-internal-linkage): registration needs external linkage.
struct CgPing {
    int value = 0;
};
struct CgModel {
    int execute(const CgPing& act) { return act.value; }
};
BRIDGE_REGISTER_MODEL(CgModel, "CG_Model")
BRIDGE_REGISTER_ACTION(CgModel, CgPing, "CG_Ping")
// NOLINTEND(misc-use-internal-linkage)

namespace {

using morph::async::CallbackToken;
using morph::async::Completion;
using morph::async::detail::CompletionState;

/// A state on @p owner that carries a stop source, as a Task-handler call's does.
std::shared_ptr<CompletionState<int>> stoppableState(morph::exec::IExecutor* owner) {
    auto state = std::make_shared<CompletionState<int>>();
    state->cbExec = owner;
    state->stopSource = std::make_shared<::core::async::StopSource>();
    state->onErrAttached = true;
    return state;
}

}  // namespace

TEST_CASE("Completion: a gated handler on an empty completion does nothing", "[completion][token]") {
    Completion<int> empty;
    bool ran = false;
    empty.then(CallbackToken{}, [&](const int&) { ran = true; });
    empty.onError(CallbackToken{}, [&](const std::exception_ptr&) { ran = true; });
    REQUIRE_FALSE(ran);
}

TEST_CASE("CompletionState::linkStop declines when it has nothing to link", "[completion][token]") {
    morph::exec::MainThreadExecutor owner;
    ::core::async::StopSource const scope;

    SECTION("no executor to deliver on") {
        auto state = stoppableState(nullptr);
        state->linkStop(scope.get_token());
        REQUIRE(state->stopLinks.empty());
    }
    SECTION("the state was already delivered") {
        auto state = stoppableState(&owner);
        state->delivered.store(true);
        state->linkStop(scope.get_token());
        REQUIRE(state->stopLinks.empty());
    }
    SECTION("the scope can no longer be stopped") {
        auto state = stoppableState(&owner);
        state->linkStop(::core::async::StopToken{});
        REQUIRE(state->stopLinks.empty());
    }
    SECTION("a live scope is linked") {
        auto state = stoppableState(&owner);
        state->linkStop(scope.get_token());
        REQUIRE(state->stopLinks.size() == 1);
    }
}

TEST_CASE("CompletionState: a scope's stop reaching a call whose stop source is gone stops nothing",
          "[completion][token]") {
    morph::exec::MainThreadExecutor owner;
    // NOLINTNEXTLINE(misc-const-correctness): request_stop() is a non-const member.
    ::core::async::StopSource scope;
    auto state = stoppableState(&owner);
    state->linkStop(scope.get_token());
    REQUIRE(state->stopLinks.size() == 1);

    auto source = std::move(state->stopSource);
    source.reset();
    REQUIRE(scope.request_stop());
}

TEST_CASE("CompletionState::checkOwner reports a thread that touches a delivered state", "[completion][owner]") {
    morph::exec::MainThreadExecutor owner;
    morph::testing::OwnerProbeRecorder const recorder{owner.coreExecutor()};
    auto state = stoppableState(&owner);

    // Delivered on another thread than the one now touching it.
    std::thread{[&] { state->deliveredOn.store(std::this_thread::get_id()); }}.join();
    state->delivered.store(true);
    state->checkOwner("test::touch");
    REQUIRE(recorder.count("test::touch") == 1);

    // Delivered on this very thread: nothing to report.
    state->deliveredOn.store(std::this_thread::get_id());
    state->checkOwner("test::again");
    REQUIRE(recorder.count("test::again") == 0);
}

TEST_CASE("Completion: a default gate on a live completion links no stop", "[completion][token]") {
    morph::exec::MainThreadExecutor owner;
    auto [completion, promise] = Completion<int>::makeSettleable(&owner);
    std::atomic<int> got{0};
    completion.then(CallbackToken{}, [&](const int&) { got.store(1); });
    promise.resolve(3);
    owner.runFor(std::chrono::milliseconds{5});
    // A default token suppresses the handler unconditionally.
    REQUIRE(got.load() == 0);
}

TEST_CASE("TimeoutScheduler::cancel after the timer fired finds nothing to retire", "[timeout_scheduler]") {
    morph::async::detail::TimeoutScheduler scheduler;
    std::atomic<bool> fired{false};
    auto const handle = scheduler.schedule(std::chrono::milliseconds{1}, [&fired] { fired.store(true); });
    REQUIRE(morph::testing::waitUntil([&] { return fired.load(); }));
    scheduler.cancel(handle);
    // The loop runs its tasks in order: once this later timer has fired, the
    // cancel before it has been applied.
    std::atomic<bool> after{false};
    scheduler.schedule(std::chrono::milliseconds{1}, [&after] { after.store(true); });
    REQUIRE(morph::testing::waitUntil([&] { return after.load(); }));
}

TEST_CASE("SimulatedRemoteBackend: an attach the server refuses fails the bind with the server's reason",
          "[remote][bind]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::ServerConfig config;
    config.limits.maxLiveModels = 1;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, config);
    morph::exec::MainThreadExecutor owner;
    morph::backend::SimulatedRemoteBackend backend{*server};
    backend.setOwner(morph::exec::detail::OwnerAffinity{owner});

    auto const factory = [] { return morph::model::detail::ModelFactory::create<CgModel>(); };
    static_cast<void>(morph::testing::bindShared(backend, "CG_Model", factory, "1"));
    REQUIRE_THROWS_WITH(
        morph::testing::bindAttach(backend, "CG_Model", factory, "2", morph::exec::detail::ModelId{987654}),
        Catch::Matchers::ContainsSubstring("attach failed"));
}
