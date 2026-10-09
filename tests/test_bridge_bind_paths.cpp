// SPDX-License-Identifier: Apache-2.0

// The Bridge's bind machinery from the outside: a bind that is still in flight
// when something else happens to its binding — a newer bind, a backend switch,
// a reconnect, the handler or the bridge going away — and the late reply that
// follows. Every test drives a backend whose bind and promote replies the test
// settles by hand, so each ordering is exact rather than a race.

#include <algorithm>
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
#include <morph/core/logger.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "bind_support.hpp"
#include "test_support.hpp"

// Model, action and result types need external linkage for glaze reflection and
// for the BRIDGE_* specialisations.
// NOLINTBEGIN(misc-use-internal-linkage)
struct BbpLoad {
    std::int64_t id = 0;
};
struct BbpMake {
    int unused = 0;
};
struct BbpPlain {
    int value = 0;
};
struct BbpValue {
    int value = 0;
};
struct BbpMade {
    std::int64_t id = 0;
};

struct BbpModel {
    int loaded = 0;

    BbpValue execute(const BbpLoad& act) {
        loaded = static_cast<int>(act.id);
        return {.value = loaded};
    }
    BbpMade execute(const BbpMake& /*act*/) { return {.id = 4242}; }
    [[nodiscard]] BbpValue execute(const BbpPlain& act) const { return {.value = act.value + loaded}; }
};

BRIDGE_REGISTER_MODEL(BbpModel, "BBP_Model")
BRIDGE_REGISTER_ACTION(BbpModel, BbpLoad, "BBP_Load")
BRIDGE_REGISTER_ACTION(BbpModel, BbpMake, "BBP_Make")
BRIDGE_REGISTER_ACTION(BbpModel, BbpPlain, "BBP_Plain")
BRIDGE_MODEL_KEY(BbpModel, BbpLoad, &BbpLoad::id);
BRIDGE_KEY_FROM_RESULT(BbpMake, &BbpMade::id);
// NOLINTEND(misc-use-internal-linkage)

namespace {

using morph::backend::detail::BindRequest;
using morph::bridge::AllowShared;
using morph::bridge::Bridge;
using morph::bridge::BridgeHandler;
using morph::exec::detail::ModelId;
using morph::testing::pumpOwnerUntil;

using morph::testing::GateBackend;
using Reply = morph::testing::GateReply;

/// What an error carried, by its dynamic type or message.
struct Failure {
    std::string what;
    bool settled = false;
};

template <typename R>
void recordFailure(morph::async::Completion<R> completion, Failure& out) {
    std::move(completion).then([](const R&) {}).onError([&out](const std::exception_ptr& err) {
        try {
            std::rethrow_exception(err);
        } catch (const std::exception& exc) {
            out.what = exc.what();
        }
        out.settled = true;
    });
}

struct Rig {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    GateBackend* gate = nullptr;
    std::unique_ptr<Bridge> bridge;

    Rig() {
        auto backend = std::make_unique<GateBackend>(pool);
        gate = backend.get();
        bridge = std::make_unique<Bridge>(std::move(backend), owner);
    }
};

}  // namespace

// ── A backend switch while a bind is in flight ───────────────────────────────

TEST_CASE("Bridge::switchBackend rejects the operation an unattached shared handler's bind was running",
          "[bridge][bind][switch]") {
    Rig rig;
    BridgeHandler<BbpModel, AllowShared> handler{*rig.bridge, &rig.owner};

    Failure failure;
    recordFailure(handler.execute(BbpLoad{.id = 7}), failure);
    REQUIRE(rig.gate->heldBinds() == 1);

    // The attach is still in flight; the new backend answers at once, and the
    // old reply is superseded.
    auto next = std::make_unique<GateBackend>(rig.pool);
    next->bindMode = Reply::Inline;
    rig.bridge->switchBackend(std::move(next));

    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return failure.settled; }));
    REQUIRE(rig.bridge->pendingCalls() == 0);
}

