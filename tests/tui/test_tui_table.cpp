// SPDX-License-Identifier: Apache-2.0

#include <libunicode/convert.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/tui/backend.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "grid_test_support.hpp"
#include "tui/container_widgets.hpp"
#include "tui/display_widgets.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui/table_widget.hpp"
#include "tui_harness.hpp"
#include "ui_fake_rows.hpp"

namespace ui = morph::ui;
using core::tui::EventResult;
using core::tui::KeyCode;
using morph::tui::detail::BadgeImpl;
using morph::tui::detail::ButtonImpl;
using morph::tui::detail::ScrollImpl;
using morph::tui::detail::StackImpl;
using morph::tui::detail::TableImpl;
using morph::tui::detail::TextImpl;
using morph::tui::testing::Harness;
using Rows = std::vector<std::string>;
using Positions = std::vector<std::size_t>;

namespace {

ui::Key key(std::int64_t value) { return ui::Key{value}; }

/// A table of fruit: Name (content width) and Qty (fixed 5), rows keyed 1 and 2.
struct Fruit {
    explicit Fruit(Harness& harness, ui::SelectionMode mode, ui::ContainerWidget* parent = nullptr)
        : table{harness.make<TableImpl>(parent)} {
        table->setColumns(
            {{.label = "Name", .width = ui::Sizing::content()}, {.label = "Qty", .width = ui::Sizing::fixed(5)}});
        table->setSelectionMode(mode);
        addRow(harness, key(1), "apple", "3");
        addRow(harness, key(2), "kiwi", "12");
    }
    /// @brief Destroys the rows as the mount does: each row's cells before the row.
    ~Fruit() {
        cells.clear();
        rows.clear();
    }
    Fruit(Fruit const&) = delete;
    Fruit& operator=(Fruit const&) = delete;
    Fruit(Fruit&&) = delete;
    Fruit& operator=(Fruit&&) = delete;

    void addRow(Harness& harness, ui::Key const& rowKey, std::string const& name, std::string const& quantity) {
        // As the mount builds a row: a horizontal stack, its cells, then the row's key.
        rows.push_back(harness.make<StackImpl>(table.get(), ui::Axis::Horizontal));
        for (auto const& text : {name, quantity}) {
            cells.push_back(harness.make<TextImpl>(rows.back().get()));
            cells.back()->setText(text);
        }
        table->setRowKey(*rows.back(), rowKey);
    }

    /// Destroys the row at @p index of `rows` as the mount does: its cells first.
    void removeRow(std::size_t index) {
        auto const* const row = rows.at(index).get();
        std::erase_if(cells, [row](std::unique_ptr<TextImpl> const& cell) { return cell->container() == row; });
        rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(index));
    }

    std::unique_ptr<TableImpl> table;
    std::vector<std::unique_ptr<StackImpl>> rows;
    std::vector<std::unique_ptr<TextImpl>> cells;
};

/// A mouse event of @p type at the 0-based viewport @p cell.
core::tui::MouseEvent mouse(core::tui::MouseEvent::Type type, core::tui::Point cell) {
    return core::tui::MouseEvent{.type = type, .button = 0, .x = cell.x + 1, .y = cell.y + 1};
}

}  // namespace

TEST_CASE("tui table: a header row above keyed rows, in solved column widths", "[tui][table]") {
    Harness harness{30, 4};
    Fruit const fruit{harness, ui::SelectionMode::None};
    CHECK(harness.draw() == Rows{"  Name  Qty", "  apple 3", "  kiwi  12"});
}

TEST_CASE("tui table: Space toggles rows in Multiple mode, Enter activates", "[tui][table]") {
    Harness harness{30, 4};
    Fruit fruit{harness, ui::SelectionMode::Multiple};
    std::vector<std::vector<ui::Key>> selections;
    std::vector<ui::Key> activated;
    fruit.table->setOnSelectionChange(
        [&](std::vector<ui::Key> keys, ui::SelectionChange /*change*/) { selections.push_back(std::move(keys)); });
    fruit.table->setOnActivate([&](ui::Key activatedKey) { activated.push_back(std::move(activatedKey)); });
    harness.focus(*fruit.table);
    CHECK(harness.draw().at(1) == " >apple 3");
    static_cast<void>(harness.type(" "));
    CHECK(selections.back() == std::vector<ui::Key>{key(1)});
    CHECK(harness.draw().at(1) == "*>apple 3");
    static_cast<void>(harness.key(KeyCode::Down));
    static_cast<void>(harness.type(" "));
    CHECK(selections.back() == std::vector<ui::Key>{key(1), key(2)});
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(activated == std::vector<ui::Key>{key(2)});
    CHECK(selections.size() == 2);
}

TEST_CASE("tui table: Single mode selects one row; setSelection marks rows without reporting", "[tui][table]") {
    Harness harness{30, 4};
    Fruit fruit{harness, ui::SelectionMode::Single};
    std::vector<std::vector<ui::Key>> selections;
    fruit.table->setOnSelectionChange(
        [&](std::vector<ui::Key> keys, ui::SelectionChange /*change*/) { selections.push_back(std::move(keys)); });
    fruit.table->setSelection({key(2)});
    CHECK(harness.draw() == Rows{"  Name  Qty", "  apple 3", "* kiwi  12"});
    CHECK(selections.empty());
    harness.focus(*fruit.table);
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(selections.back() == std::vector<ui::Key>{key(1)});
}

TEST_CASE("tui table: moving a row moves its key with it", "[tui][table]") {
    Harness harness{30, 4};
    Fruit fruit{harness, ui::SelectionMode::Single};
    std::vector<ui::Key> activated;
    fruit.table->setOnActivate([&](ui::Key activatedKey) { activated.push_back(std::move(activatedKey)); });
    fruit.table->moveChild(*fruit.rows.at(1), 0);
    CHECK(harness.draw() == Rows{"  Name  Qty", "  kiwi  12", "  apple 3"});
    harness.focus(*fruit.table);
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(activated == std::vector<ui::Key>{key(2)});
}

TEST_CASE("tui table: a hidden cell keeps its column, so the cells after it stay under their own headers",
          "[tui][table][columns]") {
    Harness harness{30, 3};
    auto const table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "Note"}, {.label = "Name"}});
    auto const row = harness.make<StackImpl>(table.get(), ui::Axis::Horizontal);
    auto const note = harness.make<TextImpl>(row.get());
    note->setText("x");
    note->setVisible(false);
    auto const name = harness.make<TextImpl>(row.get());
    name->setText("wide-name");
    table->setRowKey(*row, key(1));
    CHECK(harness.draw() == Rows{"  Note Name", "       wide-name"});
}

// Mutation: leave the `shown()` test out of `TableImpl::naturalWidths` (the hidden cell's 11 cells hold the column
// open).
TEST_CASE("tui table: a hidden cell widens no column, and the column goes back to its header",
          "[tui][table][columns]") {
    Harness harness{30, 3};
    auto const table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "Note"}, {.label = "Name"}});
    auto const row = harness.make<StackImpl>(table.get(), ui::Axis::Horizontal);
    auto const note = harness.make<TextImpl>(row.get());
    note->setText("secret-note");
    auto const name = harness.make<TextImpl>(row.get());
    name->setText("a");
    table->setRowKey(*row, key(1));
    Rows const shown{"  Note        Name", "  secret-note a"};
    CHECK(harness.draw() == shown);
    note->setVisible(false);
    CHECK(harness.draw() == Rows{"  Note Name", "       a"});
}

TEST_CASE("tui table: a label wider than its column is cut at the column's edge", "[tui][table][columns]") {
    Harness harness{30, 3};
    auto const table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "Quantity", .width = ui::Sizing::fixed(3)}, {.label = "Name"}});
    CHECK(harness.draw() == Rows{"  Qua Name"});
}

// Mutation: leave the table's `setElided` out of `arrangeRows` (both cells are cut at the column's edge with no
// ellipsis).
TEST_CASE("tui table: a text cell a column cuts ends in an ellipsis, whether the column wraps or not",
          "[tui][table][columns]") {
    Harness harness{30, 3};
    auto const table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "Assay", .width = ui::Sizing::fixed(6), .wrap = false},
                       {.label = "Note", .width = ui::Sizing::fixed(6)}});
    auto const row = harness.make<StackImpl>(table.get(), ui::Axis::Horizontal);
    auto const assay = harness.make<TextImpl>(row.get());
    assay->setText("99.4 % LC");
    auto const note = harness.make<TextImpl>(row.get());
    note->setText("99.4 % LC");
    table->setRowKey(*row, key(1));
    CHECK(harness.draw() == Rows{"  Assay  Note", "  99.4 … 99.4 …"});
}

// Mutation: leave `_elided` unread for a text with spans (the runs draw to the column's edge with no ellipsis, and the
// run after the cut one is not dropped).
TEST_CASE("tui table: spans in a column that does not wrap share one row and end in an ellipsis where it is cut",
          "[tui][table][columns]") {
    Harness harness{30, 3};
    auto const table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "Assay", .width = ui::Sizing::fixed(6), .wrap = false},
                       {.label = "Note", .width = ui::Sizing::fixed(6)}});
    auto const row = harness.make<StackImpl>(table.get(), ui::Axis::Horizontal);
    auto const assay = harness.make<TextImpl>(row.get());
    assay->setSpans({{.text = "99.4"}, {.text = " % LC", .strong = true}});
    auto const note = harness.make<TextImpl>(row.get());
    note->setText("ok");
    table->setRowKey(*row, key(1));
    CHECK(harness.draw() == Rows{"  Assay  Note", "  99.4 … ok"});
}

TEST_CASE("tui table: a Multiple-mode user change reports exactly the existing rows selected",
          "[tui][table][selection]") {
    Harness harness{30, 6};
    Fruit fruit{harness, ui::SelectionMode::Multiple};
    std::vector<std::vector<ui::Key>> selections;
    fruit.table->setOnSelectionChange(
        [&](std::vector<ui::Key> keys, ui::SelectionChange /*change*/) { selections.push_back(std::move(keys)); });
    fruit.table->setSelection({key(2), key(9)});
    CHECK(fruit.table->markedRows() == Positions{1});
    harness.focus(*fruit.table);
    static_cast<void>(harness.type(" "));
    REQUIRE(selections.size() == 1);
    CHECK(selections.back() == std::vector<ui::Key>{key(2), key(1)});
    CHECK(fruit.table->markedRows() == Positions{0, 1});
    // The user's change dropped the waiting key 9: its row arriving now marks nothing.
    fruit.addRow(harness, key(9), "fig", "7");
    CHECK(fruit.table->markedRows() == Positions{0, 1});
    // A key requested by setSelection keeps waiting for its row.
    fruit.table->setSelection({key(5)});
    CHECK(fruit.table->markedRows().empty());
    fruit.addRow(harness, key(5), "plum", "1");
    CHECK(fruit.table->markedRows() == Positions{3});
    CHECK(harness.draw() == Rows{"  Name  Qty", " >apple 3", "  kiwi  12", "  fig   7", "* plum  1"});
    CHECK(selections.size() == 1);
}

TEST_CASE("tui table: a removed row is unmarked and marked again when it returns, and no setter reports",
          "[tui][table][selection]") {
    Harness harness{30, 5};
    Fruit fruit{harness, ui::SelectionMode::Single};
    int reports = 0;
    int activations = 0;
    fruit.table->setOnSelectionChange(
        [&](std::vector<ui::Key> const& /*keys*/, ui::SelectionChange /*change*/) { ++reports; });
    fruit.table->setOnActivate([&](ui::Key const& /*key*/) { ++activations; });
    fruit.table->setSelection({key(2)});
    CHECK(fruit.table->markedRows() == Positions{1});
    fruit.removeRow(1);
    CHECK(fruit.table->markedRows().empty());
    CHECK(harness.draw() == Rows{"  Name  Qty", "  apple 3"});
    fruit.addRow(harness, key(2), "kiwi", "12");
    CHECK(fruit.table->markedRows() == Positions{1});
    fruit.table->moveChild(*fruit.rows.at(1), 0);
    CHECK(fruit.table->markedRows() == Positions{0});
    fruit.table->setColumns({{.label = "Name"}});
    fruit.table->setSelectionMode(ui::SelectionMode::Multiple);
    fruit.table->setSelection({key(1), key(2)});
    CHECK(fruit.table->markedRows() == Positions{0, 1});
    CHECK(reports == 0);
    CHECK(activations == 0);
}

TEST_CASE("tui table: the cursor and the marks stay with a moved row", "[tui][table][selection]") {
    Harness harness{30, 4};
    Fruit fruit{harness, ui::SelectionMode::Multiple};
    std::vector<ui::Key> activated;
    fruit.table->setOnActivate([&](ui::Key activatedKey) { activated.push_back(std::move(activatedKey)); });
    harness.focus(*fruit.table);
    static_cast<void>(harness.key(KeyCode::Down));
    static_cast<void>(harness.type(" "));
    CHECK(harness.draw() == Rows{"  Name  Qty", "  apple 3", "*>kiwi  12"});
    fruit.table->moveChild(*fruit.rows.at(1), 0);
    CHECK(fruit.table->markedRows() == Positions{0});
    CHECK(harness.draw() == Rows{"  Name  Qty", "*>kiwi  12", "  apple 3"});
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(activated == std::vector<ui::Key>{key(2)});
}

