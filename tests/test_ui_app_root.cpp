// SPDX-License-Identifier: Apache-2.0
//
// The app root scope (spec 6 §5 and §12 "App root and session"): the app document is mounted once per session; its
// query runs once however many screens read it, a screen reads its names as `app.name` and writes its state with
// `set`, and destroying the root takes its names away.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/ui/interpreter.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;

namespace {

constexpr char const* kApp = R"({
  "vocab": "ui/1", "id": "app",
  "models": { "clients": { "type": "Lims_ClientModel" } },
  "state": { "tenant": { "init": "north" }, "mounted": { "init": false } },
  "queries": { "clientList": { "model": "clients", "action": "Lims_ClientList" } },
  "onMount": [ { "set": "mounted", "value": true } ] })";

/// A screen that shows the app's tenant and client count, and writes the tenant.
std::string screen(std::string const& id) {
    return R"({ "vocab": "ui/1", "id": ")" + id + R"(",
      "view": { "kind": "column", "children": [
        { "kind": "text", "testId": "tenant", "text": { "fn": "coalesce", "args": [ { "ref": "app.tenant" }, "-" ] } },
        { "kind": "text", "testId": "count",
          "text": { "fn": "format", "args": [ "{0}", { "fn": "count", "args": [ { "fn": "coalesce",
            "args": [ { "ref": "app.clientList" }, { "list": [] } ] } ] } ] } },
        { "kind": "button", "testId": "south", "label": "south", "onClick": { "set": "app.tenant", "value": "south" } },
        { "kind": "button", "testId": "bad", "label": "bad", "onClick": { "set": "app.nothing", "value": 1 } } ] } })";
}

/// The text of the widget whose test id is @p testId on @p backend.
std::string textOn(ui::testing::RecordingBackend const& backend, std::string_view testId) {
    auto const found = backend.find("Text", "testId", testId);
    REQUIRE(found.has_value());
    return backend.prop(*found, "text");
}

/// An expression read in the app document's scope, as JSON text.
std::string readApp(ui::AppRoot const& root, std::string_view json) {
    auto const parsed = ui::parseJson(json);
    REQUIRE(parsed.has_value());
    return glz::write_json(ui::valueToJson(root.read(*parsed))).value_or("");
}

/// Mounts a second screen on its own backend, so the two are told apart.
std::unique_ptr<ui::Screen> mountOn(Harness& h, ui::testing::RecordingBackend& backend, std::string const& text) {
    auto mounted = h.interpreter.mount(ui::testing::loadOrFail(text, ui::testing::corpusCatalog()), {}, backend);
    REQUIRE(mounted.has_value());
    h.settle();
    return std::move(*mounted);
}

}  // namespace

TEST_CASE("app root: an app-document query is shared by two screens and runs once", "[ui][interpreter][app]") {
    Harness h;
    auto root = h.interpreter.mountApp(ui::testing::loadOrFail(kApp, ui::testing::corpusCatalog()));
    REQUIRE(root.has_value());
    h.settle();
    CHECK(readApp(**root, R"({"ref": "mounted"})") == "true");
    ui::testing::RecordingBackend other;
    auto const first = h.mount(screen("one"));
    auto const second = mountOn(h, other, screen("two"));
    CHECK(h.dispatcher.count("Lims_ClientList") == 1);
    CHECK(h.text("tenant") == "north");
    CHECK(textOn(other, "tenant") == "north");

    h.reply(h.dispatcher.last("Lims_ClientList"), R"([{"id": 1}, {"id": 2}])");
    CHECK(h.text("count") == "2");
    CHECK(textOn(other, "count") == "2");
    CHECK(h.dispatcher.count("Lims_ClientList") == 1);
}

TEST_CASE("app root: a screen writes the app document's state, and every screen sees it", "[ui][interpreter][app]") {
    Harness h;
    auto root = h.interpreter.mountApp(ui::testing::loadOrFail(kApp, ui::testing::corpusCatalog()));
    REQUIRE(root.has_value());
    ui::testing::RecordingBackend other;
    auto const first = h.mount(screen("one"));
    auto const second = mountOn(h, other, screen("two"));
    h.backend.click(h.byTestId("Button", "south"));
    h.settle();
    CHECK(h.text("tenant") == "south");
    CHECK(textOn(other, "tenant") == "south");
    CHECK(readApp(**root, R"({"ref": "tenant"})") == R"("south")");

    // A name the app document does not declare as state is refused, and the host is not asked.
    h.backend.click(h.byTestId("Button", "bad"));
    h.settle();
    REQUIRE_FALSE(h.diagnostics.empty());
    CHECK(h.diagnostics.back().reason == "`app.nothing` is not state of the app document");
    CHECK(h.host.log.empty());
}

