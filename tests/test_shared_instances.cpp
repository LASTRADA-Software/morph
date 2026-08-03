// SPDX-License-Identifier: Apache-2.0
//
// Tests for keyed, shareable model instances (F2).
//
// A model declares a `PrimaryKey` alias; actions declare which field carries it
// (`BRIDGE_KEY_FROM`) or that their result establishes it
// (`BRIDGE_KEY_FROM_RESULT`); `BridgeHandler<M, AllowShared>` joins a
// server-side directory keyed on `(typeId, primary)`. These tests pin the four
// properties the design turns on: two shared handlers naming one key reach one
// instance, a plain handler never does, the instance survives until the last
// attachment goes away, and all of it behaves identically local and remote.
//
// See docs/planned/shared_model_instances.md.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <string>
#include <thread>
#include <vector>

#include "test_support.hpp"

namespace {

// Model/action types need external linkage for glaze reflection, so they live
// at namespace scope below rather than inside the test cases.

}  // namespace

/// A counter whose value lives *in the instance* — the whole point of keying.
// Model, action and result types need **external** linkage: glaze's
// plain-aggregate reflection cannot see into an anonymous namespace, and the
// BRIDGE_REGISTER_* macros specialise templates at global scope.
// NOLINTBEGIN(misc-use-internal-linkage)
/// A stateless model would make every one of these tests vacuous.
struct ShiCounterState {
    std::int64_t value = 0;
};

struct ShiAddTo {
    std::int64_t id = 0;
    std::int64_t amount = 0;
};

struct ShiRead {
    std::int64_t id = 0;
};

/// Keyless: runs against whatever instance the handler already holds.
struct ShiPeek {
    int unused = 0;
};

struct ShiAwareRead {
    std::int64_t id = 0;
};

/// Actions for the auto-attach worked example below.
struct AutoLoad {
    std::int64_t id = 0;
};
struct AutoAdd {
    std::int64_t amount = 0;
};
struct AutoPeek {
    int unused = 0;
};

/// Creates the entity, so its key cannot be in the request — it comes back in
/// the reply, exactly as a database insert returns its generated primary key.
struct ShiCreate {
    std::int64_t initial = 0;
};

struct ShiCreated {
    std::int64_t id = 0;
    std::int64_t value = 0;
};

/// Like ShiCreate, but the caller picks the id the reply will carry — the only
/// way to force a key collision through the public API.
struct ShiCreateAs {
    std::int64_t wantId = 0;
    std::int64_t initial = 0;
};

struct ShiCounterModel {

    std::int64_t value = 0;

    ShiCounterState execute(const ShiAddTo& act) {
        value += act.amount;
        return {.value = value};
    }
    [[nodiscard]] ShiCounterState execute(const ShiRead& /*act*/) const { return {.value = value}; }
    [[nodiscard]] ShiCounterState execute(const ShiPeek& /*act*/) const { return {.value = value}; }

    ShiCreated execute(const ShiCreate& act) {
        static std::atomic<std::int64_t> nextId{9000};
        value = act.initial;
        return {.id = nextId.fetch_add(1), .value = value};
    }

    ShiCreated execute(const ShiCreateAs& act) {
        value = act.initial;
        return {.id = act.wantId, .value = value};
    }
};

BRIDGE_REGISTER_MODEL(ShiCounterModel, "SHI_CounterModel")
BRIDGE_REGISTER_ACTION(ShiCounterModel, ShiAddTo, "SHI_AddTo")
BRIDGE_REGISTER_ACTION(ShiCounterModel, ShiRead, "SHI_Read")
BRIDGE_REGISTER_ACTION(ShiCounterModel, ShiPeek, "SHI_Peek")
BRIDGE_REGISTER_ACTION(ShiCounterModel, ShiCreate, "SHI_Create")
BRIDGE_REGISTER_ACTION(ShiCounterModel, ShiCreateAs, "SHI_CreateAs")

BRIDGE_MODEL_KEY(ShiCounterModel, ShiAddTo, &ShiAddTo::id);
BRIDGE_KEY_FROM(ShiRead, &ShiRead::id);
BRIDGE_KEY_FROM_RESULT(ShiCreate, &ShiCreated::id);
BRIDGE_KEY_FROM_RESULT(ShiCreateAs, &ShiCreated::id);
// NOLINTEND(misc-use-internal-linkage)

// NOLINTBEGIN(misc-use-internal-linkage)
/// Declares onBackendChanged(), so registering it shared must also record it as
/// change-aware — the bookkeeping LocalBackend keeps to avoid a dynamic_cast
/// sweep on every backend switch.
struct ShiAwareModel {
    std::int64_t notified = 0;
    void onBackendChanged() { notified += 1; }
    [[nodiscard]] ShiCounterState execute(const ShiAwareRead& /*act*/) const { return {.value = notified}; }
};

BRIDGE_REGISTER_MODEL(ShiAwareModel, "SHI_AwareModel")
BRIDGE_REGISTER_ACTION(ShiAwareModel, ShiAwareRead, "SHI_AwareRead")
BRIDGE_MODEL_KEY(ShiAwareModel, ShiAwareRead, &ShiAwareRead::id);
// NOLINTEND(misc-use-internal-linkage)

// NOLINTBEGIN(misc-use-internal-linkage)
/// The whole point of BRIDGE_MODEL_KEY: this class is a plain C++ class. No
/// nested alias, no base, no macro inside the body — the key is declared once,
/// below, next to the registrations the author is already writing.
struct AutoLoadModel {
    std::int64_t loaded = 0;
    std::int64_t total = 0;

    ShiCounterState execute(const AutoLoad& act) {
        loaded = act.id;
        return {.value = total};
    }
    ShiCounterState execute(const AutoAdd& act) {  // keyless
        total += act.amount;
        return {.value = total};
    }
    [[nodiscard]] ShiCounterState execute(const AutoPeek& /*act*/) const { return {.value = total}; }
};

