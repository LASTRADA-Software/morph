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

namespace {

// A table with two columns (the first sortable, the second filterable), a group over both, two data rows and every
// handler a driver can reach; `calls` counts how often any of them ran.
struct DrivenTable {
    RecordingBackend backend;
    std::unique_ptr<ui::TableWidget> table = backend.createTable(nullptr);
    std::unique_ptr<ui::ContainerWidget> rowA;
    std::unique_ptr<ui::ContainerWidget> rowB;
    int calls = 0;
    static constexpr int kTable = 1;

    DrivenTable() {
        table->setColumns({{.label = "A", .sortable = true}, {.label = "B", .filterable = true}});
        table->setColumnGroups({{.label = "G", .first = 0, .count = 2}});
        table->setSelectionMode(ui::SelectionMode::Multiple);
        rowA = backend.createStack(table.get(), ui::Axis::Horizontal);
        rowB = backend.createStack(table.get(), ui::Axis::Horizontal);
        table->setRowKey(*rowA, intKey(1));
        table->setRowKey(*rowB, intKey(2));
        table->setStatus({.entries = {{.id = "more", .label = "More"}}});
        table->setOnSort([this](std::size_t, ui::HeaderClick) { ++calls; });
        table->setOnColumnMove([this](std::size_t, std::size_t) { ++calls; });
        table->setOnColumnResize([this](std::size_t, std::optional<int>) { ++calls; });
        table->setOnFilter([this](std::size_t) { ++calls; });
        table->setOnGroupFilter([this](std::size_t) { ++calls; });
        table->setOnCopy([this] { ++calls; });
        table->setOnSelectAll([this] { ++calls; });
        table->setOnStatusEntry([this](const std::string&) { ++calls; });
        table->setOnRetry([this] { ++calls; });
        table->setOnText([this](const std::string&) { ++calls; });
        table->setOnCellClick([this](std::size_t, std::size_t, bool) { ++calls; });
        table->setOnSelectionChange([this](const std::vector<ui::Key>&, ui::SelectionChange) { ++calls; });
    }
};

struct Driver {
    std::string name;
    std::function<void(RecordingBackend&)> drive;
};

std::vector<Driver> tableDrivers() {
    constexpr int table = DrivenTable::kTable;
    return {
        {"clickHeader", [](RecordingBackend& b) { b.clickHeader(table, 0, ui::HeaderClick::Plain); }},
        {"moveColumn", [](RecordingBackend& b) { b.moveColumn(table, 0, 1); }},
        {"resizeColumn", [](RecordingBackend& b) { b.resizeColumn(table, 0, 12); }},
        {"clickFilter", [](RecordingBackend& b) { b.clickFilter(table, 1); }},
        {"clickGroupFilter", [](RecordingBackend& b) { b.clickGroupFilter(table, 0); }},
        {"pressCopy", [](RecordingBackend& b) { b.pressCopy(table); }},
        {"selectAll", [](RecordingBackend& b) { b.selectAll(table); }},
        {"chooseStatusEntry", [](RecordingBackend& b) { b.chooseStatusEntry(table, "more"); }},
        {"retryTable", [](RecordingBackend& b) { b.retryTable(table); }},
        {"typeInto", [](RecordingBackend& b) { b.typeInto(table, "x"); }},
        {"clickCell", [](RecordingBackend& b) { b.clickCell(table, 0, 0); }},
        {"selectRows", [](RecordingBackend& b) { b.selectRows(table, {intKey(1)}); }},
        {"clickRow", [](RecordingBackend& b) { b.clickRow(table, intKey(1), ui::RowClick::Only); }},
    };
}

}  // namespace

// Mutation: drop the reachability check of one driver (it then reaches a table a user cannot see).
TEST_CASE("cove: every table driver reaches a visible table and none reaches a hidden one", "[ui][cove]") {
    for (Driver const& driver : tableDrivers()) {
        INFO(driver.name);
        DrivenTable shown;
        driver.drive(shown.backend);
        CHECK(shown.calls == 1);
        DrivenTable hidden;
        hidden.table->setVisible(false);
        driver.drive(hidden.backend);
        CHECK(hidden.calls == 0);
    }
}