TEST_CASE("tui table: a removed cursor row hands the cursor to its neighbour", "[tui][table]") {
    Harness harness{30, 5};
    Fruit fruit{harness, ui::SelectionMode::None};
    fruit.addRow(harness, key(3), "fig", "7");
    harness.focus(*fruit.table);
    static_cast<void>(harness.key(KeyCode::Down));
    fruit.removeRow(1);
    CHECK(harness.draw() == Rows{"  Name  Qty", "  apple 3", " >fig   7"});
    fruit.removeRow(1);
    CHECK(harness.draw() == Rows{"  Name  Qty", " >apple 3"});
}

TEST_CASE("tui table: a click selects only its row, a Ctrl-click toggles one, and a double click activates",
          "[tui][table][click]") {
    Harness harness{30, 5};
    std::vector<std::vector<ui::Key>> selections;
    std::vector<ui::Key> activated;

    SECTION("Single") {
        Fruit fruit{harness, ui::SelectionMode::Single};
        fruit.table->setOnSelectionChange(
            [&](std::vector<ui::Key> keys, ui::SelectionChange /*change*/) { selections.push_back(std::move(keys)); });
        fruit.table->setOnActivate([&](ui::Key activatedKey) { activated.push_back(std::move(activatedKey)); });
        static_cast<void>(harness.draw());
        CHECK(harness.click({.x = 0, .y = 0}) == EventResult::Handled);  // the header selects nothing
        CHECK(harness.click({.x = 2, .y = 3}) == EventResult::Handled);  // below the rows: nothing either
        CHECK(selections.empty());
        CHECK(harness.click({.x = 3, .y = 2}) == EventResult::Handled);
        CHECK(harness.focused(*fruit.table));
        CHECK(selections == std::vector<std::vector<ui::Key>>{{key(2)}});
        CHECK(harness.draw() == Rows{"  Name  Qty", "  apple 3", "*>kiwi  12"});
        CHECK(activated.empty());
        // The second click of a double click activates, and changes the selection no further.
        CHECK(harness.click({.x = 9, .y = 2}) == EventResult::Handled);
        CHECK(activated == std::vector<ui::Key>{key(2)});
        CHECK(selections.size() == 1);
        // A third click starts a new double click.
        CHECK(harness.click({.x = 9, .y = 1}) == EventResult::Handled);
        CHECK(selections.back() == std::vector<ui::Key>{key(1)});
        CHECK(activated.size() == 1);
    }
    SECTION("Multiple") {
        Fruit fruit{harness, ui::SelectionMode::Multiple};
        fruit.table->setOnSelectionChange(
            [&](std::vector<ui::Key> keys, ui::SelectionChange /*change*/) { selections.push_back(std::move(keys)); });
        fruit.table->setOnActivate([&](ui::Key activatedKey) { activated.push_back(std::move(activatedKey)); });
        static_cast<void>(harness.draw());
        CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
        CHECK(harness.click({.x = 3, .y = 2}) == EventResult::Handled);
        CHECK(selections == std::vector<std::vector<ui::Key>>{{key(1)}, {key(2)}});
        CHECK(activated.empty());
        CHECK(harness.click({.x = 3, .y = 2}) == EventResult::Handled);
        CHECK(activated == std::vector<ui::Key>{key(2)});
        CHECK(selections.size() == 2);
        CHECK(harness.draw() == Rows{"  Name  Qty", "  apple 3", "*>kiwi  12"});
        // Two quick Ctrl-clicks toggle a row in and out; neither is half of a double click.
        CHECK(harness.click({.x = 3, .y = 1}, core::tui::Modifier::Ctrl) == EventResult::Handled);
        CHECK(harness.click({.x = 3, .y = 1}, core::tui::Modifier::Ctrl) == EventResult::Handled);
        CHECK(selections.at(2) == std::vector<ui::Key>{key(2), key(1)});
        CHECK(selections.at(3) == std::vector<ui::Key>{key(2)});
        CHECK(activated.size() == 1);
    }
    SECTION("None") {
        Fruit fruit{harness, ui::SelectionMode::None};
        fruit.table->setOnSelectionChange(
            [&](std::vector<ui::Key> keys, ui::SelectionChange /*change*/) { selections.push_back(std::move(keys)); });
        fruit.table->setOnActivate([&](ui::Key activatedKey) { activated.push_back(std::move(activatedKey)); });
        static_cast<void>(harness.draw());
        CHECK(harness.click({.x = 3, .y = 2}) == EventResult::Handled);
        CHECK(harness.draw() == Rows{"  Name  Qty", "  apple 3", " >kiwi  12"});
        CHECK(harness.click({.x = 3, .y = 2}) == EventResult::Handled);
        CHECK(activated == std::vector<ui::Key>{key(2)});
        CHECK(selections.empty());
    }
}

TEST_CASE("tui table: two clicks on a row far apart in time are two clicks, not a double click",
          "[tui][table][click]") {
    Harness harness{30, 4};
    Fruit fruit{harness, ui::SelectionMode::Multiple};
    std::vector<std::vector<ui::Key>> selections;
    int activations = 0;
    fruit.table->setOnSelectionChange(
        [&](std::vector<ui::Key> keys, ui::SelectionChange /*change*/) { selections.push_back(std::move(keys)); });
    fruit.table->setOnActivate([&](ui::Key const& /*key*/) { ++activations; });
    auto now = std::chrono::steady_clock::time_point{};
    harness.context().now = [&now] { return now; };
    static_cast<void>(harness.draw());
    CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
    now += std::chrono::milliseconds{501};
    CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
    // The second click selects what the first did, so it reports nothing.
    CHECK(selections == std::vector<std::vector<ui::Key>>{{key(1)}});
    CHECK(activations == 0);
    // Within the double-click time, measured on the same clock, the second click activates.
    now += std::chrono::milliseconds{1000};
    CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
    now += std::chrono::milliseconds{500};
    CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
    CHECK(activations == 1);
}

TEST_CASE("tui table: a key between two clicks on a row makes them two clicks, not a double click",
          "[tui][table][click]") {
    Harness harness{30, 4};
    Fruit fruit{harness, ui::SelectionMode::None};
    int activations = 0;
    fruit.table->setOnActivate([&](ui::Key const& /*key*/) { ++activations; });
    auto const now = std::chrono::steady_clock::time_point{};
    harness.context().now = [now] { return now; };
    static_cast<void>(harness.draw());
    CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
    CHECK(harness.key(KeyCode::Down) == EventResult::Handled);
    CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
    CHECK(activations == 0);
    CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
    CHECK(activations == 1);
}

TEST_CASE("tui table: Space and Enter in a None table select nothing; Enter still activates", "[tui][table]") {
    Harness harness{30, 4};
    Fruit fruit{harness, ui::SelectionMode::None};
    int reports = 0;
    std::vector<ui::Key> activated;
    fruit.table->setOnSelectionChange(
        [&](std::vector<ui::Key> const& /*keys*/, ui::SelectionChange /*change*/) { ++reports; });
    fruit.table->setOnActivate([&](ui::Key activatedKey) { activated.push_back(std::move(activatedKey)); });
    harness.focus(*fruit.table);
    CHECK(harness.type(" ") == EventResult::Handled);
    CHECK(harness.key(KeyCode::End) == EventResult::Handled);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
    CHECK(reports == 0);
    CHECK(activated == std::vector<ui::Key>{key(2)});
    CHECK(fruit.table->markedRows().empty());
    // Single mode: Space on the row already selected is no change, and reports nothing.
    fruit.table->setSelectionMode(ui::SelectionMode::Single);
    CHECK(harness.type(" ") == EventResult::Handled);
    CHECK(harness.type(" ") == EventResult::Handled);
    CHECK(reports == 1);
}

TEST_CASE("tui table: a table taller than its area scrolls its rows to keep the cursor row in view",
          "[tui][table][scroll]") {
    Harness harness{30, 3};
    Fruit fruit{harness, ui::SelectionMode::Single};
    fruit.addRow(harness, key(3), "fig", "7");
    fruit.addRow(harness, key(4), "plum", "1");
    std::vector<std::vector<ui::Key>> selections;
    fruit.table->setOnSelectionChange(
        [&](std::vector<ui::Key> keys, ui::SelectionChange /*change*/) { selections.push_back(std::move(keys)); });
    harness.focus(*fruit.table);
    CHECK(harness.draw() == Rows{"  Name  Qty", " >apple 3", "  kiwi  12"});
    static_cast<void>(harness.key(KeyCode::Down));
    static_cast<void>(harness.key(KeyCode::Down));
    CHECK(harness.draw() == Rows{"  Name  Qty", "  kiwi  12", " >fig   7"});
    static_cast<void>(harness.key(KeyCode::End));
    CHECK(harness.draw() == Rows{"  Name  Qty", "  fig   7", " >plum  1"});
    // A row scrolled out of view takes no click: the click lands on the row drawn there.
    CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
    CHECK(selections == std::vector<std::vector<ui::Key>>{{key(3)}});
    CHECK(harness.draw() == Rows{"  Name  Qty", "*>fig   7", "  plum  1"});
    static_cast<void>(harness.key(KeyCode::Home));
    CHECK(harness.draw() == Rows{"  Name  Qty", " >apple 3", "  kiwi  12"});
    static_cast<void>(harness.key(KeyCode::PageDown));
    CHECK(harness.draw() == Rows{"  Name  Qty", "  kiwi  12", "*>fig   7"});
    static_cast<void>(harness.key(KeyCode::PageUp));
    CHECK(harness.draw() == Rows{"  Name  Qty", " >apple 3", "  kiwi  12"});
}

TEST_CASE("tui table: the wheel scrolls the rows, and passes outward at either end", "[tui][table][scroll]") {
    using Type = core::tui::MouseEvent::Type;
    Harness harness{30, 3};
    Fruit fruit{harness, ui::SelectionMode::Single};
    fruit.addRow(harness, key(3), "fig", "7");
    fruit.addRow(harness, key(4), "plum", "1");
    harness.focus(*fruit.table);
    static_cast<void>(harness.draw());
    CHECK(harness.send(mouse(Type::ScrollUp, {.x = 3, .y = 1})) == EventResult::Ignored);
    CHECK(harness.send(mouse(Type::ScrollDown, {.x = 3, .y = 1})) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"  Name  Qty", "  kiwi  12", "  fig   7"});
    CHECK(harness.send(mouse(Type::ScrollDown, {.x = 3, .y = 1})) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"  Name  Qty", "  fig   7", "  plum  1"});
    CHECK(harness.send(mouse(Type::ScrollDown, {.x = 3, .y = 1})) == EventResult::Ignored);
    // The view stays where the wheel left it until the cursor moves; then the cursor row comes back into view.
    CHECK(harness.draw() == Rows{"  Name  Qty", "  fig   7", "  plum  1"});
    static_cast<void>(harness.key(KeyCode::Down));
    CHECK(harness.draw() == Rows{"  Name  Qty", " >kiwi  12", "  fig   7"});
}

TEST_CASE("tui table: a focused widget inside a row keeps that row in view", "[tui][table][scroll]") {
    Harness harness{30, 3};
    auto const table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "Name"}, {.label = "Go"}});
    std::vector<std::unique_ptr<StackImpl>> rows;
    std::vector<std::unique_ptr<TextImpl>> names;
    std::vector<std::unique_ptr<ButtonImpl>> buttons;
    for (std::int64_t index = 0; index < 4; ++index) {
        rows.push_back(harness.make<StackImpl>(table.get(), ui::Axis::Horizontal));
        names.push_back(harness.make<TextImpl>(rows.back().get()));
        names.back()->setText("row" + std::to_string(index));
        buttons.push_back(harness.make<ButtonImpl>(rows.back().get()));
        buttons.back()->setLabel("go");
        table->setRowKey(*rows.back(), key(index));
    }
    CHECK(harness.draw() == Rows{"  Name Go", "  row0 [ go ]", "  row1 [ go ]"});
    harness.focus(*buttons.at(3));
    CHECK(harness.draw() == Rows{"  Name Go", "  row2 [ go ]", "  row3 [ go ]"});
    buttons.clear();
    names.clear();
    rows.clear();
}

