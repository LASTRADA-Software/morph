// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/testing/owner_probe_recorder.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "reactive_fake_server.hpp"
#include "test_support.hpp"

namespace {

using morph::async::Completion;
using morph::reactive::Computed;
// Spelled rx::Concurrency: on Windows a using-declaration collides with the ConcRT
// `::Concurrency` namespace the MSVC headers declare.
namespace rx = morph::reactive;
using morph::reactive::Effect;
using morph::reactive::Mutation;
using morph::reactive::MutationOptions;
using morph::reactive::Query;
using morph::reactive::Runtime;
using morph::reactive::RuntimeOptions;
using morph::reactive::Signal;
using Owner = morph::testing::StepExecutor;
using ListServer = morph::testing::FakeServer<>;
using SaveServer = morph::testing::FakeServer<int>;
namespace site = morph::reactive::detail::site;

struct Lookup {
    int id = 0;
    bool operator==(Lookup const&) const = default;
};

struct Save {
    int id = 0;
};

using List = Query<Lookup, std::string>;
using Saver = Mutation<Save, int>;
using SaveOptions = MutationOptions<Save>;

// A screen's worth of control nodes, destroyed together.
struct Screen {
    Screen(Runtime& runtime, List::Fetch fetchList, List::Fetch fetchDetail, Saver::Run run)
        : list{runtime, std::move(fetchList), [] { return std::optional{Lookup{0}}; }},
          detail{runtime, std::move(fetchDetail), [] { return std::optional{Lookup{1}}; }},
          save{runtime, std::move(run), SaveOptions{.invalidates = {list.link(), detail.link()}}} {}

    List list;
    List detail;
    Saver save;
};

}  // namespace

// Mutation: Mutation::send sets the in-flight count to one instead of adding one.
TEST_CASE("reactive::Mutation: pending() counts overlapping calls", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    SaveServer server{owner};
    Saver save{runtime, server.via<Save>(), SaveOptions{.concurrency = rx::Concurrency::Parallel}};
    CHECK_FALSE(save.pending());
    save.run(Save{1});
    save.run(Save{2});
    CHECK(save.pending());
    server.resolve(0, 10);
    owner.runAll();
    CHECK(save.pending());
    server.resolve(1, 20);
    owner.runAll();
    CHECK_FALSE(save.pending());
    CHECK(save.lastResult() == std::optional{20});
}

// ── Concurrency modes ──────────────────────────────────────────────────────

// Mutation: drop the busy check from the Exclusive case of Mutation::run.
TEST_CASE("reactive::Mutation: Exclusive is the default and refuses a run while one is in flight",
          "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    SaveServer server{owner};
    Saver save{runtime, server.via<Save>()};
    CHECK(save.run(Save{1}));
    CHECK_FALSE(save.run(Save{2}));
    CHECK(server.calls() == 1);
    server.resolve(0, 10);
    owner.runAll();
    CHECK_FALSE(save.pending());
    CHECK(save.run(Save{3}));
    CHECK(server.calls() == 2);
}

// Mutation: apply only the newest reply in every mode (`applies` true only for the newest generation).
TEST_CASE("reactive::Mutation: Parallel sends every run and applies each reply as it arrives", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    SaveServer server{owner};
    Saver save{runtime, server.via<Save>(), SaveOptions{.concurrency = rx::Concurrency::Parallel}};
    CHECK(save.run(Save{1}));
    CHECK(save.run(Save{2}));
    REQUIRE(server.calls() == 2);
    server.resolve(1, 20);
    owner.runAll();
    CHECK(save.pending());
    CHECK(save.lastResult() == std::optional{20});
    server.resolve(0, 10);
    owner.runAll();
    CHECK_FALSE(save.pending());
    CHECK(save.lastResult() == std::optional{10});
    CHECK(save.successCount() == 2);
}

// Mutation: make `applies` always true in Mutation::settle.
TEST_CASE("reactive::Mutation: Latest sends every run and applies only the newest reply", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    SaveServer server{owner};
    Saver save{runtime, server.via<Save>(), SaveOptions{.concurrency = rx::Concurrency::Latest}};
    CHECK(save.run(Save{1}));
    CHECK(save.run(Save{2}));
    REQUIRE(server.calls() == 2);
    server.resolve(1, 20);
    owner.runAll();
    CHECK(save.pending());      // the older run is still in flight
    server.reject(0, "stale");  // not applied: it only stops counting as pending
    owner.runAll();
    CHECK_FALSE(save.pending());
    CHECK(save.lastResult() == std::optional{20});
    CHECK(save.error() == nullptr);
    CHECK(save.successCount() == 1);
}

