// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/testing/owner_probe_recorder.hpp>
#include <optional>
#include <thread>

#include "test_support.hpp"

// Model, action and result types need external linkage: the BRIDGE_REGISTER_* macros specialise templates at
// global scope.
// NOLINTBEGIN(misc-use-internal-linkage)
struct ReactiveTimes {
    int value = 0;
    bool operator==(ReactiveTimes const&) const = default;
};
struct ReactiveReadHits {
    bool operator==(ReactiveReadHits const&) const = default;
};
struct ReactiveBump {};
struct ReactiveBumped {
    int hits = 0;
};
struct ReactiveSetLevel {
    int level = 0;
};
// Has `==`, so a Subscription to it is equality-gated.
struct ReactiveLevel {
    int level = 0;
    bool operator==(ReactiveLevel const&) const = default;
};

struct ReactiveHitModel {
    int hits = 0;

    [[nodiscard]] static int execute(ReactiveTimes action) { return action.value * 10; }

    [[nodiscard]] int execute(ReactiveReadHits /*action*/) const { return hits; }

    ReactiveBumped execute(ReactiveBump /*action*/) { return ReactiveBumped{++hits}; }

    [[nodiscard]] static ReactiveLevel execute(ReactiveSetLevel action) { return ReactiveLevel{action.level}; }
};

BRIDGE_REGISTER_MODEL(ReactiveHitModel, "Test_ReactiveHitModel")
BRIDGE_REGISTER_ACTION(ReactiveHitModel, ReactiveTimes, "Test_ReactiveTimes")
BRIDGE_REGISTER_ACTION(ReactiveHitModel, ReactiveReadHits, "Test_ReactiveReadHits")
BRIDGE_REGISTER_ACTION(ReactiveHitModel, ReactiveBump, "Test_ReactiveBump")
BRIDGE_REGISTER_ACTION(ReactiveHitModel, ReactiveSetLevel, "Test_ReactiveSetLevel")
// NOLINTEND(misc-use-internal-linkage)

namespace {

using morph::reactive::Concurrency;
using morph::reactive::Effect;
using morph::reactive::Mutation;
using morph::reactive::MutationOptions;
using morph::reactive::Query;
using morph::reactive::Runtime;
using morph::reactive::Signal;
using morph::reactive::Subscription;
using morph::testing::pumpOwnerUntil;
namespace site = morph::reactive::detail::site;

using Bumps = Subscription<ReactiveBumped>;

// A real LocalBackend on a one-thread pool, delivering to the test thread's owner.
struct BridgeWiring {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<ReactiveHitModel> handler{bridge, &owner};
};

}  // namespace

// Mutation: the Query's Effect reads the key but does not hand it to onKey.
TEST_CASE("reactive control over a bridge: a Query re-fetches when its key changes", "[reactive][control]") {
    BridgeWiring wiring;
    Runtime runtime{wiring.owner};
    Signal<int> factor{runtime, 1};
    Query<ReactiveTimes> const scaled{runtime, wiring.handler,
                                      [&] { return std::optional{ReactiveTimes{factor.get()}}; }};
    REQUIRE(pumpOwnerUntil(wiring.owner, [&] { return scaled.value() == std::optional{10}; }));
    factor.set(5);
    REQUIRE(pumpOwnerUntil(wiring.owner, [&] { return scaled.value() == std::optional{50}; }));
}

// Mutation: drop the invalidation loop from Mutation::settle.
TEST_CASE("reactive control over a bridge: a Mutation invalidates a Query", "[reactive][control]") {
    BridgeWiring wiring;
    Runtime runtime{wiring.owner};
    Query<ReactiveReadHits> hits{runtime, wiring.handler, [] { return std::optional{ReactiveReadHits{}}; }};
    Mutation<ReactiveBump> bump{runtime, wiring.handler, MutationOptions<ReactiveBump>{.invalidates = {hits.link()}}};
    REQUIRE(pumpOwnerUntil(wiring.owner, [&] { return hits.value() == std::optional{0}; }));
    bump.run(ReactiveBump{});
    REQUIRE(pumpOwnerUntil(wiring.owner, [&] { return hits.value() == std::optional{1}; }));
    CHECK(bump.lastResult().has_value());
}

