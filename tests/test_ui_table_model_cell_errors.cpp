// SPDX-License-Identifier: Apache-2.0

// A reply's field errors on a model-backed table's cells, and focus after an edit.
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

#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace table = morph::table;

using namespace table_model_support;

using morph::ui::testing::describeJson;
using namespace std::chrono_literals;
using morph::ui::testing::change;
using morph::ui::testing::TableHarness;

// Mutation: editErrorsOf reading no `errors` member (the refused cell shows no error).

TEST_CASE("model table: a reply's field errors land on the cells they name", "[ui][table][edit][interpreter]") {
    using namespace morph::ui::testing;
    TableHarness h;
    addEditActions(h);
    auto const screen = mountEdits(h, editDoc(R"(,"editMode":"row")"));
    typeCell(h, 0, "label", "a");
    typeCell(h, 0, "remark", "b");
    h.backend.clickCell(h.byTestId("Table", "t"), 1, columnIndex(h, "label"));  // focus leaves row 1
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_EditSpecimenRow") == 1);
    h.reply(h.dispatcher.last("Lims_EditSpecimenRow"), R"({"errors":{"values.remarkText":["too long"]}})");
    CHECK(cellMarks(h, 1, "remark").contains('e'));
    CHECK(!cellMarks(h, 1, "label").contains('e'));
}

// Mutation: sendRowEdit ignoring the reply's failed rows (the cell is marked modified instead).

TEST_CASE("model table: a failed row refuses its cells", "[ui][table][edit][interpreter]") {
    using namespace morph::ui::testing;
    TableHarness h;
    addEditActions(h);
    auto const screen = mountEdits(h, editDoc());
    int const table = h.byTestId("Table", "t");
    h.backend.clickCell(table, 0, columnIndex(h, "count"));
    h.settle();
    h.backend.typeInto(table, "4");
    h.settle();
    h.backend.press(table, "Enter");
    h.settle();
    h.reply(h.dispatcher.last("Lims_EditSpecimenCount"),
            R"({"rows":{"rows":[],"failed":[{"key":1,"message":"Locked by Ann"}]}})");
    CHECK(cellMarks(h, 1, "count") == "e");
}

// Mutation: a success whose rows do not name the row waiting for a fetch (the cell stays modified).

TEST_CASE("model table: an edit that changes no row leaves nothing waiting", "[ui][table][edit][interpreter]") {
    using namespace morph::ui::testing;
    TableHarness h;
    addEditActions(h);
    auto const screen = mountEdits(h, editDoc());
    int const table = h.byTestId("Table", "t");
    h.backend.clickCell(table, 0, columnIndex(h, "count"));
    h.settle();
    h.backend.typeInto(table, "1");  // the value it holds
    h.settle();
    h.backend.typeInto(table, "4");
    h.settle();
    h.backend.press(table, "Enter");
    h.settle();
    h.reply(h.dispatcher.last("Lims_EditSpecimenCount"), R"({"rows":{"rows":[]}})");
    CHECK(cellMarks(h, 1, "count").empty());  // with no page since
}

// Mutation: an unnamed refusal of one call landing on every cell of the request (the label, saved by its own call,
// shows the count's error).

TEST_CASE("model table: a refusal of one call of a row lands on that call's cells", "[ui][table][edit][interpreter]") {
    using namespace morph::ui::testing;
    TableHarness h;
    addEditActions(h);
    auto const screen = mountEdits(h, editDoc(R"(,"editMode":"row")"));
    typeCell(h, 0, "count", "4");
    typeCell(h, 0, "label", "a");
    h.backend.clickCell(h.byTestId("Table", "t"), 1, columnIndex(h, "label"));
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_EditSpecimenCount") == 1);
    REQUIRE(h.dispatcher.count("Lims_EditSpecimenRow") == 1);
    h.fail(h.dispatcher.last("Lims_EditSpecimenCount"));
    h.reply(h.dispatcher.last("Lims_EditSpecimenRow"), R"({"rows":{"rows":[{"key":1}]}})");
    CHECK(cellMarks(h, 1, "count").contains('e'));
    CHECK(!cellMarks(h, 1, "label").contains('e'));
}

