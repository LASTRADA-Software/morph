// SPDX-License-Identifier: Apache-2.0
//
// Tests for instance subscriptions (F3).
//
// `subscribe<R>(cb)` is keyed on the **result/state type** and fires whenever an
// `R` is produced on the instance the handler is attached to — by this handler,
// by another handler sharing the instance, or by another screen entirely. It
// replaces the reactive-draft mechanism (`set<&A::field>`, `reset<A>`, and an
// action-keyed `subscribe`), whose job a stateful model does better by holding
// the draft itself; see docs/planned/instance_subscriptions.md.
//
// The subscriber names *what it renders*, not what somebody else must call to
// produce it, so adding an action that also yields an `R` never breaks an
// existing subscriber.

#include <any>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <stdexcept>
#include <string>
#include <typeindex>

#include "test_support.hpp"

// ── Fixture: a stateful counter, so a subscription reports real shared state ──

/// The state type subscribers name. Produced by more than one action, which is
// Model, action and result types need **external** linkage: glaze's
// plain-aggregate reflection cannot see into an anonymous namespace, and the
// BRIDGE_REGISTER_* macros specialise templates at global scope.
// NOLINTBEGIN(misc-use-internal-linkage)
/// exactly the case result-keyed subscription exists to serve.
struct SubCounterState {
    std::int64_t value = 0;
};

/// A second, unrelated state type — used to prove types do not cross-talk.
struct SubLabelState {
    std::string text;
};

struct SubBump {
    std::int64_t id = 0;
    std::int64_t by = 0;
};

struct SubRead {
    std::int64_t id = 0;
};

struct SubLabel {
    std::int64_t id = 0;
};

struct SubExplode {
    std::int64_t id = 0;
};

struct SubCounterModel {
    std::int64_t value = 0;

    SubCounterState execute(const SubBump& act) {
        value += act.by;
        return {.value = value};
    }
    [[nodiscard]] SubCounterState execute(const SubRead& /*act*/) const { return {.value = value}; }
    [[nodiscard]] static SubLabelState execute(const SubLabel& /*act*/) { return {.text = "label"}; }
    static SubCounterState execute(const SubExplode& /*act*/) { throw std::runtime_error{"boom"}; }
};

BRIDGE_REGISTER_MODEL(SubCounterModel, "SUB_CounterModel")
BRIDGE_REGISTER_ACTION(SubCounterModel, SubBump, "SUB_Bump")
BRIDGE_REGISTER_ACTION(SubCounterModel, SubRead, "SUB_Read")
BRIDGE_REGISTER_ACTION(SubCounterModel, SubLabel, "SUB_Label")
BRIDGE_REGISTER_ACTION(SubCounterModel, SubExplode, "SUB_Explode")

BRIDGE_MODEL_KEY(SubCounterModel, SubBump, &SubBump::id);
BRIDGE_KEY_FROM(SubRead, &SubRead::id);
BRIDGE_KEY_FROM(SubLabel, &SubLabel::id);
BRIDGE_KEY_FROM(SubExplode, &SubExplode::id);
// NOLINTEND(misc-use-internal-linkage)

namespace {

using morph::bridge::AllowShared;
using morph::bridge::Bridge;
using morph::bridge::BridgeHandler;

/// Runs a completion to resolution, ignoring its value.
template <typename T>
void drain(morph::async::Completion<T> comp) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::move(comp).then([done](const T&) { done->store(true); }).onError([done](const std::exception_ptr&) {
        done->store(true);
    });
    REQUIRE(morph::testing::waitUntil([&] { return done->load(); }));
}

std::unique_ptr<morph::backend::detail::IBackend> makeLocal(morph::exec::IExecutor& pool) {
    return std::make_unique<morph::backend::LocalBackend>(pool);
}

}  // namespace

TEST_CASE("a subscriber hears results produced by its own handler", "[bridge][subscription]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};
    BridgeHandler<SubCounterModel, AllowShared> handler{bridge, &exec};

    std::int64_t seen = 0;
    int fires = 0;
    handler.subscribe<SubCounterState>([&](SubCounterState state) {
        seen = state.value;
        ++fires;
    });

    drain(handler.execute(SubBump{.id = 1, .by = 7}));
    REQUIRE(fires == 1);
    REQUIRE(seen == 7);
}

