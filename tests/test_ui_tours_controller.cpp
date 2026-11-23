// SPDX-License-Identifier: Apache-2.0
//
// The tour controller (docs/spec/ui/tours.md, "The controller"), with no shell: a fake `TourShell`, an `AppRoot` over
// a test app document, and the recording backend's spotlight, whose target the backend resolves.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/navigator.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/tours.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_interpreter_support.hpp"
#include "ui_tours_support.hpp"

namespace ui = morph::ui;

namespace {

constexpr std::string_view kApp = R"({
  "vocab": "ui/1", "id": "app",
  "tours": [
    { "id": "basics", "title": { "t": "tours.basics", "fallback": "Basics" },
      "steps": [
        { "target": "shell_search", "title": { "t": "tours.search", "fallback": "Search" }, "body": "Find anything." },
        { "title": "Second" },
        { "title": "Last" } ] },
    { "id": "getting-started", "title": "Getting started",
      "steps": [
        { "target": "nav_samples", "advanceOn": { "screen": "samples" }, "title": "Open Samples" },
        { "screen": "samples", "target": "samples_table", "title": "The table" },
        { "title": "Done" } ] },
    { "id": "register", "screen": "samples", "title": "Register",
      "steps": [ { "target": "action_new", "title": "New" },
                 { "screen": "home", "params": { "page": 2 }, "title": "Home" } ] },
    { "id": "find", "screen": "samples", "title": "Find", "steps": [ { "title": "Filter" } ] },
    { "id": "lost", "title": "Lost", "steps": [ { "screen": "locked", "title": "Locked" } ] } ] })";

struct Rig {
    Rig() {
        h.session.setTranslations([](std::string_view key, std::string_view locale) -> std::optional<std::string> {
            if (!locale.starts_with("de")) {
                return std::nullopt;
            }
            if (key == "tours.basics") {
                return std::string{"Grundlagen"};
            }
            if (key == "tours.search") {
                return std::string{"Suche"};
            }
            return std::nullopt;
        });
        auto mounted = h.interpreter.mountApp(ui::testing::loadOrFail(kApp));
        REQUIRE(mounted.has_value());
        root = std::move(*mounted);
        page = h.backend.createStack(nullptr, ui::Axis::Vertical);
        controller = std::make_unique<ui::TourController>(
            h.runtime, *root, shell,
            ui::TourControllerOptions{.warn = [this](std::string const& text) { warnings.push_back(text); },
                                      .onChanged = [this] { ++changes; }});
        shell.onActive = [this] { controller->activeChanged(); };
    }

    /// Puts a widget carrying @p testId on the page.
    void carry(std::string const& testId) {
        parts.push_back(h.backend.createButton(page.get()));
        parts.back()->setTestId(testId);
    }

    /// A property of the spotlight.
    [[nodiscard]] std::string spot(std::string_view name) const { return h.backend.prop(shell.spotlightId(), name); }

    ui::testing::Harness h;
    ui::testing::FakeTourShell shell{h.backend};
    std::unique_ptr<ui::AppRoot> root;
    std::unique_ptr<ui::StackWidget> page;
    std::vector<std::unique_ptr<ui::ButtonWidget>> parts;
    std::unique_ptr<ui::TourController> controller;
    std::vector<std::string> warnings;
    int changes = 0;
};

}  // namespace

// Mutation: make `next` show the step it is on (the position stays 1).
TEST_CASE("tours controller: start shows the first step, next and back move, and Done on the last step ends it",
          "[ui][tours]") {
    Rig r;
    r.carry("shell_search");
    REQUIRE(r.controller->start("basics").has_value());
    REQUIRE(r.controller->view().has_value());
    CHECK(r.controller->view()->title == "Basics");
    CHECK(r.controller->view()->stepTitle == "Search");
    CHECK(r.controller->view()->body == "Find anything.");
    CHECK(r.controller->view()->position == 1);
    CHECK(r.controller->view()->count == 3);
    CHECK_FALSE(r.controller->view()->canBack);
    CHECK(r.spot("open") == "true");
    CHECK(r.spot("target") == "shell_search");
    CHECK(r.spot("targetShown") == "true");
    r.controller->next();
    CHECK(r.controller->view()->position == 2);
    CHECK(r.controller->view()->canBack);
    CHECK(r.spot("target") == "none");
    r.controller->back();
    CHECK(r.controller->view()->position == 1);
    r.controller->next();
    r.controller->next();
    CHECK(r.controller->view()->last);
    r.controller->next();
    CHECK_FALSE(r.controller->running());
    CHECK_FALSE(r.controller->view().has_value());
    CHECK(r.spot("open") == "false");
    CHECK(r.warnings.empty());
}