BRIDGE_REGISTER_MODEL(AutoLoadModel, "SHI_AutoLoadModel")
BRIDGE_REGISTER_ACTION(AutoLoadModel, AutoLoad, "SHI_AutoLoad")
BRIDGE_REGISTER_ACTION(AutoLoadModel, AutoAdd, "SHI_AutoAdd")
BRIDGE_REGISTER_ACTION(AutoLoadModel, AutoPeek, "SHI_AutoPeek")

// One line. It deduces PrimaryKey = std::int64_t from the member type *and*
// records AutoLoad as the action that carries it.
BRIDGE_MODEL_KEY(AutoLoadModel, AutoLoad, &AutoLoad::id);
// NOLINTEND(misc-use-internal-linkage)

// NOLINTBEGIN(misc-use-internal-linkage)
/// A model whose first action can be made to fail on demand, to exercise
/// "a failed first action on a freshly created shared instance releases it"
/// (docs/spec/core/shared_instances.md's Failure modes).
struct ShiHydrateFail {
    std::int64_t id = 0;
};
struct ShiHydrateOk {
    std::int64_t id = 0;
};
struct ShiHydrateModel {
    ShiCounterState execute(const ShiHydrateFail&) { throw std::runtime_error("hydration failed"); }
    ShiCounterState execute(const ShiHydrateOk&) { return {.value = 1}; }
};

BRIDGE_REGISTER_MODEL(ShiHydrateModel, "SHI_HydrateModel")
BRIDGE_REGISTER_ACTION(ShiHydrateModel, ShiHydrateFail, "SHI_HydrateFail")
BRIDGE_REGISTER_ACTION(ShiHydrateModel, ShiHydrateOk, "SHI_HydrateOk")

BRIDGE_MODEL_KEY(ShiHydrateModel, ShiHydrateFail, &ShiHydrateFail::id);
BRIDGE_KEY_FROM(ShiHydrateOk, &ShiHydrateOk::id);
// NOLINTEND(misc-use-internal-linkage)

namespace {

using morph::bridge::AllowShared;
using morph::bridge::Bridge;
using morph::bridge::BridgeHandler;

/// Drives a `Completion` to resolution and returns the value.
///
/// Polls rather than assuming synchronous resolution: `LocalBackend` resolves on
/// the caller's thread here, but `SimulatedRemoteBackend` posts to a worker pool,
/// and the same test bodies run against both.
template <typename T>
T settle(morph::async::Completion<T> comp) {
    auto out = std::make_shared<T>();
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto failed = std::make_shared<std::atomic<bool>>(false);
    std::move(comp)
        .then([out, done](T value) {
            *out = std::move(value);
            done->store(true);
        })
        .onError([failed, done](const std::exception_ptr&) {
            failed->store(true);
            done->store(true);
        });
    REQUIRE(morph::testing::waitUntil([&] { return done->load(); }));
    REQUIRE_FALSE(failed->load());
    return *out;
}

std::unique_ptr<morph::backend::detail::IBackend> makeLocal(morph::exec::IExecutor& pool) {
    return std::make_unique<morph::backend::LocalBackend>(pool);
}

}  // namespace

TEST_CASE("two AllowShared handlers naming one key reach one instance", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<ShiCounterModel, AllowShared> first{bridge, &exec};
    BridgeHandler<ShiCounterModel, AllowShared> second{bridge, &exec};

    // A keyed action attaches the handler on the way through.
    REQUIRE(settle(first.execute(ShiAddTo{.id = 42, .amount = 10})).value == 10);
    // The second handler names the same key, so it lands on the same counter
    // and observes the first handler's work.
    REQUIRE(settle(second.execute(ShiAddTo{.id = 42, .amount = 5})).value == 15);
    REQUIRE(settle(first.execute(ShiRead{.id = 42})).value == 15);

    REQUIRE(first.primary().value_or(-1) == 42);
    REQUIRE(second.primary().value_or(-1) == 42);
}

TEST_CASE("a plain handler never joins the directory", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<ShiCounterModel, AllowShared> shared{bridge, &exec};
    BridgeHandler<ShiCounterModel> priv{bridge, &exec};

    REQUIRE(settle(shared.execute(ShiAddTo{.id = 7, .amount = 100})).value == 100);
    // Same key, but this handler opted out — it gets its own instance, starting
    // from zero, exactly as every pre-existing call site does.
    REQUIRE(settle(priv.execute(ShiAddTo{.id = 7, .amount = 1})).value == 1);
    // …and the shared instance is untouched by it.
    REQUIRE(settle(shared.execute(ShiRead{.id = 7})).value == 100);
}

TEST_CASE("different keys are different instances", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<ShiCounterModel, AllowShared> handler{bridge, &exec};
    REQUIRE(settle(handler.execute(ShiAddTo{.id = 1, .amount = 3})).value == 3);
    // A different key is a different counter, not a re-labelled one.
    REQUIRE(settle(handler.execute(ShiAddTo{.id = 2, .amount = 8})).value == 8);

    // Re-pointing away released the *last* attachment to instance 1, so it was
    // destroyed: lifetime is refcounted, not cached. Coming back therefore finds
    // a fresh counter. Keeping it alive is what a second handler is for — see
    // the re-pointing test below.
    REQUIRE(settle(handler.execute(ShiRead{.id = 1})).value == 0);
}