// Mutation: Mutation::run sends a Serial run at once even when its lane is active.
TEST_CASE("reactive::Mutation: Serial queues runs and sends each after the previous one settles",
          "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    SaveServer server{owner};
    Saver save{runtime, server.via<Save>(), SaveOptions{.concurrency = rx::Concurrency::Serial}};
    CHECK(save.run(Save{1}));
    CHECK(save.run(Save{2}));
    CHECK(save.run(Save{3}));
    REQUIRE(server.calls() == 1);
    server.resolve(0, 10);
    owner.runAll();
    REQUIRE(server.calls() == 2);
    CHECK(server.idOf(1) == 2);
    server.reject(1, "conflict");  // a failure does not stall the queue
    owner.runAll();
    REQUIRE(server.calls() == 3);
    CHECK(server.idOf(2) == 3);
    CHECK(save.pending());
    server.resolve(2, 30);
    owner.runAll();
    CHECK_FALSE(save.pending());
    CHECK(save.error() == nullptr);
    CHECK(save.lastResult() == std::optional{30});
    CHECK(save.successCount() == 2);
}

// Mutation: Mutation::run ignores serialKey and puts every Serial run in one lane.
TEST_CASE("reactive::Mutation: Serial with serialKey keeps one queue per key", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    SaveServer server{owner};
    Saver save{runtime, server.via<Save>(),
               SaveOptions{.concurrency = rx::Concurrency::Serial,
                           .serialKey = [](Save const& action) { return std::to_string(action.id % 10); }}};
    save.run(Save{1});   // key 1
    save.run(Save{11});  // key 1: queued behind 1
    save.run(Save{2});   // key 2: sent at once
    REQUIRE(server.calls() == 2);
    CHECK(server.idOf(0) == 1);
    CHECK(server.idOf(1) == 2);
    server.resolve(0, 1);
    owner.runAll();
    REQUIRE(server.calls() == 3);
    CHECK(server.idOf(2) == 11);
}

// Mutation: send() counts a dequeued Serial run again (pass counted = false), and pending() never falls.
TEST_CASE("reactive::Mutation: Serial with a runner that throws moves on to the next queued run",
          "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    SaveServer server{owner};
    Saver save{runtime,
               [&](Save const& action) {
                   if (action.id == 2) {
                       throw std::runtime_error{"no route"};
                   }
                   return server.fetch(action);
               },
               SaveOptions{.concurrency = rx::Concurrency::Serial}};
    save.run(Save{1});
    save.run(Save{2});
    save.run(Save{3});
    server.resolve(0, 1);
    owner.runAll();
    REQUIRE(server.calls() == 2);
    CHECK(server.idOf(1) == 3);
    CHECK(morph::reactive::errorMessage(save.error()) == "no route");
    server.resolve(1, 3);
    owner.runAll();
    CHECK_FALSE(save.pending());
    CHECK(save.lastResult() == std::optional{3});
}

// Mutation: Mutation::send counts the run before calling the runner, so a runner that throws leaves it pending.
TEST_CASE("reactive::Mutation: after a runner throws, Exclusive accepts the next run", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    Saver save{runtime, [](Save) -> Completion<int> { throw std::runtime_error{"no connection"}; }};
    CHECK(save.run(Save{1}));
    CHECK_FALSE(save.pending());
    CHECK(morph::reactive::errorMessage(save.error()) == "no connection");
    CHECK(save.run(Save{2}));
}

// ── Invalidation ───────────────────────────────────────────────────────────

// Mutations: drop `_link->clear()` from ~Query (the success refetches the freed query: ASan, or a crash); hold raw
// query pointers instead of links.
TEST_CASE("reactive::Mutation: a destroyed query's link refetches nothing", "[reactive][control][lifetime]") {
    Owner owner;
    Runtime runtime{owner};
    ListServer listServer{owner};
    SaveServer saveServer{owner};
    auto list = std::make_unique<List>(runtime, listServer.via<Lookup>(), [] { return std::optional{Lookup{0}}; });
    auto const link = list->link();
    Saver save{runtime, saveServer.via<Save>(), SaveOptions{.invalidates = {link}}};
    list.reset();
    CHECK_FALSE(link->live());
    save.run(Save{1});
    saveServer.resolve(0, 1);
    owner.runAll();
    CHECK(save.successCount() == 1);
    CHECK(listServer.calls() == 1);
}

