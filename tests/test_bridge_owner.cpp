// SPDX-License-Identifier: Apache-2.0

// A Bridge, its handlers and its backend belong to one owner: the bind rule,
// and one posted-call test per lock the owner model removed. Each posted-call
// test drives a verb from a thread that is not the owner and checks, through
// the owner probe, that the body touching owner state ran inside a task of
// the owner — the recorder reads the executor scope itself, so the check does
// not trust the bridge's own answer. See docs/spec/core/bridge.md,
// "Thread safety — one owner".

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/forms/flows.hpp>
#include <morph/forms/sections.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "owner_probe_recorder.hpp"
#include "test_support.hpp"

namespace owner_test {

struct Ping {
    int value = 0;
};
struct Sub {
    int value = 0;
};
struct Spawn {
    int value = 0;
};

struct Pong {
    int value = 0;
};

struct SubModel {
    int execute(const Sub& action) { return action.value + 1; }
};

struct PingModel {
    Pong execute(const Ping& action) { return Pong{.value = action.value * 2}; }
    int execute(const Spawn& action);
};

struct Tally {
    int value = 0;
};
struct KeyedModel {
    using PrimaryKey = std::int64_t;
    int execute(const Tally& action) { return action.value; }
};

struct FormStep {
    int value = 0;
    [[nodiscard]] bool validate() const { return value > 0; }
};
struct FormResult {
    int total = 0;
};
struct FormModel {
    FormResult execute(const FormStep& action) { return FormResult{.total = action.value * 10}; }
};

// Registers a sub-model handler from inside a running action: the case the
// bind rule makes asynchronous. A model cannot reach its bridge through the
// framework, so the test hands it one.
struct SpawnSlot {
    morph::bridge::Bridge* bridge = nullptr;
    morph::exec::IExecutor* gui = nullptr;
    std::unique_ptr<morph::bridge::BridgeHandler<SubModel>> handler;
    std::atomic<bool> constructed{false};
};

inline SpawnSlot& spawnSlot() {
    static SpawnSlot slot;
    return slot;
}

int PingModel::execute(const Spawn& action) {
    auto& slot = spawnSlot();
    if (slot.bridge == nullptr) {
        throw std::logic_error{"Spawn executed before the test set spawnSlot().bridge"};
    }
    slot.handler = std::make_unique<morph::bridge::BridgeHandler<SubModel>>(*slot.bridge, slot.gui);
    slot.constructed.store(true);
    return action.value;
}

}  // namespace owner_test

BRIDGE_REGISTER_MODEL(owner_test::PingModel, "Owner_PingModel")
BRIDGE_REGISTER_ACTION(owner_test::PingModel, owner_test::Ping, "Owner_Ping")
BRIDGE_REGISTER_ACTION(owner_test::PingModel, owner_test::Spawn, "Owner_Spawn")
BRIDGE_REGISTER_MODEL(owner_test::SubModel, "Owner_SubModel")
BRIDGE_REGISTER_ACTION(owner_test::SubModel, owner_test::Sub, "Owner_Sub")
BRIDGE_REGISTER_MODEL(owner_test::KeyedModel, "Owner_KeyedModel")
BRIDGE_REGISTER_ACTION(owner_test::KeyedModel, owner_test::Tally, "Owner_Tally")
BRIDGE_REGISTER_MODEL(owner_test::FormModel, "Owner_FormModel")
BRIDGE_REGISTER_ACTION(owner_test::FormModel, owner_test::FormStep, "Owner_FormStep")

namespace {

using morph::backend::detail::BindRequest;
using morph::exec::detail::ModelId;
using morph::testing::OwnerProbeRecorder;
using morph::testing::pumpOwnerUntil;
using owner_test::Ping;
using owner_test::PingModel;
using owner_test::Pong;

/// A LocalBackend whose binds settle later, from a thread of the test's
/// choosing: the shape of a transport whose replies arrive off the owner.
class DeferredBindBackend : public morph::backend::LocalBackend {
public:
    explicit DeferredBindBackend(morph::exec::IExecutor& pool) : LocalBackend{pool} {}