TEST_CASE("tui table: a table inside a disabled container two levels up takes no key, click or wheel",
          "[tui][table][gating]") {
    using Type = core::tui::MouseEvent::Type;
    Harness harness{30, 3};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const outer = harness.make<StackImpl>(column.get(), ui::Axis::Vertical);
    auto const inner = harness.make<StackImpl>(outer.get(), ui::Axis::Vertical);
    {
        Fruit fruit{harness, ui::SelectionMode::Multiple, inner.get()};
        fruit.addRow(harness, key(3), "fig", "7");  // one row more than there is room for, so the wheel can scroll
        int calls = 0;
        fruit.table->setOnSelectionChange(
            [&](std::vector<ui::Key> const& /*keys*/, ui::SelectionChange /*change*/) { ++calls; });
        fruit.table->setOnActivate([&](ui::Key const& /*key*/) { ++calls; });
        static_cast<void>(harness.draw());
        harness.focus(*fruit.table);
        outer->setEnabled(false);
        CHECK_FALSE(morph::tui::detail::WidgetBase::of(*fruit.table).view().focusable());
        CHECK(harness.type(" ") == EventResult::Ignored);
        CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
        CHECK(harness.key(KeyCode::Down) == EventResult::Ignored);
        harness.screen().setFocus(nullptr);
        CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Ignored);
        CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Ignored);
        CHECK(harness.send(mouse(Type::ScrollDown, {.x = 3, .y = 1})) == EventResult::Ignored);
        CHECK(harness.draw() == Rows{"  Name  Qty", "  apple 3", "  kiwi  12"});
        CHECK(calls == 0);
        CHECK(fruit.table->markedRows().empty());

        outer->setEnabled(true);
        static_cast<void>(harness.draw());
        CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
        CHECK(calls == 1);
        CHECK(harness.send(mouse(Type::ScrollDown, {.x = 3, .y = 1})) == EventResult::Handled);
        CHECK(harness.draw() == Rows{"  Name  Qty", "  kiwi  12", "  fig   7"});
    }
}

TEST_CASE("tui table: a disabled row is neither selected nor activated", "[tui][table][gating]") {
    Harness harness{30, 4};
    Fruit fruit{harness, ui::SelectionMode::Single};
    int calls = 0;
    fruit.table->setOnSelectionChange(
        [&](std::vector<ui::Key> const& /*keys*/, ui::SelectionChange /*change*/) { ++calls; });
    fruit.table->setOnActivate([&](ui::Key const& /*key*/) { ++calls; });
    fruit.rows.at(0)->setEnabled(false);
    static_cast<void>(harness.draw());
    harness.focus(*fruit.table);
    CHECK(harness.type(" ") == EventResult::Handled);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
    CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
    CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
    CHECK(calls == 0);
    CHECK(fruit.table->markedRows().empty());
    fruit.rows.at(0)->setEnabled(true);
    CHECK(harness.type(" ") == EventResult::Handled);
    CHECK(calls == 1);
}

// Mutation: fit reporting the tables it took without watching them (the second report runs on a destroyed table:
// a use after free under ASan).
TEST_CASE("tui table: a range handler may destroy another table waiting to report", "[tui][table][lifetime]") {
    Harness harness{30, 12};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto first = std::make_unique<Fruit>(harness, ui::SelectionMode::Single, column.get());
    auto second = std::make_unique<Fruit>(harness, ui::SelectionMode::Single, column.get());
    int reports = 0;
    // Each table's first frame asks to report its columns before the next one, so both wait in the same batch; the
    // first to report destroys the other.
    first->table->setOnHorizontalRange([&](int /*scrollX*/, int /*viewport*/) {
        ++reports;
        second->table.reset();
    });
    second->table->setOnHorizontalRange([&](int /*scrollX*/, int /*viewport*/) {
        ++reports;
        first->table.reset();
    });
    static_cast<void>(harness.draw());  // both tables ask to report
    static_cast<void>(harness.draw());  // the first report destroys the other table, whose report is skipped
    CHECK(reports == 1);
    CHECK((first->table == nullptr) != (second->table == nullptr));
}

TEST_CASE("tui table: a table handler may destroy its own table", "[tui][table][lifetime]") {
    Harness harness{30, 5};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const survivor = harness.make<ButtonImpl>(column.get());
    survivor->setLabel("Stay");
    int selections = 0;
    int activations = 0;
    // The table is destroyed alone, before its rows, as a handler destroying it would leave them.
    auto fruit = std::make_unique<Fruit>(harness, ui::SelectionMode::Single, column.get());
    auto const destroyOnSelection = [&] {
        fruit->table->setOnSelectionChange([&](std::vector<ui::Key> const& /*keys*/, ui::SelectionChange /*change*/) {
            fruit->table.reset();
            ++selections;
        });
        fruit->table->setOnActivate([&](ui::Key const& /*key*/) { ++activations; });
    };
    auto const destroyOnActivation = [&] {
        fruit->table->setOnActivate([&](ui::Key const& /*key*/) {
            fruit->table.reset();
            ++activations;
        });
    };

    SECTION("onSelectionChange, by Space in Single mode") {
        destroyOnSelection();
        harness.focus(*fruit->table);
        CHECK(harness.type(" ") == EventResult::Handled);
    }
    SECTION("onSelectionChange, by Space in Multiple mode") {
        fruit->table->setSelectionMode(ui::SelectionMode::Multiple);
        destroyOnSelection();
        harness.focus(*fruit->table);
        CHECK(harness.type(" ") == EventResult::Handled);
    }
    SECTION("onSelectionChange, by Enter in Single mode: no activation follows") {
        destroyOnSelection();
        harness.focus(*fruit->table);
        CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
    }
    SECTION("onSelectionChange, by a click") {
        destroyOnSelection();
        static_cast<void>(harness.draw());
        CHECK(harness.click({.x = 3, .y = 3}) == EventResult::Handled);
    }
    SECTION("onActivate, by Enter") {
        fruit->table->setSelectionMode(ui::SelectionMode::None);
        destroyOnActivation();
        harness.focus(*fruit->table);
        CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
    }
    SECTION("onActivate, by Enter in Single mode") {
        destroyOnActivation();
        harness.focus(*fruit->table);
        CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
    }
    SECTION("onActivate, by a double click") {
        fruit->table->setSelectionMode(ui::SelectionMode::None);
        destroyOnActivation();
        static_cast<void>(harness.draw());
        CHECK(harness.click({.x = 3, .y = 3}) == EventResult::Handled);
        CHECK(harness.click({.x = 3, .y = 3}) == EventResult::Handled);
    }

    CHECK(fruit->table == nullptr);
    CHECK(selections + activations == 1);
    fruit.reset();
    // The backend stays usable: the survivor still draws and takes a click.
    int survivorClicks = 0;
    survivor->setOnClick([&] { ++survivorClicks; });
    CHECK(harness.draw() == Rows{"[ Stay ]"});
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Handled);
    CHECK(survivorClicks == 1);
}

TEST_CASE("tui table: no table handler runs after its table was destroyed", "[tui][table][lifetime]") {
    Harness harness{30, 4};
    int calls = 0;
    {
        Fruit fruit{harness, ui::SelectionMode::Single};
        fruit.table->setOnSelectionChange(
            [&](std::vector<ui::Key> const& /*keys*/, ui::SelectionChange /*change*/) { ++calls; });
        fruit.table->setOnActivate([&](ui::Key const& /*key*/) { ++calls; });
        static_cast<void>(harness.draw());
        harness.focus(*fruit.table);
        CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
        calls = 0;
        fruit.cells.clear();
        fruit.rows.clear();
        fruit.table.reset();
    }
    CHECK(harness.screen().focusedComponent() == nullptr);
    CHECK(harness.type(" ") == EventResult::Ignored);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
    CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Ignored);
    CHECK(calls == 0);
}

TEST_CASE("tui table: a typed character whose codepoint equals a special key's code moves nothing", "[tui][table]") {
    Harness harness{30, 4};
    Fruit const fruit{harness, ui::SelectionMode::None};
    harness.focus(*fruit.table);
    // U+10006 is a typed character whose value is also KeyCode::Down's.
    auto const typed = core::tui::KeyEvent{.key = KeyCode::Down, .codepoint = static_cast<char32_t>(KeyCode::Down)};
    CHECK(harness.send(typed) == EventResult::Ignored);
    CHECK(harness.draw() == Rows{"  Name  Qty", " >apple 3", "  kiwi  12"});
}

TEST_CASE("tui table: a scroll around a table brings a focused widget inside a row into view",
          "[tui][table][scroll]") {
    Harness harness{30, 3};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const table = harness.make<TableImpl>(scroll.get());
    table->setColumns({{.label = "Name"}, {.label = "Go"}});
    std::vector<std::unique_ptr<StackImpl>> rows;
    std::vector<std::unique_ptr<TextImpl>> names;
    std::vector<std::unique_ptr<ButtonImpl>> buttons;
    for (std::int64_t index = 0; index < 4; ++index) {
        rows.push_back(harness.make<StackImpl>(table.get(), ui::Axis::Horizontal));
        names.push_back(harness.make<TextImpl>(rows.back().get()));
        names.back()->setText("row" + std::to_string(index));
        buttons.push_back(harness.make<ButtonImpl>(rows.back().get()));
        buttons.back()->setLabel("go");
        table->setRowKey(*rows.back(), key(index));
    }
    CHECK(harness.draw() == Rows{"  Name Go", "  row0 [ go ]", "  row1 [ go ]"});
    harness.focus(*buttons.at(3));
    CHECK(harness.draw() == Rows{"  row1 [ go ]", "  row2 [ go ]", "  row3 [ go ]"});
}

TEST_CASE("tui table: setRowKey of a widget that is not one of its rows throws", "[tui][table]") {
    Harness harness{30, 3};
    auto const table = harness.make<TableImpl>(nullptr);
    auto const stranger = harness.make<TextImpl>(nullptr);
    CHECK_THROWS_AS(table->setRowKey(*stranger, key(1)), std::logic_error);
}

TEST_CASE("tui table: PageUp and PageDown move the cursor by a view of lines, not of rows", "[tui][table][scroll]") {
    Harness harness{30, 5};
    auto const table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "Name"}});
    std::vector<std::unique_ptr<StackImpl>> rows;
    std::vector<std::unique_ptr<TextImpl>> cells;
    for (std::int64_t index = 0; index < 6; ++index) {
        rows.push_back(harness.make<StackImpl>(table.get(), ui::Axis::Horizontal));
        cells.push_back(harness.make<TextImpl>(rows.back().get()));
        cells.back()->setText("row" + std::to_string(index) + "\n.");
        table->setRowKey(*rows.back(), key(index));
    }
    std::vector<ui::Key> activated;
    table->setOnActivate([&](ui::Key activatedKey) { activated.push_back(std::move(activatedKey)); });
    harness.focus(*table);
    CHECK(harness.draw() == Rows{"  Name", " >row0", "  .", "  row1", "  ."});
    // Four lines below the header hold two rows of two lines: a page moves two rows.
    static_cast<void>(harness.key(KeyCode::PageDown));
    CHECK(harness.draw() == Rows{"  Name", "  row1", "  .", " >row2", "  ."});
    static_cast<void>(harness.key(KeyCode::PageDown));
    static_cast<void>(harness.key(KeyCode::Enter));
    static_cast<void>(harness.key(KeyCode::PageUp));
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(activated == std::vector<ui::Key>{key(4), key(2)});
    cells.clear();
    rows.clear();
}

// Mutation: draw a toned row as any other (its cells keep the normal colour).
TEST_CASE("tui table: a toned row's cells draw in the tone's style", "[tui][table]") {
    Harness harness{30, 4};
    Fruit fruit{harness, ui::SelectionMode::Single};
    fruit.table->setRowTone(*fruit.rows.at(0), ui::Tone::Err);
    static_cast<void>(harness.draw());
    auto const& buffer = harness.screen().renderedBuffer();
    auto const& theme = harness.screen().theme();
    // Row 1 is the first data row; its first cell starts after the two-cell gutter.
    CHECK(buffer.cellAt({.x = 2, .y = 1})->style.fg == theme.error.fg);
    CHECK_FALSE(buffer.cellAt({.x = 2, .y = 2})->style.fg == theme.error.fg);
    fruit.table->setRowTone(*fruit.rows.at(0), std::nullopt);
    static_cast<void>(harness.draw());
    CHECK_FALSE(harness.screen().renderedBuffer().cellAt({.x = 2, .y = 1})->style.fg == theme.error.fg);
    CHECK_THROWS_AS(fruit.table->setRowTone(*fruit.cells.at(0), ui::Tone::Ok), std::logic_error);
}

// Mutation: setRowMarks doing nothing (no row is dim, and every line starts with the selection's gutter).
TEST_CASE("tui table: a row's marks are drawn", "[tui][table]") {
    Harness harness{30, 5};
    Fruit fruit{harness, ui::SelectionMode::Single};
    fruit.addRow(harness, key(3), "plum", "1");
    fruit.table->setRowMarks(*fruit.rows.at(0),
                             ui::RowMarks{.unmatched = true, .tooltip = "This row no longer matches the filter"});
    fruit.table->setRowMarks(*fruit.rows.at(1), ui::RowMarks{.busy = true});
    fruit.table->setRowMarks(*fruit.rows.at(2),
                             ui::RowMarks{.locked = true, .tooltip = "Being edited by another user"});
    auto const lines = harness.draw();
    auto const& buffer = harness.screen().renderedBuffer();
    // Row 0 is the first data line; its first cell starts after the gutter, two cells and the glyph's two.
    CHECK(buffer.cellAt({.x = 4, .y = 1})->style.dim);
    CHECK_FALSE(buffer.cellAt({.x = 4, .y = 2})->style.dim);
    CHECK(lines.at(2).starts_with("  … "));
    CHECK(lines.at(3).starts_with("  # "));
    CHECK(lines.at(1).starts_with("    "));
    for (auto const& row : fruit.rows) {
        fruit.table->setRowMarks(*row, ui::RowMarks{});
    }
    auto const plain = harness.draw();
    CHECK_FALSE(harness.screen().renderedBuffer().cellAt({.x = 4, .y = 1})->style.dim);
    CHECK(plain.at(2).starts_with("    "));
    CHECK(plain.at(3).starts_with("    "));
    CHECK_THROWS_AS(fruit.table->setRowMarks(*fruit.cells.at(0), ui::RowMarks{}), std::logic_error);
}