// Mutation: open a step's screen without asking `shows` (home is opened twice).
TEST_CASE("tours controller: a tour that belongs to a screen opens it first, and a shown screen is not reopened",
          "[ui][tours]") {
    Rig r;
    r.shell.active = "home";
    REQUIRE(r.controller->start("register").has_value());
    CHECK(r.shell.opened == std::vector<std::string>{"samples"});
    r.controller->next();
    CHECK(r.shell.opened == std::vector<std::string>{"samples", "home"});
    CHECK(r.shell.activeParams.at("page") == ui::Value{2});
    // Back does not undo the screen; the earlier step names none, so nothing opens.
    r.controller->back();
    CHECK(r.shell.opened.size() == 2);
    r.controller->next();
    CHECK(r.shell.opened.size() == 2);
    Rig shown;
    shown.shell.active = "samples";
    REQUIRE(shown.controller->start("find").has_value());
    CHECK(shown.shell.opened.empty());
}

// Mutation: advance on any change of the active tab in `activeChanged` (home moves the tour on).
TEST_CASE("tours controller: advanceOn moves on when its screen becomes active and ignores another; Next waits",
          "[ui][tours]") {
    Rig r;
    r.carry("nav_samples");
    REQUIRE(r.controller->start("getting-started").has_value());
    CHECK(r.controller->view()->waiting);
    CHECK(r.controller->view()->waitingFor == "Title of samples");
    CHECK(r.spot("targetFocusable") == "true");
    r.controller->next();
    CHECK(r.controller->view()->position == 1);
    r.shell.active = "home";
    r.controller->activeChanged();
    CHECK(r.controller->view()->position == 1);
    r.shell.active = "samples";
    r.controller->activeChanged();
    CHECK(r.controller->view()->position == 2);
    CHECK_FALSE(r.controller->view()->waiting);
    CHECK(r.spot("targetFocusable") == "false");
    // The step's screen is the one shown already.
    CHECK(r.shell.opened.empty());
}

// Mutation: warn at every showing of a step (Next then Back warns twice).
TEST_CASE("tours controller: a target that is not shown leaves the card centred and warns once per step",
          "[ui][tours]") {
    Rig r;
    REQUIRE(r.controller->start("basics").has_value());
    CHECK(r.spot("targetShown") == "false");
    REQUIRE(r.warnings.size() == 1);
    CHECK(r.warnings.front().contains("`basics`, step 1"));
    CHECK(r.warnings.front().contains("shell_search"));
    r.controller->next();
    r.controller->back();
    CHECK(r.warnings.size() == 1);
    // A target shown later is found by the spotlight itself.
    r.carry("shell_search");
    CHECK(r.spot("targetShown") == "true");
}

// Mutation: skip `revealNavEntry` in `show` (the navigator is never asked).
TEST_CASE("tours controller: a navigator target is revealed in the wide form and substituted when compact",
          "[ui][tours]") {
    Rig r;
    REQUIRE(r.controller->start("getting-started").has_value());
    CHECK(r.shell.revealed == std::vector<std::string>{"samples"});
    CHECK(r.spot("target") == "nav_samples");
    r.controller->end();
    r.shell.isCompact = true;
    r.shell.revealed.clear();
    REQUIRE(r.controller->start("getting-started").has_value());
    CHECK(r.shell.revealed.empty());
    CHECK(r.spot("target") == "shell_nav_menu");
    r.controller->end();
    REQUIRE(r.controller->start("basics").has_value());
    CHECK(r.spot("target") == "shell_search_open");
}

// Mutation: return the target unchanged from `tourTargetFor` when compact (the first compact row fails).
TEST_CASE("tours controller: tourTargetFor keeps every target wide and substitutes what a compact frame moves",
          "[ui][tours]") {
    CHECK(ui::tourTargetFor("nav_samples", false) == "nav_samples");
    CHECK(ui::tourTargetFor("shell_search", false) == "shell_search");
    CHECK(ui::tourTargetFor("nav_samples", true) == "shell_nav_menu");
    CHECK(ui::tourTargetFor("shell_search", true) == "shell_search_open");
    for (std::string_view const moved : {"shell_context", "shell_help", "shell_principal_menu", "shell_tours"}) {
        CHECK(ui::tourTargetFor(moved, true) == "shell_nav_menu");
    }
    CHECK(ui::tourTargetFor("shell_inbox", true) == "shell_inbox");
    CHECK(ui::tourTargetFor("samples_table", true) == "samples_table");
}

