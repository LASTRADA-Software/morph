// SPDX-License-Identifier: Apache-2.0
//
// The tour card (docs/spec/ui/tours.md, "The card"): a client document bound to the controller through `tour.*` host
// values and commands, its keys, and `tour.start` from any document.

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/host_vocabulary.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/tour_card.hpp>
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
    { "id": "basics", "title": "Basics",
      "steps": [ { "target": "shell_search", "title": "Search", "body": "Find anything." },
                 { "title": "Second" }, { "title": "Last" } ] },
    { "id": "getting-started", "title": "Getting started",
      "steps": [ { "target": "nav_samples", "advanceOn": { "screen": "samples" }, "title": "Open Samples" },
                 { "title": "Done" } ] } ] })";

/// A screen that starts a tour by a literal id, one by a computed id the app does not declare, and that reads what
/// the card reads and whether it can start a tour.
constexpr std::string_view kStarter = R"({
  "vocab": "ui/1", "id": "lims.home", "state": { "which": { "init": "nope" } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "take_tour", "label": "Take the tour",
      "onClick": [ { "host": "tour.start", "args": { "obj": { "id": "basics" } } } ] },
    { "kind": "button", "testId": "bad_tour", "label": "Bad",
      "onClick": [ { "host": "tour.start", "args": { "obj": { "id": { "ref": "which" } } } } ] },
    { "kind": "text", "testId": "can_tour", "text": { "fn": "format", "args": [ "{0}", { "can": "host:tour.start" } ] } },
    { "kind": "text", "testId": "can_next", "text": { "fn": "format", "args": [ "{0}", { "can": "host:tour.next" } ] } },
    { "kind": "text", "testId": "can_back", "text": { "fn": "format", "args": [ "{0}", { "can": "host:tour.back" } ] } },
    { "kind": "text", "testId": "can_end", "text": { "fn": "format", "args": [ "{0}", { "can": "host:tour.end" } ] } },
    { "kind": "text", "testId": "peek", "text": { "fn": "coalesce", "args": [ { "host": "tour.title" }, "-" ] } } ] } })";

constexpr std::string_view kOtherApp =
    R"({ "vocab": "ui/1", "id": "app", "tours": [ { "id": "elsewhere", "title": "E", "steps": [ { "title": "x" } ] } ] })";

struct Card {
    Card() {
        auto mounted = h.interpreter.mountApp(ui::testing::loadOrFail(kApp));
        REQUIRE(mounted.has_value());
        root = std::move(*mounted);
        host.connect(h.session);
        binding = std::make_unique<ui::TourCardBinding>(
            host, [this] { return controller.get(); },
            [this](std::function<void()> work) {
                if (deferred) {
                    posted.push_back(std::move(work));
                } else {
                    work();
                }
            },
            [this](std::string const& text) { reports.push_back(text); });
        controller = std::make_unique<ui::TourController>(
            h.runtime, *root, shell,
            ui::TourControllerOptions{.warn = {}, .onChanged = [this] { binding->refresh(); }});
        shell.onActive = [this] { controller->activeChanged(); };
        shell.onSpotlight = [this](ui::SpotlightWidget& spotlight) {
            auto const document = ui::tourCardDocument();
            REQUIRE(document.has_value());
            auto made = h.interpreter.mount(*document, {}, h.backend, ui::MountOptions{.parent = &spotlight});
            REQUIRE(made.has_value());
            card = std::move(*made);
        };
        page = h.backend.createStack(nullptr, ui::Axis::Vertical);
    }

    void carry(std::string const& testId) {
        parts.push_back(h.backend.createButton(page.get()));
        parts.back()->setTestId(testId);
    }
    [[nodiscard]] int button(std::string_view testId) const { return h.byTestId("Button", testId); }
    [[nodiscard]] bool shown(std::string_view kind, std::string_view testId) const {
        return h.backend.prop(h.byTestId(kind, testId), "visible") != "false";
    }

    ui::HostVocabulary host;
    // Holds the commands' work until a test runs it, as a shell that posts it does; and what the binding reported.
    bool deferred = false;
    std::vector<std::function<void()>> posted;
    std::vector<std::string> reports;
    ui::testing::Harness h;
    ui::testing::FakeTourShell shell{h.backend};
    std::unique_ptr<ui::AppRoot> root;
    std::unique_ptr<ui::StackWidget> page;
    std::vector<std::unique_ptr<ui::ButtonWidget>> parts;
    std::unique_ptr<ui::TourCardBinding> binding;
    std::unique_ptr<ui::TourController> controller;
    std::unique_ptr<ui::Screen> card;
};

}  // namespace

// Mutation: set `tour.stepTitle` from the tour's title in `refresh` (the card's heading reads "Basics").
TEST_CASE("tour card: the card shows the running step's texts, its progress, Back, and Done on the last step",
          "[ui][tours]") {
    Card c;
    c.carry("shell_search");
    REQUIRE(c.controller->start("basics").has_value());
    c.h.settle();
    CHECK(c.h.text("tour_eyebrow") == "Basics");
    CHECK(c.h.text("tour_title") == "Search");
    CHECK(c.h.text("tour_body") == "Find anything.");
    CHECK(c.h.text("tour_progress") == "1 of 3");
    CHECK_FALSE(c.shown("Button", "tour_back"));
    CHECK(c.h.backend.prop(c.button("tour_next"), "label") == "Next");
    c.controller->next();
    c.h.settle();
    CHECK(c.h.text("tour_progress") == "2 of 3");
    CHECK(c.shown("Button", "tour_back"));
    CHECK_FALSE(c.shown("Text", "tour_body"));
    c.controller->next();
    c.h.settle();
    CHECK(c.h.backend.prop(c.button("tour_next"), "label") == "Done");
}

