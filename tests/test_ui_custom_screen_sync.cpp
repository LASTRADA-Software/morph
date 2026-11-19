// SPDX-License-Identifier: Apache-2.0
//
// The client of the custom screens model (docs/spec/ui/custom_screens.md, "The client"): discovery by `list`, the
// cache `DocumentScreenHostOptions::documents` answers custom ids from, its refresh by hash, the server's check and
// the store's refusal passed through, routes fetched before a restore, and the list refreshed by events.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
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
    CHECK(
        std::ranges::none_of(warnings.seen, [](std::string const& seen) { return seen.contains("custom screens"); }));
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

// Mutations: ignore a `changes` event (the list stays empty); relist on any record's change (the event naming another
// record already shows the screen).
TEST_CASE("custom screen sync: a changes event naming the record relists, and one naming another does not",
          "[ui][custom-screens][sync]") {
    screens_test::Server server;
    SyncClient client{*server.server, "ann", "lims"};
    REQUIRE(client.probe());
    REQUIRE(client.sync.listed().screens.empty());
    static_cast<void>(custom_client::savedIdOf(server.save(screens_test::baseScreen("lims.base"), "ann")));

    client.sync.deliverEvent(changesOf("SomethingElse"));
    static_cast<void>(client.pump([] { return false; }, std::chrono::milliseconds{200}));
    CHECK(client.sync.listed().screens.empty());

    client.sync.deliverEvent(changesOf(morph::ui::kCustomScreenRecord));
    REQUIRE(client.pump([&client] { return client.sync.listed().screens.size() == 1; }));
}

// Mutation: skip the watch in reconnected (two watches are sent, not three).
TEST_CASE("custom screen sync: the record is watched once offered, again after a reconnect and after a resync",
          "[ui][custom-screens][sync]") {
    screens_test::Server server;
    SyncClient client{*server.server, "ann", "lims", true};
    REQUIRE(client.probe());
    REQUIRE(client.watches.size() == 1);
    CHECK(client.watches.front().starts_with("watch "));
    CHECK(client.watches.front().contains(R"("records":["morph.customScreens"])"));
    CHECK(client.watches.front().contains(R"("replace":false)"));
    // Each of the two relists is awaited as well: the server's pool must be idle before the fixture is destroyed.
    std::size_t relisted = 0;
    client.sync.setOnListed([&relisted](morph::ui::CustomScreensListed const&) { ++relisted; });
    client.sync.reconnected();
    client.sync.deliverEvent(
        morph::wire::EventBody{.topic = std::string{morph::wire::kResyncTopic}, .payload = glz::raw_json{"null"}});
    REQUIRE(client.pump([&client, &relisted] { return client.watches.size() == 3 && relisted == 2; }));
}
