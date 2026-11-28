// SPDX-License-Identifier: Apache-2.0

// The table menu's commands over saved views and the clipboard, driven through a mounted rows table whose store keeps
// the user's views and the shared ones.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/table/engine.hpp>
#include <morph/table/state_store.hpp>
#include <morph/ui/interpreter.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ui_table_support.hpp"

namespace {

namespace table = morph::table;
using namespace std::chrono_literals;
using morph::ui::MenuEntry;
using morph::ui::testing::chooseMenu;
using morph::ui::testing::elementValue;
using morph::ui::testing::entryOf;
using morph::ui::testing::kNine;
using morph::ui::testing::labelsOf;
using morph::ui::testing::menuOf;
using morph::ui::testing::RecordingTableStateStore;
using morph::ui::testing::statusOf;
using morph::ui::testing::storeExecutor;
using morph::ui::testing::TableHarness;

table::TableStateKey partKey(std::string part) {
    return table::TableStateKey{.application = "lims",
                                .model = "Lims_SampleModel",
                                .table = "samples",
                                .qualifier = "samples",
                                .part = std::move(part)};
}

table::SavedView sortedView(std::string name, std::string column) {
    table::ColumnLayoutValue layout;
    layout.order = {"c1", "c2", "c3", "c4", "c5", "c6", "c7", "c8", "c9"};
    return table::SavedView{
        .name = std::move(name),
        .layout = std::move(layout),
        .sort = {table::SortKey{.column = std::move(column), .dir = table::SortDirection::Ascending}},
        .filters = {}};
}

std::string viewsText(std::vector<table::SavedView> views, table::ViewsPart part,
                      std::optional<std::string> def = {}) {
    table::ViewsDocument document;
    document.views = std::move(views);
    document.defaultView = std::move(def);
    return table::writeViews(document, part);
}

void seed(TableHarness& h, RecordingTableStateStore& store, std::string part, std::string json) {
    std::optional<table::SaveResult> saved;
    store.save(partKey(std::move(part)), std::move(json)).thenDetached([&saved](table::SaveResult const& result) {
        saved = result;
    });
    h.settle();
    REQUIRE(saved.has_value());
    REQUIRE(saved->outcome == table::SaveOutcome::Saved);
}

table::ViewsDocument storedViews(TableHarness& h, RecordingTableStateStore& store, std::string part) {
    std::optional<std::optional<table::StoredTableState>> got;
    store.load(partKey(std::move(part))).thenDetached([&got](std::optional<table::StoredTableState> const& stored) {
        got = stored;
    });
    h.settle();
    REQUIRE(got.has_value());
    REQUIRE(got->has_value());
    auto const document = table::readViews((*got)->json);
    REQUIRE(document.has_value());
    return *document;
}

std::vector<MenuEntry> viewsOf(TableHarness& h, int table) {
    return entryOf(menuOf(h, table, std::nullopt), "Views").items;
}

void nameView(TableHarness& h, std::string name) {
    h.backend.edit(h.byTestId("TextInput", "table_view_name"), std::move(name));
    h.settle();
    h.backend.click(h.byTestId("Button", "table_view_save"));
    h.settle();
}
}  // namespace

TEST_CASE("cov views menu: a view is renamed, made the default, removed and the default-on-mount toggled",
          "[ui][table][state][cov]") {
    auto const store = std::make_shared<RecordingTableStateStore>(storeExecutor());
    TableHarness h{store};
    seed(h, *store, "views", viewsText({sortedView("A", "c1"), sortedView("B", "c2")}, table::ViewsPart::Own));
    auto const screen = h.mount(kNine);
    int const table = h.backend.all("Table").front();
    chooseMenu(h, table, std::nullopt, {"Views", "A"});
    CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"c1","direction":"asc"}])");

    chooseMenu(h, table, std::nullopt, {"Views", "Set as default"});
    CHECK(storedViews(h, *store, "views").defaultView == std::optional<std::string>{"A"});

    chooseMenu(h, table, std::nullopt, {"Views", "Start from the default view"});
    CHECK(storedViews(h, *store, "views").defaultOnMount);
    chooseMenu(h, table, std::nullopt, {"Views", "Start from the default view"});
    CHECK_FALSE(storedViews(h, *store, "views").defaultOnMount);

    chooseMenu(h, table, std::nullopt, {"Views", "Rename view…"});
    nameView(h, "Renamed");
    auto const afterRename = storedViews(h, *store, "views");
    REQUIRE(afterRename.views.size() == 2);
    std::vector<std::string> names;
    names.reserve(afterRename.views.size());
    for (auto const& view : afterRename.views) {
        names.push_back(view.name);
    }
    CHECK(std::ranges::contains(names, std::string{"Renamed"}));
    CHECK_FALSE(std::ranges::contains(names, std::string{"A"}));

    chooseMenu(h, table, std::nullopt, {"Views", "Remove view"});
    auto const afterRemove = storedViews(h, *store, "views");
    REQUIRE(afterRemove.views.size() == 1);
    CHECK(afterRemove.views.front().name == "B");
    // With no view applied the commands that need one are disabled and a choice does nothing.
    CHECK_FALSE(entryOf(viewsOf(h, table), "Remove view").enabled);
    chooseMenu(h, table, std::nullopt, {"Views", "Remove view"});
    CHECK(storedViews(h, *store, "views").views.size() == 1);
}

TEST_CASE("cov views menu: a shared view is applied and saved among the user's own", "[ui][table][state][cov]") {
    auto const store = std::make_shared<RecordingTableStateStore>(storeExecutor(), table::Sharing::Shared);
    TableHarness h{store};
    seed(h, *store, "shared", viewsText({sortedView("Team", "c3")}, table::ViewsPart::Shared));
    auto const screen = h.mount(kNine);
    int const table = h.backend.all("Table").front();
    CHECK(labelsOf(viewsOf(h, table)).back() == "Publish view…");
    chooseMenu(h, table, std::nullopt, {"Views", "Team"});
    CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"c3","direction":"asc"}])");
    CHECK(entryOf(viewsOf(h, table), "Team").checked == std::optional<bool>{true});
    // A shared view applied cannot be renamed or removed as the user's own.
    CHECK_FALSE(entryOf(viewsOf(h, table), "Rename view…").enabled);
    chooseMenu(h, table, std::nullopt, {"Views", "Save as my view"});
    CHECK(statusOf(h, table).message == "Saved view Team");
    auto const own = storedViews(h, *store, "views");
    REQUIRE(own.views.size() == 1);
    CHECK(own.views.front().name == "Team");
    // The saved copy is the applied view now: it can be renamed.
    CHECK(entryOf(viewsOf(h, table), "Rename view…").enabled);
}

TEST_CASE("cov views menu: Reset columns and Clear all filters restore the arrangement", "[ui][table][state][cov]") {
    TableHarness h;
    auto const screen = h.mount(kNine);
    int const table = h.backend.all("Table").front();
    chooseMenu(h, table, 0, {"Hide column"});
    CHECK_FALSE(elementValue(*screen, "t", "visibleColumns").contains(R"("c1")"));
    chooseMenu(h, table, std::nullopt, {"Reset columns"});
    CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["c1","c2","c3","c4","c5","c6","c7","c8","c9"])");
    CHECK_FALSE(entryOf(menuOf(h, table, std::nullopt), "Clear all filters").enabled);
}