// Mutation: register `tour.back` to run `next` (Back moves on).
TEST_CASE("tour card: Next, Back and End tour run through the card's buttons", "[ui][tours]") {
    Card c;
    REQUIRE(c.controller->start("basics").has_value());
    c.h.settle();
    c.h.backend.click(c.button("tour_next"));
    c.h.settle();
    CHECK(c.controller->view()->position == 2);
    c.h.backend.click(c.button("tour_back"));
    c.h.settle();
    CHECK(c.controller->view()->position == 1);
    c.h.backend.click(c.button("tour_end"));
    c.h.settle();
    CHECK_FALSE(c.controller->running());
    CHECK(c.h.backend.prop(c.shell.spotlightId(), "open") == "false");
    CHECK(c.h.text("tour_title").empty());
}

// Mutation: drop `keys` from `kTourCardDocument` (no chord reaches the controller).
TEST_CASE("tour card: Right moves on, Left moves back and Esc ends the tour, from a focused button", "[ui][tours]") {
    Card c;
    REQUIRE(c.controller->start("basics").has_value());
    c.h.settle();
    int const next = c.button("tour_next");
    CHECK(c.h.backend.press(next, "Right"));
    c.h.settle();
    CHECK(c.controller->view()->position == 2);
    CHECK(c.h.backend.press(next, "Left"));
    c.h.settle();
    CHECK(c.controller->view()->position == 1);
    CHECK(c.h.backend.press(next, "Esc"));
    c.h.settle();
    CHECK_FALSE(c.controller->running());
}

// Mutation: show `tour_next` on a waiting step (both it and the hint show).
TEST_CASE("tour card: on a waiting step the hint replaces Next, and Right does not move on", "[ui][tours]") {
    Card c;
    c.carry("nav_samples");
    REQUIRE(c.controller->start("getting-started").has_value());
    c.h.settle();
    CHECK_FALSE(c.shown("Button", "tour_next"));
    CHECK(c.shown("Text", "tour_waiting"));
    CHECK(c.h.text("tour_waiting") == "Open Title of samples to continue");
    CHECK(c.h.backend.press(c.button("tour_end"), "Right"));
    c.h.settle();
    CHECK(c.controller->view()->position == 1);
    c.shell.active = "samples";
    c.controller->activeChanged();
    c.h.settle();
    CHECK(c.controller->view()->position == 2);
    CHECK(c.shown("Button", "tour_next"));
}

// Mutation: limit `tour.start` to the card's document (the screen's button reports it missing).
TEST_CASE("tour card: tour.start from a document starts the tour, an undeclared id is reported, and can is true",
          "[ui][tours]") {
    Card c;
    auto const starter = c.h.mount(kStarter);
    CHECK(c.h.text("can_tour") == "true");
    c.h.backend.click(c.button("bad_tour"));
    c.h.settle();
    CHECK_FALSE(c.controller->running());
    bool reported = false;
    for (ui::Refusal const& refusal : c.h.diagnostics) {
        reported = reported || refusal.reason.contains("declares no tour `nope`");
    }
    CHECK(reported);
    c.h.backend.click(c.button("take_tour"));
    c.h.settle();
    REQUIRE(c.controller->running());
    CHECK(c.controller->view()->tourId == "basics");
    // The card's values are the card's: another document reads none of them.
    CHECK(c.h.text("peek") == "-");
}

// Mutation: drop the report of a refused or unrun posted `tour.start` (the tour silently does not start).
TEST_CASE("tour card: a posted tour.start that finds no controller or a refusing one is reported", "[ui][tours]") {
    Card c;
    auto const starter = c.h.mount(kStarter);
    c.deferred = true;
    // The frame goes away between the click and the posted run.
    c.h.backend.click(c.button("take_tour"));
    REQUIRE(c.posted.size() == 1);
    auto gone = std::move(c.controller);
    c.posted.front()();
    CHECK_FALSE(gone->running());
    REQUIRE(c.reports.size() == 1);
    CHECK(c.reports.front().contains("`basics`"));
    // A controller that no longer declares the tour: the command is refused when it runs.
    c.posted.clear();
    c.reports.clear();
    c.controller = std::move(gone);
    c.h.backend.click(c.button("take_tour"));
    REQUIRE(c.posted.size() == 1);
    ui::testing::Harness elsewhere;
    auto other = elsewhere.interpreter.mountApp(ui::testing::loadOrFail(kOtherApp));
    REQUIRE(other.has_value());
    c.controller.reset();
    c.controller =
        std::make_unique<ui::TourController>(elsewhere.runtime, **other, c.shell, ui::TourControllerOptions{});
    c.posted.front()();
    CHECK_FALSE(c.controller->running());
    REQUIRE(c.reports.size() == 1);
    CHECK(c.reports.front().contains("declares no tour `basics`"));
    // Before the harness and the root the controller reads go.
    c.controller.reset();
}

// Mutation: drop `documents` from the `tour.next`, `tour.back` and `tour.end` commands (every document can run them).
TEST_CASE("tour card: tour.next, tour.back and tour.end are not available to a document other than the card",
          "[ui][tours]") {
    Card c;
    auto const starter = c.h.mount(kStarter);
    CHECK(c.h.text("can_tour") == "true");
    CHECK(c.h.text("can_next") == "false");
    CHECK(c.h.text("can_back") == "false");
    CHECK(c.h.text("can_end") == "false");
}