// Mutation: Query::refreshOn subscribes a callback that does nothing.
TEST_CASE("reactive control over a bridge: refreshOn re-fetches when a result is published", "[reactive][control]") {
    BridgeWiring wiring;
    Runtime runtime{wiring.owner};
    Query<ReactiveReadHits> hits{runtime, wiring.handler, [] { return std::optional{ReactiveReadHits{}}; }};
    hits.refreshOn<ReactiveBumped>(wiring.handler);
    Mutation<ReactiveBump> bump{runtime, wiring.handler};  // invalidates nothing itself
    REQUIRE(pumpOwnerUntil(wiring.owner, [&] { return hits.value() == std::optional{0}; }));
    bump.run(ReactiveBump{});
    REQUIRE(pumpOwnerUntil(wiring.owner, [&] { return hits.value() == std::optional{1}; }));
    CHECK_FALSE(hits.pending());
}

// Mutation: the Subscription's callback does not write `_latest`.
TEST_CASE("reactive control over a bridge: a Subscription follows publishes", "[reactive][control]") {
    BridgeWiring wiring;
    Runtime runtime{wiring.owner};
    Bumps const latest{runtime, wiring.handler};
    // Two runs back to back: Serial sends the second after the first, where the Exclusive default refuses it.
    Mutation<ReactiveBump> bump{runtime, wiring.handler,
                                MutationOptions<ReactiveBump>{.concurrency = Concurrency::Serial}};
    CHECK_FALSE(latest.latest().has_value());
    CHECK(bump.run(ReactiveBump{}));
    CHECK(bump.run(ReactiveBump{}));
    REQUIRE(pumpOwnerUntil(wiring.owner, [&] { return latest.latest().has_value() && latest.latest()->hits == 2; }));
}

// Mutation: the Subscription's callback does not write `_latest`.
TEST_CASE("reactive control over a bridge: a Subscription is tracked", "[reactive][control]") {
    BridgeWiring wiring;
    Runtime runtime{wiring.owner};
    Bumps const latest{runtime, wiring.handler};
    Mutation<ReactiveBump> bump{runtime, wiring.handler};
    int seen = 0;
    Effect const follow{runtime, [&] {
                            if (latest.latest().has_value()) {
                                seen = latest.latest()->hits;
                            }
                        }};
    bump.run(ReactiveBump{});
    REQUIRE(pumpOwnerUntil(wiring.owner, [&] { return seen == 1; }));
}

// Mutation: subscribe without `_lifetime` (crashes).
TEST_CASE("reactive control over a bridge: a destroyed Subscription gates later publishes",
          "[reactive][control][lifetime]") {
    BridgeWiring wiring;
    Runtime runtime{wiring.owner};
    auto latest = std::make_unique<Bumps>(runtime, wiring.handler);
    Mutation<ReactiveBump> bump{runtime, wiring.handler};
    latest.reset();
    bump.run(ReactiveBump{});
    REQUIRE(pumpOwnerUntil(wiring.owner, [&] { return bump.lastResult().has_value(); }));
    // The bridge posts the publish and the reply from one task, so the publish has run or is still queued;
    // drain() runs it. A delivery into the destroyed Subscription is a use-after-free under ASan.
    wiring.owner.drain();
}