// Mutation: show the step after a refused open (the tour keeps running and nothing is told).
TEST_CASE("tours controller: a screen that fails to open ends the tour with a notice naming it", "[ui][tours]") {
    Rig r;
    r.shell.refused.insert("locked");
    REQUIRE(r.controller->start("lost").has_value());
    CHECK_FALSE(r.controller->running());
    REQUIRE(r.shell.notices.size() == 1);
    CHECK(r.shell.notices.front().first.contains("Title of locked"));
    CHECK(r.shell.notices.front().second == ui::Tone::Warn);
    CHECK(r.spot("open") == "false");
}

// Mutation: keep the running tour when another starts (the second's title does not show).
TEST_CASE("tours controller: a new tour replaces a running one, and end closes the spotlight and hands focus back",
          "[ui][tours]") {
    Rig r;
    auto const field = r.h.backend.createTextInput(r.page.get(), ui::TextInputMode::SingleLine);
    field->focus();
    REQUIRE(r.controller->start("basics").has_value());
    REQUIRE(r.controller->start("find").has_value());
    CHECK(r.controller->view()->title == "Find");
    CHECK(r.controller->view()->position == 1);
    r.controller->end();
    CHECK(r.spot("open") == "false");
    CHECK(r.h.backend.focused() == r.h.backend.idOf(*field));
    CHECK_FALSE(r.controller->running());
}

// Mutation: accept any id in `start` (it runs the first tour).
TEST_CASE("tours controller: an unknown tour id is refused and starts nothing", "[ui][tours]") {
    Rig r;
    auto const started = r.controller->start("nope");
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().reason.contains("`nope`"));
    CHECK_FALSE(r.controller->running());
    CHECK_FALSE(r.controller->declares("nope"));
    CHECK(r.controller->declares("basics"));
}

// Mutation: read the tours once in `start` and never follow them (the German title never shows).
TEST_CASE("tours controller: a change of locale re-evaluates the running step's texts", "[ui][tours]") {
    Rig r;
    REQUIRE(r.controller->start("basics").has_value());
    int const before = r.changes;
    r.h.session.setLocale("de");
    r.h.settle();
    CHECK(r.controller->view()->title == "Grundlagen");
    CHECK(r.controller->view()->stepTitle == "Suche");
    CHECK(r.changes > before);
}

// Mutation: list general tours before the screen's in `offeredTours` (the screen's come second).
TEST_CASE("tours controller: offeredTours lists the screen's tours in order, then the general ones", "[ui][tours]") {
    Rig r;
    ui::OfferedTours const offered = ui::offeredTours(r.root->tours().get(), "samples");
    REQUIRE(offered.screen.size() == 2);
    CHECK(offered.screen.at(0).id == "register");
    CHECK(offered.screen.at(1).id == "find");
    REQUIRE(offered.general.size() == 3);
    CHECK(offered.general.at(0).id == "basics");
    CHECK(ui::offeredTours(r.root->tours().get(), "home").screen.empty());
    CHECK(ui::offeredTours({}, "home").general.empty());
}

// Mutation: leave the search text in `Navigator::reveal` (the filtered entry stays hidden).
TEST_CASE("tours controller: Navigator::reveal clears the search and opens the entry's module", "[ui][tours]") {
    ui::testing::RecordingBackend backend;
    ui::NavigatorOptions options;
    ui::CatalogEntry samples;
    samples.id = "lims.samples";
    samples.title = "Samples";
    samples.module = "Lab";
    ui::CatalogEntry invoices;
    invoices.id = "billing.invoices";
    invoices.title = "Invoices";
    invoices.module = "Billing";
    options.entries = {samples, invoices};
    ui::Navigator navigator{backend, nullptr, std::move(options)};
    navigator.setModuleGroups(true);
    REQUIRE(navigator.setOpenModule("Lab"));
    std::optional<int> const search = backend.find("TextInput", "testId", "navigator_search");
    REQUIRE(search.has_value());
    backend.edit(*search, "sam");
    CHECK(navigator.reveal("billing.invoices"));
    CHECK(navigator.openModule() == "Billing");
    CHECK(backend.prop(*search, "text").empty());
    CHECK_FALSE(navigator.reveal("lims.nope"));
}