TEST_CASE("a subscriber hears another handler's work on the shared instance", "[bridge][subscription]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<SubCounterModel, AllowShared> watcher{bridge, &exec};
    BridgeHandler<SubCounterModel, AllowShared> actor{bridge, &exec};

    watcher.attach(10);
    std::int64_t seen = -1;
    watcher.subscribe<SubCounterState>([&](SubCounterState state) { seen = state.value; });

    // A different handler, on the same instance: the watcher does not need to
    // know that SubBump exists, only that SubCounterState is what it renders.
    drain(actor.execute(SubBump{.id = 10, .by = 3}));
    REQUIRE(seen == 3);
}

TEST_CASE("a subscriber hears nothing from a different instance", "[bridge][subscription]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<SubCounterModel, AllowShared> watcher{bridge, &exec};
    BridgeHandler<SubCounterModel, AllowShared> elsewhere{bridge, &exec};

    watcher.attach(20);
    bool fired = false;
    watcher.subscribe<SubCounterState>([&](SubCounterState) { fired = true; });

    drain(elsewhere.execute(SubBump{.id = 21, .by = 1}));
    REQUIRE_FALSE(fired);
}

TEST_CASE("a subscription follows its handler when it re-points", "[bridge][subscription]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<SubCounterModel, AllowShared> watcher{bridge, &exec};
    BridgeHandler<SubCounterModel, AllowShared> actor{bridge, &exec};

    watcher.attach(30);
    std::int64_t seen = -1;
    watcher.subscribe<SubCounterState>([&](SubCounterState state) { seen = state.value; });

    // "Tell me about the account I am looking at" must keep working when the
    // user switches accounts, so the subscription moves with the handler.
    watcher.attach(31);
    drain(actor.execute(SubBump{.id = 31, .by = 5}));
    REQUIRE(seen == 5);

    seen = -1;
    drain(actor.execute(SubBump{.id = 30, .by = 9}));
    REQUIRE(seen == -1);  // the instance it left behind is no longer its business
}

TEST_CASE("distinct result types do not interfere", "[bridge][subscription]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};
    BridgeHandler<SubCounterModel, AllowShared> handler{bridge, &exec};
    handler.attach(40);

    int counters = 0;
    int labels = 0;
    handler.subscribe<SubCounterState>([&](SubCounterState) { ++counters; });
    handler.subscribe<SubLabelState>([&](const SubLabelState&) { ++labels; });

    drain(handler.execute(SubBump{.id = 40, .by = 1}));
    REQUIRE(counters == 1);
    REQUIRE(labels == 0);

    drain(handler.execute(SubLabel{.id = 40}));
    REQUIRE(counters == 1);
    REQUIRE(labels == 1);
}

TEST_CASE("every action producing the type notifies the subscriber", "[bridge][subscription]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};
    BridgeHandler<SubCounterModel, AllowShared> handler{bridge, &exec};
    handler.attach(50);

    int fires = 0;
    handler.subscribe<SubCounterState>([&](SubCounterState) { ++fires; });

    drain(handler.execute(SubBump{.id = 50, .by = 1}));
    drain(handler.execute(SubRead{.id = 50}));  // a different action, same state type
    REQUIRE(fires == 2);
}

TEST_CASE("subscribing again replaces the previous callback", "[bridge][subscription]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};
    BridgeHandler<SubCounterModel, AllowShared> handler{bridge, &exec};
    handler.attach(60);

    int first = 0;
    int second = 0;
    handler.subscribe<SubCounterState>([&](SubCounterState) { ++first; });
    handler.subscribe<SubCounterState>([&](SubCounterState) { ++second; });

    drain(handler.execute(SubBump{.id = 60, .by = 1}));
    REQUIRE(first == 0);
    REQUIRE(second == 1);
}

TEST_CASE("unsubscribe stops further delivery", "[bridge][subscription]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};
    BridgeHandler<SubCounterModel, AllowShared> handler{bridge, &exec};
    handler.attach(70);

    int fires = 0;
    handler.subscribe<SubCounterState>([&](SubCounterState) { ++fires; });
    drain(handler.execute(SubBump{.id = 70, .by = 1}));
    REQUIRE(fires == 1);

    handler.unsubscribe<SubCounterState>();
    drain(handler.execute(SubBump{.id = 70, .by = 1}));
    REQUIRE(fires == 1);
}

TEST_CASE("a failed action notifies nobody", "[bridge][subscription]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};
    BridgeHandler<SubCounterModel, AllowShared> handler{bridge, &exec};
    handler.attach(80);

    bool fired = false;
    handler.subscribe<SubCounterState>([&](SubCounterState) { fired = true; });

    drain(handler.execute(SubExplode{.id = 80}));
    REQUIRE_FALSE(fired);
}