// Mutation: the glyph written over the gutter's first cell, which hides the selection mark `*` of a busy row.
TEST_CASE("tui table: a busy or locked row's glyph hides no mark", "[tui][table]") {
    Harness harness{30, 5};
    Fruit fruit{harness, ui::SelectionMode::Multiple};
    fruit.addRow(harness, key(3), "plum", "1");
    fruit.table->setSelection({key(1), key(3)});
    auto const before = harness.draw();
    CHECK(before.at(1).starts_with("* apple"));
    fruit.table->setRowMarks(*fruit.rows.at(0), ui::RowMarks{.busy = true});
    fruit.table->setRowMarks(*fruit.rows.at(2), ui::RowMarks{.locked = true});
    auto const lines = harness.draw();
    // Selection mark, cursor mark, then the glyph and a gap; the cells start two further right.
    CHECK(lines.at(1).starts_with("* … apple"));
    CHECK(lines.at(2).starts_with("    kiwi"));
    CHECK(lines.at(3).starts_with("* # plum"));
    fruit.table->setRowMarks(*fruit.rows.at(0), ui::RowMarks{});
    CHECK(harness.draw().at(1).starts_with("*   apple"));
}

// Mutation: the marks' text written over the row's own tooltip, which leaves the row's hover blank once its marks
// clear.
TEST_CASE("tui table: a busy or locked row says why on hover", "[tui][table]") {
    Harness harness{30, 5};
    Fruit fruit{harness, ui::SelectionMode::Single};
    auto& apple = morph::tui::detail::WidgetBase::of(*fruit.rows.at(0));
    auto const& kiwi = morph::tui::detail::WidgetBase::of(*fruit.rows.at(1));
    apple.applyTooltip("Fresh today");
    fruit.table->setRowMarks(*fruit.rows.at(0), ui::RowMarks{.busy = true});
    fruit.table->setRowMarks(*fruit.rows.at(1),
                             ui::RowMarks{.locked = true, .tooltip = "Being edited by another user"});
    CHECK(apple.hoverText() == "Saving this row");
    CHECK(kiwi.hoverText() == "Being edited by another user");
    fruit.table->setRowMarks(*fruit.rows.at(1), ui::RowMarks{.locked = true});
    CHECK(kiwi.hoverText() == "Locked by another user");
    fruit.table->setRowMarks(*fruit.rows.at(0), ui::RowMarks{});
    CHECK(apple.hoverText() == "Fresh today");
    fruit.table->setRowMarks(*fruit.rows.at(1), ui::RowMarks{});
    CHECK(kiwi.hoverText().empty());
}

namespace {

/// A table of two fixed columns @p fixed wide and a stretch column headed `Analyte`, with one row.
struct Overfull {
    Overfull(Harness& harness, int fixed) : table{harness.make<TableImpl>(nullptr)} {
        table->setColumns({{.label = "Sample", .width = ui::Sizing::fixed(fixed)},
                           {.label = "Lot", .width = ui::Sizing::fixed(fixed)},
                           {.label = "Analyte", .width = ui::Sizing::stretch(1)}});
        row = harness.make<StackImpl>(table.get(), ui::Axis::Horizontal);
        for (auto const* const text : {"S-1", "L-1", "Assay"}) {
            cells.push_back(harness.make<TextImpl>(row.get()));
            cells.back()->setText(text);
        }
        table->setRowKey(*row, key(1));
    }
    ~Overfull() {
        cells.clear();
        row.reset();
    }
    Overfull(Overfull const&) = delete;
    Overfull& operator=(Overfull const&) = delete;
    Overfull(Overfull&&) = delete;
    Overfull& operator=(Overfull&&) = delete;

    std::unique_ptr<TableImpl> table;
    std::unique_ptr<StackImpl> row;
    std::vector<std::unique_ptr<TextImpl>> cells;
};

}  // namespace

// Mutation: drop the header minimum in TableImpl::columnWidths (the stretch column gets what is left, 6 columns, and
// its header is cut to `Analyt`; with less room, `Lot` is squeezed to nothing).
TEST_CASE("tui table: no column is squeezed below its header, and an overfull table is cut at its edge",
          "[tui][table][columns]") {
    {
        // The stretch column is floored at its header; the overflow that makes comes from the last fixed column.
        Harness harness{30, 3};
        Overfull const table{harness, 10};
        CHECK(harness.draw() == Rows{"  Sample     Lot       Analyte", "  S-1        L-1       Assay"});
    }
    {
        // Two fixed columns of 20 give way down to their headers, and the stretch column keeps its own.
        Harness harness{30, 3};
        Overfull const table{harness, 20};
        CHECK(harness.draw() == Rows{"  Sample           Lot Analyte", "  S-1              L-1 Assay"});
    }
    {
        // Narrower than the headers together: the columns past the edge are cut, not drawn at width 0.
        Harness harness{12, 3};
        Overfull const table{harness, 20};
        CHECK(harness.draw() == Rows{"  Sample Lot", "  S-1    L-1"});
    }
}

// Mutation: place a table row's cells by the row's own alignment (the short cell sits on the first line).
TEST_CASE("tui table: a one-line cell beside a three-line cell is drawn on the middle line", "[tui][table]") {
    Harness harness{30, 5};
    auto const table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "Name"}, {.label = "Notes"}});
    auto const row = harness.make<StackImpl>(table.get(), ui::Axis::Horizontal);
    auto const name = harness.make<TextImpl>(row.get());
    name->setText("pH");
    auto const notes = harness.make<TextImpl>(row.get());
    notes->setText("one\ntwo\nthree");
    table->setRowKey(*row, key(1));
    CHECK(harness.draw() == Rows{"  Name Notes", "       one", "  pH   two", "       three"});
}

// Mutation: place every cell at its column's start (the digits start the column, and the header does too).
TEST_CASE("tui table: an end column draws its text and its header label at its end", "[tui][table][columns]") {
    Harness harness{30, 3};
    auto const table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "Name"}, {.label = "Area", .width = ui::Sizing::fixed(12), .align = ui::Align::End}});
    auto const row = harness.make<StackImpl>(table.get(), ui::Axis::Horizontal);
    auto const name = harness.make<TextImpl>(row.get());
    name->setText("pH");
    auto const area = harness.make<TextImpl>(row.get());
    area->setText("0.21");
    table->setRowKey(*row, key(1));
    // The gutter's 2 cells, `Name` 4, a gap, then the 12-cell column: its last four cells hold the digits.
    CHECK(harness.draw() == Rows{"  Name         Area", "  pH           0.21"});
}

// Mutation: do not tell a badge cell it is elided (the caption is cut by the cell's edge: `[Out of t`).
TEST_CASE("tui table: a badge cell wider than its column is cut with an ellipsis and keeps its closing bracket",
          "[tui][table][columns]") {
    Harness harness{30, 3};
    auto const table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "Name"}, {.label = "Check", .width = ui::Sizing::fixed(9)}});
    auto const row = harness.make<StackImpl>(table.get(), ui::Axis::Horizontal);
    auto const name = harness.make<TextImpl>(row.get());
    name->setText("pH");
    auto const check = harness.make<BadgeImpl>(row.get());
    check->setText("Out of trend");
    table->setRowKey(*row, key(1));
    CHECK(harness.draw() == Rows{"  Name Check", "  pH   [Out of…]"});
}

// Mutation: let the column place a text that has its own alignment (the start-aligned text moves to the column's end).
TEST_CASE("tui table: a text with its own start alignment keeps the column's start in an end column",
          "[tui][table][columns]") {
    Harness harness{30, 3};
    auto const table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "Name"}, {.label = "Area", .width = ui::Sizing::fixed(12), .align = ui::Align::End}});
    auto const row = harness.make<StackImpl>(table.get(), ui::Axis::Horizontal);
    auto const name = harness.make<TextImpl>(row.get());
    name->setText("pH");
    auto const area = harness.make<TextImpl>(row.get());
    area->setText("0.21");
    area->setAlign(ui::TextAlign::Start);
    table->setRowKey(*row, key(1));
    CHECK(harness.draw() == Rows{"  Name         Area", "  pH   0.21"});
}

// Mutation: ignore Shift with an arrow (the cursor moves, nothing is selected).
TEST_CASE("tui table: Shift+Down twice from the first row selects the first three rows", "[tui][table][selection]") {
    Harness harness{30, 5};
    Fruit fruit{harness, ui::SelectionMode::Multiple};
    fruit.addRow(harness, key(3), "lime", "4");
    std::vector<std::pair<std::vector<ui::Key>, ui::SelectionChange>> reports;
    fruit.table->setOnSelectionChange(
        [&](std::vector<ui::Key> keys, ui::SelectionChange change) { reports.emplace_back(std::move(keys), change); });
    harness.focus(*fruit.table);
    static_cast<void>(harness.draw());
    CHECK(harness.key(KeyCode::Down, core::tui::Modifier::Shift) == EventResult::Handled);
    CHECK(harness.key(KeyCode::Down, core::tui::Modifier::Shift) == EventResult::Handled);
    REQUIRE(reports.size() == 2);
    CHECK(reports.back().first == std::vector<ui::Key>{key(1), key(2), key(3)});
    CHECK(reports.back().second == ui::SelectionChange::Replace);
    CHECK(fruit.table->markedRows() == Positions{0, 1, 2});
}

// Mutation: draw the header without its box (the header line starts with the column's label).
TEST_CASE("tui table: the select column's header box shows a partial selection, and a click on it selects every row",
          "[tui][table][selection]") {
    Harness harness{30, 4};
    Fruit fruit{harness, ui::SelectionMode::Multiple};
    fruit.table->setSelectColumn(true);
    std::vector<std::pair<std::vector<ui::Key>, ui::SelectionChange>> reports;
    fruit.table->setOnSelectionChange(
        [&](std::vector<ui::Key> keys, ui::SelectionChange change) { reports.emplace_back(std::move(keys), change); });
    fruit.table->setSelection({key(1)});
    CHECK(harness.draw() == Rows{"[-]  Name  Qty", "[x]  apple 3", "[ ]  kiwi  12"});
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Handled);
    REQUIRE(reports.size() == 1);
    CHECK(reports.back().first == std::vector<ui::Key>{key(1), key(2)});
    CHECK(reports.back().second == ui::SelectionChange::Extend);
    CHECK(harness.draw().front() == "[x]  Name  Qty");
    // A row's box is a Ctrl-click on its row.
    CHECK(harness.click({.x = 1, .y = 1}) == EventResult::Handled);
    CHECK(reports.back().first == std::vector<ui::Key>{key(2)});
    CHECK(harness.draw() == Rows{"[-]  Name  Qty", "[ ]> apple 3", "[x]  kiwi  12"});
}

// Mutation: drop the mark from `headerText` (the header reads `Name` alone).
TEST_CASE("tui table: the sorted column's header shows its direction, and a click on a sortable header asks to sort",
          "[tui][table][sort]") {
    Harness harness{30, 3};
    auto const table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "Name", .sortable = true}, {.label = "Qty", .width = ui::Sizing::fixed(5)}});
    auto const row = harness.make<StackImpl>(table.get(), ui::Axis::Horizontal);
    auto const name = harness.make<TextImpl>(row.get());
    name->setText("apple");
    auto const quantity = harness.make<TextImpl>(row.get());
    quantity->setText("3");
    table->setRowKey(*row, key(1));
    std::vector<std::size_t> asked;
    table->setOnSort([&asked](std::size_t column, ui::HeaderClick) { asked.push_back(column); });
    // A sortable header keeps room for its mark, so the columns stay where they are when the sort comes.
    CHECK(harness.draw() == Rows{"  Name   Qty", "  apple  3"});
    table->setSort({ui::TableSort{.column = 0, .direction = ui::SortDirection::Descending}});
    CHECK(harness.draw() == Rows{"  Name ▼ Qty", "  apple  3"});
    CHECK(asked.empty());
    CHECK(harness.click({.x = 3, .y = 0}) == EventResult::Handled);
    CHECK(harness.click({.x = 9, .y = 0}) == EventResult::Handled);
    CHECK(asked == std::vector<std::size_t>{0});
}

