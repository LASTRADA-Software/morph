// SPDX-License-Identifier: Apache-2.0
//
// The client side of preferences (spec 5 §12b, spec 6 §8 and §12 "Preferences"): PreferenceSync loads a mounted
// screen's stored choices, ignoring the ones that no longer fit, and writes the user's changes and resets, through
// BridgeDispatcher and a RemoteServer.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/core/remote.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/session/session.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/preference_sync.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_support.hpp"
#include "ui_interpreter_support.hpp"
#include "ui_preferences_support.hpp"

namespace prefs_test {

constexpr char const* kPrefs = R"({
  "vocab": "ui/1", "id": "lims.results",
  "customize": {
    "density": { "kind": "choice", "options": [ "comfortable", "compact" ], "default": "comfortable" },
    "filter": { "kind": "value", "type": "string?", "default": null },
    "limit": { "kind": "value", "type": "int", "default": 10 } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "density", "text": { "pref": "density" } },
    { "kind": "text", "testId": "filter", "text": { "fn": "coalesce", "args": [ { "pref": "filter" }, "-" ] } },
    { "kind": "customize", "testId": "edit-density", "point": "density" },
    { "kind": "button", "testId": "reset", "label": "reset", "onClick": { "resetPrefs": "all" } } ] } })";

/// The application's host: forwards the customization hooks to a sync.
struct SyncHost final : morph::ui::InterpreterHost {
    void resetPrefs(std::string const& screen, std::string const& point) override { sync->reset(screen, point); }
    void preferenceChanged(std::string const& screen, std::string const& point,
                           morph::ui::Value const& value) override {
        sync->changed(screen, point, value);
    }
    morph::ui::PreferenceSync* sync = nullptr;
};

/// One client of @p server, signed in as @p person: a bridge to the server, the interpreter over it and a sync.
struct Client {
    Client(Server& server, std::string person)
        : bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server.server), owner} {
        morph::session::Context signedIn;
        signedIn.token = std::move(person);
        bridge.setDefaultSession(std::move(signedIn));
        host.sync = &sync;
    }

    /// Runs the owner until @p done holds, for at most @p budget.
    template <typename Pred>
    bool pump(Pred done, std::chrono::milliseconds budget = morph::testing::kDefaultWaitBudget) {
        return morph::testing::waitUntil(
            [&] {
                owner.runAll();
                return done();
            },
            morph::testing::WaitBudget{budget});
    }

    /// Mounts the fixture's screen and loads its stored choices.
    std::unique_ptr<morph::ui::Screen> mount(std::expected<morph::ui::PreferencesLoaded, std::string>& loaded) {
        auto screen = interpreter.mount(morph::ui::testing::loadOrFail(kPrefs), {}, backend);
        REQUIRE(screen);
        std::optional<std::expected<morph::ui::PreferencesLoaded, std::string>> result;
        sync.load(**screen, scope, [&result](auto outcome) { result = std::move(outcome); });
        REQUIRE(pump([&] { return result.has_value(); }));
        loaded = std::move(*result);
        return std::move(*screen);
    }

    std::string text(std::string_view testId) const {
        auto const found = backend.find("Text", "testId", testId);
        REQUIRE(found);
        return backend.prop(*found, "text");
    }

    morph::testing::StepExecutor owner;
    morph::reactive::Runtime runtime{owner};
    morph::ui::Session session{runtime};
    morph::bridge::Bridge bridge;
    morph::ui::BridgeDispatcher dispatcher{bridge, &owner};
    std::vector<std::string> failures;
    morph::ui::PreferenceSync sync{dispatcher, "lims", [this](std::string const& why) { failures.push_back(why); }};
    SyncHost host;
    morph::ui::testing::RecordingBackend backend;
    morph::ui::Interpreter interpreter{session, dispatcher, morph::ui::InterpreterOptions{.host = &host}};
    morph::async::CallbackScope scope;
};

}  // namespace prefs_test

using prefs_test::kLoadDensity;
using prefs_test::Server;