TEST_CASE("delivery stops once the subscribing handler is destroyed", "[bridge][subscription]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};
    BridgeHandler<SubCounterModel, AllowShared> actor{bridge, &exec};
    actor.attach(90);

    int fires = 0;
    {
        BridgeHandler<SubCounterModel, AllowShared> watcher{bridge, &exec};
        watcher.attach(90);
        watcher.subscribe<SubCounterState>([&](SubCounterState) { ++fires; });
        drain(actor.execute(SubBump{.id = 90, .by = 1}));
        REQUIRE(fires == 1);
    }

    drain(actor.execute(SubBump{.id = 90, .by = 1}));
    REQUIRE(fires == 1);
}

TEST_CASE("a private handler's results stay private", "[bridge][subscription]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<SubCounterModel, AllowShared> watcher{bridge, &exec};
    BridgeHandler<SubCounterModel> priv{bridge, &exec};

    watcher.attach(100);
    bool fired = false;
    watcher.subscribe<SubCounterState>([&](SubCounterState) { fired = true; });

    // The plain handler has its own instance, so nothing it does is on the
    // instance the watcher is attached to.
    drain(priv.execute(SubBump{.id = 100, .by = 1}));
    REQUIRE_FALSE(fired);
}

TEST_CASE("SubscriptionRegistry prunes a subscription whose handler was destroyed without unsubscribing",
          "[bridge][subscription][subscription-registry]") {
    // The Bridge-level "delivery stops once the subscribing handler is
    // destroyed" test above proves the *observable* behaviour (no more
    // callbacks fire), but a `BridgeHandler`'s destructor never calls
    // `unsubscribe()` -- deregisterHandler only removes the binding from
    // `Bridge::_handlers`, not from the subscription list -- so that test
    // cannot tell "the stale entry was actually erased" apart from "the
    // entry is still sitting there, silently skipped forever because its
    // weak_ptr is expired". Driving `SubscriptionRegistry` directly, with no
    // `Bridge`/`BridgeHandler`/action round trip at all, lets us assert the
    // erasure itself via `size()`.
    morph::bridge::detail::SubscriptionRegistry<morph::bridge::detail::HandlerBinding> registry;
    auto const type = std::type_index{typeid(SubCounterState)};

    {
        auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
        binding->currentId.store(123);
        registry.addSubscription(binding, type, [](const std::any&) {}, nullptr);
        REQUIRE(registry.size() == 1);
        // `binding` is destroyed here, without any call to removeSubscription
        // -- exactly the "handler destroyed without unsubscribing" case.
    }

    // The stale entry is not erased just by its binding dying: nothing prunes
    // the list until the next publishResult() walks it.
    REQUIRE(registry.size() == 1);

    registry.publishResult(morph::exec::detail::ModelId{123}, type, std::any{});
    REQUIRE(registry.size() == 0);
}

TEST_CASE("addSubscription scans past an expired entry and a different binding without disturbing them",
          "[bridge][subscription][subscription-registry]") {
    // addSubscription's dedup loop (`owner && owner.get() == binding.get() &&
    // entry.type == type`) has three sub-conditions; only `entry.type ==
    // type` was exercised both ways before this test. This drives the other
    // two: `owner` false (an expired binding still sitting in the vector --
    // addSubscription itself never prunes, only publishResult does) and
    // `owner.get() == binding.get()` false (a different, still-alive
    // binding occupying a slot the loop must scan past).
    morph::bridge::detail::SubscriptionRegistry<morph::bridge::detail::HandlerBinding> registry;
    auto const typeA = std::type_index{typeid(SubCounterState)};

    {
        auto expired = std::make_shared<morph::bridge::detail::HandlerBinding>();
        registry.addSubscription(expired, typeA, [](const std::any&) {}, nullptr);
        // `expired` dies here, without removeSubscription -- its slot stays
        // in the vector (addSubscription never prunes) but its weak_ptr is
        // now expired, so the next addSubscription's scan must step over it
        // via the `owner` false arm.
    }
    REQUIRE(registry.size() == 1);

    auto bindingA = std::make_shared<morph::bridge::detail::HandlerBinding>();
    registry.addSubscription(bindingA, typeA, [](const std::any&) {}, nullptr);
    REQUIRE(registry.size() == 2);  // scanned past the expired slot, appended a new one

    // A second, live binding subscribing to the *same* type: the scan must
    // step over bindingA's live-but-different entry via the
    // `owner.get() == binding.get()` false arm before appending its own.
    auto bindingB = std::make_shared<morph::bridge::detail::HandlerBinding>();
    registry.addSubscription(bindingB, typeA, [](const std::any&) {}, nullptr);
    REQUIRE(registry.size() == 3);

    // Neither existing entry was mismatched or clobbered: re-subscribing
    // bindingA replaces its own entry only, still leaving 3.
    registry.addSubscription(bindingA, typeA, [](const std::any&) {}, nullptr);
    REQUIRE(registry.size() == 3);
}