// The delivery is one owner task, and the result, the success count, the pending count and both refetches it
// writes post exactly one flush. Mutation: Mutation::settle posts each refetch to the owner instead of running it
// in the batch.
TEST_CASE("reactive::Mutation: a success and its invalidations are one flush, counted in the owner's posts",
          "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    ListServer listServer{owner};
    ListServer detailServer{owner};
    SaveServer saveServer{owner};
    Screen screen{runtime, listServer.via<Lookup>(), detailServer.via<Lookup>(), saveServer.via<Save>()};
    std::vector<std::string> frames;
    Effect const render{runtime, [&] {
                            frames.push_back(std::string{screen.list.pending() ? "list loading" : "list"} + ", " +
                                             (screen.detail.pending() ? "detail loading" : "detail") + ", " +
                                             (screen.save.pending() ? "saving" : "saved") + " " +
                                             std::to_string(screen.save.successCount()));
                        }};
    listServer.resolve(0, "list");
    detailServer.resolve(0, "detail");
    owner.runAll();
    screen.save.run(Save{1});
    owner.runAll();
    frames.clear();

    saveServer.resolve(0, 1);
    REQUIRE(owner.pending() == 1);  // the delivery
    REQUIRE(owner.runOne());
    CHECK(owner.pending() == 1);  // one flush, for everything the delivery wrote
    CHECK(listServer.calls() == 2);
    CHECK(detailServer.calls() == 2);
    owner.runAll();
    CHECK(frames == std::vector<std::string>{"list loading, detail loading, saved 1"});
}

// Mutation: drop the invalidation loop from Mutation::settle.
TEST_CASE("reactive::Mutation: success invalidates every listed query in one flush", "[reactive][control]") {
    Owner owner;
    int frames = 0;
    Runtime runtime{owner, RuntimeOptions{.afterFlush = [&] { ++frames; }}};
    ListServer listServer{owner};
    ListServer detailServer{owner};
    SaveServer saveServer{owner};
    List list{runtime, listServer.via<Lookup>(), [] { return std::optional{Lookup{0}}; }};
    List detail{runtime, detailServer.via<Lookup>(), [] { return std::optional{Lookup{1}}; }};
    Saver save{runtime, saveServer.via<Save>(), SaveOptions{.invalidates = {list.link(), detail.link()}}};
    Effect const render{runtime, [&] {
                            static_cast<void>(list.pending());
                            static_cast<void>(detail.pending());
                            static_cast<void>(save.pending());
                        }};
    listServer.resolve(0, "list");
    detailServer.resolve(0, "detail");
    owner.runAll();

    save.run(Save{1});
    owner.runAll();
    int const before = frames;
    saveServer.resolve(0, 1);
    owner.runAll();
    CHECK(listServer.calls() == 2);
    CHECK(detailServer.calls() == 2);
    CHECK(frames == before + 1);
    CHECK(list.pending());
    CHECK(list.value() == std::optional<std::string>{"list"});
}

// Mutation: Mutation::settle refetches the invalidated queries on a failure too.
TEST_CASE("reactive::Mutation: a failure sets error() and invalidates nothing", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    ListServer listServer{owner};
    SaveServer saveServer{owner};
    List const list{runtime, listServer.via<Lookup>(), [] { return std::optional{Lookup{0}}; }};
    Saver save{runtime, saveServer.via<Save>(), SaveOptions{.invalidates = {list.link()}}};
    save.run(Save{1});
    saveServer.reject(0, "conflict");
    owner.runAll();
    CHECK(morph::reactive::errorMessage(save.error()) == "conflict");
    CHECK_FALSE(save.pending());
    CHECK_FALSE(save.lastResult().has_value());
    CHECK(listServer.calls() == 1);

    save.run(Save{2});
    saveServer.resolve(1, 2);
    owner.runAll();
    CHECK(save.error() == nullptr);
    CHECK(save.lastResult() == std::optional{2});
    CHECK(listServer.calls() == 2);
}