TEST_CASE("a keyed action re-points the handler rather than re-keying the instance", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<ShiCounterModel, AllowShared> mover{bridge, &exec};
    BridgeHandler<ShiCounterModel, AllowShared> pinned{bridge, &exec};

    settle(mover.execute(ShiAddTo{.id = 100, .amount = 50}));
    settle(pinned.execute(ShiRead{.id = 100}));  // pin instance 100 alive

    // The primary is deliberately not write-once: naming another key moves the
    // *handler*, leaving instance 100 and its state intact for `pinned`.
    settle(mover.execute(ShiAddTo{.id = 200, .amount = 1}));
    REQUIRE(mover.primary().value_or(-1) == 200);
    REQUIRE(settle(pinned.execute(ShiPeek{})).value == 50);
}

TEST_CASE("an instance outlives any single handler and dies with the last one", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<ShiCounterModel, AllowShared> survivor{bridge, &exec};
    settle(survivor.execute(ShiAddTo{.id = 9, .amount = 4}));
    {
        BridgeHandler<ShiCounterModel, AllowShared> transient{bridge, &exec};
        settle(transient.execute(ShiAddTo{.id = 9, .amount = 6}));
        REQUIRE(settle(transient.execute(ShiRead{.id = 9})).value == 10);
    }  // transient releases one attachment — the instance must survive

    REQUIRE(settle(survivor.execute(ShiPeek{})).value == 10);

    {
        // Once the last handler goes, the instance and its directory entry go
        // with it, so a later attach starts from a fresh counter.
        BridgeHandler<ShiCounterModel, AllowShared> lastOne{bridge, &exec};
        settle(lastOne.execute(ShiRead{.id = 9}));
    }
}

TEST_CASE("releasing every handler drops the instance and its directory entry", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    {
        BridgeHandler<ShiCounterModel, AllowShared> only{bridge, &exec};
        settle(only.execute(ShiAddTo{.id = 500, .amount = 77}));
        REQUIRE(settle(only.instances()).size() == 1);
    }

    BridgeHandler<ShiCounterModel, AllowShared> fresh{bridge, &exec};
    REQUIRE(settle(fresh.instances()).empty());
    // A fresh attach to the same key starts from zero — the old instance is gone.
    REQUIRE(settle(fresh.execute(ShiRead{.id = 500})).value == 0);
}

TEST_CASE("instances() lists the live shared keys", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<ShiCounterModel, AllowShared> alpha{bridge, &exec};
    BridgeHandler<ShiCounterModel, AllowShared> beta{bridge, &exec};
    BridgeHandler<ShiCounterModel> hidden{bridge, &exec};

    settle(alpha.execute(ShiAddTo{.id = 11, .amount = 1}));
    settle(beta.execute(ShiAddTo{.id = 22, .amount = 1}));
    settle(hidden.execute(ShiAddTo{.id = 33, .amount = 1}));  // private: not listed

    auto keys = settle(alpha.instances());
    std::ranges::sort(keys);
    REQUIRE(keys == std::vector<std::int64_t>{11, 22});
}

TEST_CASE("explicit attach binds without executing an action", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<ShiCounterModel, AllowShared> handler{bridge, &exec};
    REQUIRE_FALSE(handler.primary().has_value());

    handler.attach(64);
    REQUIRE(handler.primary().value_or(-1) == 64);
    // A keyless action now has an instance to run against.
    REQUIRE(settle(handler.execute(ShiPeek{})).value == 0);
}

TEST_CASE("an unattached shared handler fails a keyless action fast", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<ShiCounterModel, AllowShared> handler{bridge, &exec};

    // No primary yet: there is no instance to run against, and quietly inventing
    // a private one would defeat the sharing the caller asked for.
    bool failed = false;
    handler.execute(ShiPeek{}).onError([&](const std::exception_ptr&) { failed = true; });
    REQUIRE(failed);
}

TEST_CASE("sharing works identically across a remote backend", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    // Two independent Bridges over one server stand in for two clients: the
    // directory lives server-side precisely so they meet on one instance.
    Bridge clientA{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};
    Bridge clientB{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};

    BridgeHandler<ShiCounterModel, AllowShared> fromA{clientA, &exec};
    BridgeHandler<ShiCounterModel, AllowShared> fromB{clientB, &exec};

    REQUIRE(settle(fromA.execute(ShiAddTo{.id = 314, .amount = 20})).value == 20);
    // The second *client* — not merely the second handler — sees the first's work.
    REQUIRE(settle(fromB.execute(ShiAddTo{.id = 314, .amount = 2})).value == 22);
    REQUIRE(settle(fromB.instances()) == std::vector<std::int64_t>{314});
}

TEST_CASE("a remote plain handler still gets its own instance", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};

    BridgeHandler<ShiCounterModel, AllowShared> shared{bridge, &exec};
    BridgeHandler<ShiCounterModel> priv{bridge, &exec};

    settle(shared.execute(ShiAddTo{.id = 8, .amount = 30}));
    REQUIRE(settle(priv.execute(ShiAddTo{.id = 8, .amount = 1})).value == 1);
    REQUIRE(settle(shared.execute(ShiRead{.id = 8})).value == 30);
}

TEST_CASE("a result-sourced key promotes the instance the create ran on", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<ShiCounterModel, AllowShared> creator{bridge, &exec};
    auto created = settle(creator.execute(ShiCreate{.initial = 5}));

    // The handler adopts the generated key before any user callback sees the
    // result, so a .then() could immediately act on the new instance.
    REQUIRE(created.id != 0);
    REQUIRE(creator.primary().value_or(-1) == created.id);

    // Crucially the *same* instance was filed under that key rather than a fresh
    // one being created for it: everything the create did is still there. This
    // is the whole reason promotion exists instead of re-pointing.
    REQUIRE(settle(creator.execute(ShiPeek{})).value == 5);

    // …and it is now reachable by key like any other shared instance.
    BridgeHandler<ShiCounterModel, AllowShared> latecomer{bridge, &exec};
    latecomer.attach(created.id);
    REQUIRE(settle(latecomer.execute(ShiPeek{})).value == 5);
    REQUIRE(settle(latecomer.instances()).size() == 1);
}

