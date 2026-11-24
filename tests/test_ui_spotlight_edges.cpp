// SPDX-License-Identifier: Apache-2.0
//
// The spotlight of the recording backend (docs/spec/ui/tours.md, "The spotlight"): which widget it rings, what it
// restores when it closes, and what it dims and passes to its card's chords; and the shell frame's Tours entries and
// top-bar parts.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/shell_frame.hpp>
#include <morph/ui/shell_parts_view.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <vector>

namespace ui = morph::ui;
using ui::testing::RecordingBackend;

namespace {

/// A spotlight, open, ringing the widget whose test id is `t`.
std::unique_ptr<ui::SpotlightWidget> ringing(RecordingBackend& backend) {
    std::unique_ptr<ui::SpotlightWidget> spotlight = backend.createSpotlight(nullptr);
    spotlight->setOpen(true);
    spotlight->setTarget(std::string{"t"});
    return spotlight;
}

ui::ShellFrameOptions frameOptions() {
    ui::ShellFrameOptions made;
    made.applicationName = "Lab";
    made.navigator.entries = {ui::CatalogEntry{.id = "lims.samples", .title = "Samples", .group = "Lab"}};
    made.onSignOut = [] {};
    return made;
}

}  // namespace

TEST_CASE("spotlight edges: a target inside a closed dialog is not ringed until the dialog opens",
          "[ui][tours][spotlight]") {
    RecordingBackend backend;
    std::unique_ptr<ui::DialogWidget> const dialog = backend.createDialog(nullptr);
    std::unique_ptr<ui::ButtonWidget> const inside = backend.createButton(dialog.get());
    inside->setTestId("t");
    auto const spotlight = ringing(backend);
    CHECK_FALSE(spotlight->targetShown());
    dialog->setOpen(true);
    CHECK(spotlight->targetShown());
    dialog->setOpen(false);
    CHECK_FALSE(spotlight->targetShown());
}

TEST_CASE("spotlight edges: a target that is itself a closed overlay is not ringed until it opens",
          "[ui][tours][spotlight]") {
    RecordingBackend backend;
    std::unique_ptr<ui::DialogWidget> const dialog = backend.createDialog(nullptr);
    dialog->setTestId("t");
    auto const spotlight = ringing(backend);
    CHECK_FALSE(spotlight->targetShown());
    dialog->setOpen(true);
    CHECK(spotlight->targetShown());
}

TEST_CASE("spotlight edges: a closed collapsible hides its content from the spotlight but not its header",
          "[ui][tours][spotlight]") {
    RecordingBackend backend;
    std::unique_ptr<ui::CollapsibleWidget> const section = backend.createCollapsible(nullptr);
    std::unique_ptr<ui::ButtonWidget> const header = backend.createButton(section.get());
    std::unique_ptr<ui::ButtonWidget> const body = backend.createButton(section.get());
    section->setHeader(*header);
    section->setOpen(false);
    body->setTestId("t");
    auto const spotlight = ringing(backend);
    CHECK_FALSE(spotlight->targetShown());
    header->setTestId("t");
    body->setTestId("other");
    CHECK(spotlight->targetShown());
    body->setTestId("t");
    header->setTestId("other");
    CHECK_FALSE(spotlight->targetShown());
    section->setOpen(true);
    CHECK(spotlight->targetShown());
}

TEST_CASE("spotlight edges: the spotlight and its own card are never the widget it rings", "[ui][tours][spotlight]") {
    RecordingBackend backend;
    auto const spotlight = ringing(backend);
    spotlight->setTestId("t");
    std::unique_ptr<ui::ButtonWidget> const card = backend.createButton(spotlight.get());
    card->setTestId("t");
    CHECK_FALSE(spotlight->targetShown());
    std::unique_ptr<ui::ButtonWidget> const outside = backend.createButton(nullptr);
    outside->setTestId("t");
    CHECK(spotlight->targetShown());
}