// Mutation: Query::refetch issues a default key while idle.
TEST_CASE("reactive::Mutation: invalidating an idle query issues nothing", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    ListServer listServer{owner};
    SaveServer saveServer{owner};
    List const idle{runtime, listServer.via<Lookup>(), [] { return std::optional<Lookup>{}; }};
    Saver save{runtime, saveServer.via<Save>(), SaveOptions{.invalidates = {idle.link()}}};
    save.run(Save{1});
    saveServer.resolve(0, 1);
    owner.runAll();
    CHECK(save.lastResult() == std::optional{1});
    CHECK(listServer.calls() == 0);
    CHECK_FALSE(idle.pending());
}

// Mutation: Mutation::send counts the run before calling the runner.
TEST_CASE("reactive::Mutation: a runner that throws is an error, not a stuck pending()", "[reactive][control]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    SaveServer server{owner};
    bool down = false;
    Saver save{runtime,
               [&](Save const& action) {
                   if (down) {
                       throw std::runtime_error{"no route"};
                   }
                   return server.fetch(action);
               },
               SaveOptions{.concurrency = rx::Concurrency::Parallel}};
    save.run(Save{1});
    REQUIRE(save.pending());

    // A call already in flight stays counted; only the one that threw is not.
    down = true;
    save.run(Save{2});
    owner.runAll();
    CHECK(server.calls() == 1);
    CHECK(save.pending());
    CHECK(morph::reactive::errorMessage(save.error()) == "no route");

    server.resolve(0, 1);
    owner.runAll();
    CHECK_FALSE(save.pending());
    CHECK(save.error() == nullptr);
    CHECK(save.lastResult() == std::optional{1});

    // With nothing else in flight, a throw leaves nothing pending.
    save.run(Save{3});
    owner.runAll();
    CHECK_FALSE(save.pending());
    CHECK(morph::reactive::errorMessage(save.error()) == "no route");
    CHECK(probe.count(site::kOffOwner) == 0);
}

// Mutation: Mutation::settle clears lastResult on a failure.
TEST_CASE("reactive::Mutation: lastResult() survives a later failure", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    SaveServer server{owner};
    Saver save{runtime, server.via<Save>()};
    save.run(Save{1});
    server.resolve(0, 1);
    owner.runAll();
    save.run(Save{2});
    server.reject(1, "conflict");
    owner.runAll();
    CHECK(save.lastResult() == std::optional{1});
    CHECK(morph::reactive::errorMessage(save.error()) == "conflict");
}

// Mutation: attach the success handler without `_lifetime` (crashes).
TEST_CASE("reactive::Mutation: a destroyed Mutation gates late replies", "[reactive][control][lifetime]") {
    Owner owner;
    Runtime runtime{owner};
    ListServer listServer{owner};
    SaveServer saveServer{owner};
    List const list{runtime, listServer.via<Lookup>(), [] { return std::optional{Lookup{0}}; }};
    auto save =
        std::make_unique<Saver>(runtime, saveServer.via<Save>(),
                                SaveOptions{.concurrency = rx::Concurrency::Parallel, .invalidates = {list.link()}});
    save->run(Save{1});
    save->run(Save{2});
    save.reset();
    saveServer.resolve(0, 1);
    saveServer.reject(1, "late");
    owner.runAll();  // a delivery into the destroyed Mutation would be a use-after-free under ASan
    CHECK(listServer.calls() == 1);
}

// Mutation: attach the success handler without `_lifetime` (crashes).
TEST_CASE("reactive::Mutation: a reply whose earlier handler destroys the Mutation is not delivered into it",
          "[reactive][control][lifetime]") {
    Owner owner;
    Runtime runtime{owner};
    ListServer listServer{owner};
    SaveServer saveServer{owner};
    List const list{runtime, listServer.via<Lookup>(), [] { return std::optional{Lookup{0}}; }};
    std::unique_ptr<Saver> save;
    auto const run = [&](Save const& action) {
        Completion<int> completion = saveServer.fetch(action);
        completion.thenDetached([&](int) { save.reset(); });
        completion.onErrorDetached([&](std::exception_ptr const&) { save.reset(); });
        return completion;
    };
    save = std::make_unique<Saver>(runtime, run, SaveOptions{.invalidates = {list.link()}});
    save->run(Save{1});
    SECTION("a result") { saveServer.resolve(0, 1); }
    SECTION("a failure") { saveServer.reject(0, "down"); }
    owner.runAll();  // the Mutation's own handler runs after the reset: ASan sees a use-after-free if it is let in
    CHECK(save == nullptr);
    CHECK(listServer.calls() == 1);
}

