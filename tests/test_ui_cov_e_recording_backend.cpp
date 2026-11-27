// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ui = morph::ui;
using morph::ui::testing::RecordingBackend;

namespace {

ui::Key intKey(std::int64_t value) { return ui::Key{value}; }

struct Setter {
    std::string name;
    std::function<void(ui::TableWidget&, ui::Widget&, ui::Widget&)> call;
};

// Every row-taking operation of a table, with the widget it is given as the row and, where it takes one, as the cell.
std::vector<Setter> rowSetters() {
    return {
        {"setRowKey", [](ui::TableWidget& table, ui::Widget& row, ui::Widget&) { table.setRowKey(row, intKey(1)); }},
        {"setRowHeading", [](ui::TableWidget& table, ui::Widget& row, ui::Widget&) { table.setRowHeading(row); }},
        {"setRowSpans", [](ui::TableWidget& table, ui::Widget& row, ui::Widget&) { table.setRowSpans(row, {}); }},
        {"setRowSummary", [](ui::TableWidget& table, ui::Widget& row, ui::Widget&) { table.setRowSummary(row); }},
        {"setRowTone",
         [](ui::TableWidget& table, ui::Widget& row, ui::Widget&) { table.setRowTone(row, std::nullopt); }},
        {"setRowMarks", [](ui::TableWidget& table, ui::Widget& row, ui::Widget&) { table.setRowMarks(row, {}); }},
        {"setRowLabel", [](ui::TableWidget& table, ui::Widget& row, ui::Widget&) { table.setRowLabel(row, "x"); }},
        {"setRowModified",
         [](ui::TableWidget& table, ui::Widget& row, ui::Widget&) { table.setRowModified(row, true); }},
        {"setRowIndex", [](ui::TableWidget& table, ui::Widget& row, ui::Widget&) { table.setRowIndex(row, 0); }},
        {"setCellLoad", [](ui::TableWidget& table, ui::Widget& row,
                           ui::Widget&) { table.setCellLoad(row, 0, ui::CellLoad::Failed); }},
        {"setCellAccent",
         [](ui::TableWidget& table, ui::Widget& row, ui::Widget&) { table.setCellAccent(row, 0, {}); }},
        {"setCellMarks", [](ui::TableWidget& table, ui::Widget& row, ui::Widget&) { table.setCellMarks(row, 0, {}); }},
        {"setCellEditor",
         [](ui::TableWidget& table, ui::Widget& row, ui::Widget&) { table.setCellEditor(row, 0, nullptr); }},
        {"setCellColumn",
         [](ui::TableWidget& table, ui::Widget& row, ui::Widget& cell) { table.setCellColumn(row, cell, 0); }},
    };
}

}  // namespace

// Mutation: drop the parent check of one setter in FakeTable (it then records for a row that is not the table's).
TEST_CASE("cove: a table refuses every row operation on a widget that is not its row", "[ui][cove]") {
    for (Setter const& setter : rowSetters()) {
        RecordingBackend backend;
        auto table = backend.createTable(nullptr);
        auto stray = backend.createText(nullptr);
        auto cell = backend.createText(nullptr);
        INFO(setter.name);
        try {
            setter.call(*table, *stray, *cell);
            FAIL("no error for a stray row");
        } catch (std::logic_error const& error) {
            CHECK(std::string{error.what()}.contains(setter.name));
        }
    }
}

// Mutation: drop the cell-parent check of `setCellColumn`, or the editor-parent check of `setCellEditor`.
TEST_CASE("cove: a table refuses a cell or an editor that is not its row's child", "[ui][cove]") {
    RecordingBackend backend;
    auto table = backend.createTable(nullptr);
    auto row = backend.createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, intKey(1));
    auto outsider = backend.createText(nullptr);
    CHECK_THROWS_WITH(table->setCellColumn(*row, *outsider, 0),
                      Catch::Matchers::ContainsSubstring("not a cell of this table"));
    CHECK_THROWS_WITH(table->setCellEditor(*row, 0, outsider.get()),
                      Catch::Matchers::ContainsSubstring("not the row's child"));
}

// Mutation: drop the parent check of `setHeader`.
TEST_CASE("cove: a panel refuses a header that is not its child", "[ui][cove]") {
    RecordingBackend backend;
    auto panel = backend.createPanel(nullptr);
    auto header = backend.createText(nullptr);
    CHECK_THROWS_WITH(panel->setHeader(*header), Catch::Matchers::ContainsSubstring("not this panel's child"));
}

// Mutation: make `cellLoad` or `cellAccent` answer `Loaded` for a key no row has.
TEST_CASE("cove: a cell's load and accent are read only for a key a row has", "[ui][cove]") {
    RecordingBackend backend;
    auto table = backend.createTable(nullptr);
    auto row = backend.createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, intKey(1));
    table->setCellLoad(*row, 0, ui::CellLoad::Unloaded);
    CHECK(backend.cellLoad(1, intKey(1), 0) == ui::CellLoad::Unloaded);
    CHECK(backend.cellLoad(1, intKey(1), 1) == ui::CellLoad::Loaded);
    CHECK(backend.cellAccent(1, intKey(1), 0).tooltip.empty());
    CHECK_THROWS_WITH(backend.cellLoad(1, intKey(9), 0), Catch::Matchers::ContainsSubstring("has no row for"));
    CHECK_THROWS_WITH(backend.cellAccent(1, intKey(9), 0), Catch::Matchers::ContainsSubstring("has no row for"));
}
