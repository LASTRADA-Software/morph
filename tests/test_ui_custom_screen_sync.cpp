// SPDX-License-Identifier: Apache-2.0
//
// The client of the custom screens model (docs/spec/ui/custom_screens.md, "The client"): discovery by `list`, the
// cache `DocumentScreenHostOptions::documents` answers custom ids from, its refresh by hash, the server's check and
// the store's refusal passed through, routes fetched before a restore, and the list refreshed by events.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <expected>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/wire.hpp>
#include <morph/ui/custom_screen_sync.hpp>
#include <morph/ui/custom_screens.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_custom_client_support.hpp"

using custom_client::SyncClient;
using morph::ui::CustomScreensOffer;

namespace {

/// Captures every warning logged while it lives.
struct Warnings {
    std::vector<std::string> seen;
    morph::log::ScopedLoggerOverride guard{[this](morph::log::LogLevel level, std::string_view message) {
        if (level == morph::log::LogLevel::warn) {
            seen.emplace_back(message);
        }
    }};
};

morph::wire::EventBody changesOf(std::string_view record) {
    return morph::wire::EventBody{
        .topic = std::string{morph::wire::kChangesTopic},
        .payload = glz::raw_json{R"({"records":[{"record":")" + std::string{record} + R"("}],"instances":[]})"}};
}

}  // namespace

// Mutation: log every refusal at warn, `unknown model type` included (one warning is captured).
TEST_CASE("custom screen sync: a server without the model is off, and nothing is logged",
          "[ui][custom-screens][sync]") {
    Warnings warnings;
    custom_client::BareServer server;
    SyncClient client{*server.server, "ann", "lims"};
    CHECK_FALSE(client.probe());
    CHECK(client.sync.offer() == CustomScreensOffer::Off);
    INFO("refusal: " << client.sync.offRefusal());
    // The spec says the dispatcher's refusal reads `unknown model type: morph.customScreens`. If this fails, the
    // wording differs on this transport: stop and report it, since the spec's discovery rests on it.
    CHECK(client.sync.offRefusal().contains("unknown model type: morph.customScreens"));
    // Other parts of the process may warn too (the server about a refused registration); only the sync's own matters.
    CHECK(std::ranges::none_of(warnings.seen,
                               [](std::string const& seen) { return seen.contains("custom screens are off"); }));
}

// Mutation: treat a refused `list` as offered (`offer()` reads Offered and nothing is logged).
TEST_CASE("custom screen sync: any other refusal of list is off too, and is logged at warn",
          "[ui][custom-screens][sync]") {
    Warnings warnings;
    screens_test::Server server;
    SyncClient client{*server.server, "ann", "elsewhere"};
    CHECK_FALSE(client.probe());
    CHECK(client.sync.offer() == CustomScreensOffer::Off);
    CHECK(std::ranges::count_if(warnings.seen, [](std::string const& seen) {
              return seen.contains("custom screens are off") && seen.contains("morph.customScreens.list");
          }) == 1);
}