namespace {

// Answers every open `Table.samples` call with row @p key at view row 0, as a keyed reply holding the columns each
// call names.

void answerKeyed(TableHarness& h, std::int64_t key) {
    for (int round = 0; round < 8 && morph::ui::testing::openCalls(h, "Table.samples") > 0; ++round) {
        std::size_t const call = morph::ui::testing::oldestOpen(h, "Table.samples");
        table::GenericPage page;
        page.revision = editDescription().revision;
        page.columns = morph::ui::testing::queryOf(h, call).columns.value_or(std::vector<std::string>{});
        page.total = 3;
        page.totalKind = table::TotalKind::Exact;
        page.keys.emplace_back(key);
        page.indices.push_back(0);
        CompanionRow const row = editRow(key);
        table::GenericRow cells;
        for (std::string const& column : page.columns) {
            auto const found = row.find(column);
            cells.push_back(found == row.end() ? table::Cell{} : found->second);
        }
        page.rows.push_back(std::move(cells));
        h.reply(call, glz::write_json(page).value_or(""));
    }
}

}  // namespace

// Mutation: node() binding the cell selection for every source (the editable model table's backend is given one).

TEST_CASE("model table: an editable model table keeps its row selection in the mount",
          "[ui][table][edit][interpreter]") {
    using namespace morph::ui::testing;
    TableHarness h;
    addEditActions(h);
    auto const screen = mountEdits(h, editDoc(R"(,"selectionMode":"multiple")"));
    int const table = h.byTestId("Table", "t");
    CHECK_FALSE(h.backend.hasProp(table, "cellSelection"));
    CHECK(!h.backend.prop(table, "keys").contains("Shift+Down"));
    CHECK(!h.backend.prop(table, "keys").contains("Ctrl+A"));
}

// Mutation: ModelGridSource::moveKey not telling the controller (focus stays on key 1, which the view no longer has:
// typing is refused as not loaded and nothing is sent).

TEST_CASE("model table: focus follows a key an edit changed, in the mount", "[ui][table][edit][interpreter]") {
    using namespace morph::ui::testing;
    TableHarness h;
    addEditActions(h);
    auto const screen = mountEdits(h, editDoc(R"(,"enterMoves":"none")"));
    int const table = h.byTestId("Table", "t");
    std::size_t const count = columnIndex(h, "count");
    h.backend.clickCell(table, 0, count);
    h.settle();
    h.backend.typeInto(table, "4");
    h.settle();
    h.backend.press(table, "Enter");
    h.settle();
    h.reply(h.dispatcher.last("Lims_EditSpecimenCount"), R"({"rows":{"rows":[{"key":9,"from":1}]}})");
    change(h, {1, 9});
    answerKeyed(h, 9);  // row 9 now where row 1 was
    CHECK(h.backend.focusCell(table) == std::pair{std::size_t{0}, count});
    h.backend.typeInto(table, "5");
    h.settle();
    h.backend.press(table, "Enter");
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_EditSpecimenCount") == 2);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_EditSpecimenCount")).body == R"({"key":9,"value":5})");
}