// Mutation: drop one of the checks that make a driver refuse what no user could do.
TEST_CASE("cove: a table driver refuses what no user could do", "[ui][cove]") {
    DrivenTable fixture;
    RecordingBackend& backend = fixture.backend;
    CHECK_THROWS_AS(backend.selectRows(1, {intKey(9)}), std::logic_error);
    CHECK_THROWS_AS(backend.selectRows(1, {intKey(1), intKey(1)}), std::logic_error);
    CHECK_THROWS_AS(backend.clickRow(1, intKey(9), ui::RowClick::Only), std::logic_error);
    CHECK_THROWS_AS(backend.activateRow(1, intKey(9)), std::logic_error);
    CHECK_THROWS_AS(backend.chooseStatusEntry(1, "absent"), std::logic_error);
    CHECK_THROWS_AS(backend.clickGroupFilter(1, 3), std::logic_error);
    CHECK_THROWS_AS(backend.clickHeader(1, 7), std::logic_error);
    fixture.table->setSelectionMode(ui::SelectionMode::None);
    CHECK_THROWS_AS(backend.selectRows(1, {intKey(1)}), std::logic_error);
    CHECK_THROWS_AS(backend.clickRow(1, intKey(1), ui::RowClick::Only), std::logic_error);
    fixture.table->setSelectionMode(ui::SelectionMode::Single);
    CHECK_THROWS_AS(backend.selectRows(1, {intKey(1), intKey(2)}), std::logic_error);
    CHECK(fixture.calls == 0);
}

// Mutation: make `clickHeader` report an unsortable column.
TEST_CASE("cove: a click on a header that is not sortable reports nothing", "[ui][cove]") {
    DrivenTable fixture;
    fixture.backend.clickHeader(1, 1, ui::HeaderClick::Plain);
    fixture.backend.clickFilter(1, 0);
    CHECK(fixture.calls == 0);
}

// Mutation: swap the two outcomes of one of the formatters, or drop its fallback.
TEST_CASE("cove: the recording backend names each setting's value, a value outside the enumeration by its fallback",
          "[ui][cove]") {
    RecordingBackend backend;
    auto stack = backend.createStack(nullptr, ui::Axis::Vertical);
    stack->setAlign(static_cast<ui::Align>(99));
    stack->setJustify(ui::Justify::Center);
    CHECK(backend.prop(1, "align") == "stretch");
    CHECK(backend.prop(1, "justify") == "center");
    stack->setJustify(static_cast<ui::Justify>(99));
    CHECK(backend.prop(1, "justify") == "start");
    stack->setWrap(false);
    stack->setDivided(false);
    stack->setPassesBleed(false);
    CHECK(backend.prop(1, "wrap") == "false");
    CHECK(backend.prop(1, "divided") == "false");
    CHECK(backend.prop(1, "passesBleed") == "false");
    stack->setWrap(true);
    CHECK(backend.prop(1, "wrap") == "true");

    auto button = backend.createButton(nullptr);
    button->setStyle(ui::ButtonStyle::Push);
    button->setEmphasis(static_cast<ui::ButtonEmphasis>(99));
    CHECK(backend.prop(2, "style") == "Push");
    CHECK(backend.prop(2, "emphasis") == "Normal");

    auto text = backend.createText(nullptr);
    text->setRole(ui::TextRole::Small);
    CHECK(backend.prop(3, "role") == "Small");
    text->setSpans({{.text = "go", .action = true}});
    CHECK(backend.prop(3, "spans").contains(";action"));

    auto field = backend.createTextInput(nullptr, ui::TextInputMode::SingleLine);
    field->setAlign(ui::Align::Center);
    field->setEmphasis(ui::InputEmphasis::Prominent);
    CHECK(backend.prop(4, "align") == "center");
    CHECK(backend.prop(4, "emphasis") == "Prominent");
    field->setAlign(ui::Align::End);
    CHECK(backend.prop(4, "align") == "end");

    auto badge = backend.createBadge(nullptr);
    badge->setStyle(ui::BadgeStyle::Quiet);
    CHECK(backend.prop(5, "style") == "Quiet");

    auto steps = backend.createSteps(nullptr);
    steps->setLabelled(false);
    CHECK(backend.prop(6, "labelled") == "false");

    auto pairs = backend.createKeyValue(nullptr);
    pairs->setCaptionPlacement(ui::CaptionPlacement::Above);
    CHECK(backend.prop(7, "captions") == "Above");

    auto slot = backend.createSlot(nullptr);
    slot->setScrollable(false);
    CHECK(backend.prop(8, "scrollable") == "false");
}