TEST_CASE("preferences: a mounted screen takes its stored choices; an absent entry is the default",
          "[ui][preferences][remote]") {
    Server s;
    REQUIRE(
        s.call("set", R"({"app":"lims","scope":"lims.results","point":"filter","value":"\"lead\""})", "ann").kind ==
        "ok");
    prefs_test::Client client{s, "ann"};
    std::expected<morph::ui::PreferencesLoaded, std::string> loaded;
    auto const screen = client.mount(loaded);
    REQUIRE(loaded);
    CHECK(loaded->applied == 1);
    CHECK(loaded->ignored == 0);
    client.owner.runAll();
    CHECK(client.text("filter") == "lead");
    CHECK(client.text("density") == "comfortable");
}

TEST_CASE("preferences: an outdated entry is ignored and the point keeps its default", "[ui][preferences][remote]") {
    Server s;
    // An option the document no longer offers, a value of the wrong type, and text that is not a value at all.
    REQUIRE(
        s.call("set", R"({"app":"lims","scope":"lims.results","point":"density","value":"\"tiny\""})", "ann").kind ==
        "ok");
    REQUIRE(s.call("set", R"({"app":"lims","scope":"lims.results","point":"filter","value":"3"})", "ann").kind ==
            "ok");
    REQUIRE(s.call("set", R"({"app":"lims","scope":"lims.results","point":"limit","value":"1.5"})", "ann").kind ==
            "ok");
    prefs_test::Client client{s, "ann"};
    std::expected<morph::ui::PreferencesLoaded, std::string> loaded;
    auto const screen = client.mount(loaded);
    REQUIRE(loaded);
    CHECK(loaded->applied == 0);
    CHECK(loaded->ignored == 3);
    client.owner.runAll();
    CHECK(client.text("density") == "comfortable");
    CHECK(client.text("filter") == "-");
    CHECK(morph::ui::testing::readJson(*screen, R"({"pref": "limit"})") == "10");
}

TEST_CASE("preferences: a choice is written as the user makes it, and a reset removes it",
          "[ui][preferences][remote]") {
    Server s;
    prefs_test::Client client{s, "ann"};
    std::expected<morph::ui::PreferencesLoaded, std::string> loaded;
    auto const screen = client.mount(loaded);
    REQUIRE(loaded);

    auto const select = client.backend.find("Select", "testId", "edit-density");
    REQUIRE(select);
    client.backend.choose(*select, morph::ui::Key{std::string{"compact"}});
    auto stored = [&s] { return s.call("load", kLoadDensity, "ann").body; };
    REQUIRE(client.pump([&] { return stored() == R"({"values":{"density":"\"compact\""}})"; }));

    // A second client of the same person gets the choice on mount.
    prefs_test::Client other{s, "ann"};
    std::expected<morph::ui::PreferencesLoaded, std::string> again;
    auto const otherScreen = other.mount(again);
    other.owner.runAll();
    CHECK(other.text("density") == "compact");

    auto const reset = client.backend.find("Button", "testId", "reset");
    REQUIRE(reset);
    client.backend.click(*reset);
    REQUIRE(client.pump([&] { return stored() == R"({"values":{}})"; }));
    CHECK(client.failures.empty());
}

TEST_CASE("preferences: a load whose screen went away touches nothing", "[ui][preferences][remote]") {
    Server s;
    REQUIRE(
        s.call("set", R"({"app":"lims","scope":"lims.results","point":"filter","value":"\"lead\""})", "ann").kind ==
        "ok");
    prefs_test::Client client{s, "ann"};
    auto screen = client.interpreter.mount(morph::ui::testing::loadOrFail(prefs_test::kPrefs), {}, client.backend);
    REQUIRE(screen);
    bool called = false;
    {
        morph::async::CallbackScope const scope;
        client.sync.load(**screen, scope, [&called](auto const&) { called = true; });
        screen->reset();
    }
    // The reply arrives after the screen and the scope are gone, and is dropped.
    CHECK_FALSE(client.pump([&called] { return called; }, std::chrono::milliseconds{300}));
}

namespace {

/// A screen with a choice, a layout and a string value, for the scripted cases.
constexpr char const* kScripted = R"({
  "vocab": "ui/1", "id": "scripted",
  "customize": {
    "density": { "kind": "choice", "options": [ "comfortable", "compact" ], "default": "comfortable" },
    "panels": { "kind": "layout" },
    "filter": { "kind": "value", "type": "string?", "default": null } },
  "view": { "kind": "text", "testId": "density", "text": { "pref": "density" } } })";

using Outcome = std::expected<morph::ui::PreferencesLoaded, std::string>;

}  // namespace