namespace {

// Answers the open `Table.samples` calls of the edit table that @p keyed selects (keyed fetches, or page fetches),
// each with the columns it names: a page's rows keyed `keyAt(viewRow)`, a keyed fetch's the rows @p found gives as
// key and view row.

void answerEdits(TableHarness& h, bool keyed, std::function<std::int64_t(std::int64_t)> const& keyAt,
                 std::vector<std::pair<std::int64_t, std::int64_t>> const& found, std::int64_t total) {
    for (int round = 0; round < 32; ++round) {
        std::optional<std::size_t> call;
        for (std::size_t index = 0; index < h.dispatcher.calls.size(); ++index) {
            auto const& made = h.dispatcher.calls[index];
            if (made.action == "Table.samples" && !made.answered && !made.options.stop.stop_requested() &&
                morph::ui::testing::queryOf(h, index).keys.has_value() == keyed) {
                call = index;
                break;
            }
        }
        if (!call) {
            return;
        }
        table::TableQuery const query = morph::ui::testing::queryOf(h, *call);
        table::GenericPage page;
        page.revision = editDescription().revision;
        page.columns = query.columns.value_or(std::vector<std::string>{});
        page.total = total;
        page.totalKind = table::TotalKind::Exact;
        std::vector<std::pair<std::int64_t, std::int64_t>> rows = found;
        if (!keyed) {
            page.offset = query.page.offset;
            rows.clear();
            for (std::int64_t at = query.page.offset; at < std::min(total, query.page.offset + query.page.limit);
                 ++at) {
                rows.emplace_back(keyAt(at), at);
            }
        }
        for (auto const& [key, viewRow] : rows) {
            page.keys.emplace_back(key);
            if (keyed) {
                page.indices.push_back(viewRow);
            }
            CompanionRow const row = editRow(key);
            table::GenericRow cells;
            for (std::string const& column : page.columns) {
                auto const cell = row.find(column);
                cells.push_back(cell == row.end() ? table::Cell{} : cell->second);
            }
            page.rows.push_back(std::move(cells));
        }
        h.reply(*call, glz::write_json(page).value_or(""));
    }
}

// Mounts the edit table over 9,093 rows keyed by view row + 1, thirty in view, and focuses (8, count) at view row 7.

int mountLargeAndFocus(TableHarness& h, std::unique_ptr<morph::ui::Screen>& screen) {
    screen = h.mount(editDoc());
    morph::ui::testing::replyDescribe(h, describeJson(editDescription()));
    int const table = h.byTestId("Table", "t");
    h.backend.scrollTable(table, 0, 30);
    h.settle();
    answerEdits(h, false, [](std::int64_t at) { return at + 1; }, {}, 9'093);
    h.backend.clickCell(table, 7, morph::ui::testing::columnIndex(h, "count"));
    h.settle();
    REQUIRE(h.backend.focusCell(table).first == 7);
    return table;
}

}  // namespace

// Mutation: the mount's focusLocated ignoring the located index (focus stays at view row 7, which another key now
// holds, and typing edits nothing there).

TEST_CASE("model table: a focused row a new order moves off the window is located, in the mount",
          "[ui][table][edit][interpreter]") {
    using namespace morph::ui::testing;
    TableHarness h;
    addEditActions(h);
    std::unique_ptr<morph::ui::Screen> screen;
    int const table = mountLargeAndFocus(h, screen);
    h.backend.clickHeader(table, 1);  // a new order: the window resets
    h.settle();
    auto const reordered = [](std::int64_t at) -> std::int64_t {
        if (at == 7) {
            return 10'000;
        }
        return at == 5'000 ? 8 : at + 1;
    };
    answerEdits(h, false, reordered, {}, 9'093);
    answerEdits(h, true, reordered, {{8, 5'000}}, 9'093);  // the mount's locate: row 8 is at 5,000
    CHECK(h.backend.focusCell(table).first == 5'000);
    h.advance(80ms);  // the window's settle after a scroll
    answerEdits(h, false, reordered, {}, 9'093);
    CHECK(h.backend.focusCell(table).first == 5'000);
    h.backend.typeInto(table, "3");
    h.settle();
    h.backend.press(table, "Enter");
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_EditSpecimenCount") == 1);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_EditSpecimenCount")).body == R"({"key":8,"value":3})");
}

// Mutation: the mount's reportFocus not handing a grid the row that took focus (the grid keeps the gone key: typing
// posts "This value is not loaded yet" and nothing is sent).

TEST_CASE("model table: when the focused row leaves the view, focus moves to the row now at its place",
          "[ui][table][edit][interpreter]") {
    using namespace morph::ui::testing;
    TableHarness h;
    addEditActions(h);
    std::unique_ptr<morph::ui::Screen> screen;
    int const table = mountLargeAndFocus(h, screen);
    h.backend.clickHeader(table, 1);
    h.settle();
    auto const without = [](std::int64_t at) { return at == 7 ? std::int64_t{10'000} : at + 1; };  // row 8 is gone
    answerEdits(h, false, without, {}, 9'093);
    answerEdits(h, true, without, {}, 9'093);  // the locate: the view lacks row 8
    CHECK(h.backend.focusCell(table).first == 7);
    h.backend.typeInto(table, "3");
    h.settle();
    h.backend.press(table, "Enter");
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_EditSpecimenCount") == 1);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_EditSpecimenCount")).body == R"({"key":10000,"value":3})");
}
