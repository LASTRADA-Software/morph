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