TEST_CASE("Bridge::switchBackend drains a call held behind a bind the old backend never answered",
          "[bridge][bind][switch]") {
    Rig rig;
    BridgeHandler<BbpModel> handler{*rig.bridge, &rig.owner};
    REQUIRE_FALSE(handler.isBound());

    std::atomic<int> result{-1};
    handler.execute(BbpPlain{.value = 3}).then([&](const BbpValue& val) { result.store(val.value); });

    auto next = std::make_unique<GateBackend>(rig.pool);
    next->bindMode = Reply::Inline;
    rig.bridge->switchBackend(std::move(next));

    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return result.load() == 3; }));
    REQUIRE(handler.isBound());
}

TEST_CASE("Bridge::switchBackend skips a binding whose handler is already gone", "[bridge][bind][switch]") {
    Rig rig;
    {
        auto dropped = rig.bridge->registerHandler<BbpModel>();
        REQUIRE_FALSE(Bridge::isBound(dropped));
    }
    BridgeHandler<BbpModel> const handler{*rig.bridge, &rig.owner};

    auto next = std::make_unique<GateBackend>(rig.pool);
    next->bindMode = Reply::Inline;
    rig.bridge->switchBackend(std::move(next));
    REQUIRE(handler.isBound());
}

TEST_CASE("Bridge::switchBackend rolls back an in-flight bind that settles after the failure",
          "[bridge][bind][switch]") {
    Rig rig;
    BridgeHandler<BbpModel> const first{*rig.bridge, &rig.owner};
    BridgeHandler<BbpModel> const second{*rig.bridge, &rig.owner};
    rig.gate->bindMode = Reply::Inline;
    BridgeHandler<BbpModel> const bound{*rig.bridge, &rig.owner};
    REQUIRE(bound.isBound());

    // The new backend holds the first bind's reply and refuses the second
    // outright: the held reply, when it lands, releases its instance.
    struct HoldThenRefuse : GateBackend {
        using GateBackend::GateBackend;
        morph::async::Completion<ModelId> bindModel(BindRequest request, morph::exec::IExecutor& cbExec) override {
            if (calls++ == 0) {
                return GateBackend::bindModel(std::move(request), cbExec);
            }
            throw std::runtime_error{"second bind refused"};
        }
        int calls = 0;
    };
    auto next = std::make_shared<HoldThenRefuse>(rig.pool);
    REQUIRE_THROWS_AS(rig.bridge->switchBackend(std::static_pointer_cast<morph::backend::detail::IBackend>(next)),
                      std::runtime_error);
    REQUIRE(next->heldBinds() == 1);
    next->resolveBind();
    rig.owner.runFor(std::chrono::milliseconds{5});
    REQUIRE(*next->releasedCounter() == 1);
}

// ── The bridge's destruction ─────────────────────────────────────────────────

TEST_CASE("Bridge destruction rejects a call still waiting for its bind", "[bridge][bind][teardown]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto backend = std::make_unique<GateBackend>(pool);
    auto bridge = std::make_unique<Bridge>(std::move(backend), owner);
    auto binding = bridge->registerHandler<BbpModel>();

    Failure failure;
    recordFailure(bridge->executeVia<BbpModel, BbpPlain>(binding, BbpPlain{.value = 1}, &owner), failure);
    bridge.reset();
    REQUIRE(pumpOwnerUntil(owner, [&] { return failure.settled; }));
    // The bridge's own error, not the one a destroyed handler gives: the
    // binding is still alive, so the reason is that the bridge went away.
    REQUIRE(failure.what == morph::backend::BridgeDestroyedError{}.what());
}

TEST_CASE("A bind reply landing after the bridge is gone releases its instance on a backend the caller kept",
          "[bridge][bind][teardown]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto kept = std::make_shared<GateBackend>(pool);
    auto bridge = std::make_unique<Bridge>(std::make_unique<GateBackend>(pool), owner);
    bridge->switchBackend(std::static_pointer_cast<morph::backend::detail::IBackend>(kept));
    auto binding = bridge->registerHandler<BbpModel>();
    REQUIRE(kept->heldBinds() == 1);

    bridge.reset();
    kept->resolveBind();
    owner.runFor(std::chrono::milliseconds{5});
    REQUIRE(*kept->releasedCounter() == 1);
}

