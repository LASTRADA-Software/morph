// SPDX-License-Identifier: Apache-2.0
//
// The model-free dispatch seams, driven by type id and action id only. This
// translation unit names no model or action type; the models live in
// raw_dispatch_models.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <core/async/Cancellation.hpp>
#include <core/async/StopToken.hpp>
#include <cstdint>
#include <exception>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/session/session.hpp>
#include <optional>
#include <stdexcept>
#include <string>

#include "owner_probe_recorder.hpp"
#include "raw_dispatch_probe.hpp"
#include "test_support.hpp"

using morph::bridge::BindSharing;
using morph::bridge::RawHandler;

namespace {

enum class Mode : std::uint8_t { Local, Remote };

/// A bridge over LocalBackend or over SimulatedRemoteBackend and its server.
struct RawRig {
    explicit RawRig(Mode mode) {
        if (mode == Mode::Local) {
            bridge =
                std::make_unique<morph::bridge::Bridge>(std::make_unique<morph::backend::LocalBackend>(pool), owner);
        } else {
            server = std::make_shared<morph::backend::RemoteServer>(serverPool);
            bridge = std::make_unique<morph::bridge::Bridge>(
                std::make_unique<morph::backend::SimulatedRemoteBackend>(*server), owner);
        }
    }

    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::ThreadPoolExecutor serverPool{2};
    morph::exec::MainThreadExecutor owner;
    std::shared_ptr<morph::backend::RemoteServer> server;
    std::unique_ptr<morph::bridge::Bridge> bridge;
};

struct Reply {
    std::optional<std::string> value;
    std::exception_ptr error;
    [[nodiscard]] bool settled() const { return value.has_value() || error != nullptr; }
};

Reply await(RawRig& rig, morph::async::Completion<std::string> completion) {
    Reply reply;
    completion.then([&](std::string json) { reply.value = std::move(json); })
        .onError([&](const std::exception_ptr& err) { reply.error = err; });
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [&] { return reply.settled(); }));
    return reply;
}

std::string message(const std::exception_ptr& err) {
    try {
        std::rethrow_exception(err);
    } catch (const std::exception& exc) {
        return exc.what();
    }
}

// Whether @p err holds an @p E. A helper rather than CHECK_THROWS_AS over
// std::rethrow_exception: that call is [[noreturn]], so MSVC flags the
// macro's code after it as unreachable (C4702, an error here).
template <typename E>
bool holds(const std::exception_ptr& err) {
    if (!err) {
        return false;
    }
    try {
        std::rethrow_exception(err);
    } catch (const E&) {
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

TEST_CASE("RawHandler: dispatches by id and keeps the instance's state", "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};

    CHECK(await(rig, handler.execute("Raw_Add", R"({"by":5})")).value == "5");
    CHECK(await(rig, handler.execute("Raw_Add", R"({"by":2})")).value == "7");
    CHECK(await(rig, handler.execute("Raw_Rename", R"({"name":"lab"})")).value == R"("renamed to lab")");
}

TEST_CASE("RawHandler: a model's refusal, an unknown action and a malformed body reject the call", "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};

    auto refused = await(rig, handler.execute("Raw_Fail", "{}"));
    REQUIRE(refused.error);
    CHECK(message(refused.error).contains("counter refused"));

    auto unknown = await(rig, handler.execute("Raw_Nope", "{}"));
    REQUIRE(unknown.error);
    CHECK(message(unknown.error).contains("Raw_Counter/Raw_Nope"));

    auto malformed = await(rig, handler.execute("Raw_Add", R"({"by":"x"})"));
    REQUIRE(malformed.error);
}

TEST_CASE("RawHandler: an unknown model type fails the bind and rejects the call", "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Missing"};

    auto reply = await(rig, handler.execute("Raw_Add", R"({"by":1})"));
    REQUIRE(reply.error);
    CHECK(message(reply.error).contains("Raw_Missing"));
    CHECK_FALSE(handler.isBound());
}

// Typed counterpart: test_bridge_owner.cpp, "a handler constructed inside a
// running action posts its registration to the owner".
TEST_CASE("RawHandler: one constructed inside a running action posts its registration to the owner",
          "[bridge][raw][owner]") {
    RawRig rig{Mode::Local};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};
    auto& slot = rawprobe::spawnSlot();
    slot.bridge = rig.bridge.get();
    slot.gui = &rig.owner;

    morph::testing::OwnerProbeRecorder const recorder{rig.owner.coreExecutor()};
    REQUIRE(await(rig, handler.execute("Raw_Spawn", R"({"key":""})")).value);
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [&] { return slot.handler && slot.handler->isBound(); }));
    // Constructed on a pool thread; registered in an owner task.
    CHECK(recorder.allPosted("Bridge::registerHandler"));
    CHECK(recorder.allPosted("LocalBackend::bindModel"));

    CHECK(await(rig, slot.handler->execute("Raw_Add", R"({"by":4})")).value == "4");
    slot.handler.reset();
    slot.bridge = nullptr;
}