// Mutation: make `invoke` call an empty handler, or one driver throw for a table that has no handler of its kind.
TEST_CASE("cove: a table with no handlers takes every driver without reporting anything", "[ui][cove]") {
    RecordingBackend backend;
    auto table = backend.createTable(nullptr);
    table->setColumns({{.label = "A", .sortable = true}, {.label = "B", .filterable = true}});
    table->setColumnGroups({{.label = "G", .first = 0, .count = 2}});
    table->setSelectionMode(ui::SelectionMode::Multiple);
    auto row = backend.createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, intKey(1));
    CHECK_NOTHROW(backend.clickHeader(1, 0, ui::HeaderClick::Shift));
    CHECK_NOTHROW(backend.moveColumn(1, 0, 1));
    CHECK_NOTHROW(backend.resizeColumn(1, 0, std::nullopt));
    CHECK_NOTHROW(backend.clickFilter(1, 1));
    CHECK_NOTHROW(backend.clickGroupFilter(1, 0));
    CHECK_NOTHROW(backend.pressCopy(1));
    CHECK_NOTHROW(backend.retryTable(1));
    CHECK_NOTHROW(backend.typeInto(1, "x"));
    CHECK_NOTHROW(backend.clickCell(1, 0, 0, true));
    CHECK_NOTHROW(backend.selectRows(1, {intKey(1)}));
    CHECK_NOTHROW(backend.clickRow(1, intKey(1), ui::RowClick::Toggle));
    CHECK_NOTHROW(backend.activateRow(1, intKey(1)));
    CHECK_THROWS_AS(backend.selectAll(1), std::logic_error);
    CHECK_THROWS_AS(backend.chooseStatusEntry(1, "x"), std::logic_error);
    CHECK_FALSE(backend.press(1, "Ctrl+C"));
}

// Mutation: let Ctrl+A select in a table that is not `Multiple`, or let Enter activate a row nothing marks.
TEST_CASE("cove: a focused table takes Ctrl+A only when it selects many, and Enter only for a marked row",
          "[ui][cove]") {
    DrivenTable fixture;
    RecordingBackend& backend = fixture.backend;
    fixture.table->setOnActivate([&fixture](const ui::Key&) { fixture.calls += 100; });
    CHECK(backend.press(1, "Enter"));
    CHECK(fixture.calls == 0);
    backend.selectRows(1, {intKey(2)});
    CHECK(fixture.calls == 1);
    CHECK(backend.press(1, "Enter"));
    CHECK(fixture.calls == 101);
    CHECK(backend.press(1, "Ctrl+A"));
    CHECK(fixture.calls == 102);
    CHECK(backend.press(1, "Ctrl+C"));
    CHECK(fixture.calls == 103);
    fixture.table->setSelectionMode(ui::SelectionMode::Single);
    CHECK_FALSE(backend.press(1, "Ctrl+A"));
    CHECK(fixture.calls == 103);
    CHECK_FALSE(backend.press(1, "F9"));
}

namespace {

// A table whose cell menu has an enabled entry with a chord, a disabled one, a separator and a submenu.
struct CellMenuTable {
    RecordingBackend backend;
    std::unique_ptr<ui::TableWidget> table = backend.createTable(nullptr);
    std::unique_ptr<ui::ContainerWidget> row;
    std::vector<std::string> chosen;