// Mutation: subscribe without `_lifetime` (crashes).
TEST_CASE("reactive control over a bridge: a Subscription destroyed with a publish queued drops it",
          "[reactive][control][lifetime]") {
    // The handler delivers to a step executor on the test thread, so the publish waits in its queue until
    // the test runs it.
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::MainThreadExecutor owner;
    morph::testing::StepExecutor gui;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<ReactiveHitModel> handler{bridge, &gui};
    Runtime runtime{gui};
    auto latest = std::make_unique<Bumps>(runtime, handler);
    gui.runAll();

    static_cast<void>(handler.execute(ReactiveBump{}));
    // The bridge posts the publish and the reply's delivery to `gui` together; nothing runs them until the
    // test does.
    REQUIRE(pumpOwnerUntil(owner, [&] { return gui.pending() >= 2; }));
    latest.reset();
    gui.runAll();  // a delivery into the destroyed Subscription is a use-after-free under ASan
}

// Mutation: the Subscription's callback does not write `_latest`, so the Effect never runs; the destruction itself
// is gated by `_lifetime`, as in the tests above.
TEST_CASE("reactive control over a bridge: an Effect a publish wakes may destroy the Subscription",
          "[reactive][control][lifetime]") {
    BridgeWiring wiring;
    Runtime runtime{wiring.owner};
    auto latest = std::make_unique<Bumps>(runtime, wiring.handler);
    Mutation<ReactiveBump> bump{runtime, wiring.handler};
    Effect const unmount{runtime, [&] {
                             if (latest != nullptr && latest->latest().has_value()) {
                                 latest.reset();
                             }
                         }};
    bump.run(ReactiveBump{});
    REQUIRE(pumpOwnerUntil(wiring.owner, [&] { return latest == nullptr; }));

    // The handler still holds the destroyed Subscription's sink; this publish reaches it and is dropped.
    bump.run(ReactiveBump{});
    REQUIRE(
        pumpOwnerUntil(wiring.owner, [&] { return bump.lastResult().has_value() && bump.lastResult()->hits == 2; }));
    wiring.owner.drain();  // runs the publish, if it is still queued behind the reply
}

// Mutation: build `_latest` with EqualityPolicy::Always.
TEST_CASE("reactive control over a bridge: an equal consecutive publish does not notify", "[reactive][control]") {
    // The handler and the Runtime both run on a step executor, so each publish, its reply and the flush it
    // queues are run to completion, one publish at a time.
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::MainThreadExecutor owner;
    morph::testing::StepExecutor gui;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<ReactiveHitModel> handler{bridge, &gui};
    Runtime runtime{gui};
    Subscription<ReactiveLevel> const level{runtime, handler};
    int runs = 0;
    Effect const watch{runtime, [&] {
                           static_cast<void>(level.latest());
                           ++runs;
                       }};
    gui.runAll();
    REQUIRE(runs == 1);

    auto const publish = [&](int value) {
        static_cast<void>(handler.execute(ReactiveSetLevel{value}));
        REQUIRE(pumpOwnerUntil(owner, [&] { return gui.pending() >= 2; }));  // the publish and the reply
        gui.runAll();
    };
    publish(1);
    CHECK(runs == 2);
    publish(1);
    CHECK(runs == 2);
    publish(2);
    CHECK(runs == 3);
    CHECK(level.latest() == std::optional{ReactiveLevel{2}});
}

// Mutation: the Subscription's callback writes whatever checkOwner() says.
TEST_CASE("reactive control over a bridge: a publish delivered off the owner is reported and dropped",
          "[reactive][control][misuse]") {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::MainThreadExecutor owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    // The handler delivers on `foreign`, which a second thread runs: never the Runtime's owner.
    morph::exec::MainThreadExecutor foreign;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<ReactiveHitModel> handler{bridge, &foreign};
    Runtime runtime{owner};
    Bumps const latest{runtime, handler};

    static_cast<void>(handler.execute(ReactiveBump{}));
    std::atomic<bool> reported{false};
    std::thread deliverer{
        [&] { reported = pumpOwnerUntil(foreign, [&] { return probe.count(site::kOffOwner) >= 1; }); }};
    // The bridge's own work (the bind, the publish) runs here, on its owner.
    static_cast<void>(pumpOwnerUntil(owner, [&] { return probe.count(site::kOffOwner) >= 1; }));
    deliverer.join();
    CHECK(reported);
    CHECK(probe.count(site::kOffOwner) == 1);
    CHECK_FALSE(latest.latest().has_value());
}