    morph::async::Completion<ModelId> bindModel(BindRequest request, morph::exec::IExecutor& cbExec) override {
        auto [completion, promise] = morph::async::Completion<ModelId>::makeSettleable(&cbExec);
        // The instance is created now, on the owner; only the reply is held.
        auto local = LocalBackend::bindModel(std::move(request), cbExec);
        _held.push_back(Held{.id = morph::bridge::detail::takeSettled(local)->id, .promise = std::move(promise)});
        ++binds;
        return std::move(completion);
    }

    void deregisterModel(ModelId mid) override {
        released.push_back(mid.v);
        LocalBackend::deregisterModel(mid);
    }

    /// Settles every held bind from a thread that is not the owner.
    void settleFromAnotherThread(const std::exception_ptr& failure = nullptr) {
        auto held = std::exchange(_held, {});
        std::thread settler{[&held, failure] {
            for (auto& entry : held) {
                if (failure) {
                    entry.promise.reject(failure);
                } else {
                    entry.promise.resolve(entry.id);
                }
            }
        }};
        settler.join();
    }

    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): a test double the test reads directly.
    int binds = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes): a test double the test reads directly.
    std::vector<std::uint64_t> released;

private:
    struct Held {
        ModelId id;
        morph::async::Completion<ModelId>::Promise promise;
    };
    std::vector<Held> _held;
};

}  // namespace

// ── The bind rule ────────────────────────────────────────────────────────────

TEST_CASE("Bridge: a LocalBackend handler's first execute dispatches without pumping the owner",
          "[bridge][owner][bind]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;  // never pumped in this test
    morph::exec::MainThreadExecutor gui;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<PingModel> handler{bridge, &gui};

    // Bound before the constructor returned: the default bind settled inline.
    REQUIRE(handler.isBound());
    std::atomic<int> result{0};
    handler.execute(Ping{.value = 21}).then([&](const Pong& pong) { result.store(pong.value); });
    REQUIRE(morph::testing::pumpOwnerUntil(gui, [&] { return result.load() == 42; }));
}

TEST_CASE("Bridge: an execute made before the bind settles is held and dispatched on the owner",
          "[bridge][owner][bind]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto backend = std::make_unique<DeferredBindBackend>(pool);
    auto* deferred = backend.get();
    morph::bridge::Bridge bridge{std::move(backend), owner};
    morph::bridge::BridgeHandler<PingModel> handler{bridge, &owner};
    REQUIRE_FALSE(handler.isBound());

    std::atomic<int> result{0};
    handler.execute(Ping{.value = 5}).then([&](const Pong& pong) { result.store(pong.value); });
    REQUIRE(bridge.pendingCalls() == 1);

    OwnerProbeRecorder const recorder{owner.coreExecutor()};
    deferred->settleFromAnotherThread();
    REQUIRE(pumpOwnerUntil(owner, [&] { return result.load() == 10; }));
    // The reply settled on another thread and was applied inside an owner task.
    REQUIRE(recorder.allPosted("Bridge::applyBind"));
    REQUIRE(handler.isBound());
}

TEST_CASE("Bridge: a failed bind rejects the held call with the bind's error", "[bridge][owner][bind]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto backend = std::make_unique<DeferredBindBackend>(pool);
    auto* deferred = backend.get();
    morph::bridge::Bridge bridge{std::move(backend), owner};
    morph::bridge::BridgeHandler<PingModel> handler{bridge, &owner};

    std::optional<std::string> failure;
    handler.execute(Ping{.value = 5}).onError([&](const std::exception_ptr& err) {
        try {
            std::rethrow_exception(err);
        } catch (const std::exception& exc) {
            failure = exc.what();
        }
    });
    deferred->settleFromAnotherThread(std::make_exception_ptr(std::runtime_error{"register refused"}));
    REQUIRE(pumpOwnerUntil(owner, [&] { return failure.has_value(); }));
    REQUIRE(*failure == "register refused");
    REQUIRE(bridge.pendingCalls() == 0);
}

