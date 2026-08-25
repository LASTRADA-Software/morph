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