TEST_CASE("A bind failure landing after the bridge is gone touches nothing", "[bridge][bind][teardown]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto kept = std::make_shared<GateBackend>(pool);
    auto bridge = std::make_unique<Bridge>(std::make_unique<GateBackend>(pool), owner);
    bridge->switchBackend(std::static_pointer_cast<morph::backend::detail::IBackend>(kept));
    auto binding = bridge->registerHandler<BbpModel>();

    bridge.reset();
    kept->rejectBind();
    owner.runFor(std::chrono::milliseconds{5});
    REQUIRE(*kept->releasedCounter() == 0);
}

// ── A bind that throws, fails, or is superseded ──────────────────────────────

TEST_CASE("A bind that throws from the backend is recorded and rejects the held call", "[bridge][bind]") {
    Rig rig;
    rig.gate->bindMode = Reply::Throw;
    BridgeHandler<BbpModel> handler{*rig.bridge, &rig.owner};
    REQUIRE_FALSE(handler.isBound());

    Failure failure;
    recordFailure(handler.execute(BbpPlain{.value = 1}), failure);
    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return failure.settled; }));
    REQUIRE(failure.what == "bind threw");
}

TEST_CASE("A bind reply from a backend the bridge has switched away from releases its instance",
          "[bridge][bind][switch]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto old = std::make_shared<GateBackend>(pool);
    Bridge bridge{std::make_unique<GateBackend>(pool), owner};
    bridge.switchBackend(std::static_pointer_cast<morph::backend::detail::IBackend>(old));
    BridgeHandler<BbpModel> const handler{bridge, &owner};
    REQUIRE(old->heldBinds() == 1);

    auto next = std::make_unique<GateBackend>(pool);
    next->bindMode = Reply::Inline;
    bridge.switchBackend(std::move(next));
    REQUIRE(handler.isBound());

    // The old bind's reply is superseded by the switch: its instance goes back
    // to the backend that issued it, and the binding keeps the new one.
    auto const bound = handler.binding()->currentId.load();
    old->resolveBind();
    owner.runFor(std::chrono::milliseconds{5});
    REQUIRE(*old->releasedCounter() == 1);
    REQUIRE(handler.binding()->currentId.load() == bound);
}

TEST_CASE("A reconnect supersedes the bind a keyed call was waiting on", "[bridge][bind][reconnect]") {
    Rig rig;
    BridgeHandler<BbpModel, AllowShared> handler{*rig.bridge, &rig.owner};

    Failure failure;
    recordFailure(handler.execute(BbpLoad{.id = 5}), failure);
    REQUIRE(rig.gate->heldBinds() == 1);
    handler.attach(5);  // waits behind the keyed call's bind

    // An attached binding is rebound on reconnect; this one has no key yet and
    // is skipped, so settle it first to give it one.
    rig.gate->resolveBind();
    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return rig.gate->heldBinds() == 0 && handler.isBound(); }));

    Failure second;
    recordFailure(handler.execute(BbpLoad{.id = 6}), second);
    REQUIRE(rig.gate->heldBinds() == 1);
    rig.gate->fireReconnect();
    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return second.settled; }));
}

// ── Held calls ───────────────────────────────────────────────────────────────

TEST_CASE("A held call whose deadline expired before the bind settled is abandoned, not dispatched",
          "[bridge][bind][deadline]") {
    Rig rig;
    rig.bridge->setExecuteDeadline(std::chrono::milliseconds{20});
    BridgeHandler<BbpModel> handler{*rig.bridge, &rig.owner};

    Failure failure;
    recordFailure(handler.execute(BbpPlain{.value = 1}), failure);
    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return failure.settled; }));

    rig.gate->resolveBind();
    rig.owner.runFor(std::chrono::milliseconds{5});
    REQUIRE(rig.bridge->pendingCalls() == 0);
}