TEST_CASE("a result-sourced key promotes across a remote backend", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};

    BridgeHandler<ShiCounterModel, AllowShared> creator{bridge, &exec};
    auto created = settle(creator.execute(ShiCreate{.initial = 11}));
    REQUIRE(creator.primary().value_or(-1) == created.id);
    REQUIRE(settle(creator.execute(ShiPeek{})).value == 11);
}

TEST_CASE("promoting onto a key another instance holds leaves the incumbent alone", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    // Hold key 4242 with a real instance carrying state.
    BridgeHandler<ShiCounterModel, AllowShared> incumbent{bridge, &exec};
    settle(incumbent.execute(ShiAddTo{.id = 4242, .amount = 99}));

    // A creating action whose generated key collides with it must not displace
    // the incumbent: the existing holder always wins, so no handler silently
    // ends up pointing at a different instance than the one it created.
    BridgeHandler<ShiCounterModel, AllowShared> collider{bridge, &exec};
    settle(collider.execute(ShiCreateAs{.wantId = 4242, .initial = 7}));

    REQUIRE(settle(incumbent.execute(ShiPeek{})).value == 99);
    REQUIRE(settle(incumbent.instances()) == std::vector<std::int64_t>{4242});
}

TEST_CASE("attaching to the key a handler already holds is a no-op", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<ShiCounterModel, AllowShared> handler{bridge, &exec};
    settle(handler.execute(ShiAddTo{.id = 55, .amount = 8}));

    // Re-attaching to the same key must not release and re-acquire the
    // instance, which would silently discard its state.
    handler.attach(55);
    handler.attach(55);
    REQUIRE(settle(handler.execute(ShiPeek{})).value == 8);
}

TEST_CASE("instances() is empty before anything is attached", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};
    BridgeHandler<ShiCounterModel, AllowShared> handler{bridge, &exec};
    REQUIRE(settle(handler.instances()).empty());
}

TEST_CASE("a keyed action attaches automatically; keyless ones follow the handler",
          "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<AutoLoadModel, AllowShared> first{bridge, &exec};
    BridgeHandler<AutoLoadModel, AllowShared> second{bridge, &exec};

    // The keyed action is the only place a key is ever named.
    settle(first.execute(AutoLoad{.id = 32}));
    settle(second.execute(AutoLoad{.id = 32}));

    // One instance, not two: the second handler found the live one for key 32
    // and constructed nothing.
    REQUIRE(settle(first.instances()) == std::vector<std::int64_t>{32});
    REQUIRE(first.primary().value_or(-1) == 32);
    REQUIRE(second.primary().value_or(-1) == 32);

    // From here on neither handler mentions a key: the keyless actions simply
    // land on the instance their handler is attached to — the same one.
    REQUIRE(settle(first.execute(AutoAdd{.amount = 1})).value == 1);
    REQUIRE(settle(second.execute(AutoAdd{.amount = 2})).value == 3);
    REQUIRE(settle(first.execute(AutoPeek{})).value == 3);
    REQUIRE(settle(second.execute(AutoPeek{})).value == 3);
}

TEST_CASE("a model needs no key declaration in its own class body", "[shared-instances]") {
    // AutoLoadModel declares no nested PrimaryKey; BRIDGE_MODEL_KEY deduced it
    // from the action member it was handed.
    STATIC_REQUIRE(morph::model::KeyedModel<AutoLoadModel>);
    STATIC_REQUIRE(std::same_as<morph::model::PrimaryKeyOf<AutoLoadModel>, std::int64_t>);
    STATIC_REQUIRE(morph::model::detail::PayloadKeyed<AutoLoad>);
    STATIC_REQUIRE_FALSE(morph::model::ActionKeyTraits<AutoAdd>::hasKey);
}

TEST_CASE("primary keys round-trip through their canonical encoding", "[shared-instances]") {
    REQUIRE(morph::model::keyToString<std::int64_t>(-17) == "-17");
    REQUIRE(morph::model::keyFromString<std::int64_t>("-17") == -17);
    REQUIRE(morph::model::keyToString<std::string>("abc") == "abc");
    REQUIRE(morph::model::keyFromString<std::string>("abc") == "abc");
    // A malformed key is a protocol error, never a value to clamp: decoding it
    // to 0 would silently route the caller to the wrong instance.
    REQUIRE_THROWS(morph::model::keyFromString<std::int64_t>("12x"));
    REQUIRE_THROWS(morph::model::keyFromString<std::int64_t>(""));
}

TEST_CASE("keyed models and keyed actions are detected structurally", "[shared-instances]") {
    STATIC_REQUIRE(morph::model::KeyedModel<ShiCounterModel>);
    STATIC_REQUIRE_FALSE(morph::model::KeyedModel<ShiAddTo>);
    STATIC_REQUIRE(morph::model::detail::PayloadKeyed<ShiAddTo>);
    STATIC_REQUIRE_FALSE(morph::model::detail::PayloadKeyed<ShiPeek>);
    STATIC_REQUIRE_FALSE(morph::model::ActionKeyTraits<ShiPeek>::hasKey);
}

// ── Paths the happy path never reaches ──────────────────────────────────────

namespace {

/// Refuses everything, to drive the server's unauthorized replies.
struct DenyAllAuthorizer : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(const morph::session::Context& /*ctx*/, std::string_view /*modelType*/,
                                 std::string_view /*actionType*/) const override {
        return false;
    }
    [[nodiscard]] bool authorizeRegister(const morph::session::Context& /*ctx*/,
                                         std::string_view /*modelType*/) const override {
        return false;
    }
};