TEST_CASE("removeSubscription leaves entries for a different binding or type untouched",
          "[bridge][subscription][subscription-registry]") {
    // removeSubscription's erase_if predicate (`!owner || (owner.get() ==
    // binding.get() && entry.type == type)`) had never observed its "keep
    // this one" arm: every prior call's vector held only entries that
    // matched. This registers three entries -- two for the same binding
    // under different types, one for a different binding under the removed
    // type -- so removing one specific (binding, type) pair must survive
    // both the type-mismatch and the binding-mismatch arm.
    morph::bridge::detail::SubscriptionRegistry<morph::bridge::detail::HandlerBinding> registry;
    auto const typeX = std::type_index{typeid(SubCounterState)};
    auto const typeY = std::type_index{typeid(SubLabelState)};

    auto bindingA = std::make_shared<morph::bridge::detail::HandlerBinding>();
    auto bindingB = std::make_shared<morph::bridge::detail::HandlerBinding>();
    registry.addSubscription(bindingA, typeX, [](const std::any&) {}, nullptr);
    registry.addSubscription(bindingA, typeY, [](const std::any&) {}, nullptr);
    registry.addSubscription(bindingB, typeX, [](const std::any&) {}, nullptr);
    REQUIRE(registry.size() == 3);

    registry.removeSubscription(bindingA, typeX);
    // (bindingA, typeY) survives via the type-mismatch arm; (bindingB,
    // typeX) survives via the binding-mismatch arm. Only (bindingA, typeX)
    // is gone.
    REQUIRE(registry.size() == 2);
}

TEST_CASE("publishResult skips an entry with no sink without disturbing other subscribers",
          "[bridge][subscription][subscription-registry]") {
    // publishResult's delivery guard (`owner && entry.type == type &&
    // owner->currentId.load() == mid.v && entry.sink`) had never observed
    // `entry.sink` false: nothing in the public API path constructs an
    // entry with an empty sink except calling addSubscription directly with
    // a default-constructed std::function, which is exactly what this does.
    morph::bridge::detail::SubscriptionRegistry<morph::bridge::detail::HandlerBinding> registry;
    auto const type = std::type_index{typeid(SubCounterState)};

    auto silent = std::make_shared<morph::bridge::detail::HandlerBinding>();
    silent->currentId.store(200);
    registry.addSubscription(silent, type, std::function<void(const std::any&)>{}, nullptr);

    auto loud = std::make_shared<morph::bridge::detail::HandlerBinding>();
    loud->currentId.store(200);
    int fires = 0;
    registry.addSubscription(loud, type, [&](const std::any&) { ++fires; }, nullptr);

    // Neither entry is pruned (both bindings are alive) and both match on
    // type/instance; only `loud`'s non-empty sink actually fires. If the
    // `entry.sink` guard were absent, the empty std::function would be
    // invoked and throw std::bad_function_call.
    REQUIRE_NOTHROW(registry.publishResult(morph::exec::detail::ModelId{200}, type, std::any{}));
    REQUIRE(fires == 1);
    REQUIRE(registry.size() == 2);
}

TEST_CASE("instance subscriptions work under SimulatedRemoteBackend", "[bridge][subscription][remote]") {
    morph::testing::InlineExecutor exec;
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};

    BridgeHandler<SubCounterModel, AllowShared> watcher{bridge, &exec};
    BridgeHandler<SubCounterModel, AllowShared> actor{bridge, &exec};
    watcher.attach(110);

    auto seen = std::make_shared<std::atomic<std::int64_t>>(-1);
    watcher.subscribe<SubCounterState>([seen](SubCounterState state) { seen->store(state.value); });

    drain(actor.execute(SubBump{.id = 110, .by = 4}));
    REQUIRE(morph::testing::waitUntil([&] { return seen->load() == 4; }));
}
