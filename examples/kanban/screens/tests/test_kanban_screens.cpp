// SPDX-License-Identifier: Apache-2.0
//
// kanban's screens on the screen harness (spec 4 §7): every screen validates; signing in and creating a project opens
// its board; columns, a swimlane and a task build the grid; dragging the task's card onto another column's cell moves
// it there through `MoveTaskPosition`. The models run in process over the rung's test database.

#include <app/app_source.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>
#include <vector>

#include "kanban_screens.hpp"

namespace {

using morph::examples::LocalApp;
using morph::examples::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ladder::testkit::DbFixture;

LocalApp kanbanApp() {
    return LocalApp{.name = "kanban", .bundle = &kanban::screens::bundle, .setUp = [](LocalSetup const&) {
                        kanban::screens::prepareLocal();
                    }};
}

void reportProblems(ScreenHarness const& harness) {
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
}

/// The `Panel`s with test id `cell`, in mount order: one per swimlane and column.
std::vector<int> cells(ScreenHarness& harness) {
    std::vector<int> out;
    for (int const panel : harness.backend().all("Panel")) {
        if (harness.backend().prop(panel, "testId") == "cell") {
            out.push_back(panel);
        }
    }
    return out;
}

/// Whether a `Text` showing @p text is under @p root.
bool holds(ScreenHarness& harness, int root, std::string_view text) {
    std::vector<int> pending{root};
    while (!pending.empty()) {
        int const widget = pending.back();
        pending.pop_back();
        if (harness.backend().kindOf(widget) == "Text" && harness.backend().prop(widget, "text") == text) {
            return true;
        }
        for (int const child : harness.backend().children(widget)) {
            pending.push_back(child);
        }
    }
    return false;
}

/// Submits @p form with @p field set to @p value.
void submit(ScreenHarness& harness, std::string_view form, std::string const& field, std::string value) {
    REQUIRE(harness.waitFor([&] { return harness.findIn(form, "TextInput", "field_" + field).has_value(); }));
    harness.editField(form, field, std::move(value));
    REQUIRE(harness.submitForm(form));
}

}  // namespace

TEST_CASE("kanban screens: every screen validates, and the bundle builds", "[kanban-screens]") {
    auto const bundle = kanban::screens::bundle();
    INFO((bundle ? std::string{} : bundle.error().message()));
    REQUIRE(bundle.has_value());
}

// Mutation: drop a card into the cell it came from (the card stays in the first column).
TEST_CASE("kanban screens: a new project's board takes columns, a lane and a task, and a card moves by drag and drop",
          "[kanban-screens]") {
    DbFixture fixture;
    ScreenHarness harness{
        kanbanApp(), std::string{kanban::screens::kProjects}, RigMode::Local, {}, LocalSetup{.db = {}, .user = {}}};
    harness.edit("username", "alice");
    harness.click("signIn");
    REQUIRE(harness.waitForText("count", "0 projects"));
    submit(harness, "createForm", "name", "Demo");
    REQUIRE(harness.waitFor([&harness] { return harness.mounted().size() == 2; }));
    REQUIRE(harness.waitForText("boardName", "Demo"));

    submit(harness, "columnForm", "name", "Todo");
    REQUIRE(harness.waitForText("summary", "1 columns, 0 swimlanes, 0 tasks"));
    submit(harness, "columnForm", "name", "Done");
    REQUIRE(harness.waitForText("summary", "2 columns, 0 swimlanes, 0 tasks"));
    submit(harness, "laneForm", "name", "Main");
    REQUIRE(harness.waitFor([&harness] { return cells(harness).size() == 2; }));
    submit(harness, "taskForm", "title", "Write");
    REQUIRE(harness.waitFor([&harness] { return holds(harness, cells(harness).at(0), "Write"); }));

    int const card = harness.find("Text", "card").value();
    REQUIRE(harness.backend().drag(card, cells(harness).at(1)));
    bool const moved = harness.waitFor([&harness] { return holds(harness, cells(harness).at(1), "Write"); });
    reportProblems(harness);
    CHECK(moved);
    CHECK_FALSE(holds(harness, cells(harness).at(0), "Write"));
    CHECK(harness.text("error").value_or("x").empty());
    CHECK(harness.problems().empty());
}