    CellMenuTable() {
        table->setColumns({{.label = "A"}});
        table->setSelectionMode(ui::SelectionMode::Multiple);
        row = backend.createStack(table.get(), ui::Axis::Horizontal);
        table->setRowKey(*row, intKey(1));
        table->setRowIndex(*row, 0);
        table->setCellMenuKeys({"Ctrl+K", "Ctrl+Q"});
        table->setCellMenu(
            [](ui::Key const&, std::size_t) {
                return std::vector<ui::MenuEntry>{
                    {.label = "Off", .keys = "Ctrl+K", .enabled = false},
                    {.separator = true},
                    {.label = "More",
                     .items = {{.label = "Sub", .keys = "Ctrl+K"}, {.label = "Locked", .enabled = false}}},
                    {.label = "Plain", .keys = "Ctrl+Z"}};
            },
            [this](ui::Key const&, std::size_t, std::vector<std::size_t> const& path) {
                std::string text;
                for (std::size_t const index : path) {
                    text += std::to_string(index);
                }
                chosen.push_back(text);
            });
    }
};

}  // namespace

// Mutation: run a disabled entry's chord, or an entry of a submenu before its parent's turn.
TEST_CASE("cove: a cell menu chord runs the first enabled entry that declares it, a submenu's after its parent",
          "[ui][cove]") {
    CellMenuTable fixture;
    CHECK_FALSE(fixture.backend.press(1, "Ctrl+K"));  // no focused cell yet
    fixture.table->setFocusCell(0, 0);
    CHECK(fixture.backend.press(1, "Ctrl+K"));
    CHECK(fixture.chosen == std::vector<std::string>{"20"});
    CHECK_FALSE(fixture.backend.press(1, "Ctrl+Q"));  // a chord of the table's keys no entry declares
    CHECK_FALSE(fixture.backend.press(1, "Ctrl+Z"));  // a chord no table key asks for
}

// Mutation: report a chosen entry that is disabled, inside a disabled one, or opens a submenu.
TEST_CASE("cove: a cell menu reports only an enabled entry without a submenu, and refuses a label it does not have",
          "[ui][cove]") {
    CellMenuTable fixture;
    auto const key = intKey(1);
    fixture.backend.chooseCellMenu(1, key, 0, {"Plain"});
    fixture.backend.chooseCellMenu(1, key, 0, {"More", "Sub"});
    CHECK(fixture.chosen == std::vector<std::string>{"3", "20"});
    fixture.backend.chooseCellMenu(1, key, 0, {"Off"});
    fixture.backend.chooseCellMenu(1, key, 0, {"More"});
    fixture.backend.chooseCellMenu(1, key, 0, {"More", "Locked"});
    CHECK(fixture.chosen.size() == 2);
    CHECK_THROWS_AS(fixture.backend.chooseCellMenu(1, key, 0, {"Absent"}), std::logic_error);
    CHECK_THROWS_AS(fixture.backend.rightClickCell(1, intKey(9), 0), std::logic_error);
    fixture.table->setVisible(false);
    CHECK(fixture.backend.rightClickCell(1, key, 0).empty());
    fixture.backend.chooseCellMenu(1, key, 0, {"Plain"});
    CHECK(fixture.chosen.size() == 2);
}

// Mutation: let a menu choose a separator, an entry past the end, or an entry under a disabled one.
TEST_CASE("cove: a menu runs only an enabled entry without a submenu", "[ui][cove]") {
    RecordingBackend backend;
    auto menu = backend.createMenu(nullptr, ui::MenuStyle::Button);
    std::vector<std::vector<std::size_t>> paths;
    menu->setItems({{.label = "Go"},
                    {.separator = true},
                    {.label = "Off", .enabled = false},
                    {.label = "More", .items = {{.label = "Deep"}}}});
    menu->setOnActivate([&paths](std::vector<std::size_t> path) { paths.push_back(std::move(path)); });
    backend.chooseMenuEntry(1, {0});
    backend.chooseMenuEntry(1, {1});
    backend.chooseMenuEntry(1, {2});
    backend.chooseMenuEntry(1, {3});
    backend.chooseMenuEntry(1, {9});
    backend.chooseMenuEntry(1, {3, 0});
    backend.chooseMenuEntry(1, {3, 4});
    backend.chooseMenuEntry(1, {0, 0});
    backend.chooseMenuEntry(1, {});
    CHECK(paths == std::vector<std::vector<std::size_t>>{{0}, {3, 0}});
    menu->setVisible(false);
    backend.chooseMenuEntry(1, {0});
    CHECK(paths.size() == 2);
}