TEST_CASE("spotlight edges: closing restores focus only to a widget that still exists", "[ui][tours][spotlight]") {
    RecordingBackend backend;
    std::unique_ptr<ui::SpotlightWidget> const spotlight = backend.createSpotlight(nullptr);
    // No widget had focus when it opened: closing moves it nowhere.
    spotlight->setOpen(true);
    spotlight->setOpen(false);
    CHECK(backend.prop(backend.idOf(*spotlight), "open") == "false");
    // The widget that had focus is gone by the time it closes.
    auto earlier = backend.createButton(nullptr);
    earlier->focus();
    int const earlierId = backend.idOf(*earlier);
    spotlight->setOpen(true);
    earlier.reset();
    spotlight->setOpen(false);
    CHECK(backend.prop(backend.idOf(*spotlight), "open") == "false");
    CHECK(backend.focused() != earlierId);
}

TEST_CASE("spotlight edges: an open spotlight without a target dims what is outside its card",
          "[ui][tours][spotlight]") {
    RecordingBackend backend;
    std::unique_ptr<ui::SpotlightWidget> const spotlight = backend.createSpotlight(nullptr);
    std::unique_ptr<ui::ButtonWidget> const inCard = backend.createButton(spotlight.get());
    std::unique_ptr<ui::ButtonWidget> const outside = backend.createButton(nullptr);
    int pressedInside = 0;
    int pressedOutside = 0;
    inCard->setOnClick([&pressedInside] { ++pressedInside; });
    outside->setOnClick([&pressedOutside] { ++pressedOutside; });
    spotlight->setOpen(true);
    backend.click(backend.idOf(*outside));
    backend.click(backend.idOf(*inCard));
    CHECK(pressedOutside == 0);
    CHECK(pressedInside == 1);
    // A chord nothing in the card declares is not taken.
    CHECK_FALSE(backend.press(backend.idOf(*inCard), "Right"));
}

TEST_CASE("shell edges: a tour group with no tours draws no heading, and a choice outside the list is ignored",
          "[ui][shell][tours]") {
    RecordingBackend backend;
    ui::ShellFrame frame{backend, nullptr, frameOptions()};
    std::vector<std::string> chosen;
    ui::ShellToursView view;
    view.groups = {ui::ShellTourGroup{.heading = "Empty", .tours = {}},
                   ui::ShellTourGroup{.heading = "General",
                                      .tours = {ui::ShellTourView{.id = "one", .title = "One", .summary = {}}}}};
    frame.setTours(view);
    int const menu = backend.find("Menu", "testId", "shell_tours").value();
    CHECK_FALSE(backend.prop(menu, "items").contains("Empty"));
    // With no handler a choice does nothing; with one, a heading and an index past the list do nothing.
    backend.chooseMenuEntry(menu, {1});
    frame.setOnTour([&chosen](std::string const& id) { chosen.push_back(id); });
    backend.chooseMenuEntry(menu, {0});
    backend.chooseMenuEntry(menu, {7});
    CHECK(chosen.empty());
    backend.chooseMenuEntry(menu, {1});
    CHECK(chosen == std::vector<std::string>{"one"});
    // A path that is not one entry deep is not a tour.
    backend.chooseMenuEntry(menu, {1, 0});
    CHECK(chosen.size() == 1);
    // The same in the compact navigation menu: the Tours submenu is entry 2; a path of one is the submenu itself.
    frame.setCompact(true);
    int const nav = backend.find("Menu", "testId", "shell_nav_menu").value();
    backend.chooseMenuEntry(nav, {2});
    CHECK(chosen.size() == 1);
}

TEST_CASE("shell edges: the top bar's tools show for the help, the inbox or the person's menu alone",
          "[ui][shell][tours]") {
    RecordingBackend backend;
    ui::ShellFrame frame{backend, nullptr, frameOptions()};
    int const tools = backend.find("Row", "testId", "shell_topbar_tools").value();
    CHECK(backend.prop(tools, "visible") == "false");
    {
        ui::ShellPartsView view;
        view.help.shown = true;
        frame.setParts(view);
        CHECK(backend.prop(tools, "visible") != "false");
    }
    {
        ui::ShellPartsView const view;
        frame.setParts(view);
        CHECK(backend.prop(tools, "visible") == "false");
    }
    {
        ui::ShellPartsView view;
        view.inbox.shown = true;
        view.inbox.title = "Inbox";
        frame.setParts(view);
        CHECK(backend.prop(tools, "visible") != "false");
    }
    {
        ui::ShellPartsView view;
        view.principal.shown = true;
        view.principal.name = "Ann";
        frame.setParts(view);
        CHECK(backend.prop(tools, "visible") != "false");
    }
}