TEST_CASE("A held call whose dispatch throws is rejected rather than thrown", "[bridge][bind]") {
    Rig rig;
    BridgeHandler<BbpModel> handler{*rig.bridge, &rig.owner};
    rig.gate->throwOnExecute = true;

    Failure failure;
    recordFailure(handler.execute(BbpPlain{.value = 1}), failure);
    rig.gate->resolveBind();
    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return failure.settled; }));
    REQUIRE(failure.what == "dispatch threw");
    REQUIRE(rig.bridge->pendingCalls() == 0);
}

// ── Promotion ────────────────────────────────────────────────────────────────

TEST_CASE("A promote settling after the bridge is gone is ignored", "[bridge][promote][teardown]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto kept = std::make_shared<GateBackend>(pool);
    kept->bindMode = Reply::Inline;
    auto bridge = std::make_unique<Bridge>(std::make_unique<GateBackend>(pool), owner);
    bridge->switchBackend(std::static_pointer_cast<morph::backend::detail::IBackend>(kept));
    {
        BridgeHandler<BbpModel, AllowShared> handler{*bridge, &owner};
        std::atomic<bool> done{false};
        handler.execute(BbpMake{}).then([&](const BbpMade&) { done.store(true); });
        REQUIRE(pumpOwnerUntil(owner, [&] { return done.load(); }));
        REQUIRE(kept->heldPromotes() == 1);
        bridge.reset();
        kept->resolvePromote();
        owner.runFor(std::chrono::milliseconds{5});
    }
    SUCCEED();
}

TEST_CASE("A promote failure after the bridge is gone is ignored", "[bridge][promote][teardown]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto kept = std::make_shared<GateBackend>(pool);
    kept->bindMode = Reply::Inline;
    auto bridge = std::make_unique<Bridge>(std::make_unique<GateBackend>(pool), owner);
    bridge->switchBackend(std::static_pointer_cast<morph::backend::detail::IBackend>(kept));
    BridgeHandler<BbpModel, AllowShared> handler{*bridge, &owner};
    std::atomic<bool> done{false};
    handler.execute(BbpMake{}).then([&](const BbpMade&) { done.store(true); });
    REQUIRE(pumpOwnerUntil(owner, [&] { return done.load(); }));
    REQUIRE(kept->heldPromotes() == 1);
    bridge.reset();
    kept->rejectPromote();
    owner.runFor(std::chrono::milliseconds{5});
    SUCCEED();
}

TEST_CASE("A promote that lands after the backend was switched keeps the key the new backend gave",
          "[bridge][promote][switch]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto old = std::make_shared<GateBackend>(pool);
    old->bindMode = Reply::Inline;
    Bridge bridge{std::make_unique<GateBackend>(pool), owner};
    bridge.switchBackend(std::static_pointer_cast<morph::backend::detail::IBackend>(old));
    BridgeHandler<BbpModel, AllowShared> handler{bridge, &owner};
    std::atomic<bool> done{false};
    handler.execute(BbpMake{}).then([&](const BbpMade&) { done.store(true); });
    REQUIRE(pumpOwnerUntil(owner, [&] { return done.load(); }));
    REQUIRE(old->heldPromotes() == 1);
    REQUIRE(bridge.bindingPrimary(handler.binding()).empty());

    auto next = std::make_unique<GateBackend>(pool);
    next->bindMode = Reply::Inline;
    bridge.switchBackend(std::move(next));
    old->resolvePromote();
    owner.runFor(std::chrono::milliseconds{5});
    REQUIRE(bridge.bindingPrimary(handler.binding()).empty());
}

TEST_CASE("A promote that lands after an attach already keyed the binding keeps the attach's key",
          "[bridge][promote]") {
    Rig rig;
    rig.gate->bindMode = Reply::Inline;
    BridgeHandler<BbpModel, AllowShared> handler{*rig.bridge, &rig.owner};
    std::atomic<bool> done{false};
    handler.execute(BbpMake{}).then([&](const BbpMade&) { done.store(true); });
    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return done.load(); }));
    REQUIRE(rig.gate->heldPromotes() == 1);

    handler.attach(9);
    REQUIRE(rig.bridge->bindingPrimary(handler.binding()) == "9");
    rig.gate->resolvePromote();
    rig.owner.runFor(std::chrono::milliseconds{5});
    REQUIRE(rig.bridge->bindingPrimary(handler.binding()) == "9");
}