// Mutation: let Left or Right step a segmented select that marks nothing, marks a key it does not offer, or sits at
// the end the step leads past.
TEST_CASE("cove: a segmented select steps only from an offered option and stops at its ends", "[ui][cove]") {
    RecordingBackend backend;
    auto select = backend.createSelect(nullptr, ui::SelectStyle::Segmented);
    select->setOptions({{.key = intKey(1), .label = "one"}, {.key = intKey(2), .label = "two"}});
    int picked = 0;
    select->setOnSelect([&picked](ui::Key const&) { ++picked; });
    CHECK(backend.press(1, "Right"));  // nothing marked: no neighbour
    CHECK(picked == 0);
    select->setSelected(intKey(9));  // a key the options do not offer
    CHECK(backend.press(1, "Right"));
    CHECK(picked == 0);
    select->setSelected(intKey(1));
    CHECK(backend.press(1, "Left"));  // at the start
    CHECK(picked == 0);
    CHECK(backend.press(1, "Right"));
    CHECK(picked == 1);
    CHECK(backend.prop(1, "selected") == "2");
    CHECK(backend.press(1, "Right"));  // at the end
    CHECK(picked == 1);
}

// Mutation: drop the collapsed-panel or the closed-collapsible check of `shownInStore`, or let a closed section's
// header count as content.
TEST_CASE(
    "cove: a widget under a collapsed panel or a closed section takes no input unless it is the section's header",
    "[ui][cove]") {
    RecordingBackend backend;
    int clicks = 0;
    auto panel = backend.createPanel(nullptr);
    auto inPanel = backend.createButton(panel.get());
    inPanel->setOnClick([&clicks] { ++clicks; });
    panel->setCollapsed(true);
    backend.click(2);
    CHECK(clicks == 0);
    panel->setCollapsed(false);
    backend.click(2);
    CHECK(clicks == 1);

    auto section = backend.createCollapsible(nullptr);
    auto header = backend.createButton(section.get());
    auto content = backend.createButton(section.get());
    header->setOnClick([&clicks] { clicks += 10; });
    content->setOnClick([&clicks] { clicks += 100; });
    section->setHeader(*header);
    section->setOpen(false);
    backend.click(4);
    backend.click(5);
    CHECK(clicks == 11);
    section->setOpen(true);
    backend.click(5);
    CHECK(clicks == 111);

    auto bare = backend.createCollapsible(nullptr);
    auto child = backend.createButton(bare.get());
    child->setOnClick([&clicks] { clicks += 1000; });
    bare->setOpen(false);
    backend.click(7);
    CHECK(clicks == 111);
}

// Mutation: tell the table of focus that moves inside it, or not of focus that leaves it.
TEST_CASE("cove: a table is told when focus leaves it and everything in it, and not when focus moves inside",
          "[ui][cove]") {
    RecordingBackend backend;
    auto stack = backend.createStack(nullptr, ui::Axis::Vertical);
    auto table = backend.createTable(stack.get());
    auto row = backend.createStack(table.get(), ui::Axis::Horizontal);
    auto cellA = backend.createText(row.get());
    auto cellB = backend.createText(row.get());
    auto outside = backend.createText(stack.get());
    int left = 0;
    table->setOnFocusOut([&left] { ++left; });
    cellA->focus();
    cellB->focus();
    CHECK(left == 0);
    outside->focus();
    CHECK(left == 1);
    outside->focus();
    CHECK(left == 1);
}