TEST_CASE("RawHandler: one constructed inside a running action refuses an instance key", "[bridge][raw][owner]") {
    RawRig rig{Mode::Local};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};
    auto& slot = rawprobe::spawnSlot();
    slot.bridge = rig.bridge.get();
    slot.gui = &rig.owner;

    auto reply = await(rig, handler.execute("Raw_Spawn", R"({"key":"3"})"));
    REQUIRE(reply.error);
    CHECK(message(reply.error).contains("attach on the owner"));
    CHECK_FALSE(slot.handler);
    slot.bridge = nullptr;
}

// Typed counterpart: test_principal.cpp / test_bridge_local.cpp session cases.
TEST_CASE("RawHandler: the default session reaches the model", "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    morph::session::Context session;
    session.requestId = "req-raw-1";
    rig.bridge->setDefaultSession(session);
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};

    CHECK(await(rig, handler.execute("Raw_RequestId", "{}")).value == R"("req-raw-1")");
}

// Typed counterpart: test_client_execute_deadline.cpp, "fires ClientTimeoutError when no reply arrives in time".
TEST_CASE("RawHandler: the execute deadline rejects a call with no reply", "[bridge][raw]") {
    rawprobe::SleeperScope const sleeping;
    RawRig rig{Mode::Local};
    rig.bridge->setExecuteDeadline(std::chrono::milliseconds{50});
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};

    auto reply = await(rig, handler.execute("Raw_Sleep", R"({"ms":60000})"));
    REQUIRE(reply.error);
    CHECK(holds<morph::backend::ClientTimeoutError>(reply.error));
    // The deadline asks the Task handler to stop, as for a typed call.
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [] { return rawprobe::sleeper().cancelled.load() == 1; }));
}

// Typed counterpart: test_bridge_pending_calls.cpp.
TEST_CASE("RawHandler: pendingCalls counts a raw call until it settles", "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};
    REQUIRE(rig.bridge->pendingCalls() == 0);

    auto completion = handler.execute("Raw_Add", R"({"by":1})");
    CHECK(rig.bridge->pendingCalls() == 1);
    auto ok = await(rig, std::move(completion));
    CHECK(ok.value == "1");
    CHECK(rig.bridge->pendingCalls() == 0);

    auto failed = await(rig, handler.execute("Raw_Fail", "{}"));
    CHECK(failed.error);
    CHECK(rig.bridge->pendingCalls() == 0);
}

// Typed counterpart: test_cancellation_policy.cpp, "execute with a stop token is G2 when stopped...".
TEST_CASE("RawHandler: a stop token cancels the call and stops its Task handler", "[bridge][raw]") {
    rawprobe::SleeperScope const sleeping;
    RawRig rig{Mode::Local};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};

    core::async::StopSource stop;  // NOLINT(misc-const-correctness): request_stop() is non-const
    Reply reply;
    handler.execute("Raw_Sleep", R"({"ms":60000})", stop.get_token())
        .then([&](std::string json) { reply.value = std::move(json); })
        .onError([&](const std::exception_ptr& err) { reply.error = err; });
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [] { return rawprobe::sleeper().started.load() == 1; }));

    static_cast<void>(stop.request_stop());
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [&] { return reply.settled(); }));
    CHECK(holds<core::async::OperationCancelled>(reply.error));
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [] { return rawprobe::sleeper().cancelled.load() == 1; }));
    CHECK(rawprobe::sleeper().finished.load() == 0);
}

TEST_CASE("RawHandler: a token already stopped rejects without dispatching", "[bridge][raw]") {
    rawprobe::SleeperScope const sleeping;
    RawRig rig{Mode::Local};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};
    core::async::StopSource stop;  // NOLINT(misc-const-correctness)
    static_cast<void>(stop.request_stop());

    auto reply = await(rig, handler.execute("Raw_Sleep", R"({"ms":10})", stop.get_token()));
    CHECK(holds<core::async::OperationCancelled>(reply.error));
    CHECK(rawprobe::sleeper().started.load() == 0);
    CHECK(rig.bridge->pendingCalls() == 0);
}