// Mutation: drop the lifetime check after each refetch in Mutation::settle: the next refetch reads the freed
// mutation's options (ASan heap-use-after-free; a plain build does not crash).
TEST_CASE("reactive::Mutation: an invalidation that destroys the screen touches nothing afterwards",
          "[reactive][control][lifetime]") {
    Owner owner;
    Runtime runtime{owner};
    ListServer listServer{owner};
    ListServer detailServer{owner};
    SaveServer saveServer{owner};
    std::unique_ptr<Screen> screen;
    auto const fetchList = [&](Lookup const& action) {
        // The action belongs to the Query: read it before destroying the Query.
        Lookup const copy = action;
        if (listServer.calls() == 1) {
            screen.reset();
        }
        return listServer.fetch(copy);
    };
    screen = std::make_unique<Screen>(runtime, fetchList, detailServer.via<Lookup>(), saveServer.via<Save>());
    REQUIRE(listServer.calls() == 1);
    REQUIRE(detailServer.calls() == 1);
    screen->save.run(Save{1});
    saveServer.resolve(0, 1);
    owner.runAll();  // invalidating `detail` after `list` destroyed the screen would be a use-after-free under ASan
    CHECK(screen == nullptr);
    CHECK(listServer.calls() == 2);
    CHECK(detailServer.calls() == 1);
}

// Mutation: Mutation::send holds the runner by reference, or settles a synchronous throw without checking the
// lifetime token (crashes); dropping the check after the runner returns is seen by ASan only.
TEST_CASE("reactive::Mutation: a runner that destroys the Mutation leaves it alone afterwards",
          "[reactive][control][lifetime]") {
    Owner owner;
    Runtime runtime{owner};
    SaveServer server{owner};
    std::unique_ptr<Saver> save;
    bool throws = false;
    auto const run = [&](Save const& action) {
        save.reset();
        if (throws) {
            throw std::runtime_error{"gone"};
        }
        return server.fetch(action);
    };
    save = std::make_unique<Saver>(runtime, run);
    SECTION("and returns") {
        save->run(Save{1});
        CHECK(server.calls() == 1);
        server.resolve(0, 1);
        owner.runAll();
    }
    SECTION("and throws") {
        throws = true;
        save->run(Save{1});
        owner.runAll();
        CHECK(server.calls() == 0);
    }
    CHECK(save == nullptr);
}

// Mutation: flush inline at the end of Mutation::settle and read a member after it: the Effect destroys the mutation
// inside its own delivery (ASan heap-use-after-free).
TEST_CASE("reactive::Mutation: an Effect reading it may destroy it when the reply lands",
          "[reactive][control][lifetime]") {
    Owner owner;
    Runtime runtime{owner};
    SaveServer server{owner};
    auto save = std::make_unique<Saver>(runtime, server.via<Save>());
    std::optional<int> seen;
    Effect const closer{runtime, [&] {
                            if (save == nullptr) {
                                return;
                            }
                            if (auto const& result = save->lastResult()) {
                                seen = result;
                                save.reset();
                            }
                        }};
    save->run(Save{1});
    server.resolve(0, 7);
    owner.runAll();
    CHECK(save == nullptr);
    CHECK(seen == std::optional{7});
}

// Mutation: Mutation::send calls the runner tracked.
TEST_CASE("reactive::Mutation: run() from an Effect does not subscribe it to what the runner reads",
          "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    SaveServer server{owner};
    Signal<int> token{runtime, 0};  // read only by the runner, as a credential would be
    Saver save{runtime, [&](Save const& action) {
                   static_cast<void>(token.get());
                   return server.fetch(action);
               }};
    int runs = 0;
    Effect const saveOnce{runtime, [&] {
                              ++runs;
                              save.run(Save{1});
                          }};
    REQUIRE(server.calls() == 1);

    token.set(1);
    owner.runAll();
    CHECK(runs == 1);
    CHECK(server.calls() == 1);
}