TEST_CASE("A failed promote leaves the binding unkeyed and logs the backend's reason", "[bridge][promote]") {
    std::mutex logMtx;
    std::vector<std::string> logged;
    morph::log::ScopedLoggerOverride const guard{
        [&](morph::log::LogLevel, std::string_view msg) {
            std::scoped_lock const lock{logMtx};
            logged.emplace_back(msg);
        },
        morph::log::LogLevel::error,
    };
    Rig rig;
    rig.gate->bindMode = Reply::Inline;
    BridgeHandler<BbpModel, AllowShared> handler{*rig.bridge, &rig.owner};
    std::atomic<bool> done{false};
    handler.execute(BbpMake{}).then([&](const BbpMade&) { done.store(true); });
    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return done.load(); }));
    rig.gate->rejectPromote();
    REQUIRE(pumpOwnerUntil(rig.owner, [&] {
        std::scoped_lock const lock{logMtx};
        return std::ranges::any_of(logged, [](const std::string& line) {
            return line.contains("promotion of 'BBP_Model' failed: promote refused");
        });
    }));
    REQUIRE(rig.bridge->bindingPrimary(handler.binding()).empty());
}

TEST_CASE("A result-keyed call whose handler is destroyed mid-bind is rejected", "[bridge][bind]") {
    Rig rig;
    Failure failure;
    {
        BridgeHandler<BbpModel, AllowShared> handler{*rig.bridge, &rig.owner};
        recordFailure(handler.execute(BbpMake{}), failure);
        REQUIRE(rig.gate->heldBinds() == 1);
    }
    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return failure.settled; }));
}

TEST_CASE("A keyed call whose bind is refused is rejected with the bind's error", "[bridge][bind]") {
    Rig rig;
    BridgeHandler<BbpModel, AllowShared> handler{*rig.bridge, &rig.owner};
    Failure failure;
    recordFailure(handler.execute(BbpLoad{.id = 3}), failure);
    rig.gate->rejectBind("attach refused");
    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return failure.settled; }));
    REQUIRE(failure.what == "attach refused");
}

TEST_CASE("A keyed call whose attach settles with no instance is rejected as not bound", "[bridge][bind]") {
    Rig rig;
    BridgeHandler<BbpModel, AllowShared> handler{*rig.bridge, &rig.owner};
    Failure failure;
    recordFailure(handler.execute(BbpLoad{.id = 3}), failure);
    REQUIRE(rig.gate->heldBinds() == 1);

    GateBackend::resolveFirst(*rig.gate->bindStash(), ModelId{});
    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return failure.settled; }));
    REQUIRE(failure.what == "handler not bound");
    REQUIRE(rig.bridge->pendingCalls() == 0);
}

// ── Binds that never produce an outcome, and the calls queued behind them ────

TEST_CASE("A backend that returns an empty bind completion fails the bind", "[bridge][bind]") {
    Rig rig;
    rig.gate->bindMode = Reply::Empty;
    BridgeHandler<BbpModel> handler{*rig.bridge, &rig.owner};

    Failure failure;
    recordFailure(handler.execute(BbpPlain{.value = 1}), failure);
    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return failure.settled; }));
    REQUIRE(failure.what == "empty bind completion");
}

TEST_CASE("A handler destroyed with attaches, keyed calls and creating calls queued rejects every one of them",
          "[bridge][bind]") {
    Rig rig;
    Failure keyed;
    Failure queuedKeyed;
    Failure creating;
    {
        BridgeHandler<BbpModel, AllowShared> handler{*rig.bridge, &rig.owner};
        recordFailure(handler.execute(BbpLoad{.id = 1}), keyed);
        REQUIRE(rig.gate->heldBinds() == 1);
        recordFailure(handler.execute(BbpLoad{.id = 2}), queuedKeyed);
        recordFailure(handler.execute(BbpMake{}), creating);
        handler.attach(3);
    }
    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return keyed.settled && queuedKeyed.settled && creating.settled; }));
    REQUIRE(rig.bridge->pendingCalls() == 0);
}