// Typed counterpart: test_cancellation_policy.cpp, "Bridge::switchBackend is G2 for the outgoing backend's calls".
TEST_CASE("RawHandler: cancelPending on a switch settles an in-flight raw call", "[bridge][raw]") {
    rawprobe::SleeperScope const sleeping;
    RawRig rig{Mode::Local};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};
    Reply reply;
    handler.execute("Raw_Sleep", R"({"ms":60000})")
        .then([&](std::string json) { reply.value = std::move(json); })
        .onError([&](const std::exception_ptr& err) { reply.error = err; });
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [] { return rawprobe::sleeper().started.load() == 1; }));

    rig.bridge->switchBackend(std::make_unique<morph::backend::LocalBackend>(rig.pool));
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [&] { return reply.settled(); }));
    CHECK(reply.error);
}

TEST_CASE("RawHandler: a raw result is not published to typed subscribers", "[bridge][raw]") {
    RawRig rig{Mode::Local};
    auto watch = rawprobe::watchRenames(*rig.bridge, rig.owner, "41");
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter", BindSharing::Shared, "41"};

    CHECK(await(rig, handler.execute("Raw_Rename", R"({"name":"x"})")).value == R"("renamed to x")");
    rig.owner.runFor(std::chrono::milliseconds{20});
    CHECK(rawprobe::renamesSeen(*watch) == 0);

    // Control: a typed result on the same instance does reach the subscriber.
    rawprobe::renameTyped(*watch);
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [&] { return rawprobe::renamesSeen(*watch) == 1; }));
}

TEST_CASE("RawHandler: two shared handlers on one key share one instance", "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    RawHandler first{*rig.bridge, &rig.owner, "Raw_Counter", BindSharing::Shared, "7"};
    RawHandler second{*rig.bridge, &rig.owner, "Raw_Counter", BindSharing::Shared};
    second.attach("7");

    CHECK(await(rig, first.execute("Raw_Add", R"({"by":2})")).value == "2");
    CHECK(await(rig, second.execute("Raw_Add", R"({"by":3})")).value == "5");
    CHECK(second.primary() == "7");
}

TEST_CASE("RawHandler: a shared handler that was never attached rejects its calls", "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter", BindSharing::Shared};

    auto reply = await(rig, handler.execute("Raw_Add", R"({"by":1})"));
    CHECK(reply.error);
    CHECK(handler.primary().empty());
}

TEST_CASE("RawHandler: re-attaching moves the handler to the other instance", "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter", BindSharing::Shared, "1"};
    CHECK(await(rig, handler.execute("Raw_Add", R"({"by":10})")).value == "10");

    handler.attach("2");
    CHECK(await(rig, handler.execute("Raw_Add", R"({"by":1})")).value == "1");
    CHECK(handler.primary() == "2");
}

TEST_CASE("RawHandler: a private handler refuses attach, and bindByType refuses a private key", "[bridge][raw]") {
    RawRig rig{Mode::Local};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Counter"};
    CHECK_THROWS_AS(handler.attach("1"), std::logic_error);
    CHECK_THROWS_AS(rig.bridge->bindByType("Raw_Counter", BindSharing::Private, "1"), std::invalid_argument);
}

// Typed counterpart: test_bridge_bind_paths.cpp, switchBackend re-binding.
TEST_CASE("RawHandler: switchBackend re-binds private and shared raw handlers", "[bridge][raw]") {
    RawRig rig{Mode::Local};
    RawHandler priv{*rig.bridge, &rig.owner, "Raw_Counter"};
    RawHandler shared{*rig.bridge, &rig.owner, "Raw_Counter", BindSharing::Shared, "9"};
    REQUIRE(await(rig, priv.execute("Raw_Add", R"({"by":1})")).value == "1");
    REQUIRE(await(rig, shared.execute("Raw_Add", R"({"by":1})")).value == "1");

    rig.bridge->switchBackend(std::make_unique<morph::backend::LocalBackend>(rig.pool));
    REQUIRE(morph::testing::pumpOwnerUntil(rig.owner, [&] { return priv.isBound() && shared.isBound(); }));
    // A fresh backend holds fresh instances, still reachable by the same handlers.
    CHECK(await(rig, priv.execute("Raw_Add", R"({"by":4})")).value == "4");
    CHECK(await(rig, shared.execute("Raw_Add", R"({"by":6})")).value == "6");
    CHECK(shared.primary() == "9");
}