// Control: leave the header unfocusable (Up on the first row then leaves the header unfocused).
TEST_CASE("tui table: Up on the first row focuses the header; Left, Right, Shift+Enter and Ctrl+Shift+Right act on it",
          "[tui]") {
    Harness harness{40, 4};
    auto const table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "Name", .sortable = true},
                       {.label = "Qty", .sortable = true},
                       {.label = "Site", .sortable = true}});
    auto const row = harness.make<StackImpl>(table.get(), ui::Axis::Horizontal);
    std::vector<std::unique_ptr<TextImpl>> cells;
    for (std::string const text : {"apple", "3", "north"}) {
        cells.push_back(harness.make<TextImpl>(row.get()));
        cells.back()->setText(text);
    }
    table->setRowKey(*row, key(1));
    table->setSelectionMode(ui::SelectionMode::Single);
    table->setSort({ui::TableSort{.column = 1, .direction = ui::SortDirection::Ascending}});
    std::vector<std::pair<std::size_t, ui::HeaderClick>> sorts;
    table->setOnSort([&sorts](std::size_t column, ui::HeaderClick click) { sorts.emplace_back(column, click); });
    std::vector<std::pair<std::size_t, std::size_t>> moves;
    table->setOnColumnMove(
        [&moves](std::size_t column, std::size_t position) { moves.emplace_back(column, position); });
    static_cast<void>(harness.draw());
    harness.focus(*table);
    CHECK(harness.key(KeyCode::Up) == EventResult::Handled);
    CHECK(table->focusedHeader() == std::optional<std::size_t>{1});
    CHECK(harness.key(KeyCode::Right) == EventResult::Handled);
    CHECK(table->focusedHeader() == std::optional<std::size_t>{2});
    CHECK(harness.key(KeyCode::Enter, core::tui::Modifier::Shift) == EventResult::Handled);
    CHECK(sorts == std::vector<std::pair<std::size_t, ui::HeaderClick>>{{2, ui::HeaderClick::Shift}});
    CHECK(harness.key(KeyCode::Left, core::tui::Modifier::Ctrl | core::tui::Modifier::Shift) == EventResult::Handled);
    CHECK(moves == std::vector<std::pair<std::size_t, std::size_t>>{{2, 1}});
    CHECK(harness.key(KeyCode::Down) == EventResult::Handled);
    CHECK(table->focusedHeader() == std::nullopt);
    // Back on row 0: Space selects it.
    CHECK(harness.type(" ") == EventResult::Handled);
    CHECK(table->markedRows() == Positions{0});
    cells.clear();
}

namespace {

/// A table of the given columns, one row holding a text per column.
struct Worksheet {
    Worksheet(Harness& harness, std::vector<ui::TableColumn> const& columns, std::vector<std::string> const& texts)
        : table{harness.make<TableImpl>(nullptr)} {
        table->setColumns(columns);
        row = harness.make<StackImpl>(table.get(), ui::Axis::Horizontal);
        for (auto const& text : texts) {
            cells.push_back(harness.make<TextImpl>(row.get()));
            cells.back()->setText(text);
        }
        table->setRowKey(*row, key(1));
    }
    ~Worksheet() {
        cells.clear();
        row.reset();
    }
    Worksheet(Worksheet const&) = delete;
    Worksheet& operator=(Worksheet const&) = delete;
    Worksheet(Worksheet&&) = delete;
    Worksheet& operator=(Worksheet&&) = delete;

    std::unique_ptr<TableImpl> table;
    std::unique_ptr<StackImpl> row;
    std::vector<std::unique_ptr<TextImpl>> cells;
};

}  // namespace

// Mutation: ignore the natural widths in `TableImpl::columnWidths` (the stretch columns share 30 by weight alone, and
// the long text is cut).
TEST_CASE("tui table: a stretch column starts from its content and the weights share only what is left",
          "[tui][table][columns]") {
    Harness harness{40, 3};
    Worksheet const sheet{harness,
                          {{.label = "A", .width = ui::Sizing::stretch(1), .wrap = false},
                           {.label = "B", .width = ui::Sizing::stretch(1), .wrap = false}},
                          {"a", "thirty-character-text-for-b-col"}};
    // 40 less the gutter of 2 and a gap leaves 37 for 1 + 30: the long text is whole, and the spare goes by weight.
    CHECK(harness.draw() == Rows{"  A    B", "  a    thirty-character-text-for-b-col"});
}

// Mutation: let the last column give first in review's shape (Spec. would be cut), or share by weight (the tag too).
TEST_CASE("tui table: the column that shrinks gives first, the others stay whole", "[tui][table][columns]") {
    Harness harness{40, 3};
    Worksheet const sheet{harness,
                          {{.label = "Analyte", .width = ui::Sizing::stretch(1), .wrap = false, .shrink = true},
                           {.label = "Spec", .width = ui::Sizing::stretch(1), .wrap = false},
                           {.label = "Change", .width = ui::Sizing::stretch(1), .wrap = false, .shrink = true}},
                          {"Assay, mean", "95.0-105.0", "integration complete"}};
    // 40 less 2 and two gaps is 36 for 11 + 10 + 20: the last column gives 5 and the first two keep their text.
    auto const drawn = harness.draw();
    REQUIRE(drawn.size() >= 2);
    CHECK(drawn.at(1) == "  Assay, mean 95.0-105.0 integration co…");
}

// Mutation: leave the `shown()` test out of `TableImpl::leastWidths`, or the cell out of `naturalWidths`.
TEST_CASE("tui table: a wrapping text cell is narrowed to its widest word, and ends its line in an ellipsis",
          "[tui][table][columns]") {
    Harness harness{24, 3};
    Worksheet const sheet{harness,
                          {{.label = "Note", .width = ui::Sizing::stretch(1)},
                           {.label = "Tag", .width = ui::Sizing::stretch(1), .wrap = false}},
                          {"alpha beta gamma", "out of spec now"}};
    // 24 less 2 and a gap leaves 21 for 16 + 15: the wrapping column gives down towards its widest word (5), so
    // the tag stays whole, and the narrowed note ends in an ellipsis.
    auto const drawn = harness.draw();
    REQUIRE(drawn.size() >= 2);
    CHECK(drawn.at(1) == "  alpha… out of spec now");
}

// Mutation: leave `leastWidth` at the text's natural width (the wrapping column never gives).
TEST_CASE("tui table: a text's least width is its widest word, a mono text's its whole width",
          "[tui][table][columns]") {
    Harness harness{30, 3};
    auto const text = harness.make<TextImpl>(nullptr);
    text->setText("alpha beta\ngamma");
    CHECK(text->leastWidth() == 5);
    text->setSpans({{.text = "ab cd"}, {.text = "efgh ij", .strong = true}});
    CHECK(text->leastWidth() == 6);
    text->setText("alpha beta");
    text->setRole(ui::TextRole::Mono);
    CHECK(text->leastWidth() == 10);
}

// Mutation: join the lines of the runs with `elidedRuns` in a wrapping column (the second line is lost).
TEST_CASE("tui table: runs in a wrapping column end each cut line in an ellipsis", "[tui][table][columns]") {
    Harness harness{30, 4};
    auto const table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "Note", .width = ui::Sizing::fixed(6)}});
    auto const row = harness.make<StackImpl>(table.get(), ui::Axis::Horizontal);
    auto const note = harness.make<TextImpl>(row.get());
    note->setSpans({{.text = "first line\n"}, {.text = "second one", .strong = true}});
    table->setRowKey(*row, key(1));
    auto const drawn = harness.draw();
    REQUIRE(drawn.size() >= 3);
    CHECK(drawn.at(1) == "  first…");
    CHECK(drawn.at(2) == "  secon…");
}

// Mutation: leave the container cell's natural width out of `leastWidths` (the column narrows below a nested text).
TEST_CASE("tui table: a container cell keeps its natural least width", "[tui][table][columns]") {
    Harness harness{24, 3};
    auto const table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "Note", .width = ui::Sizing::stretch(1)},
                       {.label = "Tag", .width = ui::Sizing::stretch(1), .wrap = false}});
    auto const row = harness.make<StackImpl>(table.get(), ui::Axis::Horizontal);
    auto const holder = harness.make<StackImpl>(row.get(), ui::Axis::Vertical);
    auto const inner = harness.make<TextImpl>(holder.get());
    inner->setText("alpha beta gamma");
    auto const tag = harness.make<TextImpl>(row.get());
    tag->setText("out of spec now");
    table->setRowKey(*row, key(1));
    auto const drawn = harness.draw();
    REQUIRE(drawn.size() >= 2);
    // The nested text is not a direct cell, so its column never narrows below it and the tag gives instead.
    CHECK(drawn.at(1).starts_with("  alpha beta gamma "));
}

// Mutation: leave `setRowSpans` empty (the covered cells show their own text, and the first row's cell stays in its
// row), or hide no covered cell (the covered rows show "kiwi" and "plum" beside the spanning text).
TEST_CASE("tui table: a spanning cell is drawn once over the rows it covers, and the covered cells draw nothing",
          "[tui][table][span]") {
    Harness harness{30, 6};
    Fruit fruit{harness, ui::SelectionMode::Single};
    fruit.addRow(harness, key(3), "plum", "7");
    static_cast<void>(harness.draw());
    CHECK(harness.draw() == Rows{"  Name  Qty", "  apple 3", "  kiwi  12", "  plum  7"});
    // The first column's first cell covers all three rows; its text sits on the middle one, as a centred cell does.
    fruit.table->setRowSpans(*fruit.rows.at(0), {{.column = 0, .rows = 3}});
    CHECK(harness.draw() == Rows{"  Name  Qty", "        3", "  apple 12", "        7"});
    // The covered cells give their column no width: the column is the spanning cell's. A covered cell longer than the
    // spanning one would widen the column if it counted.
    fruit.table->setRowSpans(*fruit.rows.at(0), {});
    CHECK(harness.draw() == Rows{"  Name  Qty", "  apple 3", "  kiwi  12", "  plum  7"});
}

// Mutation: hit-test the drawn text area alone (the blank covered lines of a centred cell select the covered row).
TEST_CASE("tui table: a click on a blank line of a centred spanning cell selects the row that gives the span",
          "[tui][table][span]") {
    Harness harness{30, 6};
    Fruit fruit{harness, ui::SelectionMode::Single};
    fruit.addRow(harness, key(3), "plum", "7");
    std::vector<std::vector<ui::Key>> selections;
    fruit.table->setOnSelectionChange(
        [&](std::vector<ui::Key> keys, ui::SelectionChange /*change*/) { selections.push_back(std::move(keys)); });
    fruit.table->setRowSpans(*fruit.rows.at(0), {{.column = 0, .rows = 3}});
    static_cast<void>(harness.draw());
    // The one-line text is centred on line 2; lines 1 and 3 are blank covered lines of the same column.
    CHECK(harness.draw().at(2).contains("apple"));
    CHECK(harness.click({.x = 4, .y = 3}) == EventResult::Handled);
    CHECK(selections == std::vector<std::vector<ui::Key>>{{key(1)}});
    CHECK(harness.click({.x = 4, .y = 1}) == EventResult::Handled);
    CHECK(selections.back() == std::vector<ui::Key>{key(1)});
    // Beside the span, the covered row's own cell is the covered row's.
    CHECK(harness.click({.x = 12, .y = 3}) == EventResult::Handled);
    CHECK(selections.back() == std::vector<ui::Key>{key(3)});
}

// Mutation: leave a table with no spans on the per-frame planning path (the cells are walked every frame).
TEST_CASE("tui table: a table that never had a span plans nothing, and one that lost its spans puts the cells back",
          "[tui][table][span]") {
    Harness harness{30, 6};
    Fruit fruit{harness, ui::SelectionMode::Single};
    static_cast<void>(harness.draw());
    CHECK(std::ranges::find(harness.context().spanTables, fruit.table.get()) == harness.context().spanTables.end());
    fruit.table->setRowSpans(*fruit.rows.at(0), {{.column = 0, .rows = 2}});
    CHECK(std::ranges::find(harness.context().spanTables, fruit.table.get()) != harness.context().spanTables.end());
    static_cast<void>(harness.draw());
    CHECK(fruit.cells.at(0)->lifted());
    fruit.table->setRowSpans(*fruit.rows.at(0), {});
    CHECK(harness.draw() == Rows{"  Name  Qty", "  apple 3", "  kiwi  12"});
    CHECK_FALSE(fruit.cells.at(0)->lifted());
    CHECK(std::ranges::find(harness.context().spanTables, fruit.table.get()) == harness.context().spanTables.end());
}

// Mutation: plan only while painting (the first frame after a span arrives still sizes the table by the cell that is
// about to be lifted, and the widget under it sits two lines too low).
TEST_CASE("tui table: lifting a tall spanning cell shrinks the table in the frame that does it",
          "[tui][table][span]") {
    Harness harness{30, 10};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    Fruit fruit{harness, ui::SelectionMode::Single, column.get()};
    auto foot = harness.make<TextImpl>(column.get());
    foot->setText("foot");
    // The first cell is three lines tall, so its row is.
    fruit.cells.at(0)->setText("a\nb\nc");
    CHECK(harness.draw().at(5) == "foot");
    fruit.table->setRowSpans(*fruit.rows.at(0), {{.column = 0, .rows = 2}});
    // Lifted, the cell leaves its row, which is a line tall; the cell's three lines cover that and the second row, a
    // line, and one more line goes to the last covered row. The table is four lines tall, and the foot sits under it.
    Rows const drawn = harness.draw();
    CHECK(drawn.at(4) == "foot");
}