TEST_CASE("Attaching to the key a binding already holds is a no-op", "[bridge][bind]") {
    Rig rig;
    rig.gate->bindMode = Reply::Inline;
    BridgeHandler<BbpModel, AllowShared> handler{*rig.bridge, &rig.owner};
    handler.attach(4);
    REQUIRE(handler.isBound());
    auto const id = handler.binding()->currentId.load();
    handler.attach(4);
    REQUIRE(handler.binding()->currentId.load() == id);
}

TEST_CASE("A keyed call re-attaches a binding whose reconnect rebind failed", "[bridge][bind][reconnect]") {
    Rig rig;
    rig.gate->bindMode = Reply::Inline;
    BridgeHandler<BbpModel, AllowShared> handler{*rig.bridge, &rig.owner};
    handler.attach(5);
    REQUIRE(handler.isBound());

    // The reconnect clears the id and its rebind throws: the binding keeps its
    // key but has no instance, and nothing is in flight.
    rig.gate->bindMode = Reply::Throw;
    rig.gate->fireReconnect();
    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return !handler.isBound(); }));

    rig.gate->bindMode = Reply::Inline;
    std::atomic<int> result{-1};
    handler.execute(BbpLoad{.id = 5}).then([&](const BbpValue& val) { result.store(val.value); });
    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return result.load() == 5; }));
    REQUIRE(handler.isBound());

    // The same, through a plain attach.
    rig.gate->bindMode = Reply::Throw;
    rig.gate->fireReconnect();
    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return !handler.isBound(); }));
    rig.gate->bindMode = Reply::Inline;
    handler.attach(5);
    REQUIRE(handler.isBound());
}

// ── A backend switch while an attach is in flight ────────────────────────────

TEST_CASE("Bridge::switchBackend leaves an attach in flight on an unattached handler with nothing to reject",
          "[bridge][bind][switch]") {
    Rig rig;
    BridgeHandler<BbpModel, AllowShared> handler{*rig.bridge, &rig.owner};
    handler.attach(1);
    REQUIRE(rig.gate->heldBinds() == 1);

    auto next = std::make_unique<GateBackend>(rig.pool);
    next->bindMode = Reply::Inline;
    rig.bridge->switchBackend(std::move(next));
    REQUIRE_FALSE(handler.isBound());
}

TEST_CASE("Bridge::switchBackend rejects the keyed call whose re-attach was still in flight",
          "[bridge][bind][switch]") {
    Rig rig;
    rig.gate->bindMode = Reply::Inline;
    BridgeHandler<BbpModel, AllowShared> handler{*rig.bridge, &rig.owner};
    handler.attach(5);
    REQUIRE(handler.isBound());

    rig.gate->bindMode = Reply::Hold;
    Failure failure;
    recordFailure(handler.execute(BbpLoad{.id = 6}), failure);
    REQUIRE(rig.gate->heldBinds() == 1);

    auto next = std::make_unique<GateBackend>(rig.pool);
    next->bindMode = Reply::Inline;
    rig.bridge->switchBackend(std::move(next));
    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return failure.settled; }));
}

// ── Replies from a backend that no longer exists ─────────────────────────────

TEST_CASE("A bind reply from a backend destroyed by a switch binds nothing and releases nothing",
          "[bridge][bind][switch]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto first = std::make_unique<GateBackend>(pool);
    auto stash = first->bindStash();
    Bridge bridge{std::move(first), owner};
    BridgeHandler<BbpModel> const handler{bridge, &owner};
    REQUIRE(stash->size() == 1);

    auto next = std::make_unique<GateBackend>(pool);
    next->bindMode = Reply::Inline;
    bridge.switchBackend(std::move(next));
    REQUIRE(handler.isBound());

    GateBackend::resolveFirst(*stash);
    owner.runFor(std::chrono::milliseconds{5});
    REQUIRE(handler.isBound());
}