// Mutation: let a page region be made in a container that is not a page.
TEST_CASE("cove: a page region belongs to a page, and a cell load outside the enumeration reads as loaded",
          "[ui][cove]") {
    RecordingBackend backend;
    auto stack = backend.createStack(nullptr, ui::Axis::Vertical);
    CHECK_THROWS_AS(backend.createPageRegion(stack.get(), ui::RegionGeometry{}), std::logic_error);

    auto table = backend.createTable(nullptr);
    auto row = backend.createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, intKey(1));
    table->setCellLoad(*row, 0, static_cast<ui::CellLoad>(9));
    CHECK(backend.prop(3, "load0") == "Loaded");
}

// Mutation: let a heading row be selected, clicked or activated, or a row with no recorded tone or marks answer one.
TEST_CASE("cove: a heading row takes no selection and a row with no recorded look answers none", "[ui][cove]") {
    DrivenTable fixture;
    RecordingBackend& backend = fixture.backend;
    fixture.table->setRowHeading(*fixture.rowA);
    CHECK_THROWS_AS(backend.selectRows(1, {intKey(1)}), std::logic_error);
    CHECK_THROWS_AS(backend.activateRow(1, intKey(1)), std::logic_error);
    backend.clickRow(1, intKey(1), ui::RowClick::Only);
    CHECK(fixture.calls == 0);
    CHECK(backend.rowTone(1, intKey(2)).empty());
    CHECK_FALSE(backend.rowMarks(1, intKey(2)).has_value());
    fixture.table->setRowTone(*fixture.rowB, ui::Tone::Warn);
    fixture.table->setRowMarks(*fixture.rowB, {.busy = true});
    CHECK(backend.rowTone(1, intKey(2)) == "Warn");
    CHECK(backend.rowMarks(1, intKey(2))->busy);
}

// Mutation: report a status entry that has no handler, or take a standing entry for no entry.
TEST_CASE("cove: a status entry is chosen when the table shows it and has a handler for it", "[ui][cove]") {
    RecordingBackend backend;
    auto table = backend.createTable(nullptr);
    table->setStatus(
        {.entries = {{.id = "more", .label = "More"}}, .standingEntries = {{.id = "stay", .label = "Stay"}}});
    CHECK_THROWS_AS(backend.chooseStatusEntry(1, "stay"), std::logic_error);  // no handler yet
    std::vector<std::string> chosen;
    table->setOnStatusEntry([&chosen](std::string entry) { chosen.push_back(std::move(entry)); });
    backend.chooseStatusEntry(1, "stay");
    backend.chooseStatusEntry(1, "more");
    CHECK(chosen == std::vector<std::string>{"stay", "more"});
}

// Mutation: let a cell menu or a header menu choose with no label, or a heading by its label.
TEST_CASE("cove: a menu chosen with no labels, or by a disabled or heading entry's label, reports nothing",
          "[ui][cove]") {
    RecordingBackend backend;
    auto table = backend.createTable(nullptr);
    table->setColumns({{.label = "A"}});
    auto row = backend.createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, intKey(1));
    int chosen = 0;
    table->setMenu(
        [](std::optional<std::size_t>) {
            return std::vector<ui::MenuEntry>{{.label = "Off", .enabled = false}, {.label = "On"}};
        },
        [&chosen](std::optional<std::size_t>, const std::vector<std::size_t>&) { ++chosen; });
    table->setCellMenu(
        [](ui::Key const&, std::size_t) {
            return std::vector<ui::MenuEntry>{{.label = "Title", .heading = true}, {.label = "Off", .enabled = false}};
        },
        [&chosen](ui::Key const&, std::size_t, std::vector<std::size_t> const&) { ++chosen; });
    backend.chooseMenu(1, 0, {});
    backend.chooseMenu(1, 0, {"Off"});
    backend.chooseCellMenu(1, intKey(1), 0, {});
    backend.chooseCellMenu(1, intKey(1), 0, {"Off"});
    CHECK(chosen == 0);
    CHECK_THROWS_AS(backend.chooseCellMenu(1, intKey(1), 0, {"Title"}), std::logic_error);
    backend.chooseMenu(1, 0, {"On"});
    CHECK(chosen == 1);
}