// ── The handler's callback executor ────────────────────────────────────────

// Mutation: detail::checkHandlerExecutor reports whatever executor the handler delivers on.
TEST_CASE("reactive control over a bridge: a handler delivering on the Runtime's owner is accepted",
          "[reactive][control]") {
    BridgeWiring wiring;
    morph::testing::OwnerProbeRecorder const probe{wiring.owner.coreExecutor()};
    Runtime runtime{wiring.owner};
    Query<ReactiveReadHits> const hits{runtime, wiring.handler, [] { return std::optional{ReactiveReadHits{}}; }};
    Mutation<ReactiveBump> const bump{runtime, wiring.handler};
    Bumps const latest{runtime, wiring.handler};
    REQUIRE(pumpOwnerUntil(wiring.owner, [&] { return hits.value().has_value(); }));
    CHECK(probe.count(site::kHandlerExecutor) == 0);
}

// A distinct executor whose tasks run on the owner's thread delivers where the runtime runs. Mutation:
// detail::checkHandlerExecutor compares the executors by identity and reports any other one at once.
TEST_CASE("reactive control over a bridge: a distinct serial executor on the owner's thread is accepted",
          "[reactive][control]") {
    BridgeWiring wiring;
    morph::testing::OwnerProbeRecorder const probe{wiring.owner.coreExecutor()};
    morph::testing::StepExecutor callbacks;
    morph::bridge::BridgeHandler<ReactiveHitModel> onOwnerThread{wiring.bridge, &callbacks};
    Runtime runtime{wiring.owner};
    Bumps const latest{runtime, onOwnerThread};
    CHECK(probe.count(site::kHandlerExecutor) == 0);
    callbacks.runAll();  // the check runs where the executor runs: on this thread, the owner's
    CHECK(probe.count(site::kHandlerExecutor) == 0);
}

// Mutation: drop the isSerial() branch of detail::checkHandlerExecutor.
TEST_CASE("reactive control over a bridge: a handler delivering on a pool is reported at construction",
          "[reactive][control][misuse]") {
    BridgeWiring wiring;
    morph::testing::OwnerProbeRecorder const probe{wiring.owner.coreExecutor()};
    morph::exec::ThreadPoolExecutor pool{1};
    morph::bridge::BridgeHandler<ReactiveHitModel> onPool{wiring.bridge, &pool};
    Runtime runtime{wiring.owner};
    Mutation<ReactiveBump> const bump{runtime, onPool};
    CHECK(probe.count(site::kHandlerExecutor) == 1);
    Query<ReactiveReadHits> const hits{runtime, onPool, [] { return std::optional<ReactiveReadHits>{}; }};
    Bumps const latest{runtime, onPool};
    CHECK(probe.count(site::kHandlerExecutor) == 3);
}

// Mutations: report a serial executor at construction instead of checking where it runs (the count is 1 before
// the executor runs); skip the posted check (the count stays 0).
TEST_CASE("reactive control over a bridge: a serial executor on another thread is reported where it runs",
          "[reactive][control][misuse]") {
    BridgeWiring wiring;
    morph::testing::OwnerProbeRecorder const probe{wiring.owner.coreExecutor()};
    morph::testing::StepExecutor callbacks;
    morph::bridge::BridgeHandler<ReactiveHitModel> offOwner{wiring.bridge, &callbacks};
    Runtime runtime{wiring.owner};
    Bumps const latest{runtime, offOwner};
    CHECK(probe.count(site::kHandlerExecutor) == 0);  // not yet: checked where it runs
    std::thread{[&] { callbacks.runAll(); }}.join();
    CHECK(probe.count(site::kHandlerExecutor) == 1);
}