TEST_CASE("Bridge: a handler destroyed mid-bind rejects its held call and releases the late instance",
          "[bridge][owner][bind]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto backend = std::make_unique<DeferredBindBackend>(pool);
    auto* deferred = backend.get();
    morph::bridge::Bridge bridge{std::move(backend), owner};

    bool destroyedError = false;
    {
        morph::bridge::BridgeHandler<PingModel> handler{bridge, &owner};
        handler.execute(Ping{.value = 1}).onError([&](const std::exception_ptr& err) {
            try {
                std::rethrow_exception(err);
            } catch (const morph::backend::HandlerDestroyedError&) {
                destroyedError = true;
            } catch (...) {  // NOLINT(bugprone-empty-catch): any other error fails the REQUIRE below
            }
        });
    }
    REQUIRE(pumpOwnerUntil(owner, [&] { return destroyedError; }));
    REQUIRE(bridge.pendingCalls() == 0);

    // The reply lands after the handler is gone: its instance is released.
    deferred->settleFromAnotherThread();
    REQUIRE(pumpOwnerUntil(owner, [&] { return deferred->released.size() == 1; }));
}

TEST_CASE("Bridge: a handler constructed inside a running action posts its registration to the owner",
          "[bridge][owner][bind]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<PingModel> handler{bridge, &owner};
    auto& slot = owner_test::spawnSlot();
    slot.bridge = &bridge;
    slot.gui = &owner;

    OwnerProbeRecorder const recorder{owner.coreExecutor()};
    std::atomic<bool> spawned{false};
    handler.execute(owner_test::Spawn{.value = 1}).then([&](int) { spawned.store(true); });
    REQUIRE(pumpOwnerUntil(owner, [&] { return spawned.load() && slot.handler && slot.handler->isBound(); }));
    // Constructed on a pool thread; registered in an owner task.
    REQUIRE(recorder.allPosted("Bridge::registerHandler"));
    REQUIRE(recorder.allPosted("LocalBackend::bindModel"));

    std::atomic<int> sub{0};
    slot.handler->execute(owner_test::Sub{.value = 4}).then([&](int value) { sub.store(value); });
    REQUIRE(pumpOwnerUntil(owner, [&] { return sub.load() == 5; }));
    slot.handler.reset();
}

// ── Results: bridge-side work runs on the owner ──────────────────────────────

TEST_CASE("Bridge: the subscription fan-out of a result settled on a pool thread runs on the owner",
          "[bridge][owner][subscription]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<PingModel> handler{bridge, &owner};

    std::atomic<int> seen{0};
    handler.subscribe<Pong>([&](Pong pong) { seen.store(pong.value); });
    OwnerProbeRecorder const recorder{owner.coreExecutor()};
    handler.execute(Ping{.value = 3});
    REQUIRE(pumpOwnerUntil(owner, [&] { return seen.load() == 6; }));
    REQUIRE(recorder.allPosted("Bridge::publishResult"));
}

TEST_CASE("Bridge: a result the backend settles on the owner is published before execute returns",
          "[bridge][owner][subscription]") {
    // A backend whose strands run inline settles on the calling thread, which
    // is the owner here: the bridge-side work runs there at once rather than
    // as a later owner task. The owner is never pumped.
    morph::testing::InlineExecutor inlinePool;
    morph::testing::InlineExecutor gui;
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(inlinePool), owner};
    morph::bridge::BridgeHandler<PingModel> handler{bridge, &gui};

    int seen = 0;
    handler.subscribe<Pong>([&](Pong pong) { seen = pong.value; });
    handler.execute(Ping{.value = 3});
    REQUIRE(seen == 6);
}