// Mutation: ask a table with no cell menu for entries, or report a cell menu chord with no chosen handler.
TEST_CASE("cove: a table with no cell menu, or no handler for its chosen entry, answers nothing", "[ui][cove]") {
    RecordingBackend backend;
    auto table = backend.createTable(nullptr);
    table->setColumns({{.label = "A"}});
    table->setSelectionMode(ui::SelectionMode::Multiple);
    auto row = backend.createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, intKey(1));
    table->setRowIndex(*row, 0);
    CHECK(backend.rightClickCell(1, intKey(1), 0).empty());
    CHECK(backend.rightClickCell(1, intKey(1), 0).empty());  // the row is the selection already
    CHECK(backend.cellMenuAsks(1) == 0);
    table->setCellMenuKeys({"Ctrl+K"});
    table->setCellMenu(
        [](ui::Key const&, std::size_t) { return std::vector<ui::MenuEntry>{{.label = "Go", .keys = "Ctrl+K"}}; }, {});
    table->setFocusCell(0, 0);
    CHECK_FALSE(backend.press(1, "Ctrl+K"));
    row->setVisible(false);
    CHECK(backend.rightClickCell(1, intKey(1), 0).empty());
}

// Mutation: report a move to the column's own place or past the last column, or a paste from a missing clipboard.
TEST_CASE("cove: a column move to its own place or past the end reports nothing, and a missing clipboard pastes none",
          "[ui][cove]") {
    DrivenTable fixture;
    fixture.backend.moveColumn(1, 0, 0);
    fixture.backend.moveColumn(1, 0, 5);
    CHECK(fixture.calls == 0);
    fixture.backend.moveColumn(1, 0, 1);
    CHECK(fixture.calls == 1);
    CHECK(fixture.backend.pasteText().value_or("none").empty());
    fixture.backend.setClipboard(false);
    CHECK_FALSE(fixture.backend.pasteText().has_value());
    CHECK_FALSE(fixture.backend.copyText("x"));
}

// Mutation: let Left or Right step a select that is not segmented, or a hidden slider be dragged.
TEST_CASE("cove: arrows step only a segmented select, and a hidden slider is not dragged", "[ui][cove]") {
    RecordingBackend backend;
    auto select = backend.createSelect(nullptr, ui::SelectStyle::Dropdown);
    CHECK_FALSE(backend.press(1, "Left"));
    auto slider = backend.createSlider(nullptr);
    int moved = 0;
    slider->setOnChange([&moved](std::int64_t) { ++moved; });
    slider->setRange(0, 10, 1);
    backend.dragSlider(2, 4);
    CHECK(moved == 1);
    slider->setVisible(false);
    backend.dragSlider(2, 5);
    CHECK(moved == 1);
}

// Mutation: ignore a handler a bare table has not been given when its drivers run.
TEST_CASE("cove: a table with no scroll, measure, focus or range handler takes those drivers without error",
          "[ui][cove]") {
    RecordingBackend backend;
    auto table = backend.createTable(nullptr);
    table->setColumns({{.label = "A"}, {.label = "B"}});
    auto row = backend.createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, intKey(1));
    CHECK_NOTHROW(backend.scrollTableColumns(1, 10, 80));
    CHECK_NOTHROW(backend.focusMove(1, ui::FocusMove::Right));
    CHECK_NOTHROW(backend.selectCells(1, 0, 0, 0, 1));
    CHECK_NOTHROW(backend.measureColumn(1, 0, 12));
    CHECK_NOTHROW(backend.scrollTable(1, 0, 1));
    table->setVisible(false);
    CHECK_NOTHROW(backend.selectCells(1, 0, 0, 0, 1));
    CHECK_NOTHROW(backend.typeInto(1, "x"));
}