TEST_CASE("A rolled-back switch's held bind reply, landing after the backend is destroyed, releases nothing",
          "[bridge][bind][switch]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    Bridge bridge{std::make_unique<GateBackend>(pool), owner};
    bridge.switchBackend(std::make_unique<GateBackend>(pool));
    BridgeHandler<BbpModel> const first{bridge, &owner};
    BridgeHandler<BbpModel> const second{bridge, &owner};

    struct HoldThenRefuse : GateBackend {
        using GateBackend::GateBackend;
        morph::async::Completion<ModelId> bindModel(BindRequest request, morph::exec::IExecutor& cbExec) override {
            if (calls++ == 0) {
                return GateBackend::bindModel(std::move(request), cbExec);
            }
            throw std::runtime_error{"second bind refused"};
        }
        int calls = 0;
    };
    auto refusing = std::make_unique<HoldThenRefuse>(pool);
    auto stash = refusing->bindStash();
    REQUIRE_THROWS_AS(bridge.switchBackend(std::move(refusing)), std::runtime_error);
    REQUIRE(stash->size() == 1);

    GateBackend::resolveFirst(*stash);
    owner.runFor(std::chrono::milliseconds{5});
    SUCCEED();
}

// ── A handler constructed off the owner, then the bridge goes away ───────────

TEST_CASE("A handler registration posted to the owner is dropped when the bridge is gone first",
          "[bridge][bind][teardown]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto bridge = std::make_unique<Bridge>(std::make_unique<GateBackend>(pool), owner);

    std::unique_ptr<BridgeHandler<BbpModel>> handler;
    std::thread constructor{[&] { handler = std::make_unique<BridgeHandler<BbpModel>>(*bridge, &owner); }};
    constructor.join();

    bridge.reset();
    owner.runFor(std::chrono::milliseconds{5});
    handler.reset();
    SUCCEED();
}

// A superseded reply must stay superseded however many binds follow it: the
// count that marks it stale must never come back round to the value it carries.

TEST_CASE("An attach superseded by a switch stays superseded once a newer attach is issued",
          "[bridge][bind][switch]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto old = std::make_shared<GateBackend>(pool);
    Bridge bridge{std::make_unique<GateBackend>(pool), owner};
    bridge.switchBackend(std::static_pointer_cast<morph::backend::detail::IBackend>(old));
    BridgeHandler<BbpModel, AllowShared> handler{bridge, &owner};
    handler.attach(1);
    REQUIRE(old->heldBinds() == 1);

    auto next = std::make_unique<GateBackend>(pool);
    auto* nextGate = next.get();
    bridge.switchBackend(std::move(next));
    handler.attach(2);
    REQUIRE(nextGate->heldBinds() == 1);

    // The first attach's reply lands last: it is released, and binds nothing.
    old->resolveBind();
    owner.runFor(std::chrono::milliseconds{5});
    REQUIRE(*old->releasedCounter() == 1);
    REQUIRE_FALSE(handler.isBound());
    REQUIRE(bridge.bindingPrimary(handler.binding()).empty());
}

TEST_CASE("A bind superseded by a switch stays superseded once a reconnect issues another",
          "[bridge][bind][switch][reconnect]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto old = std::make_shared<GateBackend>(pool);
    Bridge bridge{std::make_unique<GateBackend>(pool), owner};
    bridge.switchBackend(std::static_pointer_cast<morph::backend::detail::IBackend>(old));
    BridgeHandler<BbpModel> const handler{bridge, &owner};
    REQUIRE(old->heldBinds() == 1);

    auto next = std::make_unique<GateBackend>(pool);
    auto* nextGate = next.get();
    bridge.switchBackend(std::move(next));
    REQUIRE(nextGate->heldBinds() == 1);
    nextGate->fireReconnect();
    REQUIRE(pumpOwnerUntil(owner, [&] { return nextGate->heldBinds() == 2; }));

    // The first bind's reply lands last: it is released, and binds nothing.
    old->resolveBind();
    owner.runFor(std::chrono::milliseconds{5});
    REQUIRE(*old->releasedCounter() == 1);
    REQUIRE_FALSE(handler.isBound());
}