// ── Owner-only verbs ─────────────────────────────────────────────────────────

TEST_CASE("Bridge: owner-only verbs run inside the owner's task, and are caught off it", "[bridge][owner]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<PingModel> handler{bridge, &owner};

    auto touchEveryVerb = [&bridge, &handler, &pool] {
        bridge.setDefaultSession(morph::session::Context{.principal = "alice"});
        static_cast<void>(bridge.defaultSession());
        bridge.setPrincipal(morph::session::Principal{.id = "alice", .roles = {}});
        static_cast<void>(bridge.currentPrincipal());
        bridge.setExecuteDeadline(std::chrono::milliseconds{0});
        static_cast<void>(bridge.executeDeadline());
        bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool));
        handler.execute(Ping{.value = 1});
    };
    char const* const sites[] = {"Bridge::setDefaultSession", "Bridge::defaultSession",     "Bridge::setPrincipal",
                                 "Bridge::currentPrincipal",  "Bridge::setExecuteDeadline", "Bridge::executeDeadline",
                                 "Bridge::switchBackend",     "Bridge::executeVia"};

    SECTION("posted to the owner from another thread, every body runs in an owner task") {
        OwnerProbeRecorder const recorder{owner.coreExecutor()};
        std::atomic<bool> ran{false};
        std::thread poster{[&] {
            owner.post([&] {
                touchEveryVerb();
                ran.store(true);
            });
        }};
        poster.join();
        REQUIRE(pumpOwnerUntil(owner, [&] { return ran.load(); }));
        for (char const* site : sites) {
            INFO(site);
            REQUIRE(recorder.allPosted(site));
        }
    }

    SECTION("called from another thread, every body is caught off the owner") {
        OwnerProbeRecorder const recorder{owner.coreExecutor()};
        std::thread caller{touchEveryVerb};
        caller.join();
        for (char const* site : sites) {
            INFO(site);
            auto const seen = recorder.at(site);
            REQUIRE_FALSE(seen.empty());
            REQUIRE_FALSE(seen.front().onOwner);
        }
        // The backend installed off the owner was told the wrong owner; put
        // one installed on the owner back before the recorder goes.
        bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool));
        owner.runFor(std::chrono::milliseconds{5});
    }
}

TEST_CASE("BridgeHandler: teardown runs on the owner, and a destruction off it is caught", "[bridge][owner]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};

    SECTION("destroyed in an owner task") {
        auto handler = std::make_unique<morph::bridge::BridgeHandler<PingModel>>(bridge, &owner);
        OwnerProbeRecorder const recorder{owner.coreExecutor()};
        owner.post([&handler] { handler.reset(); });
        REQUIRE(pumpOwnerUntil(owner, [&] { return handler == nullptr; }));
        REQUIRE(recorder.allPosted("BridgeHandler::~BridgeHandler"));
        REQUIRE(recorder.allPosted("Bridge::deregisterHandler"));
    }

    SECTION("destroyed on another thread") {
        auto handler = std::make_unique<morph::bridge::BridgeHandler<PingModel>>(bridge, &owner);
        OwnerProbeRecorder const recorder{owner.coreExecutor()};
        std::thread destroyer{[&handler] { handler.reset(); }};
        destroyer.join();
        auto const seen = recorder.at("BridgeHandler::~BridgeHandler");
        REQUIRE(seen.size() == 1);
        REQUIRE_FALSE(seen.front().onOwner);
    }

    SECTION("destroyed after its bridge, on the owner, deregisters nothing") {
        auto ownBridge =
            std::make_unique<morph::bridge::Bridge>(std::make_unique<morph::backend::LocalBackend>(pool), owner);
        auto handler = std::make_unique<morph::bridge::BridgeHandler<PingModel>>(*ownBridge, &owner);
        ownBridge.reset();
        OwnerProbeRecorder const recorder{owner.coreExecutor()};
        handler.reset();
        REQUIRE(recorder.count("Bridge::deregisterHandler") == 0);
    }
}

