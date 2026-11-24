// SPDX-License-Identifier: Apache-2.0
//
// The shell frame's tours (docs/spec/ui/shell_frame.md, "Tours"): the Tours button lists the active screen's tours
// under the screen's title and then the general ones, reports the tour chosen, sits after help and before the person's
// menu, and moves into the navigation menu when compact; the frame makes the spotlight once, over the main column.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/shell_frame.hpp>
#include <morph/ui/shell_parts_model.hpp>
#include <morph/ui/shell_parts_view.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <string>
#include <vector>

namespace ui = morph::ui;
using ui::testing::RecordingBackend;

namespace {

ui::ShellFrameOptions options() {
    ui::ShellFrameOptions made;
    made.applicationName = "Lab";
    made.navigator.entries = {ui::CatalogEntry{.id = "lims.samples", .title = "Samples", .group = "Lab"}};
    made.onSignOut = [] {};
    return made;
}

ui::ShellToursView samplesTours() {
    ui::ShellToursView view;
    view.groups = {
        ui::ShellTourGroup{
            .heading = "Samples",
            .tours = {ui::ShellTourView{
                          .id = "register", .title = "Register a sample", .summary = "From the form to the label"},
                      ui::ShellTourView{.id = "find", .title = "Find a sample", .summary = {}}}},
        ui::ShellTourGroup{
            .heading = "General",
            .tours = {ui::ShellTourView{
                .id = "getting-started", .title = "Getting started", .summary = "The shell in four steps"}}},
    };
    return view;
}

}  // namespace

// Mutation: list the groups' tours without their headings (the items have no heading entry).
TEST_CASE("shell tours: the Tours button lists the screen's tours, then the general ones, and reports the choice",
          "[ui][shell][tours]") {
    RecordingBackend backend;
    ui::ShellFrame frame{backend, nullptr, options()};
    std::vector<std::string> chosen;
    frame.setOnTour([&chosen](std::string const& id) { chosen.push_back(id); });
    frame.setTours(samplesTours());
    int const menu = backend.find("Menu", "testId", "shell_tours").value();
    CHECK(backend.prop(menu, "visible") != "false");
    CHECK(backend.prop(menu, "label").contains("Tours"));
    CHECK(backend.prop(menu, "a11yName") == "Guided tours");
    std::string const items = backend.prop(menu, "items");
    CHECK(items.find("Samples;enabled=false;heading") < items.find("Register a sample"));
    CHECK(items.find("Register a sample") < items.find("Find a sample"));
    CHECK(items.find("Find a sample") < items.find("General;enabled=false;heading"));
    CHECK(items.find("General;enabled=false;heading") < items.find("Getting started"));
    CHECK(items.contains("description=From the form to the label"));
    backend.chooseMenuEntry(menu, {4});
    backend.chooseMenuEntry(menu, {1});
    CHECK(chosen == std::vector<std::string>{"getting-started", "register"});
    // The top bar's tools show for tours alone.
    CHECK(backend.prop(backend.find("Row", "testId", "shell_topbar_tools").value(), "visible") != "false");
}

// Mutation: show the button whatever the view holds (it shows with no tour).
TEST_CASE("shell tours: with no tour the Tours button is hidden", "[ui][shell][tours]") {
    RecordingBackend backend;
    ui::ShellFrame frame{backend, nullptr, options()};
    frame.setTours(samplesTours());
    frame.setTours(ui::ShellToursView{});
    CHECK(backend.prop(backend.find("Menu", "testId", "shell_tours").value(), "visible") == "false");
}

// Mutation: make the Tours menu before the help button (it comes first among the tools).
TEST_CASE("shell tours: the Tours button sits after the help button and before the person's menu",
          "[ui][shell][tours]") {
    RecordingBackend backend;
    ui::ShellFrame const frame{backend, nullptr, options()};
    int const tools = backend.find("Row", "testId", "shell_topbar_tools").value();
    std::vector<int> const children = backend.children(tools);
    auto const at = [&](int widget) { return std::ranges::find(children, widget) - children.begin(); };
    int const help = backend.find("Button", "testId", "shell_help").value();
    int const tours = backend.find("Menu", "testId", "shell_tours").value();
    int const person = backend.find("Menu", "testId", "shell_principal_menu").value();
    CHECK(at(help) + 1 == at(tours));
    CHECK(at(tours) + 1 == at(person));
}

