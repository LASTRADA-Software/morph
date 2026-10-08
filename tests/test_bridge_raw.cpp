// SPDX-License-Identifier: Apache-2.0
//
// The model-free dispatch seams, driven by type id and action id only. This
// translation unit names no model or action type; the models live in
// raw_dispatch_models.cpp.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <exception>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
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
    CHECK(message(refused.error).find("counter refused") != std::string::npos);

    auto unknown = await(rig, handler.execute("Raw_Nope", "{}"));
    REQUIRE(unknown.error);
    CHECK(message(unknown.error).find("Raw_Counter/Raw_Nope") != std::string::npos);

    auto malformed = await(rig, handler.execute("Raw_Add", R"({"by":"x"})"));
    REQUIRE(malformed.error);
}

TEST_CASE("RawHandler: an unknown model type fails the bind and rejects the call", "[bridge][raw]") {
    auto const mode = GENERATE(Mode::Local, Mode::Remote);
    RawRig rig{mode};
    RawHandler handler{*rig.bridge, &rig.owner, "Raw_Missing"};

    auto reply = await(rig, handler.execute("Raw_Add", R"({"by":1})"));
    REQUIRE(reply.error);
    CHECK(message(reply.error).find("Raw_Missing") != std::string::npos);
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
    CHECK(message(reply.error).find("attach on the owner") != std::string::npos);
    CHECK_FALSE(slot.handler);
    slot.bridge = nullptr;
}