#ifndef NDEBUG
TEST_CASE("BridgeHandler: a guiExec other than the owner is checked once, where it runs its tasks",
          "[bridge][owner]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    OwnerProbeRecorder const recorder{owner.coreExecutor()};

    SECTION("the owner itself needs no check") {
        morph::bridge::BridgeHandler<PingModel> const handler{bridge, &owner};
        owner.runFor(std::chrono::milliseconds{5});
        REQUIRE(recorder.count("BridgeHandler::guiExec") == 0);
    }

    SECTION("another executor is checked inside one of its own tasks") {
        morph::exec::MainThreadExecutor gui;
        morph::bridge::BridgeHandler<PingModel> const handler{bridge, &gui};
        REQUIRE(pumpOwnerUntil(gui, [&] { return recorder.count("BridgeHandler::guiExec") == 1; }));
        REQUIRE_FALSE(recorder.at("BridgeHandler::guiExec").front().onOwner);
    }
}
#endif

// ── Backends below the bridge ────────────────────────────────────────────────

TEST_CASE("SynchronousBackendAdapter: a bind registered from inside an action is issued on the owner",
          "[bridge][owner][adapter]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::ThreadPoolExecutor control{1};
    morph::exec::MainThreadExecutor owner;
    auto inner = std::make_shared<morph::backend::LocalBackend>(pool);
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SynchronousBackendAdapter>(inner, control), owner};
    morph::bridge::BridgeHandler<PingModel> handler{bridge, &owner};
    auto& slot = owner_test::spawnSlot();
    slot.bridge = &bridge;
    slot.gui = &owner;

    OwnerProbeRecorder const recorder{owner.coreExecutor()};
    std::atomic<bool> spawned{false};
    handler.execute(owner_test::Spawn{.value = 1}).then([&](int) { spawned.store(true); });
    REQUIRE(pumpOwnerUntil(owner, [&] { return spawned.load() && slot.handler && slot.handler->isBound(); }));
    REQUIRE(recorder.allPosted("SynchronousBackendAdapter::bindModel"));
    slot.handler.reset();
    slot.constructed.store(false);
}

TEST_CASE("SimulatedRemoteBackend: a bind registered from inside an action is issued on the owner",
          "[bridge][owner][remote]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    morph::exec::MainThreadExecutor owner;
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server), owner};
    morph::bridge::BridgeHandler<PingModel> handler{bridge, &owner};
    auto& slot = owner_test::spawnSlot();
    slot.bridge = &bridge;
    slot.gui = &owner;

    OwnerProbeRecorder const recorder{owner.coreExecutor()};
    std::atomic<bool> spawned{false};
    handler.execute(owner_test::Spawn{.value = 1}).then([&](int) { spawned.store(true); });
    REQUIRE(pumpOwnerUntil(owner, [&] { return spawned.load() && slot.handler && slot.handler->isBound(); }));
    REQUIRE(recorder.allPosted("SimulatedRemoteBackend::bindModel"));
    slot.handler.reset();
    slot.constructed.store(false);
}

// ── Forms: continuations on the owner ────────────────────────────────────────

TEST_CASE("SectionSet and FlowSession: a result settled on a pool thread is captured on the owner", "[forms][owner]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<owner_test::FormModel> handler{bridge, &owner};

    OwnerProbeRecorder const recorder{owner.coreExecutor()};
    morph::flows::FlowSession<owner_test::FormModel, owner_test::FormStep> flow{handler};
    flow.set<&owner_test::FormStep::value>(4);
    REQUIRE(pumpOwnerUntil(owner, [&] { return flow.ready(); }));
    REQUIRE(recorder.allPosted("FlowSession::captureResult"));
}

