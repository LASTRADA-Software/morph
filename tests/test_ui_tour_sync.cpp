// SPDX-License-Identifier: Apache-2.0
//
// The client of the runtime tours (docs/spec/ui/tours.md, "Runtime tours on the client"): it lists over a simulated
// connection, watches `morph.tours` once offered, lists again on a `changes` event naming it, after a reconnect and
// after a `resync`, and reads a server without the model as "the app document's tours alone", logged once at debug.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/tour_registry.hpp>
#include <morph/ui/tour_sync.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "test_support.hpp"
#include "ui_custom_client_support.hpp"
#include "ui_service_support.hpp"

namespace ui = morph::ui;

namespace {

std::shared_ptr<ui::ServedBundle const> served() {
    auto built = uiservice::registry().build();
    REQUIRE(built.has_value());
    return *built;
}

std::string resultsTour(std::string const& id) {
    return R"({"id":")" + id + R"(","screen":"lims.results","title":"Results","steps":[{"title":"Here"}]})";
}

/// A server whose registry offers runtime tours, or not.
struct ToursServer {
    explicit ToursServer(bool offer = true) {
        if (offer) {
            ui::registerTours(tours, authorizer, dispatcher, registry);
        }
        server = std::make_shared<morph::backend::RemoteServer>(pool, authorizer, dispatcher, registry);
    }
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    morph::exec::ThreadPoolExecutor pool{2};
    std::shared_ptr<uiservice::RuleAuthorizer> authorizer = std::make_shared<uiservice::RuleAuthorizer>();
    std::shared_ptr<ui::ServedBundle const> bundle = served();
    std::shared_ptr<ui::TourRegistry> tours = std::make_shared<ui::TourRegistry>([this] { return bundle; });
    std::shared_ptr<morph::backend::RemoteServer> server;
};

/// One client of @p server signed in as @p person, with a recording watcher, and a sync.
struct ToursClient {
    ToursClient(morph::backend::RemoteServer& server, std::string person)
        : session{runtime, ui::SessionOptions{.watcher = watcher()}},
          bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(server), owner},
          sync{dispatcher, &session} {
        morph::session::Context signedIn;
        signedIn.token = std::move(person);
        bridge.setDefaultSession(std::move(signedIn));
        sync.setOnListed([this](std::vector<ui::TourDecl> const&) { ++listings; });
    }
    template <typename Pred>
    bool pump(Pred done) {
        return morph::testing::waitUntil([&] {
            owner.runAll();
            return done();
        });
    }
    bool list() {
        std::optional<bool> offered;
        sync.list([&offered](bool answer) { offered = answer; });
        REQUIRE(pump([&offered] { return offered.has_value(); }));
        return *offered;
    }
    /// Lists again and waits for that list: every list asked before it has then arrived, since one channel carries
    /// all.
    void barrier() {
        bool done = false;
        sync.relist([&done] { done = true; });
        REQUIRE(pump([&done] { return done; }));
    }
    ui::Watcher watcher() {
        return [this](std::string const& kind, std::string const& body) {
            watches.push_back(kind + " " + body);
            auto [completion, promise] = morph::async::Completion<std::string>::makeSettleable(&owner);
            promise.resolve(std::string{R"({"refusedRecords":[],"refusedInstances":[]})"});
            return std::move(completion);
        };
    }
    std::vector<std::string> watches;
    int listings = 0;
    morph::testing::StepExecutor owner;
    morph::reactive::Runtime runtime{owner};
    ui::Session session;
    morph::bridge::Bridge bridge;
    ui::BridgeDispatcher dispatcher{bridge, &owner};
    ui::TourSync sync;
};

morph::wire::EventBody changesOf(std::string_view record) {
    return morph::wire::EventBody{
        .topic = std::string{morph::wire::kChangesTopic},
        .payload = glz::raw_json{R"({"records":[{"record":")" + std::string{record} + R"("}],"instances":[]})"}};
}

}  // namespace

// Mutation: skip `loadTourText` and keep every listed tour unparsed (the sync's tours are empty).
TEST_CASE("tour sync: list answers the runtime tours in order, loaded as tours, and watches morph.tours",
          "[ui][tours][runtime][sync]") {
    ToursServer const server;
    REQUIRE(server.tours->add(resultsTour("first")).has_value());
    REQUIRE(server.tours->add(resultsTour("second")).has_value());
    ToursClient client{*server.server, "ann"};
    REQUIRE(client.list());
    CHECK(client.sync.offered());
    REQUIRE(client.sync.tours().size() == 2);
    CHECK(client.sync.tours().at(0).id == "first");
    CHECK(client.sync.tours().at(1).id == "second");
    CHECK(client.sync.tours().at(0).revision > 0);
    REQUIRE(client.watches.size() == 1);
    CHECK(client.watches.front().contains(R"("records":["morph.tours"])"));
}

