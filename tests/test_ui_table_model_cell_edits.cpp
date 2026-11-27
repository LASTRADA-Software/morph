// SPDX-License-Identifier: Apache-2.0

// Editing a model-backed table's cells: selection, Delete and the cell edit lowering.
//
// Part of the client table model tests; see docs/spec/table/table.md.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/edits.hpp>
#include <morph/table/query.hpp>
#include <morph/table/sort.hpp>
#include <morph/table/view_change.hpp>
#include <morph/ui/grid.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/value.hpp>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "grid_test_support.hpp"
#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

using namespace table_model_support;

using namespace std::chrono_literals;
using morph::test::grid::at;
using morph::test::grid::ModelGridFixture;
using morph::ui::Key;
using morph::ui::testing::change;
using morph::ui::testing::replyPage;
using morph::ui::testing::statusOf;
using morph::ui::testing::TableHarness;

// Mutation: ModelGridSource selecting cells (Shift+Down makes a rectangle; the chords are claimed from the table).

TEST_CASE("model table: the table keeps its row selection, and Delete blanks a cell", "[ui][table][edit]") {
    ModelGridFixture f{{1, 2}};
    f.grid->focusCell(at(1, "sampled"));
    std::vector<std::string> const chords = f.grid->keyChords();
    CHECK_FALSE(std::ranges::contains(chords, std::string{"Shift+Down"}));
    CHECK_FALSE(std::ranges::contains(chords, std::string{"Ctrl+A"}));
    CHECK(std::ranges::contains(chords, std::string{"Delete"}));
    f.grid->press("Shift+Down");
    CHECK_FALSE(f.grid->selection().has_value());
    f.grid->press("Delete");
    REQUIRE(f.commits.sent.size() == 1);
    CHECK(f.commits.sent.at(0).cells.at(0).column == "sampled");
    CHECK(f.commits.sent.at(0).cells.at(0).json == "null");
}

// Mutation: positionOf scanning the whole view total, and rebuilding its index on each change of a cell's draft.

TEST_CASE("model table: finding a row by key reads only the rows held", "[ui][table][edit]") {
    ModelGridFixture f{{1, 2, 3}};
    f.rows->unheld = 1'000'000;  // a large view, three rows held
    f.rows->rowReads = 0;
    CHECK(f.source->positionOf(key(3)) == std::size_t{2});
    std::size_t const first = f.rows->rowReads;
    CHECK(first <= 3);
    f.grid->focusCell(at(1, "label"));
    for (std::string const text : {"a", "ab", "abc"}) {
        f.grid->type(text);  // each keystroke a draft
    }
    f.rows->rowReads = 0;
    CHECK(f.source->positionOf(key(3)) == std::size_t{2});
    CHECK(f.rows->rowReads <= 1);  // no rebuild for a draft
}

// Mutations: the lowering leaving a column with `edit` read-only (no request is sent); node() replacing the base cell
// view with a plain text cell (the template's text is not drawn, and the tone is gone).

TEST_CASE("model table: an editable column lowers to a cell edit", "[ui][table][edit][interpreter]") {
    using namespace morph::ui::testing;
    TableHarness h;
    addEditActions(h);
    auto const screen = mountEdits(h, editDoc());
    CHECK(cellText(h, 1, "count") == "1");  // the column's own cell view
    CHECK(cellAccent(h, 1, "count").tone == morph::ui::Tone::Warn);
    int const table = h.byTestId("Table", "t");
    std::size_t const count = columnIndex(h, "count");
    h.backend.clickCell(table, 0, count, true);
    h.settle();
    std::optional<int> const first = h.backend.tableRow(table, Key{std::int64_t{1}});
    REQUIRE(first.has_value());
    CHECK_FALSE(h.backend.prop(*first, "editor:" + std::to_string(count)).empty());  // the editor opened
    h.backend.press(table, "Escape");
    h.settle();
    h.backend.typeInto(table, "4");
    h.settle();
    h.backend.press(table, "Enter");
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_EditSpecimenCount") == 1);
    std::size_t const call = h.dispatcher.last("Lims_EditSpecimenCount");
    CHECK(h.dispatcher.calls.at(call).body == R"({"key":1,"value":4})");
    CHECK(statusOf(h, h.byTestId("Table", "t")).standing.empty());  // no run of the session: no "Saving"
    h.reply(call, R"({"rows":{"rows":[{"key":1}]}})");
    change(h, {1});
    CHECK(cellMarks(h, 1, "count") == "m");  // modified until the refetch
    replyPage(h, editPage(h, {editRow(1, 4)}, {0}));
    CHECK(cellText(h, 1, "count") == "4");
    CHECK(cellMarks(h, 1, "count").empty());
}

// Mutation: the lowering ignoring the catalog's permitted set (the refused principal's cell opens an editor).

TEST_CASE("model table: a column whose action is not permitted is read-only", "[ui][table][edit][interpreter]") {
    using namespace morph::ui::testing;
    TableHarness h;
    addEditActions(h, /*count*/ false);
    auto const screen = mountEdits(h, editDoc());
    int const table = h.byTestId("Table", "t");
    std::size_t const count = columnIndex(h, "count");
    h.backend.clickCell(table, 0, count, true);
    h.settle();
    std::optional<int> const first = h.backend.tableRow(table, Key{std::int64_t{1}});
    REQUIRE(first.has_value());
    CHECK(h.backend.prop(*first, "editor:" + std::to_string(count)).empty());
    h.backend.typeInto(table, "4");
    h.settle();
    CHECK(statusOf(h, h.byTestId("Table", "t")).message == "This cell is read-only");
    CHECK(h.dispatcher.count("Lims_EditSpecimenCount") == 0);
}

// Mutation: the loader accepting editMode on a `rows` table.

TEST_CASE("model table: editMode is refused on a table that is not a model's", "[ui][table][edit][interpreter]") {
    using namespace morph::ui::testing;
    TableHarness h;
    addEditActions(h);
    morph::ui::LoadOptions const load{.catalog = h.session.options().catalog};
    auto const rows = morph::ui::loadDocumentText(
        R"({"vocab":"ui/1","id":"rows","state":{"items":{"init":{"lit":[]}}},)"
        R"("view":{"kind":"table","rows":{"ref":"items"},"columns":[{"id":"a"}],"editMode":"row"}})",
        load);
    REQUIRE_FALSE(rows.has_value());
    CHECK(rows.error().reason.contains("editMode"));
    auto const sideways = morph::ui::loadDocumentText(editDoc(R"(,"editMode":"sideways")"), load);
    REQUIRE_FALSE(sideways.has_value());
    CHECK(sideways.error().reason.contains("sideways"));
    auto const screen = mountEdits(h, editDoc(R"(,"editMode":"row")"));
    int const table = h.byTestId("Table", "t");
    h.backend.clickCell(table, 0, columnIndex(h, "label"));
    h.settle();
    h.backend.typeInto(table, "a");
    h.settle();
    h.backend.press(table, "Tab");  // to (1, remark)
    h.settle();
    h.backend.typeInto(table, "b");
    h.settle();
    CHECK(h.dispatcher.count("Lims_EditSpecimenRow") == 0);  // focus is still in row 1
    h.backend.clickCell(table, 1, columnIndex(h, "label"));  // row 2
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_EditSpecimenRow") == 1);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_EditSpecimenRow")).body ==
          R"({"key":1,"values":{"label":"a","remarkText":"b"}})");
}