TEST_CASE("preferences: a load reply is read point by point; a reply that is not one is a failure",
          "[ui][preferences]") {
    morph::ui::testing::Harness h;
    std::vector<std::string> failures;
    morph::ui::PreferenceSync sync{h.dispatcher, "lims",
                                   [&failures](std::string const& why) { failures.push_back(why); }};
    auto const screen = h.mount(kScripted);
    morph::async::CallbackScope const scope;

    std::optional<Outcome> loaded;
    sync.load(*screen, scope, [&loaded](Outcome outcome) { loaded = std::move(outcome); });
    REQUIRE(h.dispatcher.binds == std::vector<std::string>{"morph.preferences private "});
    std::size_t const call = h.dispatcher.last("load");
    CHECK(h.dispatcher.calls.at(call).body ==
          R"({"app":"lims","scope":"scripted","points":["density","filter","panels"]})");
    // A choice and a layout are taken; a point the document lacks and text that is not JSON are ignored.
    h.reply(call, R"({"values":{"density":"\"compact\"","panels":"{\"rail\":true}","gone":"1","filter":"{no"}})");
    REQUIRE(loaded);
    REQUIRE(loaded->has_value());
    CHECK((*loaded)->applied == 2);
    CHECK((*loaded)->ignored == 2);
    CHECK(h.text("density") == "compact");

    loaded.reset();
    sync.load(*screen, scope, [&loaded](Outcome outcome) { loaded = std::move(outcome); });
    h.reply(h.dispatcher.last("load"), "[1, 2]");
    REQUIRE(loaded);
    REQUIRE_FALSE(loaded->has_value());
    CHECK(loaded->error().starts_with("not a preferences reply"));

    loaded.reset();
    sync.load(*screen, scope, [&loaded](Outcome outcome) { loaded = std::move(outcome); });
    h.fail(h.dispatcher.last("load"));
    REQUIRE(loaded);
    REQUIRE_FALSE(loaded->has_value());
    CHECK(loaded->error() == "it failed");

    // Without a callback, a reply and a failure are taken quietly.
    sync.load(*screen, scope);
    h.reply(h.dispatcher.last("load"), R"({"values":{}})");
    sync.load(*screen, scope);
    h.fail(h.dispatcher.last("load"));
    CHECK(h.dispatcher.count("load") == 5);
    CHECK(failures.empty());
}

TEST_CASE("preferences: a screen with no points loads at once; writes report their failures", "[ui][preferences]") {
    morph::ui::testing::Harness h;
    std::vector<std::string> failures;
    morph::ui::PreferenceSync sync{h.dispatcher, "lims",
                                   [&failures](std::string const& why) { failures.push_back(why); }};
    auto const plain = h.mount(R"({"vocab": "ui/1", "id": "plain", "view": {"kind": "text", "text": "x"}})");
    morph::async::CallbackScope const scope;
    std::optional<Outcome> loaded;
    sync.load(*plain, scope, [&loaded](Outcome outcome) { loaded = std::move(outcome); });
    REQUIRE(loaded);
    CHECK((*loaded)->applied == 0);
    sync.load(*plain, scope);
    CHECK(h.dispatcher.calls.empty());

    sync.changed("scripted", "density", morph::ui::Value{"compact"});
    CHECK(h.dispatcher.calls.back().body ==
          R"({"app":"lims","scope":"scripted","point":"density","value":"\"compact\""})");
    h.dispatcher.fail(h.dispatcher.last("set"), "error", "refused");
    h.settle();
    CHECK(failures == std::vector<std::string>{"refused"});

    // A single point is removed; `all` of a screen the sync never loaded removes nothing.
    sync.reset("scripted", "density");
    CHECK(h.dispatcher.calls.back().body == R"({"app":"lims","scope":"scripted","point":"density"})");
    h.dispatcher.fail(h.dispatcher.last("remove"), "error", "gone");
    h.settle();
    CHECK(failures == std::vector<std::string>{"refused", "gone"});
    sync.reset("never", "all");
    CHECK(h.dispatcher.count("remove") == 1);

    // A sync with nowhere to report a failure drops it.
    morph::ui::PreferenceSync quiet{h.dispatcher, "lims"};
    quiet.changed("scripted", "density", morph::ui::Value{"compact"});
    h.fail(h.dispatcher.last("set"));
    CHECK(failures.size() == 2);
}