/// An IBackend that overrides nothing beyond the two pure virtuals, so the
/// sharing defaults on the interface itself are exercised: they must degrade to
/// private, unshared behaviour rather than silently hand two callers one model.
struct MinimalBackend : morph::backend::detail::IBackend {
    morph::exec::detail::ModelId registerModel(
        const std::string& /*typeId*/,
        std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory) override {
        auto holder = factory();
        return morph::exec::detail::ModelId{++nextId};
    }
    void deregisterModel(morph::exec::detail::ModelId /*mid*/) override {}
    morph::async::Completion<std::shared_ptr<void>> execute(morph::exec::detail::ModelId /*mid*/,
                                                            morph::backend::detail::ActionCall /*call*/,
                                                            morph::exec::IExecutor* cbExec) override {
        auto state = std::make_shared<morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        morph::async::Completion<std::shared_ptr<void>> comp{state, cbExec};
        state->setException(std::make_exception_ptr(std::runtime_error("not implemented")));
        return comp;
    }
    void notifyBackendChanged() override {}
    void cancelPending(const std::exception_ptr& /*exc*/) override {}

    std::uint64_t nextId = 0;
};

}  // namespace

TEST_CASE("IBackend's sharing defaults degrade to private instances", "[shared-instances]") {
    MinimalBackend backend;
    // A backend that has not implemented sharing must not pretend it has: the
    // default registerModelShared ignores the primary and makes a private
    // instance, and the directory it does not keep is empty.
    auto first = backend.registerModelShared("SHI_CounterModel",
                                             [] { return morph::model::detail::ModelFactory::create<ShiCounterModel>(); },
                                             {.contextKey = {}, .primary = "1"});
    auto second = backend.registerModelShared("SHI_CounterModel",
                                              [] { return morph::model::detail::ModelFactory::create<ShiCounterModel>(); },
                                              {.contextKey = {}, .primary = "1"});
    REQUIRE(first.v != second.v);
    REQUIRE(backend.listInstances("SHI_CounterModel").empty());
    REQUIRE_NOTHROW(backend.assignPrimary(first, "SHI_CounterModel", "1"));
}

TEST_CASE("assignPrimary promotes an anonymous instance and ignores unusable input", "[shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::LocalBackend backend{pool};

    // An anonymous instance -- no primary yet -- is the only kind
    // assignPrimary may ever promote.
    auto mid = backend.registerModelShared("SHI_CounterModel",
                                           [] { return morph::model::detail::ModelFactory::create<ShiCounterModel>(); },
                                           {.contextKey = {}, .primary = {}});
    REQUIRE(backend.listInstances("SHI_CounterModel").empty());

    backend.assignPrimary(mid, "SHI_CounterModel", "new");
    REQUIRE(backend.listInstances("SHI_CounterModel") == std::vector<std::string>{"new"});

    // Already keyed, so a second promotion is a no-op: instances never change
    // key once they have a real one.
    backend.assignPrimary(mid, "SHI_CounterModel", "other");
    REQUIRE(backend.listInstances("SHI_CounterModel") == std::vector<std::string>{"new"});

    // Neither of these names something actionable, so both are also no-ops.
    backend.assignPrimary(mid, "SHI_CounterModel", "");
    backend.assignPrimary(morph::exec::detail::ModelId{99999}, "SHI_CounterModel", "ghost");
    REQUIRE(backend.listInstances("SHI_CounterModel") == std::vector<std::string>{"new"});
}

TEST_CASE("a shared handler survives switchBackend", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    morph::exec::ThreadPoolExecutor poolA{2};
    morph::exec::ThreadPoolExecutor poolB{2};
    Bridge bridge{std::make_unique<morph::backend::LocalBackend>(poolA)};

    BridgeHandler<ShiCounterModel, AllowShared> attached{bridge, &exec};
    BridgeHandler<ShiCounterModel, AllowShared> neverAttached{bridge, &exec};
    settle(attached.execute(ShiAddTo{.id = 77, .amount = 4}));

    // The attached binding is re-registered on the new backend through the
    // directory; the one that never attached has no instance to re-create and
    // must simply stay unbound rather than being handed a stray one.
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(poolB));

    REQUIRE(attached.primary().value_or(-1) == 77);
    REQUIRE(settle(attached.instances()) == std::vector<std::int64_t>{77});
    REQUIRE_FALSE(neverAttached.primary().has_value());
}

TEST_CASE("a shared register is refused once the server is at its model cap", "[shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    server->setLimitPolicy({.maxLiveModels = 1});

    morph::wire::Envelope const first = morph::wire::makeRegisterShared("SHI_CounterModel", "1");
    auto firstReply = morph::wire::decode(server->handleInline(morph::wire::encode(first)));
    REQUIRE(firstReply.kind == "ok");

    // A *different* key needs a new instance, and there is no room for one.
    auto second = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeRegisterShared("SHI_CounterModel", "2"))));
    REQUIRE(second.kind == "err");
    REQUIRE(second.message == "too many models");

    // Attaching to the key that already exists still works — it creates nothing.
    auto again = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeRegisterShared("SHI_CounterModel", "1"))));
    REQUIRE(again.kind == "ok");
    REQUIRE(again.modelId == firstReply.modelId);
}

TEST_CASE("attach and instances are refused by an authorizer that denies", "[shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<DenyAllAuthorizer>());

    auto attach = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeAttach("SHI_CounterModel", "1"))));
    REQUIRE(attach.kind == "err");
    REQUIRE(attach.message == "unauthorized");

    // Enumeration is a read channel over the directory, gated separately so a
    // deployer can refuse listing without refusing use.
    auto listed = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeInstances("SHI_CounterModel"))));
    REQUIRE(listed.kind == "err");
    REQUIRE(listed.message == "unauthorized");
}