// Mutation: let the hit-test read the line alone (the click on the covered line selects the second row).
TEST_CASE("tui table: a click on a spanning cell, on a line of a covered row, selects the row that gives the span",
          "[tui][table][span]") {
    Harness harness{30, 6};
    Fruit fruit{harness, ui::SelectionMode::Single};
    fruit.addRow(harness, key(3), "plum", "7");
    std::vector<std::vector<ui::Key>> selections;
    fruit.table->setOnSelectionChange(
        [&](std::vector<ui::Key> keys, ui::SelectionChange /*change*/) { selections.push_back(std::move(keys)); });
    fruit.table->setRowSpans(*fruit.rows.at(0), {{.column = 0, .rows = 3}});
    static_cast<void>(harness.draw());
    // Line 2 is the covered second row's; column 4 lies in the spanning cell's column.
    CHECK(harness.click({.x = 4, .y = 2}) == EventResult::Handled);
    CHECK(selections == std::vector<std::vector<ui::Key>>{{key(1)}});
    // The same line, in the other column, is the covered row's own.
    CHECK(harness.click({.x = 12, .y = 2}) == EventResult::Handled);
    CHECK(selections.back() == std::vector<ui::Key>{key(2)});
}

// Mutation: leave the plan alone when a row goes (the click reads the removed origin row's freed spanning cell, and
// selects the freed row; ASan reports it).
TEST_CASE("tui table: a click between removing a span's origin row and the next draw selects the row under it",
          "[tui][table][span]") {
    Harness harness{30, 6};
    Fruit fruit{harness, ui::SelectionMode::Single};
    fruit.addRow(harness, key(3), "plum", "7");
    fruit.addRow(harness, key(4), "pear", "9");
    std::vector<std::vector<ui::Key>> selections;
    fruit.table->setOnSelectionChange(
        [&](std::vector<ui::Key> keys, ui::SelectionChange /*change*/) { selections.push_back(std::move(keys)); });
    fruit.table->setRowSpans(*fruit.rows.at(0), {{.column = 0, .rows = 3}});
    static_cast<void>(harness.draw());
    fruit.removeRow(0);
    // No draw has run: the rows are kiwi, plum and pear on lines 1 to 3, and line 3 in the first column is pear's.
    CHECK(harness.click({.x = 4, .y = 3}) == EventResult::Handled);
    CHECK(selections == std::vector<std::vector<ui::Key>>{{key(4)}});
}

// Mutation: place the spanning cell without the table's clip (it draws above the header, or not at all).
TEST_CASE("tui table: a span whose origin row has scrolled off the top still shows its text on the covered lines",
          "[tui][table][span]") {
    Harness harness{30, 3};
    Fruit fruit{harness, ui::SelectionMode::Single};
    fruit.addRow(harness, key(3), "plum", "7");
    fruit.addRow(harness, key(4), "fig", "1");
    fruit.table->setRowSpans(*fruit.rows.at(0), {{.column = 0, .rows = 3}});
    static_cast<void>(harness.draw());
    harness.focus(*fruit.table);
    for (int step = 0; step < 3; ++step) {
        static_cast<void>(harness.key(KeyCode::Down));
    }
    Rows const drawn = harness.draw();
    // The cursor is on the fourth row; the first is above the view, the second and third are covered and shown.
    CHECK(drawn.at(0) == "  Name  Qty");
    CHECK(drawn.at(1).contains("apple"));
}

// Mutation: build a Ctrl-click's selection from the rows drawn instead of from the selection the table holds.
TEST_CASE("tui table: a Ctrl-click keeps every selected key whose row is off screen", "[tui][table][selection]") {
    Harness harness{30, 5};
    Fruit fruit{harness, ui::SelectionMode::Multiple};
    constexpr std::int64_t rowCount = 30;
    for (std::int64_t index = 3; index <= rowCount; ++index) {
        fruit.addRow(harness, key(index), "row", "1");
    }
    std::vector<std::vector<ui::Key>> selections;
    fruit.table->setOnSelectionChange(
        [&](std::vector<ui::Key> keys, ui::SelectionChange /*change*/) { selections.push_back(std::move(keys)); });
    std::vector<ui::Key> all;
    all.reserve(rowCount);
    for (std::int64_t index = 1; index <= rowCount; ++index) {
        all.push_back(key(index));
    }
    fruit.table->setSelection(all);
    static_cast<void>(harness.draw());

    // Deselecting a drawn row leaves the other 29 keys, drawn or not.
    CHECK(harness.click({.x = 3, .y = 1}, core::tui::Modifier::Ctrl) == EventResult::Handled);
    REQUIRE(selections.size() == 1);
    CHECK(selections.back().size() == static_cast<std::size_t>(rowCount - 1));
    CHECK(std::ranges::find(selections.back(), key(1)) == selections.back().end());
    CHECK(std::ranges::find(selections.back(), key(30)) != selections.back().end());

    // Selecting it again keeps them all.
    CHECK(harness.click({.x = 3, .y = 1}, core::tui::Modifier::Ctrl) == EventResult::Handled);
    REQUIRE(selections.size() == 2);
    CHECK(selections.back().size() == static_cast<std::size_t>(rowCount));
}

// ── The column window ───────────────────────────────────────────────────────

namespace {

using morph::testing::FakeRows;
using morph::tui::detail::ContainerBase;
using morph::tui::detail::WidgetBase;

struct TuiWindowOptions {
    struct Level {
        std::size_t column = 0;
        std::string level;
    };
    std::size_t pinned = 0;
    std::vector<Level> levels;
    ui::SelectionMode selection = ui::SelectionMode::None;
    // Each column's unit, by index; a column past the list has none.
    std::vector<std::string> units;
};

// `n` with its thousands grouped by commas.
std::string grouped(std::int64_t n) {
    std::string digits = std::to_string(n);
    for (auto at = static_cast<std::ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3) {
        digits.insert(static_cast<std::size_t>(at), ",");
    }
    return digits;
}

// The text of a record's field, or nothing.
std::string fieldOf(ui::Value const& record, std::string const& field) {
    ui::Value const* const value = record.field(field);
    return value != nullptr && value->asString() != nullptr ? *value->asString() : std::string{};
}

// One table over `FakeRows` on the terminal backend, in a terminal of 120 × 32: every column 11 cells and its gap
// wide with the default text cell, so ten columns and thirty rows are in view under the header and over the status
// line. Its window has no overscan, so the cells alive are exactly those in view. The ranges the table asks the mount
// for are kept in `ranges`.
struct TuiWindowFixture {
    TuiWindowFixture(std::int64_t rowCount, std::size_t columnCount, TuiWindowOptions const& options = {})
        : terminal{std::make_unique<core::tui::MockTerminalOutput>(120, 32)},
          screen{terminal},
          backend{screen},
          rows{std::make_shared<FakeRows>(rowCount, columnCount, owner)} {
        std::vector<ui::TableColumn> columns;
        for (std::size_t column = 0; column < columnCount; ++column) {
            // NOLINTNEXTLINE(performance-inefficient-vector-operation): `columns` is a fixture list built once; sizing it first would not change what the case checks
            columns.push_back(
                ui::TableColumn{.id = "c" + std::to_string(column),
                                .label = "C" + std::to_string(column),
                                .width = ui::Sizing::fixed(11),
                                .unit = column < options.units.size() ? options.units.at(column) : std::string{}});
        }
        ui::TableSource source;
        source.rows = rows;
        source.ownCell.assign(columnCount, false);
        source.levels.assign(columnCount, std::string{});
        for (TuiWindowOptions::Level const& level : options.levels) {
            source.levels.at(level.column) = level.level;
        }
        source.defaultCellView = [](morph::reactive::Signal<std::size_t> const& column,
                                    morph::reactive::Signal<ui::Value> const& row) {
            return ui::text(
                {.text = [&column, &row] { return fieldOf(row.get(), "c" + std::to_string(column.get())); }});
        };
        source.cellView = [](std::size_t column, morph::reactive::Signal<ui::Value> const& row) {
            return ui::text({.text = [&row, column] { return fieldOf(row.get(), "c" + std::to_string(column)); }});
        };
        ui::TableOptions tableOptions;
        tableOptions.virtualize = ui::Virtualization{.overscan = 0, .columnOverscan = 0};
        tableOptions.arrangement = ui::TableArrangement{.visible = {}, .pinned = options.pinned, .widths = {}};
        tableOptions.status = ui::TableStatus{.counts = grouped(rowCount) + " rows", .shown = true};
        tableOptions.selectionMode = options.selection;
        tableOptions.onSelectRange = [this](std::size_t anchor, std::size_t focus, ui::SelectionChange change) {
            ranges.push_back(Range{.anchor = anchor, .focus = focus, .change = change});
        };
        view.emplace(runtime, backend, ui::table(std::move(columns), std::move(source), std::move(tableOptions)));
        table = &dynamic_cast<TableImpl&>(WidgetBase::of(view->root()));
        settle();
    }
    TuiWindowFixture(TuiWindowFixture const&) = delete;
    TuiWindowFixture& operator=(TuiWindowFixture const&) = delete;
    TuiWindowFixture(TuiWindowFixture&&) = delete;
    TuiWindowFixture& operator=(TuiWindowFixture&&) = delete;
    ~TuiWindowFixture() { view.reset(); }

    // One frame, as the frontend draws it after an event: the owner's work is not run.
    void draw() {
        backend.fit();
        screen.draw();
    }

    // Frames and the owner's turns until a frame changes nothing and the owner has nothing left to run.
    void settle() {
        Rows last;
        for (int turn = 0; turn < 64; ++turn) {
            draw();
            std::size_t const ran = owner.runAll();
            Rows lines = morph::tui::testing::rowsOf(screen);
            if (ran == 0 && lines == last) {
                return;
            }
            last = std::move(lines);
        }
        FAIL("the table did not settle");
    }

    void scrollRows(std::size_t first) {
        static_cast<void>(table->wheel(static_cast<int>(first) - static_cast<int>(firstRow)));
        firstRow = first;
        settle();
    }

    void scrollColumns(int scrollX) {
        table->scrollColumnsTo(scrollX);
        settle();
    }

    [[nodiscard]] std::size_t cellWidgetsAlive() const {
        std::size_t alive = 0;
        for (WidgetBase const* const row : table->children()) {
            alive += dynamic_cast<ContainerBase const&>(*row).children().size();
        }
        return alive;
    }

    [[nodiscard]] std::string screenLine(std::size_t line) const {
        Rows const lines = morph::tui::testing::rowsOf(screen);
        return line < lines.size() ? lines.at(line) : std::string{};
    }

    struct Range {
        std::size_t anchor = 0;
        std::size_t focus = 0;
        ui::SelectionChange change = ui::SelectionChange::Replace;
        bool operator==(Range const& other) const = default;
    };

    morph::testing::StepExecutor owner;
    morph::reactive::Runtime runtime{owner};
    std::vector<Range> ranges;
    core::tui::Terminal terminal;
    core::tui::Screen screen;
    morph::tui::Backend backend;
    std::shared_ptr<FakeRows> rows;
    std::optional<ui::Mounted> view;
    TableImpl* table = nullptr;
    std::size_t firstRow = 0;
};

// The text in the slot of each of the ten columns in view on @p line, trimmed: the header's labels on line 0.
std::vector<std::string> slotsOf(std::string const& line) {
    std::vector<std::string> slots;
    for (std::size_t at = 2; at < 2 + (10 * 12); at += 12) {
        std::string slot = at < line.size() ? line.substr(at, 11) : std::string{};
        while (!slot.empty() && slot.back() == ' ') {
            slot.pop_back();
        }
        slots.push_back(std::move(slot));
    }
    return slots;
}

// Whether every cell drawn on lines 1 to 30 is its own row's, under its own column's header, or blank.
bool cellsUnderTheirHeaders(TuiWindowFixture const& f) {
    std::vector<std::string> const headers = slotsOf(f.screenLine(0));
    for (std::size_t line = 1; line <= 30; ++line) {
        std::vector<std::string> const cells = slotsOf(f.screenLine(line));
        for (std::size_t at = 0; at < cells.size(); ++at) {
            std::string const own = "r" + std::to_string(f.firstRow + line - 1) + "c" +
                                    (headers.at(at).empty() ? std::string{} : headers.at(at).substr(1));
            if (!cells.at(at).empty() && cells.at(at) != own) {
                UNSCOPED_INFO("line " << line << ", slot " << at << ": " << cells.at(at) << ", not " << own);
                return false;
            }
        }
    }
    return true;
}

}  // namespace