// Mutation: log the absent model at warn in `TourSync` (a warning is captured).
TEST_CASE("tour sync: a server without the model offers none, logged once at debug and never at warn",
          "[ui][tours][runtime][sync]") {
    // Debug lines are dropped below the default level, warn: lower it for this case and restore it after.
    struct LevelGuard {
        morph::log::LogLevel previous = morph::log::getLogLevel();
        LevelGuard() { morph::log::setLogLevel(morph::log::LogLevel::debug); }
        ~LevelGuard() { morph::log::setLogLevel(previous); }
        LevelGuard(LevelGuard const&) = delete;
        LevelGuard& operator=(LevelGuard const&) = delete;
        LevelGuard(LevelGuard&&) = delete;
        LevelGuard& operator=(LevelGuard&&) = delete;
    } const levelGuard;
    std::vector<std::string> warnings;
    std::vector<std::string> debug;
    morph::log::ScopedLoggerOverride const guard{[&](morph::log::LogLevel level, std::string_view message) {
        if (level == morph::log::LogLevel::warn) {
            warnings.emplace_back(message);
        } else if (level == morph::log::LogLevel::debug && message.contains("runtime tours")) {
            debug.emplace_back(message);
        }
    }};
    ToursServer const server{false};
    ToursClient client{*server.server, "ann"};
    CHECK_FALSE(client.list());
    CHECK_FALSE(client.list());
    CHECK(client.sync.tours().empty());
    CHECK(client.watches.empty());
    CHECK(std::ranges::none_of(warnings, [](std::string const& line) { return line.contains("runtime tours"); }));
    CHECK(debug.size() == 1);
}

// Mutation: relist on every `changes` event in `deliverEvent` (the event naming another record relists too).
TEST_CASE("tour sync: a changes event naming morph.tours lists again, one naming another record does not",
          "[ui][tours][runtime][sync]") {
    ToursServer const server;
    ToursClient client{*server.server, "ann"};
    REQUIRE(client.list());
    REQUIRE(client.sync.tours().empty());
    REQUIRE(server.tours->add(resultsTour("added")).has_value());
    int const before = client.listings;
    client.sync.deliverEvent(changesOf("SomethingElse"));
    client.sync.deliverEvent(changesOf(ui::kTourRecord));
    client.barrier();
    CHECK(client.listings == before + 2);
    REQUIRE(client.sync.tours().size() == 1);
    CHECK(client.sync.tours().front().id == "added");
    REQUIRE(server.tours->remove("added").has_value());
    client.sync.deliverEvent(changesOf(ui::kTourRecord));
    client.barrier();
    CHECK(client.sync.tours().empty());
}

// Mutation: do nothing in `reconnected` (the tour added while away is not listed).
TEST_CASE("tour sync: a reconnect and a resync list again", "[ui][tours][runtime][sync]") {
    ToursServer const server;
    ToursClient client{*server.server, "ann"};
    REQUIRE(client.list());
    REQUIRE(server.tours->add(resultsTour("away")).has_value());
    int const before = client.listings;
    client.sync.reconnected();
    REQUIRE(client.pump([&] { return client.listings > before; }));
    CHECK(client.sync.tours().size() == 1);
    morph::wire::EventBody const resync{.topic = std::string{morph::wire::kResyncTopic},
                                        .payload = glz::raw_json{"null"}};
    int const again = client.listings;
    client.sync.deliverEvent(resync);
    REQUIRE(client.pump([&] { return client.listings > again; }));
}

// Mutation: keep the list in `reset` (the tours survive it).
TEST_CASE("tour sync: reset forgets the list and stops the watch", "[ui][tours][runtime][sync]") {
    ToursServer const server;
    REQUIRE(server.tours->add(resultsTour("kept")).has_value());
    ToursClient client{*server.server, "ann"};
    REQUIRE(client.list());
    REQUIRE(client.sync.tours().size() == 1);
    client.sync.reset();
    CHECK(client.sync.tours().empty());
    CHECK_FALSE(client.sync.offered());
    REQUIRE(client.watches.size() == 2);
    CHECK(client.watches.back().starts_with("unwatch "));
}