// Mutation: keep a cached document whose listed hash changed (`find` still answers it after the relist).
TEST_CASE("custom screen sync: list, open into the cache, and a screen changed on the server is dropped from it",
          "[ui][custom-screens][sync]") {
    screens_test::Server server;
    std::string const id = custom_client::savedIdOf(server.save(screens_test::baseScreen("lims.base"), "ann", "Mine"));
    SyncClient client{*server.server, "ann", "lims"};
    REQUIRE(client.probe());
    REQUIRE(client.sync.listed().screens.size() == 1);
    morph::ui::CustomScreenSummary const listed = client.sync.listed().screens.front();
    CHECK(listed.id == id);
    CHECK(listed.title == "Mine");
    CHECK(listed.mine);

    std::optional<std::expected<std::shared_ptr<morph::ui::Document const>, std::string>> opened;
    client.sync.open(id, [&opened](auto loaded) { opened = std::move(loaded); });
    REQUIRE(client.pump([&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    CHECK((**opened)->id == id);
    REQUIRE(client.sync.documents().find(id) != nullptr);
    REQUIRE(client.sync.documents().entry(id) != nullptr);
    CHECK(client.sync.documents().entry(id)->summary.hash == listed.hash);

    std::string const changed =
        screens_test::replaced(screens_test::baseScreen("lims.base"), R"("Results")", R"("Changed")");
    REQUIRE(server
                .call(morph::ui::kCustomScreensSave, custom_client::saveBody("lims", "Mine", "lims.base", changed, id),
                      "ann")
                .kind == "ok");
    client.sync.relist();
    REQUIRE(client.pump([&] {
        return !client.sync.listed().screens.empty() && client.sync.listed().screens.front().hash != listed.hash;
    }));
    CHECK(client.sync.documents().find(id) == nullptr);
}

// Mutation: answer every check as passing (the refusal is absent).
TEST_CASE("custom screen sync: check answers the server's refusal with its pointer", "[ui][custom-screens][sync]") {
    screens_test::Server server;
    SyncClient client{*server.server, "ann", "lims"};
    REQUIRE(client.probe());
    std::string const erasing =
        screens_test::replaced(screens_test::baseScreen("lims.base"), R"({"host":"print"})", R"({"host":"erase"})");
    std::optional<std::expected<std::optional<morph::ui::CheckRefusal>, std::string>> refused;
    client.sync.check("lims.base", erasing, [&refused](auto checked) { refused = std::move(checked); });
    REQUIRE(client.pump([&refused] { return refused.has_value(); }));
    REQUIRE(refused->has_value());
    REQUIRE((*refused)->has_value());
    CHECK_FALSE((**refused)->pointer.empty());
    CHECK_FALSE((**refused)->reason.empty());

    std::optional<std::expected<std::optional<morph::ui::CheckRefusal>, std::string>> passed;
    client.sync.check("lims.base", screens_test::baseScreen("lims.base"),
                      [&passed](auto checked) { passed = std::move(checked); });
    REQUIRE(client.pump([&passed] { return passed.has_value(); }));
    REQUIRE(passed->has_value());
    CHECK_FALSE((*passed)->has_value());
}

// Mutation: report a refused save as saved (`done` gets an id).
TEST_CASE("custom screen sync: a store's refusal of save reaches the caller as the store gave it",
          "[ui][custom-screens][sync]") {
    morph::exec::ThreadPoolExecutor storePool{1};
    screens_test::Server server{std::make_shared<custom_client::RefusingStore>(storePool)};
    SyncClient client{*server.server, "ann", "lims"};
    REQUIRE(client.probe());
    std::optional<std::expected<std::string, std::string>> saved;
    client.sync.save(morph::ui::CustomScreensSave{.app = {},
                                                  .id = std::nullopt,
                                                  .title = "Mine",
                                                  .basedOn = "lims.base",
                                                  .document = screens_test::baseScreen("lims.base")},
                     [&saved](std::expected<std::string, std::string> answer) { saved = std::move(answer); });
    REQUIRE(client.pump([&saved] { return saved.has_value(); }));
    REQUIRE_FALSE(saved->has_value());
    CHECK(saved->error().contains("over your limit of custom screens"));
}

// Mutation: skip the fetch in prefetchRoutes (nothing is cached and nothing is missing).
TEST_CASE("custom screen sync: prefetchRoutes loads the custom screens a record names, and reports those get refuses",
          "[ui][custom-screens][sync]") {
    screens_test::Server server;
    std::string const id = custom_client::savedIdOf(server.save(screens_test::baseScreen("lims.base"), "ann"));
    SyncClient client{*server.server, "ann", "lims"};
    REQUIRE(client.probe());
    std::string const gone = "custom:" + std::string(32, '0');
    std::optional<std::vector<std::string>> missing;
    client.sync.prefetchRoutes({std::string{"lims.base"}, id, gone + "?x=1"},
                               [&missing](std::vector<std::string> ids) { missing = std::move(ids); });
    REQUIRE(client.pump([&missing] { return missing.has_value(); }));
    CHECK(*missing == std::vector<std::string>{gone});
    CHECK(client.sync.documents().find(id) != nullptr);
}

// Mutations: ignore a `changes` event (no list is received); relist on any record's change (two lists are received).
TEST_CASE("custom screen sync: a changes event naming the record relists, and one naming another does not",
          "[ui][custom-screens][sync]") {
    screens_test::Server server;
    SyncClient client{*server.server, "ann", "lims"};
    REQUIRE(client.probe());
    REQUIRE(client.sync.listed().screens.empty());
    std::string const id = custom_client::savedIdOf(server.save(screens_test::baseScreen("lims.base"), "ann"));
    std::size_t relisted = 0;
    client.sync.setOnListed(
        [&relisted](morph::ui::CustomScreensListed const&, std::vector<std::string> const&) { ++relisted; });

    client.sync.deliverEvent(changesOf("SomethingElse"));
    client.sync.deliverEvent(changesOf(morph::ui::kCustomScreenRecord));
    // The replies of one sync's calls arrive in call order, so once this `get` answers, every list asked before it has
    // been received: the count is final without waiting out a time.
    std::optional<std::expected<morph::ui::CustomScreenFound, std::string>> barrier;
    client.sync.get(id, [&barrier](auto found) { barrier = std::move(found); });
    REQUIRE(client.pump([&barrier] { return barrier.has_value(); }));
    CHECK(relisted == 1);
    CHECK(client.sync.listed().screens.size() == 1);
}

// Mutation: the session's watch set leaves the extra watch out (the `replace` watch lacks the record).
TEST_CASE("custom screen sync: the record is watched once offered, and a reconnect or resync keeps it watched",
          "[ui][custom-screens][sync]") {
    screens_test::Server server;
    SyncClient client{*server.server, "ann", "lims", true};
    REQUIRE(client.probe());
    REQUIRE(client.watches.size() == 1);
    CHECK(client.watches.front().starts_with("watch "));
    CHECK(client.watches.front().contains(R"("records":["morph.customScreens"])"));
    CHECK(client.watches.front().contains(R"("replace":false)"));

    auto const replaced = [&client](std::size_t index) {
        REQUIRE(client.watches.size() > index);
        CHECK(client.watches.at(index).contains(R"("replace":true)"));
        CHECK(client.watches.at(index).contains(R"("records":["morph.customScreens"])"));
    };
    // Either order of the sync's and the session's reconnect leaves the record watched.
    client.sync.reconnected();
    client.session.reconnected();
    replaced(1);
    client.session.reconnected();
    client.sync.reconnected();
    replaced(2);
    morph::wire::EventBody const resync{.topic = std::string{morph::wire::kResyncTopic},
                                        .payload = glz::raw_json{"null"}};
    client.sync.deliverEvent(resync);
    client.session.deliverEvent(resync);
    replaced(3);
    CHECK(client.watches.size() == 4);
}

// Mutation: `removeExtraWatch` sends nothing (no `unwatch` follows the reset); `addExtraWatch` sends every time.
TEST_CASE("custom screen sync: reset stops the watch, and watching a record twice sends one watch",
          "[ui][custom-screens][sync]") {
    screens_test::Server server;
    SyncClient client{*server.server, "ann", "lims", true};
    REQUIRE(client.probe());
    client.session.addExtraWatch(std::string{morph::ui::kCustomScreenRecord});
    REQUIRE(client.watches.size() == 1);
    client.sync.reset();
    REQUIRE(client.watches.size() == 2);
    CHECK(client.watches.back().starts_with("unwatch "));
    CHECK(client.watches.back().contains(R"("records":["morph.customScreens"])"));
}

// Mutation: the sync asks the session to watch before it knows the server offers custom screens.
TEST_CASE("custom screen sync: a server that does not offer custom screens is not watched",
          "[ui][custom-screens][sync]") {
    custom_client::BareServer server;
    SyncClient client{*server.server, "ann", "lims", true};
    CHECK_FALSE(client.probe());
    CHECK(client.watches.empty());
}

namespace {

/// Waits until the owner has a task queued, without running it.
void awaitQueued(morph::testing::StepExecutor& owner) {
    REQUIRE(morph::testing::waitUntil([&owner] { return owner.pending() > 0; },
                                      morph::testing::WaitBudget{morph::testing::kDefaultWaitBudget}));
}

}  // namespace

// Mutation: drop the scope from `.then` in `call` (the reply lands after the reset and the offer reads Offered).
TEST_CASE("custom screen sync: a reply that arrives after reset is dropped", "[ui][custom-screens][sync]") {
    screens_test::Server server;
    // The owner runs one task at a time until the probe answers, which counts the tasks the answer takes.
    std::size_t steps = 0;
    {
        SyncClient counting{*server.server, "ann", "lims"};
        std::optional<bool> answered;
        counting.sync.probe([&answered](bool offered) { answered = offered; });
        while (!answered.has_value()) {
            awaitQueued(counting.owner);
            steps += counting.owner.runOne() ? 1 : 0;
        }
    }
    REQUIRE(steps > 0);
    SyncClient client{*server.server, "ann", "lims"};
    std::optional<bool> answered;
    client.sync.probe([&answered](bool offered) { answered = offered; });
    // All but the task that delivers the reply, which is queued and not run.
    for (std::size_t step = 0; step + 1 < steps; ++step) {
        awaitQueued(client.owner);
        REQUIRE(client.owner.runOne());
    }
    awaitQueued(client.owner);
    REQUIRE_FALSE(answered.has_value());
    client.sync.reset();
    client.owner.runAll();
    CHECK_FALSE(answered.has_value());
    CHECK(client.sync.offer() == CustomScreensOffer::Unknown);
    CHECK(client.sync.listed().screens.empty());
}

namespace {

/// A sync of "ann" for "lims" that can be destroyed on its own, over a bridge that outlives it.
struct DestroyableSync {
    explicit DestroyableSync(morph::backend::RemoteServer& server)
        : bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(server), owner} {
        morph::session::Context signedIn;
        signedIn.token = "ann";
        bridge.setDefaultSession(std::move(signedIn));
        sync.emplace(dispatcher, "lims", [] { return uiservice::facts(); });
    }

    morph::testing::StepExecutor owner;
    morph::bridge::Bridge bridge;
    morph::ui::BridgeDispatcher dispatcher{bridge, &owner};
    std::optional<morph::ui::CustomScreenSync> sync;
};

}  // namespace

// Mutation: drop the scope from `.then` in `call` (the `list` reply runs on the destroyed sync; the probe's callback
// is called).
TEST_CASE("custom screen sync: a reply that arrives after the sync is destroyed is dropped",
          "[ui][custom-screens][sync]") {
    screens_test::Server server;
    // The owner runs one task at a time until the probe answers, which counts the tasks up to and including the one
    // that delivers the `list` reply (the bind's reply comes earlier).
    std::size_t steps = 0;
    {
        DestroyableSync counting{*server.server};
        std::optional<bool> answered;
        counting.sync->probe([&answered](bool offered) { answered = offered; });
        while (!answered.has_value()) {
            awaitQueued(counting.owner);
            steps += counting.owner.runOne() ? 1 : 0;
        }
    }
    REQUIRE(steps > 1);
    DestroyableSync rig{*server.server};
    std::optional<bool> answered;
    rig.sync->probe([&answered](bool offered) { answered = offered; });
    for (std::size_t step = 0; step + 1 < steps; ++step) {
        awaitQueued(rig.owner);
        REQUIRE(rig.owner.runOne());
    }
    // Only the task that delivers the `list` reply is left, queued.
    awaitQueued(rig.owner);
    REQUIRE_FALSE(answered.has_value());
    REQUIRE(rig.owner.pending() == 1);
    rig.sync.reset();
    rig.owner.runAll();
    CHECK_FALSE(answered.has_value());
}

// Mutation: the session reads no watch reply (no warning names the record).
TEST_CASE("custom screen sync: a refused watch on the record is logged at warn", "[ui][custom-screens][sync]") {
    Warnings warnings;
    screens_test::Server server;
    SyncClient client{*server.server, "ann", "lims", true};
    client.refuseWatches = true;
    REQUIRE(client.probe());
    REQUIRE(client.pump([&warnings] {
        return std::ranges::count_if(warnings.seen, [](std::string const& seen) {
                   return seen.contains("refused the watch") && seen.contains("morph.customScreens");
               }) == 1;
    }));
}

// Mutation: `put` keeps a screen whose summary is broken (`find` answers it, and open answers a document).
TEST_CASE("custom screen sync: a broken screen is refused with its reason and never cached",
          "[ui][custom-screens][sync]") {
    screens_test::Server server;
    std::string const id = custom_client::savedIdOf(server.save(screens_test::baseScreen("lims.base"), "ann"));
    server.authorizer->denied["ann"] = {{"Lims_Sample", "Lims_Verify"}};
    SyncClient client{*server.server, "ann", "lims"};
    REQUIRE(client.probe());
    REQUIRE(client.sync.listed().screens.size() == 1);
    REQUIRE(client.sync.listed().screens.front().broken.has_value());

    std::optional<std::expected<std::shared_ptr<morph::ui::Document const>, std::string>> opened;
    client.sync.open(id, [&opened](auto loaded) { opened = std::move(loaded); });
    REQUIRE(client.pump([&opened] { return opened.has_value(); }));
    REQUIRE_FALSE(opened->has_value());
    CHECK(opened->error().contains("broken"));
    CHECK(opened->error().contains(client.sync.listed().screens.front().broken->reason));
    CHECK(client.sync.documents().find(id) == nullptr);

    std::optional<std::vector<std::string>> missing;
    client.sync.prefetchRoutes({id}, [&missing](std::vector<std::string> ids) { missing = std::move(ids); });
    REQUIRE(client.pump([&missing] { return missing.has_value(); }));
    CHECK(*missing == std::vector<std::string>{id});
    CHECK(client.sync.documents().find(id) == nullptr);
}

// Mutation: a reconnect does not probe again (the second warning never comes).
TEST_CASE("custom screen sync: a reconnect asks again when the offer is off for a reason other than an absent model",
          "[ui][custom-screens][sync]") {
    Warnings warnings;
    screens_test::Server server;
    // `list` is refused for the application: a refusal, not an absence.
    SyncClient client{*server.server, "ann", "elsewhere"};
    REQUIRE_FALSE(client.probe());
    std::size_t const warned = warnings.seen.size();
    client.sync.reconnected();
    REQUIRE(client.pump([&] { return warnings.seen.size() > warned; }));
    CHECK(client.sync.offer() == CustomScreensOffer::Off);
}

// Mutation: a reconnect probes whatever the cause (the owner then has a reply queued).
TEST_CASE("custom screen sync: a reconnect does not ask a server that lacks the model again",
          "[ui][custom-screens][sync]") {
    custom_client::BareServer server;
    SyncClient client{*server.server, "ann", "lims"};
    REQUIRE_FALSE(client.probe());
    client.sync.reconnected();
    // A probe sent now would have its reply queued on the owner. Nothing runs the owner here, so a queued task is
    // a reply, and the bounded wait for none is a negative that cannot be drained.
    CHECK_FALSE(morph::testing::waitUntil([&client] { return client.owner.pending() > 0; },
                                          morph::testing::WaitBudget{std::chrono::milliseconds{100}}));
}

namespace {

morph::ui::CustomScreenFound foundNamed(std::string const& id, std::string const& hash,
                                        std::string const& documentId) {
    return morph::ui::CustomScreenFound{.summary = morph::ui::CustomScreenSummary{.id = id, .hash = hash},
                                        .document = screens_test::baseScreen(documentId)};
}

morph::ui::CustomScreensListed listShowing(std::vector<std::pair<std::string, std::string>> const& shown,
                                           bool truncated = false) {
    morph::ui::CustomScreensListed listed{.screens = {}, .truncated = truncated};
    for (auto const& [id, hash] : shown) {
        listed.screens.push_back(morph::ui::CustomScreenSummary{.id = id, .hash = hash});
    }
    return listed;
}

}  // namespace

// Mutation: `!listed.truncated` -> true (a screen missing from a truncated list is dropped).
TEST_CASE("custom documents: refresh keeps what a list shows unchanged and drops what it changed or no longer shows",
          "[ui][custom-screens][sync]") {
    morph::ui::LoadOptions const options{.catalog = uiservice::facts()};
    std::string const a = "custom:a";
    std::string const b = "custom:b";
    std::string const c = "custom:c";
    morph::ui::CustomDocuments documents;
    for (std::string const& id : {a, b, c}) {
        REQUIRE(documents.put(foundNamed(id, "sha256:" + id, id), options).has_value());
    }
    // Same hash kept, another hash dropped, absent from a full list dropped.
    CHECK(documents.refresh(listShowing({{a, "sha256:" + a}, {b, "sha256:changed"}})) ==
          std::vector<std::string>{b, c});
    CHECK(documents.find(a) != nullptr);
    CHECK(documents.find(b) == nullptr);
    CHECK(documents.find(c) == nullptr);

    // Absent from a truncated list: it may lie beyond the cut, so it is kept.
    CHECK(documents.refresh(listShowing({}, true)).empty());
    CHECK(documents.find(a) != nullptr);
    // A truncated list still drops what it shows changed.
    CHECK(documents.refresh(listShowing({{a, "sha256:changed"}}, true)) == std::vector<std::string>{a});
}

// Mutation: skip the id comparison in `put` (the document under another id is kept).
TEST_CASE("custom documents: put refuses a document that names another id and keeps nothing",
          "[ui][custom-screens][sync]") {
    morph::ui::CustomDocuments documents;
    auto const put = documents.put(foundNamed("custom:a", "sha256:a", "custom:other"),
                                   morph::ui::LoadOptions{.catalog = uiservice::facts()});
    REQUIRE_FALSE(put.has_value());
    CHECK(put.error().reason.contains("custom:other"));
    CHECK(documents.find("custom:a") == nullptr);
    CHECK(documents.find("custom:other") == nullptr);
}