// Mutation: the terminal table never reporting the columns in view (every column realized: 300 cells a row).
// Gate U7.
TEST_CASE("tui table: cell widgets alive are rows in view times columns in view", "[tui]") {
    TuiWindowFixture f{100'000, 300, {.pinned = 0}};
    f.scrollRows(5'000);
    f.scrollColumns(100 * 12);
    CHECK(f.cellWidgetsAlive() == std::size_t{30} * 10);  // the terminal keeps no overscan
    CHECK(f.screenLine(31).starts_with("100,000 rows"));  // the status line, last
}

// Mutation: the terminal table ignoring setCellLoad (the lines show neither mark).
TEST_CASE("tui table: an unloaded cell draws its placeholder and a failed one its mark", "[tui]") {
    TuiWindowFixture f{100, 4, {.pinned = 0, .levels = {{.column = 1, .level = "recipe"}}}};
    f.rows->unload(0, "recipe");
    f.rows->fail(1, "recipe");
    f.settle();
    CHECK(f.screenLine(1).contains("···"));
    CHECK(f.screenLine(2).contains('!'));
    CHECK_FALSE(f.screenLine(3).contains("···"));
}

// Mutations: `unit` ignored (the header reads `C0` alone); `invalid` ignored (the cell's text is drawn and no mark).
TEST_CASE("tui table: a header with a unit reads Mass [kg]", "[tui][table]") {
    TuiWindowFixture f{100, 4, {.pinned = 0, .units = {"kg"}}};
    CHECK(f.screenLine(0).contains("C0 [kg]"));
    CHECK_FALSE(f.screenLine(0).contains("C1 ["));
    REQUIRE(f.screenLine(1).contains("r0c1"));
    auto& row = dynamic_cast<ui::Widget&>(*f.table->children().front());
    f.table->setCellAccent(row, 1,
                           ui::CellAccent{.tone = {}, .tooltip = "The value could not be read", .invalid = true});
    f.settle();
    CHECK_FALSE(f.screenLine(1).contains("r0c1"));
    CHECK(f.screenLine(1).contains('!'));
    CHECK(f.screenLine(1).contains("r0c2"));
    f.table->setCellAccent(row, 1, ui::CellAccent{});
    f.settle();
    CHECK(f.screenLine(1).contains("r0c1"));
}

// Mutation: the terminal table placing a source table's cells by their order in the row, not by `setCellColumn` (a
// reused row's parked cells draw the row it showed before, under other columns).
TEST_CASE("tui table: a reused row shows its own cells under their columns while the frame budget holds some back",
          "[tui]") {
    TuiWindowFixture f{100'000, 300, {.pinned = 0}};
    f.scrollColumns(20 * 12);
    REQUIRE(cellsUnderTheirHeaders(f));
    // 300 cells to rebind, over the budget of 240: the rows far from the centre wait a turn with cells parked, and a
    // parked cell is not drawn. (Their bindings flush after that turn, so the text drawn here is not checked.)
    static_cast<void>(f.table->wheel(5'000));
    f.firstRow = 5'000;
    f.draw();
    REQUIRE(f.owner.pending() > 0);
    std::size_t drawn = 0;
    for (std::size_t line = 1; line <= 30; ++line) {
        for (std::string const& cell : slotsOf(f.screenLine(line))) {
            drawn += cell.empty() ? 0U : 1U;
        }
    }
    CHECK(drawn == 240);
    f.settle();
    CHECK(cellsUnderTheirHeaders(f));
    CHECK(slotsOf(f.screenLine(30)).back() == "r5029c29");
}

// Mutation: the terminal table moving its own cursor for the arrows over a source table (the mount's focus never
// moves, and no line shows `>`).
TEST_CASE("tui table: the arrows and PageDown move the focused cell through the mount, and the view follows it",
          "[tui]") {
    TuiWindowFixture f{100'000, 300, {.pinned = 0}};
    f.screen.setFocus(&WidgetBase::of(*f.table).view());
    static_cast<void>(f.screen.dispatchEvent(core::tui::test::specialKey(KeyCode::Down)));
    static_cast<void>(f.screen.dispatchEvent(core::tui::test::specialKey(KeyCode::Down)));
    f.settle();
    CHECK(f.screenLine(3).starts_with(" >"));
    static_cast<void>(f.screen.dispatchEvent(core::tui::test::specialKey(KeyCode::PageDown)));
    f.settle();
    // Row 32: the view moved down three rows to show it on its last line.
    CHECK(f.screenLine(30).starts_with(" >"));
    CHECK(slotsOf(f.screenLine(30)).front() == "r32c0");
}

// Mutation: the terminal table taking a realized window as the mark of a source table (a long `rows` table draws its
// header over blank lines).
TEST_CASE("tui table: a rows table of 2,000 rows draws its rows", "[tui]") {
    morph::testing::StepExecutor owner;
    morph::reactive::Runtime runtime{owner};
    core::tui::Terminal terminal{std::make_unique<core::tui::MockTerminalOutput>(40, 12)};
    core::tui::Screen screen{terminal};
    morph::tui::Backend backend{screen};
    std::vector<std::int64_t> numbers(2'000);
    std::ranges::iota(numbers, std::int64_t{0});
    morph::reactive::Signal<std::vector<std::int64_t>> rows{runtime, numbers};
    ui::Mounted const view{
        runtime, backend,
        ui::table<std::int64_t>(
            {{.label = "Name"}}, [&rows] { return rows.get(); }, [](std::int64_t const& row) { return ui::Key{row}; },
            [](morph::reactive::Signal<std::int64_t> const& row) {
                return std::vector<ui::Node>{
                    ui::text({.text = [&row] { return "row " + std::to_string(row.get()); }})};
            })};
    owner.runAll();
    backend.fit();
    screen.draw();
    Rows const lines = morph::tui::testing::rowsOf(screen);
    REQUIRE(lines.size() >= 3);
    CHECK(lines.at(1).contains("row 0"));
    CHECK(lines.at(2).contains("row 1"));
}

// Mutation: Shift with the arrows, a Shift-click and Ctrl+A over a source handled as on a short table (no range
// reaches the mount; Ctrl+A selects the thirty realized rows).
TEST_CASE("tui table: ranges and Ctrl+A over a source go to the mount as view rows", "[tui]") {
    TuiWindowFixture f{100'000, 300, {.pinned = 0, .selection = ui::SelectionMode::Multiple}};
    f.screen.setFocus(&WidgetBase::of(*f.table).view());
    static_cast<void>(f.screen.dispatchEvent(core::tui::test::specialKey(KeyCode::Down)));  // focus row 1
    f.settle();
    static_cast<void>(f.screen.dispatchEvent(core::tui::test::specialKey(KeyCode::Down, core::tui::Modifier::Shift)));
    static_cast<void>(f.screen.dispatchEvent(core::tui::test::specialKey(KeyCode::Down, core::tui::Modifier::Shift)));
    f.settle();
    using Range = TuiWindowFixture::Range;
    REQUIRE(f.ranges.size() == 2);
    CHECK(f.ranges.back() == Range{.anchor = 1, .focus = 3, .change = ui::SelectionChange::Replace});
    static_cast<void>(f.screen.dispatchEvent(core::tui::test::charKey('a', core::tui::Modifier::Ctrl)));
    f.settle();
    CHECK(f.ranges.back() == Range{.anchor = 0, .focus = 99'999, .change = ui::SelectionChange::Extend});
    // A click on view row 4 makes it the anchor; a Shift-click on view row 9 selects from it.
    using Type = core::tui::MouseEvent::Type;
    auto const click = [&f](int line, core::tui::Modifier modifiers) {
        static_cast<void>(f.screen.dispatchEvent(
            core::tui::MouseEvent{.type = Type::Press, .button = 0, .x = 10, .y = line + 1, .modifiers = modifiers}));
        static_cast<void>(f.screen.dispatchEvent(core::tui::MouseEvent{
            .type = Type::Release, .button = 0, .x = 10, .y = line + 1, .modifiers = modifiers}));
        f.settle();
    };
    click(5, core::tui::Modifier::None);
    click(10, core::tui::Modifier::Shift);
    CHECK(f.ranges.back() == Range{.anchor = 4, .focus = 9, .change = ui::SelectionChange::Replace});
}

namespace {

/// The cell of the last @p glyph on @p line, counting a character a cell; none when the line has none.
std::optional<int> lastCellOf(std::string const& line, char32_t glyph) {
    std::u32string const cells = unicode::convert_to<char32_t>(std::string_view{line});
    std::size_t const found = cells.rfind(glyph);
    return found == std::u32string::npos ? std::nullopt : std::optional<int>{static_cast<int>(found)};
}

/// A table of a filterable Name and a stretching filterable Qty, one row, grouped as `Lot`, with menus.
struct Configured {
    explicit Configured(Harness& harness) : table{harness.make<TableImpl>(nullptr)} {
        table->setColumns({{.label = "Name", .filterable = true},
                           {.label = "Qty", .width = ui::Sizing::stretch(1), .filterable = true}});
        row = harness.make<StackImpl>(table.get(), ui::Axis::Horizontal);
        for (std::string const text : {"apple", "3"}) {
            cells.push_back(harness.make<TextImpl>(row.get()));
            cells.back()->setText(text);
        }
        table->setRowKey(*row, key(1));
        table->setFiltered({1});
        table->setColumnGroups({{.label = "Lot", .first = 0, .count = 2}});
        table->setOnFilter([this](std::size_t column) { reported.push_back("filter " + std::to_string(column)); });
        table->setOnGroupFilter([this](std::size_t group) { reported.push_back("group " + std::to_string(group)); });
        table->setMenu(
            [](std::optional<std::size_t> column) {
                return std::vector<ui::MenuEntry>{{.label = column ? "Hide " + std::to_string(*column) : "Columns"}};
            },
            [this](std::optional<std::size_t> column, std::vector<std::size_t> path) {
                reported.push_back("menu " + (column ? std::to_string(*column) : std::string{"-"}) + "/" +
                                   std::to_string(path.front()));
            });
    }
    ~Configured() {
        cells.clear();
        row.reset();
    }
    Configured(Configured const&) = delete;
    Configured& operator=(Configured const&) = delete;
    Configured(Configured&&) = delete;
    Configured& operator=(Configured&&) = delete;

    std::unique_ptr<TableImpl> table;
    std::unique_ptr<StackImpl> row;
    std::vector<std::unique_ptr<TextImpl>> cells;
    std::vector<std::string> reported;
};

/// Clicks the first row of the open menu list.
void clickFirstEntry(Harness& harness, TableImpl const& table) {
    auto const* const popup = table.openPopup();
    REQUIRE(popup != nullptr);
    auto const area = popup->screenBounds();
    static_cast<void>(harness.click({.x = area.x, .y = area.y}));
}

}  // namespace

// Mutation: keep no room for the `⋯` (`TableImpl::menuRoom` answers 0): it is drawn over Qty's filled filter button,
// and a click there opens the table's menu.
TEST_CASE("tui table: filter buttons, the group line and the table menu's button are drawn apart, and each reports",
          "[tui][table]") {
    Harness harness{30, 5};
    Configured configured{harness};
    Rows const drawn = harness.draw();
    REQUIRE(drawn.size() >= 3);
    // The group line over both columns ends in the group's empty button; the header line ends in Qty's filled button,
    // a gap, and the `⋯`.
    CHECK(drawn.at(0).starts_with("  Lot"));
    CHECK(drawn.at(1).starts_with("  Name ▿"));
    CHECK(drawn.at(1).ends_with("▾ ⋯"));
    auto const qtyButton = lastCellOf(drawn.at(1), U'▾');
    auto const nameButton = lastCellOf(drawn.at(1), U'▿');
    auto const groupButton = lastCellOf(drawn.at(0), U'▿');
    auto const menuButton = lastCellOf(drawn.at(1), U'⋯');
    REQUIRE(qtyButton);
    REQUIRE(nameButton);
    REQUIRE(groupButton);
    REQUIRE(menuButton);
    CHECK(*menuButton == 29);
    static_cast<void>(harness.click({.x = *qtyButton, .y = 1}));
    static_cast<void>(harness.click({.x = *nameButton, .y = 1}));
    static_cast<void>(harness.click({.x = *groupButton, .y = 0}));
    static_cast<void>(harness.draw());
    static_cast<void>(harness.click({.x = *menuButton, .y = 1}));
    static_cast<void>(harness.draw());
    CHECK(configured.table->openMenuEntries().size() == 1);
    clickFirstEntry(harness, *configured.table);
    CHECK(configured.reported == std::vector<std::string>{"filter 1", "filter 0", "group 0", "menu -/0"});
}

// Mutation: make `TableImpl::contextPress` take nothing (the right press opens no menu).
TEST_CASE("tui table: a right click on a header opens its menu, and Shift+F10 on the focused header does too",
          "[tui][table]") {
    Harness harness{30, 5};
    Configured configured{harness};
    static_cast<void>(harness.draw());
    using Type = core::tui::MouseEvent::Type;
    // Name's header, on the line under the group line.
    static_cast<void>(harness.send(core::tui::MouseEvent{.type = Type::Press, .button = 2, .x = 3, .y = 2}));
    static_cast<void>(harness.send(core::tui::MouseEvent{.type = Type::Release, .button = 2, .x = 3, .y = 2}));
    static_cast<void>(harness.draw());
    REQUIRE(configured.table->openMenuEntries().size() == 1);
    CHECK(configured.table->openMenuEntries().front().label == "Hide 0");
    clickFirstEntry(harness, *configured.table);
    static_cast<void>(harness.draw());
    harness.focus(*configured.table);
    CHECK(harness.key(KeyCode::Up) == EventResult::Handled);
    CHECK(harness.key(KeyCode::Right) == EventResult::Handled);
    CHECK(harness.key(KeyCode::F10, core::tui::Modifier::Shift) == EventResult::Handled);
    static_cast<void>(harness.draw());
    REQUIRE(configured.table->openMenuEntries().size() == 1);
    CHECK(configured.table->openMenuEntries().front().label == "Hide 1");
    // The list has the focus: Enter chooses its entry.
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(configured.reported == std::vector<std::string>{"menu 0/0", "menu 1/0"});
}

// Mutation: leave an open menu open in `TableImpl::openMenu` (the second right click opens nothing, and the first
// header's menu stays).
TEST_CASE("tui table: a right click on another header while a menu is open closes it and opens that header's menu",
          "[tui][table]") {
    Harness harness{30, 5};
    Configured configured{harness};
    static_cast<void>(harness.draw());
    using Type = core::tui::MouseEvent::Type;
    auto const rightClick = [&harness](int x, int y) {
        static_cast<void>(harness.send(core::tui::MouseEvent{.type = Type::Press, .button = 2, .x = x, .y = y}));
        static_cast<void>(harness.send(core::tui::MouseEvent{.type = Type::Release, .button = 2, .x = x, .y = y}));
        static_cast<void>(harness.draw());
    };
    // Name's header, then Qty's, on the line under the group line (1-based cells).
    rightClick(3, 2);
    REQUIRE(configured.table->openMenuEntries().size() == 1);
    CHECK(configured.table->openMenuEntries().front().label == "Hide 0");
    auto const qty = lastCellOf(harness.draw().at(1), U'Q');
    REQUIRE(qty);
    rightClick(*qty + 1, 2);
    REQUIRE(configured.table->openMenuEntries().size() == 1);
    CHECK(configured.table->openMenuEntries().front().label == "Hide 1");
    clickFirstEntry(harness, *configured.table);
    CHECK(configured.reported == std::vector<std::string>{"menu 1/0"});
}

// Mutation: `setOnStatusEntry` ignored, under which Alt+1 reports nothing.
TEST_CASE("tui table: select-all asks the handler; a status entry reports its id", "[tui]") {
    TuiWindowFixture f{100, 30, {.pinned = 0, .selection = ui::SelectionMode::Multiple}};
    int asked = 0;
    std::vector<std::string> chosen;
    f.table->setOnSelectAll([&asked] { ++asked; });
    f.table->setOnStatusEntry([&chosen](std::string id) { chosen.push_back(std::move(id)); });
    f.screen.setFocus(&WidgetBase::of(*f.table).view());
    static_cast<void>(f.screen.dispatchEvent(core::tui::test::charKey('a', core::tui::Modifier::Ctrl)));
    f.settle();
    CHECK(asked == 1);
    f.table->setStatus(ui::TableStatus{
        .counts = "100 rows",
        .message = "m",
        .entries = {{.id = "run", .label = "Run on these"}},
        .standing = "1 row could not be saved",
        .standingEntries = {{.id = "select", .label = "Select them"}, {.id = "clear", .label = "Clear marks"}},
        .shown = true});
    f.settle();
    std::string const line = f.screenLine(31);
    CHECK(line.contains("[Alt+1 Select them]"));
    CHECK(line.contains("[Alt+2 Clear marks]"));
    CHECK(line.contains("[Alt+3 Run on these]"));
    static_cast<void>(f.screen.dispatchEvent(core::tui::test::charKey('2', core::tui::Modifier::Alt)));
    f.settle();
    CHECK(chosen == std::vector<std::string>{"clear"});
}

namespace {

using morph::tui::detail::WidgetBase;

// A grid of two specimens (`GridFixture`) between two text inputs, on the terminal backend: its columns are every
// member in schema order, label first.
struct TuiGridFixture {
    explicit TuiGridFixture(std::string schema = std::string{morph::test::grid::kSpecimens})
        : f{2, {}, std::move(schema)},
          terminal{std::make_unique<core::tui::MockTerminalOutput>(100, 12)},
          screen{terminal},
          backend{screen} {
        ui::TextInput before;
        before.common.testId = "before";
        ui::TextInput after;
        after.common.testId = "after";
        view.emplace(f.h.rt, backend,
                     ui::column(ui::Column{.children = {ui::textInput(std::move(before)),
                                                        f.grid->node(morph::test::grid::columnsOf(f), {}),
                                                        ui::textInput(std::move(after))}}));
        auto const& column = dynamic_cast<morph::tui::detail::ContainerBase const&>(WidgetBase::of(view->root()));
        table = dynamic_cast<TableImpl*>(column.children()[1]);
        afterInput = column.children()[2];
        settle();
    }
    TuiGridFixture(TuiGridFixture const&) = delete;
    TuiGridFixture& operator=(TuiGridFixture const&) = delete;
    TuiGridFixture(TuiGridFixture&&) = delete;
    TuiGridFixture& operator=(TuiGridFixture&&) = delete;
    ~TuiGridFixture() { view.reset(); }

    void settle() {
        for (int turn = 0; turn < 32; ++turn) {
            backend.fit();
            screen.draw();
            if (f.h.owner.runAll() == 0) {
                backend.fit();
                screen.draw();
                return;
            }
        }
        FAIL("the grid did not settle");
    }

    void send(core::tui::InputEvent const& event) {
        static_cast<void>(screen.dispatchEvent(event));
        settle();
    }

    [[nodiscard]] std::string line(std::size_t at) const {
        Rows const lines = morph::tui::testing::rowsOf(screen);
        return at < lines.size() ? lines.at(at) : std::string{};
    }

    morph::test::grid::GridFixture f;
    core::tui::Terminal terminal;
    core::tui::Screen screen;
    morph::tui::Backend backend;
    std::optional<ui::Mounted> view;
    TableImpl* table = nullptr;
    WidgetBase* afterInput = nullptr;
};

}  // namespace

// Mutation: the terminal table taking focus in an open popup of a widget inside it for focus leaving (the dropdown's
// list taking the focus commits and closes the editor).
TEST_CASE("tui grid: focus in an editor's own popup does not leave the table", "[tui][grid]") {
    TuiGridFixture g{morph::test::grid::specimensWithChoices()};
    REQUIRE(g.table != nullptr);
    g.f.grid->focusCell(morph::test::grid::at(1, "unit"));
    g.settle();
    g.screen.setFocus(&g.table->view());
    g.send(core::tui::test::specialKey(KeyCode::F2));
    REQUIRE(g.f.grid->editing() == morph::test::grid::at(1, "unit"));
    g.send(core::tui::test::specialKey(KeyCode::Down));
    // The list has the focus, in the screen's overlay rather than under the table.
    CHECK_FALSE(morph::tui::detail::isWithin(g.screen.focusedComponent(), g.table->view()));
    CHECK(g.f.grid->editing() == morph::test::grid::at(1, "unit"));
}

// Mutation: the terminal table reporting the second click of a double click twice, once as a single click (the check
// box flips back: its value is false).
TEST_CASE("tui grid: a double click on a Boolean cell flips it once", "[tui][grid]") {
    TuiGridFixture g{morph::test::grid::specimensWithChoices()};
    REQUIRE(g.table != nullptr);
    std::size_t headerLine = 0;
    while (headerLine < 12 && !g.line(headerLine).contains("checked")) {
        ++headerLine;
    }
    std::size_t const checkedAt = g.line(headerLine).find("checked");
    REQUIRE(checkedAt != std::string::npos);
    using Type = core::tui::MouseEvent::Type;
    int const x = static_cast<int>(checkedAt) + 2;
    int const y = static_cast<int>(headerLine) + 2;  // the first row, 1-based
    for (int click = 0; click < 2; ++click) {
        static_cast<void>(
            g.screen.dispatchEvent(core::tui::MouseEvent{.type = Type::Press, .button = 0, .x = x, .y = y}));
        static_cast<void>(
            g.screen.dispatchEvent(core::tui::MouseEvent{.type = Type::Release, .button = 0, .x = x, .y = y}));
    }
    g.settle();
    CHECK(g.f.grid->focus() == morph::test::grid::at(1, "checked"));
    CHECK(g.f.json("specimens[0].checked") == "true");
}

// Mutation: the terminal table ignoring setOnText (the keystroke moves nothing and opens no editor).
TEST_CASE("tui grid: typing on a focused cell opens its editor, Enter commits and moves down", "[tui][grid]") {
    TuiGridFixture g;
    REQUIRE(g.table != nullptr);
    g.f.grid->focusCell(morph::test::grid::at(1, "mass"));
    g.settle();
    g.screen.setFocus(&g.table->view());
    g.send(core::tui::test::charKey('5'));
    REQUIRE(g.f.grid->editing() == morph::test::grid::at(1, "mass"));
    // The editor has the focus, inside the table, and row 1 shows what it holds.
    auto const* const focused = g.screen.focusedComponent();
    CHECK(focused != &g.table->view());
    CHECK(morph::tui::detail::isWithin(focused, g.table->view()));
    std::size_t headerLine = 0;
    while (headerLine < 12 && !g.line(headerLine).contains("mass")) {
        ++headerLine;
    }
    std::size_t const massAt = g.line(headerLine).find("mass");
    REQUIRE(massAt != std::string::npos);
    CHECK(g.line(headerLine + 1).substr(massAt, 6).contains('5'));
    g.send(core::tui::test::specialKey(KeyCode::Enter));
    CHECK(g.f.form->field("specimens[0].mass").draft() == "5");
    CHECK_FALSE(g.f.grid->editing().has_value());
    CHECK(g.f.grid->focus() == morph::test::grid::at(2, "mass"));
    g.f.grid->focusCell(morph::test::grid::at(2, "note"));
    g.settle();
    g.screen.setFocus(&g.table->view());
    g.send(core::tui::test::specialKey(KeyCode::Tab));
    CHECK(g.screen.focusedComponent() == &g.afterInput->view());
}

// Mutation: the terminal table drawing the band over the declared columns' widths instead of the runs `setBands` gave
// (the label starts over column 0).
TEST_CASE("tui table: bands draw above the header", "[tui][table][bands]") {
    Harness harness{60, 6};
    auto table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "Name", .width = ui::Sizing::fixed(6)},
                       {.label = "M8", .width = ui::Sizing::fixed(8)},
                       {.label = "M4", .width = ui::Sizing::fixed(8)},
                       {.label = "M2", .width = ui::Sizing::fixed(8)}});
    auto row = harness.make<StackImpl>(table.get(), ui::Axis::Horizontal);
    std::vector<std::unique_ptr<TextImpl>> cells;
    for (std::string const text : {"a", "1", "2", "3"}) {
        cells.push_back(harness.make<TextImpl>(row.get()));
        cells.back()->setText(text);
    }
    table->setRowKey(*row, key(1));
    table->setBands({{.level = 0, .label = "Mass retained", .unit = "g", .first = 1, .count = 3, .pinned = false}});
    Rows const drawn = harness.draw();
    REQUIRE(drawn.size() >= 2);
    std::size_t const m8 = drawn.at(1).find("M8");
    REQUIRE(m8 != std::string::npos);
    CHECK(drawn.at(0).find("Mass retained [g]") == m8);  // over columns 1 to 3, not over Name
    CHECK(drawn.at(1).contains("Name"));
    table->setBands({});
    CHECK(harness.draw().at(0).contains("Name"));  // no band line once none is given
    cells.clear();
    row.reset();
}

// Mutations: the terminal table covering only a span's own column (B1 and C1 still drawn); a summary row drawn with a
// data row's gutter (no `Σ`).
TEST_CASE("tui table: a span across columns and a summary row", "[tui][table][spans][summary]") {
    Harness harness{60, 6};
    auto table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "A", .width = ui::Sizing::fixed(6)},
                       {.label = "B", .width = ui::Sizing::fixed(6)},
                       {.label = "C", .width = ui::Sizing::fixed(6)},
                       {.label = "D", .width = ui::Sizing::fixed(6)}});
    std::vector<std::unique_ptr<StackImpl>> rows;
    std::vector<std::unique_ptr<TextImpl>> cells;
    std::vector<std::vector<std::string>> const texts{{"wide text here", "B1", "C1", "D1"}, {"A2", "B2", "C2", "D2"}};
    for (std::size_t at = 0; at < texts.size(); ++at) {
        rows.push_back(harness.make<StackImpl>(table.get(), ui::Axis::Horizontal));
        for (std::string const& text : texts.at(at)) {
            cells.push_back(harness.make<TextImpl>(rows.back().get()));
            cells.back()->setText(text);
        }
        table->setRowKey(*rows.back(), key(static_cast<std::int64_t>(at) + 1));
    }
    table->setRowSummary(*rows.at(1));
    table->setRowSpans(*rows.at(0), {ui::RowSpan{.column = 0, .rows = 1, .columns = 3}});
    Rows const drawn = harness.draw();
    REQUIRE(drawn.size() >= 3);
    CHECK(drawn.at(1).contains("wide text here"));
    CHECK(!drawn.at(1).contains("B1"));
    CHECK(!drawn.at(1).contains("C1"));
    CHECK(drawn.at(1).contains("D1"));
    CHECK(drawn.at(2).starts_with("\u03A3"));
    CHECK(drawn.at(2).contains("A2"));
    cells.clear();
    rows.clear();
}