TEST_CASE("app root: one per session, only the app document, and its names leave with it", "[ui][interpreter][app]") {
    Harness h;
    auto const doc = ui::testing::loadOrFail(kApp, ui::testing::corpusCatalog());
    auto root = h.interpreter.mountApp(doc);
    REQUIRE(root.has_value());
    auto const again = h.interpreter.mountApp(doc);
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().reason == "an app document is already mounted on the session");
    auto const notApp = h.interpreter.mountApp(ui::testing::loadOrFail(screen("one"), ui::testing::corpusCatalog()));
    REQUIRE_FALSE(notApp.has_value());
    CHECK(notApp.error().reason == "the app document's id is `app`");

    {
        auto const shown = h.mount(screen("one"));
        CHECK(h.text("tenant") == "north");
    }
    // The screens go first, then the root, as on sign-out; a screen mounted after reads no app names.
    root->reset();
    h.settle();
    ui::testing::RecordingBackend later;
    auto const after = mountOn(h, later, screen("one"));
    CHECK(textOn(later, "tenant") == "-");
    // With no app document, `set` of `app` goes to the host, as before.
    later.click(*later.find("Button", "testId", "south"));
    h.settle();
    CHECK(h.host.log.back() == R"(app tenant "south")");
}

// Control: lower no navigation badges in mountApp (the list stays empty); or skip a part's `visible`.
TEST_CASE("app root: its navigation badges follow the app state, and only the visible ones are listed",
          "[ui][interpreter][app]") {
    Harness h;
    auto root = h.interpreter.mountApp(ui::testing::loadOrFail(R"({
      "vocab": "ui/1", "id": "app", "state": { "waiting": { "init": 4 } },
      "navigation": [
        { "screen": "lims.review", "text": { "fn": "format", "args": [ "{0}", { "ref": "waiting" } ] }, "tone": "info",
          "visible": { "fn": "ne", "args": [ { "ref": "waiting" }, 0 ] } },
        { "screen": "lims.instruments", "text": "1", "tone": "warn" } ] })",
                                                               ui::testing::corpusCatalog()));
    REQUIRE(root.has_value());
    ui::AppRoot const& app = **root;
    CHECK(app.navigationBadges().peek() ==
          std::vector<ui::NavigationBadge>{{.screen = "lims.review", .text = "4", .tone = ui::Tone::Info},
                                           {.screen = "lims.instruments", .text = "1", .tone = ui::Tone::Warn}});
    auto const screenSetsWaiting = h.mount(R"({ "vocab": "ui/1", "id": "one",
      "view": { "kind": "button", "testId": "clear", "label": "clear",
                "onClick": { "set": "app.waiting", "value": 0 } } })");
    h.backend.click(h.byTestId("Button", "clear"));
    h.settle();
    CHECK(app.navigationBadges().peek() ==
          std::vector<ui::NavigationBadge>{{.screen = "lims.instruments", .text = "1", .tone = ui::Tone::Warn}});
}

TEST_CASE("app root: `navigation` is the app document's, and each badge names its screen", "[ui][interpreter][app]") {
    auto const screen = ui::loadDocumentText(
        R"({"vocab":"ui/1","id":"one","navigation":[{"screen":"x"}],"view":{"kind":"text","text":"t"}})",
        ui::LoadOptions{.catalog = ui::testing::corpusCatalog()});
    REQUIRE_FALSE(screen.has_value());
    CHECK(screen.error().path == "/navigation");
    CHECK(screen.error().reason == "`navigation` is the app document's; a screen has none");
    auto const unnamed = ui::loadDocumentText(R"({"vocab":"ui/1","id":"app","navigation":[{"text":"1"}]})",
                                              ui::LoadOptions{.catalog = ui::testing::corpusCatalog()});
    REQUIRE_FALSE(unnamed.has_value());
    CHECK(unnamed.error().path == "/navigation/0");
    auto const empty = ui::loadDocumentText(R"({"vocab":"ui/1","id":"app","navigation":[{"screen":""}]})",
                                            ui::LoadOptions{.catalog = ui::testing::corpusCatalog()});
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().path == "/navigation/0/screen");
    CHECK(empty.error().reason == "a navigation badge names the screen of its entry");
    auto const unknown =
        ui::loadDocumentText(R"({"vocab":"ui/1","id":"app","navigation":[{"screen":"x","text":{"ref":"nothing"}}]})",
                             ui::LoadOptions{.catalog = ui::testing::corpusCatalog()});
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().path == "/navigation/0/text");
}