TEST_CASE("assign is refused by an authorizer that denies", "[shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<DenyAllAuthorizer>());

    // Register a plain (non-shared) instance first -- register itself is
    // refused too, but we want an `assign` attempt against a *known* modelId
    // to prove assign has its own gate, not just an incidental empty-primary
    // no-op.
    auto reg = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeRegister("SHI_CounterModel"))));
    REQUIRE(reg.kind == "err");  // register is refused too, as expected

    auto assign = morph::wire::decode(server->handleInline(
        morph::wire::encode(morph::wire::makeAssign("SHI_CounterModel", "1", 0))));
    REQUIRE(assign.kind == "err");
    REQUIRE(assign.message == "unauthorized");
}

TEST_CASE("a result-sourced key is not promoted when the handler already holds a real key", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<ShiCounterModel, AllowShared> handler{bridge, &exec};
    handler.attach(600);                                    // already bound to a real key…
    settle(handler.execute(ShiAddTo{.id = 600, .amount = 3}));

    // …so a creating action's result-sourced key must not re-file this
    // instance out from under 600: instances never change key, and another
    // client may still be attached under it.
    auto created = settle(handler.execute(ShiCreateAs{.wantId = 601, .initial = 9}));
    REQUIRE(created.id == 601);
    REQUIRE(handler.primary().value_or(-1) == 600);

    // Key 601 was never filed: a fresh attach to it starts from zero.
    BridgeHandler<ShiCounterModel, AllowShared> other{bridge, &exec};
    other.attach(601);
    REQUIRE(settle(other.execute(ShiPeek{})).value == 0);
}

TEST_CASE("a subscriber with no executor is called inline", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<ShiCounterModel, AllowShared> driver{bridge, &exec};
    BridgeHandler<ShiCounterModel, AllowShared> watcher{bridge, nullptr};  // no callback executor
    driver.attach(700);
    watcher.attach(700);

    std::int64_t seen = -1;
    watcher.subscribe<ShiCounterState>([&](ShiCounterState state) { seen = state.value; });
    settle(driver.execute(ShiAddTo{.id = 700, .amount = 6}));
    REQUIRE(seen == 6);
}

TEST_CASE("instances() surfaces a key this client cannot decode", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    // A key the server is happy to file but that no `std::int64_t` can hold.
    // The wire carries keys as strings, so nothing upstream rejects it; the
    // decode has to fail somewhere, and failing loudly at the client boundary
    // beats handing the caller a silently-wrong 0.
    auto reg = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeRegisterShared("SHI_CounterModel", "not-a-number"))));
    REQUIRE(reg.kind == "ok");

    Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};
    BridgeHandler<ShiCounterModel, AllowShared> handler{bridge, &exec};

    auto failed = std::make_shared<std::atomic<bool>>(false);
    handler.instances().onError([failed](const std::exception_ptr&) { failed->store(true); });
    REQUIRE(morph::testing::waitUntil([&] { return failed->load(); }));
}

TEST_CASE("an attached shared handler re-registers through a remote backend on switch",
          "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    morph::exec::ThreadPoolExecutor localPool{2};
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);

    Bridge bridge{std::make_unique<morph::backend::LocalBackend>(localPool)};
    BridgeHandler<ShiCounterModel, AllowShared> handler{bridge, &exec};
    settle(handler.execute(ShiAddTo{.id = 800, .amount = 2}));

    // Going local -> remote must carry the *key* across, not just re-register
    // something anonymous: the handler is still attached to 800 afterwards, and
    // the server's directory knows it under that key.
    bridge.switchBackend(std::make_unique<morph::backend::SimulatedRemoteBackend>(*server));

    REQUIRE(handler.primary().value_or(-1) == 800);
    REQUIRE(settle(handler.instances()) == std::vector<std::int64_t>{800});
}

namespace {

/// Verifies every caller as "verified-user", so the server's attach/instances
/// branches take their authenticate-and-overwrite path.
struct VerifyingAuthorizer : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(const morph::session::Context& /*ctx*/, std::string_view /*modelType*/,
                                 std::string_view /*actionType*/) const override {
        return true;
    }
    [[nodiscard]] std::optional<std::string> authenticate(
        const morph::session::Context& /*ctx*/) const override {
        return std::string{"verified-user"};
    }
};

}  // namespace

TEST_CASE("a change-aware model is tracked when registered shared", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    morph::exec::ThreadPoolExecutor poolA{2};
    morph::exec::ThreadPoolExecutor poolB{2};
    Bridge bridge{std::make_unique<morph::backend::LocalBackend>(poolA)};

    BridgeHandler<ShiAwareModel, AllowShared> handler{bridge, &exec};
    settle(handler.execute(ShiAwareRead{.id = 900}));

    // Registering through the shared path must record change-awareness exactly
    // as the private path does, or the model silently stops being notified.
    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(poolB));

    // No polling needed, and none wanted: notifyBackendChanged posts
    // onBackendChanged onto the instance's strand, and this execute posts onto
    // the same strand, so it is ordered strictly after — a poll here would
    // merely hide a real ordering bug behind a retry.
    REQUIRE(settle(handler.execute(ShiAwareRead{.id = 900})).value == 1);
}

TEST_CASE("the server refuses to re-file an already-keyed instance onto a different key", "[shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    auto reg = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeRegisterShared("SHI_CounterModel", "old"))));
    REQUIRE(reg.kind == "ok");

    // The instance already has a real key ("old"); assign must leave it there
    // rather than silently moving it, which would strand any other client
    // still attached under "old".
    server->handleInline(morph::wire::encode(morph::wire::makeAssign("SHI_CounterModel", "new", reg.modelId)));

    auto listed = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeInstances("SHI_CounterModel"))));
    std::vector<std::string> keys;
    REQUIRE_FALSE(glz::read_json(keys, listed.body));
    REQUIRE(keys == std::vector<std::string>{"old"});
}