// ── Every other body a removed lock guarded ──────────────────────────────────

// One task posted from a thread that is not the owner drives each backend's
// remaining owner-only bodies: the bridge's attach, a backend's execute,
// session and cancel bookkeeping, and a SectionSet's draft and result
// capture. Each must show up in the recorder as run inside the owner's task.
TEST_CASE("Bridge and its backends: the remaining owner-only bodies run in the owner's task when posted",
          "[bridge][owner]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;

    auto postFromAnotherThread = [&owner](std::function<void()> body) {
        std::atomic<bool> ran{false};
        std::thread poster{[&] {
            owner.post([&] {
                body();
                ran.store(true);
            });
        }};
        poster.join();
        REQUIRE(pumpOwnerUntil(owner, [&] { return ran.load(); }));
    };

    SECTION("LocalBackend, a shared handler's attach and a SectionSet") {
        morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
        morph::bridge::BridgeHandler<owner_test::KeyedModel, morph::bridge::AllowShared> keyed{bridge, &owner};
        morph::bridge::BridgeHandler<owner_test::FormModel> form{bridge, &owner};
        using FormSection = morph::forms::Section<owner_test::FormStep, "Form">;
        morph::forms::SectionSet<owner_test::FormModel, FormSection> sections{form};

        OwnerProbeRecorder const recorder{owner.coreExecutor()};
        std::atomic<int> tally{0};
        postFromAnotherThread([&] {
            keyed.attach(7);
            keyed.execute(owner_test::Tally{.value = 3}).then([&](int value) { tally.store(value); });
            sections.set<&owner_test::FormStep::value>(2);
        });
        REQUIRE(pumpOwnerUntil(
            owner, [&] { return tally.load() == 3 && sections.resolved("Owner_FormStep.total").has_value(); }));
        postFromAnotherThread([&] { bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool)); });

        for (char const* site : {"Bridge::attachHandler", "LocalBackend::bindModel", "LocalBackend::executeInto",
                                 "LocalBackend::cancelPending", "SectionSet::set", "SectionSet::captureResult"}) {
            INFO(site);
            REQUIRE(recorder.allPosted(site));
        }
    }

    SECTION("SimulatedRemoteBackend") {
        auto server = std::make_shared<morph::backend::RemoteServer>(pool);
        morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server), owner};
        morph::bridge::BridgeHandler<PingModel> handler{bridge, &owner};

        OwnerProbeRecorder const recorder{owner.coreExecutor()};
        std::atomic<int> pong{0};
        postFromAnotherThread([&] {
            bridge.setDefaultSession(morph::session::Context{.principal = "alice"});
            handler.execute(Ping{.value = 5}).then([&](Pong value) { pong.store(value.value); });
        });
        REQUIRE(pumpOwnerUntil(owner, [&] { return pong.load() == 10; }));
        postFromAnotherThread([&] { bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool)); });

        for (char const* site : {"SimulatedRemoteBackend::setSession", "SimulatedRemoteBackend::execute",
                                 "SimulatedRemoteBackend::cancelPending"}) {
            INFO(site);
            REQUIRE(recorder.allPosted(site));
        }
    }

    SECTION("SynchronousBackendAdapter") {
        morph::exec::ThreadPoolExecutor control{1};
        auto inner = std::make_shared<morph::backend::LocalBackend>(pool);
        morph::bridge::Bridge bridge{std::make_unique<morph::backend::SynchronousBackendAdapter>(inner, control),
                                     owner};
        morph::bridge::BridgeHandler<PingModel> handler{bridge, &owner};
        REQUIRE(pumpOwnerUntil(owner, [&] { return handler.isBound(); }));

        OwnerProbeRecorder const recorder{owner.coreExecutor()};
        postFromAnotherThread([&] { bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool)); });
        REQUIRE(recorder.allPosted("SynchronousBackendAdapter::cancelPending"));
    }
}