TEST_CASE("A superseded bind that replies with no instance releases nothing", "[bridge][bind][switch]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    auto old = std::make_shared<GateBackend>(pool);
    Bridge bridge{std::make_unique<GateBackend>(pool), owner};
    bridge.switchBackend(std::static_pointer_cast<morph::backend::detail::IBackend>(old));
    BridgeHandler<BbpModel> const handler{bridge, &owner};
    REQUIRE(old->heldBinds() == 1);

    auto next = std::make_unique<GateBackend>(pool);
    next->bindMode = Reply::Inline;
    bridge.switchBackend(std::move(next));

    GateBackend::resolveFirst(*old->bindStash(), ModelId{});
    owner.runFor(std::chrono::milliseconds{5});
    REQUIRE(*old->releasedCounter() == 0);
}

// ── What a rejected or released operation leaves behind ──────────────────────

TEST_CASE("An attach held behind a bind is dropped, not issued, when its handler is destroyed",
          "[bridge][bind][teardown]") {
    Rig rig;
    // Outlives the handler: the keyed call is rejected with its destruction,
    // and that failure is delivered on the owner afterwards.
    Failure keyed;
    {
        BridgeHandler<BbpModel, AllowShared> handler{*rig.bridge, &rig.owner};
        recordFailure(handler.execute(BbpLoad{.id = 1}), keyed);
        REQUIRE(rig.gate->heldBinds() == 1);
        handler.attach(2);  // waits behind the keyed call's bind
    }
    rig.owner.runFor(std::chrono::milliseconds{5});
    // Only the keyed call's bind was ever issued: the attach was rejected with
    // the handler's destruction rather than run against a deregistered binding.
    REQUIRE(rig.gate->heldBinds() == 1);
}

TEST_CASE("Bridge::assignHandlerPrimary with an empty key promotes nothing", "[bridge][promote]") {
    Rig rig;
    rig.gate->bindMode = Reply::Inline;
    auto binding = rig.bridge->registerHandler<BbpModel>();
    REQUIRE(Bridge::isBound(binding));

    rig.bridge->assignHandlerPrimary<BbpModel>(binding, "");
    REQUIRE(rig.gate->heldPromotes() == 0);
    REQUIRE(rig.bridge->bindingPrimary(binding).empty());
}

TEST_CASE("Destroying one handler leaves the bridge tracking the others", "[bridge][bind][switch]") {
    Rig rig;
    rig.gate->bindMode = Reply::Inline;
    BridgeHandler<BbpModel> const survivor{*rig.bridge, &rig.owner};
    {
        BridgeHandler<BbpModel> const dropped{*rig.bridge, &rig.owner};
    }

    // A switch re-binds every handler the bridge still tracks: the survivor,
    // and only the survivor.
    auto next = std::make_unique<GateBackend>(rig.pool);
    auto* nextGate = next.get();
    rig.bridge->switchBackend(std::move(next));
    REQUIRE(nextGate->heldBinds() == 1);
}

TEST_CASE("A call made on the owner before a posted handler registration runs is held, not refused",
          "[bridge][bind]") {
    Rig rig;
    rig.gate->bindMode = Reply::Inline;

    // Constructed off the owner, so its registration is posted there.
    std::unique_ptr<BridgeHandler<BbpModel>> handler;
    std::thread constructor{[&] { handler = std::make_unique<BridgeHandler<BbpModel>>(*rig.bridge, &rig.owner); }};
    constructor.join();
    REQUIRE_FALSE(handler->isBound());

    std::atomic<int> result{-1};
    Failure failure;
    handler->execute(BbpPlain{.value = 3})
        .then([&](const BbpValue& val) { result.store(val.value); })
        .onError([&](const std::exception_ptr& err) {
            try {
                std::rethrow_exception(err);
            } catch (const std::exception& exc) {
                failure.what = exc.what();
            }
            failure.settled = true;
        });
    REQUIRE(pumpOwnerUntil(rig.owner, [&] { return result.load() != -1 || failure.settled; }));
    REQUIRE(failure.what.empty());
    REQUIRE(result.load() == 3);
}