TEST_CASE("an attach that creates the instance carries contextKey to a configured LogProvider", "[shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    std::vector<std::string> requestedFor;
    server->setLogProvider([&](std::string_view modelType, std::string_view contextKey) {
        requestedFor.emplace_back(std::string{modelType} + ":" + std::string{contextKey});
        return nullptr;
    });

    // The first touch of key "77" goes through `attach` (not a shared
    // `register`) -- exercised directly at the wire level since
    // wire::makeAttach previously had no contextKey parameter at all, so the
    // entity's stable identity was silently dropped before it ever reached
    // the server.
    auto reply = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeAttach("SHI_CounterModel", "77", 0, "77"))));
    REQUIRE(reply.kind == "ok");
    REQUIRE(requestedFor == std::vector<std::string>{"SHI_CounterModel:77"});
}

TEST_CASE("attach and instances stamp the verified principal", "[shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<VerifyingAuthorizer>());

    // An authenticating authorizer's identity must overwrite the client's claim
    // on these kinds too, not only on register/execute.
    auto attached = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeAttach("SHI_CounterModel", "1"))));
    REQUIRE(attached.kind == "ok");

    auto listed = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeInstances("SHI_CounterModel"))));
    REQUIRE(listed.kind == "ok");
}

TEST_CASE("a refusing server surfaces as an exception on the remote backend", "[shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<DenyAllAuthorizer>());
    morph::backend::SimulatedRemoteBackend backend{*server};

    // Each control call reports the server's refusal rather than returning a
    // bogus id or an empty list that a caller would mistake for success.
    REQUIRE_THROWS(backend.registerModelShared(
        "SHI_CounterModel", [] { return morph::model::detail::ModelFactory::create<ShiCounterModel>(); },
        {.contextKey = {}, .primary = "1"}));
    REQUIRE_THROWS(backend.attachModel(
        "SHI_CounterModel", [] { return morph::model::detail::ModelFactory::create<ShiCounterModel>(); },
        {.contextKey = {}, .primary = "1"}, morph::exec::detail::ModelId{0}));
    REQUIRE_THROWS(backend.listInstances("SHI_CounterModel"));
}

TEST_CASE("an empty primary on the remote backend degrades to a private instance", "[shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::backend::SimulatedRemoteBackend backend{*server};

    auto held = backend.registerModel("SHI_CounterModel",
                                      [] { return morph::model::detail::ModelFactory::create<ShiCounterModel>(); });
    // No key to share on, so this releases what it holds and registers privately
    // rather than entering the directory.
    auto rebound = backend.attachModel(
        "SHI_CounterModel", [] { return morph::model::detail::ModelFactory::create<ShiCounterModel>(); },
        {.contextKey = {}, .primary = {}}, held);
    REQUIRE(rebound.v != 0U);
    REQUIRE(backend.listInstances("SHI_CounterModel").empty());
}

TEST_CASE("execute() reports an attach failure through onError instead of throwing", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    server->setLimitPolicy({.maxLiveModels = 1});

    Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};

    // Fills the server's one slot with an unrelated private instance.
    BridgeHandler<ShiCounterModel> filler{bridge, &exec};
    settle(filler.execute(ShiAddTo{.id = 1, .amount = 1}));

    // A payload-keyed action on a fresh shared handler must attach a *new*
    // instance for key 2, and the server is already full: the attach call
    // the framework makes internally throws. That must not escape execute()
    // as a synchronous exception -- it must surface through .onError(),
    // exactly like every other dispatch failure.
    BridgeHandler<ShiCounterModel, AllowShared> handler{bridge, &exec};
    bool failed = false;
    REQUIRE_NOTHROW(
        handler.execute(ShiAddTo{.id = 2, .amount = 1}).onError([&](const std::exception_ptr&) { failed = true; }));
    REQUIRE(failed);
}

TEST_CASE("IBackend::attachModel's default re-attach to an already-held key does not destroy the instance",
          "[shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::backend::LocalBackend backend{pool};

    auto held = backend.registerModelShared(
        "SHI_CounterModel", [] { return morph::model::detail::ModelFactory::create<ShiCounterModel>(); },
        {.contextKey = {}, .primary = "9"});

    // Re-attaching to the exact key already held must hand back the same
    // instance, not destroy and recreate it -- exercised directly against the
    // backend since Bridge::attachHandler's own primary-unchanged guard would
    // otherwise short-circuit before ever reaching attachModel.
    auto again = backend.attachModel(
        "SHI_CounterModel", [] { return morph::model::detail::ModelFactory::create<ShiCounterModel>(); },
        {.contextKey = {}, .primary = "9"}, held);
    REQUIRE(again.v == held.v);
    REQUIRE(backend.listInstances("SHI_CounterModel") == std::vector<std::string>{"9"});
}

TEST_CASE("the server's attach is a no-op when re-attaching to the key already held", "[shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    auto reg = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeRegisterShared("SHI_CounterModel", "5"))));
    REQUIRE(reg.kind == "ok");

    auto again = morph::wire::decode(server->handleInline(
        morph::wire::encode(morph::wire::makeAttach("SHI_CounterModel", "5", reg.modelId))));
    REQUIRE(again.kind == "ok");
    REQUIRE(again.modelId == reg.modelId);

    auto listed = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeInstances("SHI_CounterModel"))));
    std::vector<std::string> keys;
    REQUIRE_FALSE(glz::read_json(keys, listed.body));
    REQUIRE(keys == std::vector<std::string>{"5"});
}