// Mutation: leave the Tours submenu out of `showNavEntries` (the compact menu has no "Tours").
// The navigation menu holds the navigator's one group at 0, a separator at 1 and the Tours submenu at 2.
TEST_CASE("shell tours: compact, the Tours entries join the navigation menu and the button hides",
          "[ui][shell][tours]") {
    RecordingBackend backend;
    ui::ShellFrame frame{backend, nullptr, options()};
    std::vector<std::string> chosen;
    frame.setOnTour([&chosen](std::string const& id) { chosen.push_back(id); });
    frame.setTours(samplesTours());
    frame.setCompact(true);
    CHECK(backend.prop(backend.find("Menu", "testId", "shell_tours").value(), "visible") == "false");
    int const nav = backend.find("Menu", "testId", "shell_nav_menu").value();
    std::string const items = backend.prop(nav, "items");
    REQUIRE(items.contains("Tours"));
    CHECK(items.find("Samples") < items.find("Tours"));
    backend.chooseMenuEntry(nav, {2, 4});
    CHECK(chosen == std::vector<std::string>{"getting-started"});
}

// Mutation: make a new spotlight at each call (two Spotlight widgets).
TEST_CASE("shell tours: the frame makes the spotlight once, over its main column", "[ui][shell][tours]") {
    RecordingBackend backend;
    ui::ShellFrame frame{backend, nullptr, options()};
    CHECK(backend.all("Spotlight").empty());
    ui::SpotlightWidget& first = frame.spotlight();
    ui::SpotlightWidget& second = frame.spotlight();
    CHECK(&first == &second);
    REQUIRE(backend.all("Spotlight").size() == 1);
    int const spotlight = backend.all("Spotlight").front();
    CHECK(backend.prop(spotlight, "testId") == "shell_spotlight");
    int const sidebar = backend.find("Row", "testId", "shell_sidebar").value();
    int const shell = backend.find("Row", "testId", "shell").value();
    std::vector<int> const columns = backend.children(shell);
    REQUIRE(columns.size() >= 2);
    CHECK(columns.at(0) == sidebar);
    CHECK(backend.children(columns.at(1)).back() == spotlight);
}

// Mutation: drop the German row of `morph.shell.toursGeneral` (it reads "General" in German).
TEST_CASE("shell tours: the Tours texts are built in, in English and German", "[ui][shell][tours]") {
    ui::FormatContext english;
    english.locale = "en";
    ui::FormatContext german;
    german.locale = "de";
    CHECK(ui::shellText("morph.shell.tours", english) == "Tours");
    CHECK(ui::shellText("morph.shell.toursName", english) == "Guided tours");
    CHECK(ui::shellText("morph.shell.toursGeneral", german) == "Allgemein");
}

// Mutation: hand the handler a reference into the frame's entry map (`_onTour(*_tourMap.at(entry))`); the handler's
// `setTours` rebuilds the map and the id it reads afterwards is a dead string. The id is longer than any small-string
// buffer, so it lives on the heap and the dead string is no longer readable by luck.
TEST_CASE("shell tours: a handler that replaces the tours still reads the id it was given", "[ui][shell][tours]") {
    std::string const longId = "a-guided-tour-whose-id-is-far-longer-than-a-small-string-buffer-holds";
    REQUIRE(longId.size() > 32);
    auto const toursWithLongId = [&longId] {
        ui::ShellToursView view = samplesTours();
        view.groups.front().tours.front().id = longId;
        return view;
    };
    RecordingBackend backend;
    ui::ShellFrame frame{backend, nullptr, options()};
    std::string seen;
    frame.setOnTour([&](std::string const& id) {
        frame.setTours(ui::ShellToursView{});
        frame.setTours(toursWithLongId());
        seen = id;
    });
    frame.setTours(toursWithLongId());
    backend.chooseMenuEntry(backend.find("Menu", "testId", "shell_tours").value(), {1});
    CHECK(seen == longId);
}

// Mutation: leave out `setTooltip` (the button has no tooltip).
TEST_CASE("shell tours: the Tours button's tooltip is its name", "[ui][shell][tours]") {
    RecordingBackend backend;
    ui::ShellFrame frame{backend, nullptr, options()};
    frame.setTours(samplesTours());
    CHECK(backend.prop(backend.find("Menu", "testId", "shell_tours").value(), "tooltip") == "Guided tours");
}

// Mutation: hide the button for good once compact (`setVisible(tours && !_compact)` becomes `tours && !_everCompact`).
TEST_CASE("shell tours: leaving the compact form shows the Tours button again", "[ui][shell][tours]") {
    RecordingBackend backend;
    ui::ShellFrame frame{backend, nullptr, options()};
    frame.setTours(samplesTours());
    frame.setCompact(true);
    int const menu = backend.find("Menu", "testId", "shell_tours").value();
    CHECK(backend.prop(menu, "visible") == "false");
    frame.setCompact(false);
    CHECK(backend.prop(menu, "visible") != "false");
}
