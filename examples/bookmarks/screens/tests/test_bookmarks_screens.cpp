// SPDX-License-Identifier: Apache-2.0
//
// bookmarks' screens on the screen harness (spec 4 §7): every screen validates; signing in installs the principal and
// starts the queries; a bookmark created from the drawer is listed; archiving the selection is one bulk mutation that
// empties the active list, and showing archived ones brings them back. The models run in process over the rung's test
// database.

#include <app/app_source.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/ui/view.hpp>
#include <string>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>
#include <vector>

#include "bookmarks_screens.hpp"

namespace {

using morph::examples::LocalApp;
using morph::examples::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ladder::testkit::DbFixture;

LocalApp bookmarksApp() {
    return LocalApp{.name = "bookmarks", .bundle = &bookmarks::screens::bundle, .setUp = [](LocalSetup const&) {
                        bookmarks::screens::prepareLocal();
                    }};
}

ScreenHarness open() {
    return ScreenHarness{
        bookmarksApp(), std::string{bookmarks::screens::kLogin}, RigMode::Local, {}, LocalSetup{.db = {}, .user = {}}};
}

void reportProblems(ScreenHarness const& harness) {
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
}

void signIn(ScreenHarness& harness) {
    harness.edit("username", "alice");
    harness.click("signIn");
    REQUIRE(harness.waitForText("count", "0 bookmarks"));
}

/// Creates a bookmark of @p url, the @p nth of the list.
void createBookmark(ScreenHarness& harness, std::string const& url, int nth) {
    harness.click("newBookmark");
    REQUIRE(
        harness.waitFor([&harness] { return harness.findIn("createForm", "TextInput", "field_url").has_value(); }));
    harness.editField("createForm", "url", url);
    REQUIRE(harness.submitForm("createForm"));
    REQUIRE(harness.waitForText("count", std::to_string(nth) + " bookmarks"));
}

}  // namespace

TEST_CASE("bookmarks screens: the screen validates, and the bundle builds", "[bookmarks-screens]") {
    auto const bundle = bookmarks::screens::bundle();
    INFO((bundle ? std::string{} : bundle.error().message()));
    REQUIRE(bundle.has_value());
}

// Mutation: start the queries before signing in (they fail without a principal).
TEST_CASE("bookmarks screens: signing in starts the queries, and a created bookmark is listed",
          "[bookmarks-screens]") {
    DbFixture fixture;
    ScreenHarness harness = open();
    signIn(harness);
    createBookmark(harness, "https://example.org/a", 1);
    reportProblems(harness);
    CHECK(harness.text("error").value_or("").empty());
    CHECK(harness.problems().empty());
}

// Mutation: archive only the first selected id (one bookmark stays active).
TEST_CASE("bookmarks screens: archiving the selection is one bulk mutation", "[bookmarks-screens]") {
    DbFixture fixture;
    ScreenHarness harness = open();
    signIn(harness);
    createBookmark(harness, "https://example.org/a", 1);
    createBookmark(harness, "https://example.org/b", 2);
    int const table = harness.find("Table", "bookmarks").value();
    std::vector<morph::ui::Key> selected;
    for (int const row : harness.backend().children(table)) {
        if (harness.backend().hasProp(row, "rowKey")) {
            selected.emplace_back(std::stoll(harness.backend().prop(row, "rowKey")));
        }
    }
    REQUIRE(selected.size() == 2U);
    harness.backend().selectRows(table, selected);
    harness.click("archiveSelected");
    bool const archived = harness.waitForText("count", "0 bookmarks");
    reportProblems(harness);
    CHECK(archived);
    CHECK(harness.problems().empty());
}

// Mutation: drop the archive filter from the query body (the archived bookmarks never come back).
TEST_CASE("bookmarks screens: show archived lists the archived bookmarks again", "[bookmarks-screens]") {
    DbFixture fixture;
    ScreenHarness harness = open();
    signIn(harness);
    createBookmark(harness, "https://example.org/a", 1);
    int const table = harness.find("Table", "bookmarks").value();
    std::vector<morph::ui::Key> selected;
    for (int const row : harness.backend().children(table)) {
        if (harness.backend().hasProp(row, "rowKey")) {
            selected.emplace_back(std::stoll(harness.backend().prop(row, "rowKey")));
        }
    }
    harness.backend().selectRows(table, selected);
    harness.click("archiveSelected");
    REQUIRE(harness.waitForText("count", "0 bookmarks"));
    harness.toggle("showArchived");
    bool const shown = harness.waitForText("count", "1 bookmarks");
    reportProblems(harness);
    CHECK(shown);
    CHECK(harness.problems().empty());
}