namespace {

/// An IBackend whose shared-register/attach blocks until told to proceed, so
/// a test can hold Bridge's attach path "in flight" and prove unrelated
/// handler registration on the same Bridge does not wait behind it.
struct SlowAttachBackend : morph::backend::detail::IBackend {
    std::atomic<bool> attachStarted{false};
    std::atomic<bool> proceed{false};
    std::atomic<uint64_t> nextId{0};

    morph::exec::detail::ModelId registerModel(
        const std::string&, std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory) override {
        auto holder = factory();
        (void)holder;
        return morph::exec::detail::ModelId{++nextId};
    }
    morph::exec::detail::ModelId registerModelShared(const std::string& typeId,
                                                      std::function<std::unique_ptr<morph::model::detail::IModelHolder>()> factory,
                                                      morph::backend::detail::InstanceIdentity identity) override {
        if (identity.primary.empty()) {
            return registerModel(typeId, std::move(factory));
        }
        attachStarted.store(true);
        while (!proceed.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        auto holder = factory();
        (void)holder;
        return morph::exec::detail::ModelId{++nextId};
    }
    void deregisterModel(morph::exec::detail::ModelId) override {}
    morph::async::Completion<std::shared_ptr<void>> execute(morph::exec::detail::ModelId,
                                                             morph::backend::detail::ActionCall,
                                                             morph::exec::IExecutor* cbExec) override {
        auto state = std::make_shared<morph::async::detail::CompletionState<std::shared_ptr<void>>>();
        morph::async::Completion<std::shared_ptr<void>> comp{state, cbExec};
        state->setException(std::make_exception_ptr(std::runtime_error("not implemented")));
        return comp;
    }
    void notifyBackendChanged() override {}
    void cancelPending(const std::exception_ptr&) override {}
};

}  // namespace

TEST_CASE("Bridge: an in-flight shared attach does not block unrelated handler registration", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    auto backend = std::make_unique<SlowAttachBackend>();
    auto* backendPtr = backend.get();
    Bridge bridge{std::move(backend)};

    BridgeHandler<ShiCounterModel, AllowShared> attacher{bridge, &exec};
    std::thread attachThread([&] { attacher.attach(1); });
    std::thread unrelatedThread;
    std::atomic<bool> unrelatedDone{false};

    // Guarantees both threads are joined -- even if a REQUIRE below throws --
    // so a failing assertion never leaves a joinable std::thread dangling
    // (which would std::terminate) or the backend permanently blocked.
    // Unblocking the backend before joining means this cleanup itself cannot
    // hang: once `proceed` is set, attachThread's backend call returns and
    // releases whatever lock it was holding, so a still-pending
    // unrelatedThread (blocked acquiring that same lock, pre-fix) is freed to
    // finish too.
    auto joinAll = [&] {
        backendPtr->proceed.store(true);
        if (attachThread.joinable()) {
            attachThread.join();
        }
        if (unrelatedThread.joinable()) {
            unrelatedThread.join();
        }
    };
    try {
        REQUIRE(morph::testing::waitUntil([&] { return backendPtr->attachStarted.load(); }));

        // While the attach above is still blocked inside the backend, registering
        // an unrelated handler on the same Bridge must not block behind it: a
        // dedicated attach mutex means registerHandler() no longer contends for
        // the same lock as a slow shared attach.
        unrelatedThread = std::thread([&] {
            BridgeHandler<ShiCounterModel> unrelated{bridge, &exec};
            unrelatedDone.store(true);
        });
        REQUIRE(morph::testing::waitUntil([&] { return unrelatedDone.load(); }));
    } catch (...) {
        joinAll();
        throw;
    }
    joinAll();
}

TEST_CASE("a failed first action releases a freshly created shared instance from the directory", "[shared-instances]") {
    morph::testing::InlineExecutor exec;
    Bridge bridge{makeLocal(exec)};

    BridgeHandler<ShiHydrateModel, AllowShared> first{bridge, &exec};
    bool failed = false;
    first.execute(ShiHydrateFail{.id = 1}).onError([&](const std::exception_ptr&) { failed = true; });
    REQUIRE(failed);

    // The broken instance must not be handed to a second attacher: it gets a
    // fresh instance instead, on which the same key's normal action succeeds.
    BridgeHandler<ShiHydrateModel, AllowShared> second{bridge, &exec};
    REQUIRE(settle(second.execute(ShiHydrateOk{.id = 1})).value == 1);
    // ShiHydrateModel carries no observable state, so the value check above
    // cannot by itself tell a fresh instance apart from the reused, poisoned
    // one. `first` is still attached (its attachment was never released), so
    // the poisoned instance is still alive; confirm `second` really landed on
    // a *different* instance, not the same one reused.
    REQUIRE(second.binding()->currentId.load() != first.binding()->currentId.load());
}

TEST_CASE("the server releases a freshly created shared instance whose first action fails", "[shared-instances]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    auto reg = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeRegisterShared("SHI_HydrateModel", "1"))));
    REQUIRE(reg.kind == "ok");

    morph::wire::Envelope failExec;
    failExec.kind = "execute";
    failExec.modelId = reg.modelId;
    failExec.modelType = "SHI_HydrateModel";
    failExec.actionType = "SHI_HydrateFail";
    failExec.body = R"({"id":1})";
    morph::testing::WaitReply waiter;
    server->handle(morph::wire::encode(failExec), std::ref(waiter));
    REQUIRE(waiter.await());
    REQUIRE(waiter.env.kind == "err");

    // A second shared register for the same key must not reach the poisoned
    // instance -- it gets a fresh one.
    auto second = morph::wire::decode(
        server->handleInline(morph::wire::encode(morph::wire::makeRegisterShared("SHI_HydrateModel", "1"))));
    REQUIRE(second.kind == "ok");
    REQUIRE(second.modelId != reg.modelId);
}
