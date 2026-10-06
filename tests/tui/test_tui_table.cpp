// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "tui/container_widgets.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui/table_widget.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using core::tui::EventResult;
using core::tui::KeyCode;
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
    fruit.table->setOnSelectionChange([&](std::vector<ui::Key> keys) { selections.push_back(std::move(keys)); });
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
    fruit.table->setOnSelectionChange([&](std::vector<ui::Key> keys) { selections.push_back(std::move(keys)); });
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

TEST_CASE("tui table: a hidden cell still counts toward its column's width", "[tui][table][columns]") {
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
    CHECK(harness.draw() == Rows{"  Note        Name", "              a"});
}

TEST_CASE("tui table: a label wider than its column is cut at the column's edge", "[tui][table][columns]") {
    Harness harness{30, 3};
    auto const table = harness.make<TableImpl>(nullptr);
    table->setColumns({{.label = "Quantity", .width = ui::Sizing::fixed(3)}, {.label = "Name"}});
    CHECK(harness.draw() == Rows{"  Qua Name"});
}

TEST_CASE("tui table: a Multiple-mode user change reports exactly the existing rows selected",
          "[tui][table][selection]") {
    Harness harness{30, 6};
    Fruit fruit{harness, ui::SelectionMode::Multiple};
    std::vector<std::vector<ui::Key>> selections;
    fruit.table->setOnSelectionChange([&](std::vector<ui::Key> keys) { selections.push_back(std::move(keys)); });
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
    fruit.table->setOnSelectionChange([&](std::vector<ui::Key> const& /*keys*/) { ++reports; });
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

TEST_CASE("tui table: a click selects a row, toggles it in Multiple mode, and a double click activates",
          "[tui][table][click]") {
    Harness harness{30, 5};
    std::vector<std::vector<ui::Key>> selections;
    std::vector<ui::Key> activated;

    SECTION("Single") {
        Fruit fruit{harness, ui::SelectionMode::Single};
        fruit.table->setOnSelectionChange([&](std::vector<ui::Key> keys) { selections.push_back(std::move(keys)); });
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
        fruit.table->setOnSelectionChange([&](std::vector<ui::Key> keys) { selections.push_back(std::move(keys)); });
        fruit.table->setOnActivate([&](ui::Key activatedKey) { activated.push_back(std::move(activatedKey)); });
        static_cast<void>(harness.draw());
        CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
        CHECK(harness.click({.x = 3, .y = 2}) == EventResult::Handled);
        CHECK(selections == std::vector<std::vector<ui::Key>>{{key(1)}, {key(1), key(2)}});
        CHECK(activated.empty());
        CHECK(harness.click({.x = 3, .y = 2}) == EventResult::Handled);
        CHECK(activated == std::vector<ui::Key>{key(2)});
        CHECK(selections.size() == 2);
        CHECK(harness.draw() == Rows{"  Name  Qty", "* apple 3", "*>kiwi  12"});
    }
    SECTION("None") {
        Fruit fruit{harness, ui::SelectionMode::None};
        fruit.table->setOnSelectionChange([&](std::vector<ui::Key> keys) { selections.push_back(std::move(keys)); });
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
    fruit.table->setOnSelectionChange([&](std::vector<ui::Key> keys) { selections.push_back(std::move(keys)); });
    fruit.table->setOnActivate([&](ui::Key const& /*key*/) { ++activations; });
    auto now = std::chrono::steady_clock::time_point{};
    harness.context().now = [&now] { return now; };
    static_cast<void>(harness.draw());
    CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
    now += std::chrono::milliseconds{501};
    CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
    CHECK(selections == std::vector<std::vector<ui::Key>>{{key(1)}, {}});
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
    fruit.table->setOnSelectionChange([&](std::vector<ui::Key> const& /*keys*/) { ++reports; });
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
    fruit.table->setOnSelectionChange([&](std::vector<ui::Key> keys) { selections.push_back(std::move(keys)); });
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
        fruit.table->setOnSelectionChange([&](std::vector<ui::Key> const& /*keys*/) { ++calls; });
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
    fruit.table->setOnSelectionChange([&](std::vector<ui::Key> const& /*keys*/) { ++calls; });
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
        fruit->table->setOnSelectionChange([&](std::vector<ui::Key> const& /*keys*/) {
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
        fruit.table->setOnSelectionChange([&](std::vector<ui::Key> const& /*keys*/) { ++calls; });
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
