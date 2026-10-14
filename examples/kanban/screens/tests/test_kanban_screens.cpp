// SPDX-License-Identifier: Apache-2.0
//
// kanban's screens on the screen harness (spec 4 §7): every screen validates; signing in and creating a project opens
// its board; columns, a swimlane and a task build the grid; dragging the task's card onto another column's cell moves
// it there through `MoveTaskPosition`. A task's detail takes comments; the members tab gives and removes roles; the
// rules tab creates and deletes a rule over a column it fetches; the activity tab follows the board. The models run in
// process over the rung's test database.

#include <algorithm>
#include <app/app_source.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>
#include <vector>

#include "kanban/auth/kanban_authorizer.hpp"
#include "kanban_screens.hpp"

namespace {

using morph::examples::LocalApp;
using morph::examples::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ladder::testkit::DbFixture;

LocalApp kanbanApp() {
    return LocalApp{.name = "kanban",
                    .bundle = &kanban::screens::bundle,
                    .setUp = [](LocalSetup const&) { kanban::screens::prepareLocal(); },
                    // The secret prepareLocal's token issuer signs with, which a simulated server verifies.
                    .serverAuthorizer =
                        [] {
                            return std::make_shared<kanban::auth::KanbanAuthorizer>("kanban-local-development",
                                                                                    morph::session::hmacSha256);
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

/// The texts of the widgets with @p testId under the table with @p tableTestId, sorted.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the table, then the cells' test id.
std::vector<std::string> column(ScreenHarness& harness, std::string_view tableTestId, std::string_view testId) {
    std::vector<std::string> texts;
    auto const table = harness.find("Table", tableTestId);
    if (!table) {
        return texts;
    }
    std::vector<int> pending{*table};
    while (!pending.empty()) {
        int const widget = pending.back();
        pending.pop_back();
        if (harness.backend().prop(widget, "testId") == testId) {
            texts.push_back(harness.backend().prop(widget, "text"));
        }
        for (int const child : harness.backend().children(widget)) {
            pending.push_back(child);
        }
    }
    std::ranges::sort(texts);
    return texts;
}

/// The texts of every `Text` with @p testId, in mount order.
std::vector<std::string> texts(ScreenHarness& harness, std::string_view testId) {
    std::vector<std::string> out;
    for (int const text : harness.backend().all("Text")) {
        if (harness.backend().prop(text, "testId") == testId) {
            out.push_back(harness.backend().prop(text, "text"));
        }
    }
    return out;
}

/// Clicks the button with @p buttonTestId in the row of the table with @p tableTestId whose @p cellTestId shows
/// @p text.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): the table, the cell that names the row, its text, the button.
void clickInRow(ScreenHarness& harness, std::string_view tableTestId, std::string_view cellTestId,
                std::string_view text, std::string_view buttonTestId) {
    // NOLINTEND(bugprone-easily-swappable-parameters)
    int const table = harness.find("Table", tableTestId).value();
    for (int const row : harness.backend().children(table)) {
        std::optional<int> button;
        bool named = false;
        std::vector<int> pending{row};
        while (!pending.empty()) {
            int const widget = pending.back();
            pending.pop_back();
            named = named || (harness.backend().prop(widget, "testId") == cellTestId &&
                              harness.backend().prop(widget, "text") == text);
            if (harness.backend().prop(widget, "testId") == buttonTestId) {
                button = widget;
            }
            for (int const child : harness.backend().children(widget)) {
                pending.push_back(child);
            }
        }
        if (named && button) {
            harness.backend().click(*button);
            return;
        }
    }
    FAIL("no row of " << tableTestId << " shows " << text);
}

/// Selects tab @p index of the board's tabs and waits for its page, which @p testId's widget of @p kind is on.
void showTab(ScreenHarness& harness, std::size_t index, std::string_view kind, std::string_view testId) {
    harness.selectTab("sections", index);
    int const bar = harness.find("Tabs", "sections").value();
    REQUIRE(harness.waitFor([&harness, kind, testId, bar] {
        auto const widget = harness.find(kind, testId);
        return widget && harness.shownUnder(bar, *widget);
    }));
}

/// Signs alice in, creates project Demo and builds its board: columns Todo and Done, lane Main, and task Write in
/// Todo.
void buildBoard(ScreenHarness& harness) {
    harness.edit("username", "alice");
    harness.click("signIn");
    bool const signedIn = harness.waitForText("count", "0 projects");
    reportProblems(harness);
    INFO("error: " << harness.text("error").value_or("") << " count: " << harness.text("count").value_or(""));
    REQUIRE(signedIn);
    submit(harness, "createForm", "name", "Demo");
    REQUIRE(harness.waitFor([&harness] { return harness.mounted().size() == 3; }));
    REQUIRE(harness.waitForText("boardName", "Demo"));
    submit(harness, "columnForm", "name", "Todo");
    REQUIRE(harness.waitForText("summary", "1 columns, 0 swimlanes, 0 tasks"));
    submit(harness, "columnForm", "name", "Done");
    REQUIRE(harness.waitForText("summary", "2 columns, 0 swimlanes, 0 tasks"));
    submit(harness, "laneForm", "name", "Main");
    REQUIRE(harness.waitFor([&harness] { return cells(harness).size() == 2; }));
    submit(harness, "taskForm", "title", "Write");
    REQUIRE(harness.waitFor([&harness] { return holds(harness, cells(harness).at(0), "Write"); }));
}

ScreenHarness openLogin(RigMode mode = RigMode::Local) {
    return ScreenHarness{
        kanbanApp(), std::string{kanban::screens::kLogin}, mode, {}, LocalSetup{.db = {}, .user = {}}};
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
    ScreenHarness harness = openLogin();
    buildBoard(harness);

    int const card = harness.find("Text", "card").value();
    REQUIRE(harness.backend().drag(card, cells(harness).at(1)));
    bool const moved = harness.waitFor([&harness] { return holds(harness, cells(harness).at(1), "Write"); });
    reportProblems(harness);
    CHECK(moved);
    CHECK_FALSE(holds(harness, cells(harness).at(0), "Write"));
    CHECK(harness.text("error").value_or("x").empty());
    CHECK(harness.problems().empty());
}

// Mutation: prefill the comment form with no task (the comment is refused, and none is listed).
TEST_CASE("kanban screens: a task's detail lists its comments, and a new one joins them", "[kanban-screens]") {
    DbFixture fixture;
    ScreenHarness harness = openLogin();
    buildBoard(harness);
    harness.click("details");
    REQUIRE(harness.waitForText("taskTitle", "Write"));
    submit(harness, "commentForm", "body", "Looks good");
    bool const listed = harness.waitFor(
        [&harness] { return texts(harness, "comment") == std::vector<std::string>{"alice: Looks good"}; });
    reportProblems(harness);
    CHECK(listed);
    CHECK(harness.problems().empty());
}

// Mutation: send RemoveMember without the project (it is refused, and bob stays listed).
TEST_CASE("kanban screens: the members tab gives someone a role and removes them", "[kanban-screens]") {
    DbFixture fixture;
    ScreenHarness harness = openLogin();
    buildBoard(harness);
    showTab(harness, 1, "Column", "memberForm");
    REQUIRE(harness.waitFor(
        [&harness] { return column(harness, "members", "member") == std::vector<std::string>{"alice"}; }));
    CHECK(column(harness, "members", "role") == std::vector<std::string>{"Manager"});
    harness.editField("memberForm", "principal", "bob");
    harness.chooseField("memberForm", "role", "\"Member\"");
    REQUIRE(harness.submitForm("memberForm"));
    REQUIRE(harness.waitFor(
        [&harness] { return column(harness, "members", "member") == std::vector<std::string>{"alice", "bob"}; }));
    CHECK(column(harness, "members", "role") == std::vector<std::string>{"Manager", "Member"});
    clickInRow(harness, "members", "member", "bob", "removeMember");
    bool const removed = harness.waitFor(
        [&harness] { return column(harness, "members", "member") == std::vector<std::string>{"alice"}; });
    reportProblems(harness);
    INFO("error: " << harness.text("membersError").value_or(""));
    CHECK(removed);
}

// Mutation: delete with no rule id (the rule stays listed).
TEST_CASE("kanban screens: the rules tab creates a rule on a column it fetches, and deletes it", "[kanban-screens]") {
    DbFixture fixture;
    ScreenHarness harness = openLogin();
    buildBoard(harness);
    showTab(harness, 2, "Button", "newRule");
    harness.click("newRule");
    REQUIRE(harness.waitFor([&harness] { return harness.find("Column", "ruleForm").has_value(); }));
    // The trigger column's options are the board's columns, fetched by the form.
    bool const fetched = harness.waitFor([&harness] {
        auto const select = harness.findIn("ruleForm", "Select", "field_triggerColumnId");
        return select && harness.backend().prop(*select, "options").contains("Done");
    });
    if (auto const select = harness.findIn("ruleForm", "Select", "field_triggerColumnId")) {
        UNSCOPED_INFO("options: " << harness.backend().prop(*select, "options"));
    }
    reportProblems(harness);
    REQUIRE(fetched);

    // Done is the second column made, and ids count from 1 in a fresh database.
    harness.chooseField("ruleForm", "triggerColumnId", "2");
    harness.chooseField("ruleForm", "mutationType", "\"AddTag\"");
    harness.editField("ruleForm", "mutationValue", "shipped");
    bool const submitted = harness.submitForm("ruleForm");
    reportProblems(harness);
    REQUIRE(submitted);
    REQUIRE(harness.waitFor(
        [&harness] { return column(harness, "rules", "ruleTag") == std::vector<std::string>{"shipped"}; }));
    clickInRow(harness, "rules", "ruleTag", "shipped", "deleteRule");
    CHECK(harness.waitFor([&harness] { return column(harness, "rules", "ruleTag").empty(); }));
    INFO("error: " << harness.text("rulesError").value_or(""));
}

// Mutation: drop AddComment from the activity's refreshOn (the comment's entry never appears).
TEST_CASE("kanban screens: the activity tab follows what the board does", "[kanban-screens]") {
    DbFixture fixture;
    ScreenHarness harness = openLogin();
    buildBoard(harness);
    showTab(harness, 3, "Text", "activityError");
    bool const listed = harness.waitFor([&harness] { return !texts(harness, "activity").empty(); });
    reportProblems(harness);
    INFO("activity error: " << harness.text("activityError").value_or("(none)"));
    REQUIRE(listed);
    std::size_t const before = texts(harness, "activity").size();
    for (std::string const& entry : texts(harness, "activity")) {
        UNSCOPED_INFO("activity " << entry);
    }
    showTab(harness, 0, "Column", "taskForm");
    harness.click("details");
    REQUIRE(harness.waitForText("taskTitle", "Write"));
    submit(harness, "commentForm", "body", "Noted");
    showTab(harness, 3, "Text", "activityError");
    bool const grew = harness.waitFor([&harness, before] { return texts(harness, "activity").size() > before; });
    reportProblems(harness);
    CHECK(grew);
}

// Mutation: record the attachment with a storage key of its own rather than the upload's reference (the download
// finds no such file).
TEST_CASE("kanban screens: a picked file is attached to a task, and downloads back byte for byte",
          "[kanban-screens]") {
    DbFixture fixture;
    ScreenHarness harness = openLogin();
    buildBoard(harness);
    harness.click("details");
    REQUIRE(harness.waitForText("taskTitle", "Write"));

    std::string const bytes = "the attachment's bytes\n";
    auto const source = harness.downloads().parent_path() / (harness.downloads().filename().string() + "-notes.txt");
    {
        std::ofstream out{source, std::ios::binary};
        out << bytes;
    }
    harness.backend().pick(harness.find("FilePicker", "attachFile").value(), source.string());
    bool const attached =
        harness.waitFor([&harness] { return column(harness, "attachments", "attachment").size() == 1; });
    reportProblems(harness);
    INFO("error: " << harness.text("detailError").value_or(""));
    REQUIRE(attached);

    harness.click("download");
    auto const saved = harness.downloads() / source.filename();
    bool const downloaded = harness.waitFor([&saved] { return std::filesystem::exists(saved); });
    std::filesystem::remove(source);
    REQUIRE(downloaded);
    REQUIRE(harness.waitFor([&saved, &bytes] { return std::filesystem::file_size(saved) == bytes.size(); }));
    std::ifstream input{saved, std::ios::binary};
    std::string const got{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    CHECK(got == bytes);
}

// Mutation: install the principal without the reply's token (every call after sign-in is refused), or skip the
// catalog reload (the projects screen is not listed, and the navigation to it fails).
TEST_CASE("kanban screens: signing in to a server installs its token and reloads the catalog", "[kanban-screens]") {
    DbFixture fixture;
    ScreenHarness harness = openLogin(RigMode::Simulated);
    buildBoard(harness);
    reportProblems(harness);
    CHECK(harness.problems().empty());
    CHECK(harness.mounted() == std::vector<std::string>{std::string{kanban::screens::kLogin},
                                                        std::string{kanban::screens::kProjects},
                                                        std::string{kanban::screens::kBoard}});
}