// Mutation: drop the isComputing() refusal in Mutation::run.
TEST_CASE("reactive::Mutation: run() inside a Computed is reported and issues nothing",
          "[reactive][control][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    SaveServer server{owner};
    Saver save{runtime, server.via<Save>()};
    Computed<int> const sneaky{runtime, [&] {
                                   save.run(Save{1});
                                   return 1;
                               }};
    CHECK(sneaky.get() == 1);
    CHECK(probe.count(site::kIssueInComputed) == 1);
    CHECK(server.calls() == 0);
    CHECK_FALSE(save.pending());
}

// Mutation: Mutation::run ignores checkOwner().
TEST_CASE("reactive::Mutation: run() off the owner is reported and refused", "[reactive][control][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    SaveServer server{owner};
    Saver save{runtime, server.via<Save>()};
    std::thread{[&] { save.run(Save{1}); }}.join();
    CHECK(probe.count(site::kOffOwner) == 1);
    CHECK(server.calls() == 0);
    CHECK_FALSE(save.pending());
}

// Mutation: the success handler settles whatever checkOwner() says.
TEST_CASE("reactive::Mutation: a delivery off the owner is reported and dropped", "[reactive][control][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    morph::exec::MainThreadExecutor foreign;
    ListServer listServer{owner};
    SaveServer server{foreign};
    List const list{runtime, listServer.via<Lookup>(), [] { return std::optional{Lookup{0}}; }};
    Saver save{runtime, server.via<Save>(), SaveOptions{.invalidates = {list.link()}}};
    save.run(Save{1});
    SECTION("a result") { server.resolve(0, 1); }
    SECTION("a failure") { server.reject(0, "down"); }
    bool reported = false;
    std::thread{[&] {
        reported = morph::testing::pumpOwnerUntil(foreign, [&] { return probe.count(site::kOffOwner) >= 1; });
    }}.join();
    CHECK(reported);
    // Refused once, at the handler: no write and no invalidation inside it is attempted.
    CHECK(probe.count(site::kOffOwner) == 1);
    CHECK_FALSE(save.lastResult().has_value());
    CHECK(save.error() == nullptr);
    // Never counted down: pending() stays true for good, which is the visible symptom of this wiring error.
    CHECK(save.pending());
    CHECK(listServer.calls() == 1);
}

// ── Over a real LocalBackend ───────────────────────────────────────────────

// Model, action and result types need external linkage: the BRIDGE_REGISTER_* macros specialise templates at
// global scope.
// NOLINTBEGIN(misc-use-internal-linkage)
struct MutationReadHits {};
struct MutationBump {};
struct MutationHits {
    std::int64_t value = 0;
};

struct MutationHitModel {
    std::int64_t hits = 0;

    [[nodiscard]] std::int64_t execute(MutationReadHits const& /*action*/) const { return hits; }

    MutationHits execute(MutationBump const& /*action*/) {
        ++hits;
        return MutationHits{.value = hits};
    }
};

BRIDGE_REGISTER_MODEL(MutationHitModel, "Test_MutationHitModel")
BRIDGE_REGISTER_ACTION(MutationHitModel, MutationReadHits, "Test_MutationReadHits")
BRIDGE_REGISTER_ACTION(MutationHitModel, MutationBump, "Test_MutationBump")
// NOLINTEND(misc-use-internal-linkage)

namespace {

struct BumpWiring {
    morph::exec::ThreadPoolExecutor pool{1};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<MutationHitModel> handler{bridge, &owner};
};

}  // namespace

// Mutation: drop the invalidation loop from Mutation::settle.
TEST_CASE("reactive::Mutation: executes through a bridge handler and refetches what it invalidates",
          "[reactive][control]") {
    BumpWiring wiring;
    Runtime runtime{wiring.owner};
    Query<MutationReadHits> hits{runtime, wiring.handler, [] { return std::optional{MutationReadHits{}}; }};
    Mutation<MutationBump> bump{runtime, wiring.handler, MutationOptions<MutationBump>{.invalidates = {hits.link()}}};
    REQUIRE(
        morph::testing::pumpOwnerUntil(wiring.owner, [&] { return hits.value() == std::optional<std::int64_t>{0}; }));

    bump.run(MutationBump{});
    CHECK(bump.pending());
    REQUIRE(
        morph::testing::pumpOwnerUntil(wiring.owner, [&] { return hits.value() == std::optional<std::int64_t>{1}; }));
    CHECK_FALSE(bump.pending());
    REQUIRE(bump.lastResult().has_value());
    CHECK(bump.lastResult()->value == 1);
}
