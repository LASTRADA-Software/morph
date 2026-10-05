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
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "owner_probe_recorder.hpp"
#include "reactive_fake_server.hpp"
#include "test_support.hpp"

namespace {

using morph::async::Completion;
using morph::reactive::Computed;
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

// A screen's worth of control nodes, destroyed together. The queries come first by convention; the rule
// is only that none is destroyed while the mutation lives.
struct Screen {
    Screen(Runtime& runtime, List::Fetch fetchList, List::Fetch fetchDetail, Saver::Run run)
        : list{runtime, std::move(fetchList), [] { return std::optional{Lookup{0}}; }},
          detail{runtime, std::move(fetchDetail), [] { return std::optional{Lookup{1}}; }},
          save{runtime, std::move(run), MutationOptions{.invalidates = {&list, &detail}}} {}

    List list;
    List detail;
    Saver save;
};

}  // namespace

TEST_CASE("reactive::Mutation: pending() counts overlapping calls", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    SaveServer server{owner};
    Saver save{runtime, server.via<Save>()};
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

TEST_CASE("reactive::Mutation: success invalidates every listed query in one flush", "[reactive][control]") {
    Owner owner;
    int frames = 0;
    Runtime runtime{owner, RuntimeOptions{.afterFlush = [&] { ++frames; }}};
    ListServer listServer{owner};
    ListServer detailServer{owner};
    SaveServer saveServer{owner};
    List list{runtime, listServer.via<Lookup>(), [] { return std::optional{Lookup{0}}; }};
    List detail{runtime, detailServer.via<Lookup>(), [] { return std::optional{Lookup{1}}; }};
    Saver save{runtime, saveServer.via<Save>(), MutationOptions{.invalidates = {&list, &detail}}};
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

TEST_CASE("reactive::Mutation: a failure sets error() and invalidates nothing", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    ListServer listServer{owner};
    SaveServer saveServer{owner};
    List list{runtime, listServer.via<Lookup>(), [] { return std::optional{Lookup{0}}; }};
    Saver save{runtime, saveServer.via<Save>(), MutationOptions{.invalidates = {&list}}};
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

TEST_CASE("reactive::Mutation: action(make) issues only what make returns", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    SaveServer server{owner};
    Signal<std::string> amount{runtime, ""};
    Saver save{runtime, server.via<Save>()};
    auto const submit = save.action([&]() -> std::optional<Save> {
        if (amount.peek().empty()) {
            return std::nullopt;
        }
        return Save{static_cast<int>(amount.peek().size())};
    });
    submit();
    CHECK(server.calls() == 0);
    CHECK_FALSE(save.pending());
    amount.set("12");
    submit();
    REQUIRE(server.calls() == 1);
    CHECK(server.idOf(0) == 2);
    CHECK(save.pending());
    // Built per call: the second click sees the inputs as they are now.
    amount.set("12345");
    submit();
    REQUIRE(server.calls() == 2);
    CHECK(server.idOf(1) == 5);
}

TEST_CASE("reactive::Mutation: invalidating an idle query issues nothing", "[reactive][control]") {
    Owner owner;
    Runtime runtime{owner};
    ListServer listServer{owner};
    SaveServer saveServer{owner};
    List idle{runtime, listServer.via<Lookup>(), [] { return std::optional<Lookup>{}; }};
    Saver save{runtime, saveServer.via<Save>(), MutationOptions{.invalidates = {&idle}}};
    save.run(Save{1});
    saveServer.resolve(0, 1);
    owner.runAll();
    CHECK(save.lastResult() == std::optional{1});
    CHECK(listServer.calls() == 0);
    CHECK_FALSE(idle.pending());
}

TEST_CASE("reactive::Mutation: a runner that throws is an error, not a stuck pending()", "[reactive][control]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    SaveServer server{owner};
    bool down = false;
    Saver save{runtime, [&](Save const& action) {
                   if (down) {
                       throw std::runtime_error{"no route"};
                   }
                   return server.fetch(action);
               }};
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

TEST_CASE("reactive::Mutation: a destroyed Mutation gates late replies", "[reactive][control][lifetime]") {
    Owner owner;
    Runtime runtime{owner};
    ListServer listServer{owner};
    SaveServer saveServer{owner};
    List list{runtime, listServer.via<Lookup>(), [] { return std::optional{Lookup{0}}; }};
    auto save = std::make_unique<Saver>(runtime, saveServer.via<Save>(), MutationOptions{.invalidates = {&list}});
    save->run(Save{1});
    save->run(Save{2});
    save.reset();
    saveServer.resolve(0, 1);
    saveServer.reject(1, "late");
    owner.runAll();  // a delivery into the destroyed Mutation would be a use-after-free under ASan
    CHECK(listServer.calls() == 1);
}

TEST_CASE("reactive::Mutation: a reply whose earlier handler destroys the Mutation is not delivered into it",
          "[reactive][control][lifetime]") {
    Owner owner;
    Runtime runtime{owner};
    ListServer listServer{owner};
    SaveServer saveServer{owner};
    List list{runtime, listServer.via<Lookup>(), [] { return std::optional{Lookup{0}}; }};
    std::unique_ptr<Saver> save;
    auto const run = [&](Save const& action) {
        Completion<int> completion = saveServer.fetch(action);
        completion.thenDetached([&](int) { save.reset(); });
        completion.onErrorDetached([&](std::exception_ptr const&) { save.reset(); });
        return completion;
    };
    save = std::make_unique<Saver>(runtime, run, MutationOptions{.invalidates = {&list}});
    save->run(Save{1});
    SECTION("a result") { saveServer.resolve(0, 1); }
    SECTION("a failure") { saveServer.reject(0, "down"); }
    owner.runAll();  // the Mutation's own handler runs after the reset: ASan sees a use-after-free if it is let in
    CHECK(save == nullptr);
    CHECK(listServer.calls() == 1);
}

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

TEST_CASE("reactive::Mutation: a delivery off the owner is reported and dropped", "[reactive][control][misuse]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    morph::exec::MainThreadExecutor foreign;
    ListServer listServer{owner};
    SaveServer server{foreign};
    List list{runtime, listServer.via<Lookup>(), [] { return std::optional{Lookup{0}}; }};
    Saver save{runtime, server.via<Save>(), MutationOptions{.invalidates = {&list}}};
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

TEST_CASE("reactive::Mutation: executes through a bridge handler and refetches what it invalidates",
          "[reactive][control]") {
    BumpWiring wiring;
    Runtime runtime{wiring.owner};
    Query<MutationReadHits> hits{runtime, wiring.handler, [] { return std::optional{MutationReadHits{}}; }};
    Mutation<MutationBump> bump{runtime, wiring.handler, MutationOptions{.invalidates = {&hits}}};
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
