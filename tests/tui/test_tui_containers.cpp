// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Rect.hpp>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "tui/container_widgets.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using core::tui::EventResult;
using core::tui::KeyCode;
using morph::tui::detail::ButtonImpl;
using morph::tui::detail::Direction;
using morph::tui::detail::GridImpl;
using morph::tui::detail::PanelImpl;
using morph::tui::detail::ScrollImpl;
using morph::tui::detail::StackImpl;
using morph::tui::detail::TextImpl;
using morph::tui::detail::WidgetBase;
using morph::tui::testing::Harness;
using Rows = std::vector<std::string>;

namespace {

/// A mouse event of @p type on a 0-based viewport cell.
core::tui::MouseEvent mouseAt(core::tui::MouseEvent::Type type, core::tui::Point cell) {
    return core::tui::MouseEvent{.type = type, .button = 0, .x = cell.x + 1, .y = cell.y + 1};
}

}  // namespace

TEST_CASE("tui containers: a grid places cells row-major, a span covers columns and the gap", "[tui][containers]") {
    Harness harness{21, 3};
    auto const grid = harness.make<GridImpl>(nullptr);
    grid->setColumns(2);
    grid->setGap(1);
    auto const first = harness.make<TextImpl>(grid.get());
    first->setText("a");
    auto const second = harness.make<TextImpl>(grid.get());
    second->setText("b");
    auto const wide = harness.make<TextImpl>(grid.get());
    wide->setText("c");
    grid->setSpan(*wide, 2);
    CHECK(harness.draw() == Rows{"a          b", "", "c"});
    CHECK(WidgetBase::of(*wide).view().screenBounds().width == 21);
}

TEST_CASE("tui containers: a hidden grid child leaves no cell, and the cells after it move up", "[tui][containers]") {
    Harness harness{21, 3};
    auto const grid = harness.make<GridImpl>(nullptr);
    grid->setColumns(2);
    grid->setGap(1);
    auto const first = harness.make<TextImpl>(grid.get());
    first->setText("a");
    auto const second = harness.make<TextImpl>(grid.get());
    second->setText("b");
    auto const third = harness.make<TextImpl>(grid.get());
    third->setText("c");
    CHECK(harness.draw() == Rows{"a          b", "", "c"});
    first->setVisible(false);
    CHECK(harness.draw() == Rows{"b          c"});
    first->setVisible(true);
    CHECK(harness.draw() == Rows{"a          b", "", "c"});
}

TEST_CASE("tui containers: moveChild reorders a grid's cells, and a span follows its child", "[tui][containers]") {
    Harness harness{21, 3};
    auto const grid = harness.make<GridImpl>(nullptr);
    grid->setColumns(2);
    grid->setGap(1);
    auto const first = harness.make<TextImpl>(grid.get());
    first->setText("a");
    auto const second = harness.make<TextImpl>(grid.get());
    second->setText("b");
    auto const wide = harness.make<TextImpl>(grid.get());
    wide->setText("c");
    grid->setSpan(*wide, 2);
    grid->moveChild(*wide, 0);
    CHECK(harness.draw() == Rows{"c", "", "a          b"});
}

TEST_CASE("tui containers: setSpan of a widget that is not the grid's child throws", "[tui][containers]") {
    Harness harness{21, 3};
    auto const grid = harness.make<GridImpl>(nullptr);
    auto const other = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const stranger = harness.make<TextImpl>(other.get());
    CHECK_THROWS_AS(grid->setSpan(*stranger, 2), std::logic_error);
}

TEST_CASE("tui containers: a grid cell's fixed width narrows it inside its slot", "[tui][containers]") {
    Harness harness{20, 2};
    auto const grid = harness.make<GridImpl>(nullptr);
    grid->setColumns(2);
    auto const narrow = harness.make<TextImpl>(grid.get());
    narrow->setText("abcdef");
    narrow->setLayout(ui::LayoutHints{.width = ui::Sizing::fixed(3)});
    auto const full = harness.make<TextImpl>(grid.get());
    full->setText("x");
    CHECK(harness.draw() == Rows{"abc       x"});
    CHECK(WidgetBase::of(*narrow).view().screenBounds().width == 3);
    CHECK(WidgetBase::of(*full).view().screenBounds().width == 10);
}

TEST_CASE("tui containers: a panel draws its box and title around its child", "[tui][containers]") {
    Harness harness{12, 4};
    auto const panel = harness.make<PanelImpl>(nullptr);
    panel->setTitle("Info");
    auto const body = harness.make<TextImpl>(panel.get());
    body->setText("hi");
    CHECK(harness.draw() == Rows{"┌─Info─────┐", "│hi        │", "│          │", "└──────────┘"});
}

TEST_CASE("tui containers: a panel's padding insets its child", "[tui][containers]") {
    Harness harness{12, 5};
    auto const panel = harness.make<PanelImpl>(nullptr);
    panel->setTitle("Info");
    panel->setPadding(1);
    auto const body = harness.make<TextImpl>(panel.get());
    body->setText("hi");
    CHECK(harness.draw().at(2) == "│ hi       │");
}

TEST_CASE("tui containers: a collapsible panel toggles on Enter, hides its child and reports the state",
          "[tui][containers]") {
    Harness harness{14, 4};
    auto const panel = harness.make<PanelImpl>(nullptr);
    panel->setTitle("Info");
    panel->setCollapsible(true);
    panel->setCollapsed(false);
    std::vector<bool> toggles;
    panel->setOnToggle([&](bool collapsed) { toggles.push_back(collapsed); });
    auto const body = harness.make<TextImpl>(panel.get());
    body->setText("hi");
    CHECK(harness.draw().front() == "┌─▾ Info─────┐");
    harness.focus(*panel);
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(toggles == std::vector<bool>{true});
    CHECK(harness.draw() == Rows{"▸ Info"});
}

TEST_CASE("tui containers: setCollapsed hides and shows the content and calls no handler", "[tui][containers]") {
    Harness harness{14, 4};
    auto const panel = harness.make<PanelImpl>(nullptr);
    panel->setTitle("Info");
    panel->setCollapsible(true);
    int calls = 0;
    panel->setOnToggle([&](bool /*collapsed*/) { ++calls; });
    auto const body = harness.make<TextImpl>(panel.get());
    body->setText("hi");
    panel->setCollapsed(true);
    CHECK(harness.draw() == Rows{"▸ Info"});
    panel->setCollapsed(false);
    CHECK(harness.draw().at(1) == "│hi          │");
    CHECK(calls == 0);
}

TEST_CASE("tui containers: a child added to a collapsed panel stays hidden until it expands", "[tui][containers]") {
    Harness harness{14, 4};
    auto const panel = harness.make<PanelImpl>(nullptr);
    panel->setTitle("Info");
    panel->setCollapsible(true);
    panel->setCollapsed(true);
    auto const button = harness.make<ButtonImpl>(panel.get());
    button->setLabel("Go");
    CHECK(harness.draw() == Rows{"▸ Info"});
    CHECK_FALSE(WidgetBase::of(*button).view().focusable());
    panel->setCollapsed(false);
    CHECK(harness.draw().at(1) == "│[ Go ]      │");
    CHECK(WidgetBase::of(*button).view().focusable());
}

TEST_CASE("tui containers: a narrow panel shortens its title by whole characters", "[tui][containers]") {
    Harness harness{6, 3};
    auto const panel = harness.make<PanelImpl>(nullptr);
    panel->setTitle("Info");
    panel->setCollapsible(true);
    CHECK(harness.draw().front() == "┌─▾ ─┐");
}

TEST_CASE("tui containers: inside a collapsed panel nothing takes input, and the panel's own control still works",
          "[tui][containers][gating]") {
    Harness harness{14, 4};
    auto const panel = harness.make<PanelImpl>(nullptr);
    panel->setTitle("Info");
    panel->setCollapsible(true);
    std::vector<bool> toggles;
    panel->setOnToggle([&](bool collapsed) { toggles.push_back(collapsed); });
    auto const column = harness.make<StackImpl>(panel.get(), ui::Axis::Vertical);
    auto const button = harness.make<ButtonImpl>(column.get());
    button->setLabel("Go");
    int clicks = 0;
    button->setOnClick([&] { ++clicks; });
    CHECK(harness.draw().at(1) == "│[ Go ]      │");
    harness.focus(*button);

    panel->setCollapsed(true);
    CHECK(harness.draw() == Rows{"▸ Info"});
    CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
    WidgetBase::of(*button).activate();
    CHECK_FALSE(WidgetBase::of(*button).view().focusable());
    // The click lands on the collapsed panel's own view, which takes focus but toggles only on its title line.
    static_cast<void>(harness.click({.x = 2, .y = 1}));
    CHECK(clicks == 0);
    CHECK(toggles.empty());

    morph::tui::detail::moveFocus(harness.context(), Direction::Forward);
    CHECK(harness.focused(*panel));
    morph::tui::detail::moveFocus(harness.context(), Direction::Forward);
    CHECK(harness.focused(*panel));
    CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
    CHECK(toggles == std::vector<bool>{false});
    CHECK(harness.draw().at(1) == "│[ Go ]      │");
    morph::tui::detail::moveFocus(harness.context(), Direction::Forward);
    CHECK(harness.focused(*button));
    CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
    CHECK(clicks == 1);
}

TEST_CASE("tui containers: a disabled collapsible panel does not toggle", "[tui][containers][gating]") {
    Harness harness{14, 4};
    auto const panel = harness.make<PanelImpl>(nullptr);
    panel->setTitle("Info");
    panel->setCollapsible(true);
    std::vector<bool> toggles;
    panel->setOnToggle([&](bool collapsed) { toggles.push_back(collapsed); });
    static_cast<void>(harness.draw());
    harness.focus(*panel);
    panel->setEnabled(false);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
    CHECK(harness.click({.x = 3, .y = 0}) == EventResult::Ignored);
    WidgetBase::of(*panel).activate();
    CHECK(toggles.empty());
    CHECK(harness.draw().front() == "┌─▾ Info─────┐");
}

TEST_CASE("tui containers: a key a focused child ignores does not toggle the panel around it",
          "[tui][containers][gating]") {
    Harness harness{14, 4};
    auto const panel = harness.make<PanelImpl>(nullptr);
    panel->setTitle("Info");
    panel->setCollapsible(true);
    std::vector<bool> toggles;
    panel->setOnToggle([&](bool collapsed) { toggles.push_back(collapsed); });
    auto const button = harness.make<ButtonImpl>(panel.get());
    button->setLabel("Go");
    static_cast<void>(harness.draw());
    harness.focus(*button);
    button->setEnabled(false);
    // The disabled button ignores Enter, which bubbles to the panel's view.
    CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
    CHECK(toggles.empty());
}

TEST_CASE("tui containers: a click on a collapsible panel's title line toggles it, one on its body does not",
          "[tui][containers]") {
    Harness harness{14, 4};
    auto const panel = harness.make<PanelImpl>(nullptr);
    panel->setTitle("Info");
    panel->setCollapsible(true);
    std::vector<bool> toggles;
    panel->setOnToggle([&](bool collapsed) { toggles.push_back(collapsed); });
    auto const body = harness.make<TextImpl>(panel.get());
    body->setText("hi");
    static_cast<void>(harness.draw());
    static_cast<void>(harness.click({.x = 1, .y = 1}));
    static_cast<void>(harness.click({.x = 5, .y = 2}));
    CHECK(toggles.empty());
    CHECK(harness.click({.x = 4, .y = 0}) == EventResult::Handled);
    CHECK(toggles == std::vector<bool>{true});
    CHECK(harness.draw() == Rows{"▸ Info"});
    CHECK(harness.click({.x = 0, .y = 0}) == EventResult::Handled);
    CHECK(toggles == std::vector<bool>{true, false});
}

TEST_CASE("tui containers: a panel's toggle handler may destroy the panel", "[tui][containers][lifetime]") {
    Harness harness{14, 5};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const survivor = harness.make<ButtonImpl>(column.get());
    survivor->setLabel("Stay");
    auto panel = harness.make<PanelImpl>(column.get());
    panel->setTitle("Info");
    panel->setCollapsible(true);
    auto body = harness.make<TextImpl>(panel.get());
    body->setText("hi");
    int calls = 0;
    panel->setOnToggle([&](bool /*collapsed*/) {
        body.reset();
        panel.reset();
        ++calls;
    });
    static_cast<void>(harness.draw());
    SECTION("by Enter") {
        harness.focus(*panel);
        CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
    }
    SECTION("by a click") { CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled); }
    CHECK(panel == nullptr);
    CHECK(calls == 1);
    int survivorClicks = 0;
    survivor->setOnClick([&] { ++survivorClicks; });
    CHECK(harness.draw() == Rows{"[ Stay ]"});
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Handled);
    CHECK(survivorClicks == 1);
}

TEST_CASE("tui containers: no toggle handler runs after its panel was destroyed", "[tui][containers][lifetime]") {
    Harness harness{14, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto panel = harness.make<PanelImpl>(column.get());
    panel->setTitle("Info");
    panel->setCollapsible(true);
    int calls = 0;
    panel->setOnToggle([&](bool /*collapsed*/) { ++calls; });
    static_cast<void>(harness.draw());
    harness.focus(*panel);
    panel.reset();
    CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
    CHECK(harness.click({.x = 3, .y = 0}) == EventResult::Ignored);
    CHECK(calls == 0);
}

TEST_CASE("tui containers: a scroll keeps the focused child in view", "[tui][containers]") {
    Harness harness{10, 3};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const column = harness.make<StackImpl>(scroll.get(), ui::Axis::Vertical);
    std::vector<std::unique_ptr<ButtonImpl>> buttons;
    for (int i = 0; i < 5; ++i) {
        buttons.push_back(harness.make<ButtonImpl>(column.get()));
        buttons.back()->setLabel(std::to_string(i));
    }
    CHECK(harness.draw() == Rows{"[ 0 ]", "[ 1 ]", "[ 2 ]"});
    harness.focus(*buttons.back());
    CHECK(harness.draw() == Rows{"[ 2 ]", "[ 3 ]", "[ 4 ]"});
    harness.focus(*buttons.front());
    CHECK(harness.draw() == Rows{"[ 0 ]", "[ 1 ]", "[ 2 ]"});
    buttons.clear();
}

TEST_CASE("tui containers: a scroll keeps a fixed-height child's whole extent in view", "[tui][containers]") {
    Harness harness{10, 3};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const column = harness.make<StackImpl>(scroll.get(), ui::Axis::Vertical);
    auto const top = harness.make<ButtonImpl>(column.get());
    top->setLabel("0");
    auto const tall = harness.make<TextImpl>(column.get());
    tall->setText("t");
    tall->setLayout(ui::LayoutHints{.height = ui::Sizing::fixed(2)});
    auto const bottom = harness.make<ButtonImpl>(column.get());
    bottom->setLabel("2");
    harness.focus(*bottom);
    CHECK(harness.draw() == Rows{"t", "", "[ 2 ]"});
}

TEST_CASE("tui containers: a child scrolled out of view takes no click at its old place", "[tui][containers][drawn]") {
    Harness harness{10, 3};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const column = harness.make<StackImpl>(scroll.get(), ui::Axis::Vertical);
    std::vector<std::unique_ptr<ButtonImpl>> buttons;
    std::vector<int> clicked;
    for (int i = 0; i < 5; ++i) {
        buttons.push_back(harness.make<ButtonImpl>(column.get()));
        buttons.back()->setLabel(std::to_string(i));
        buttons.back()->setOnClick([&clicked, i] { clicked.push_back(i); });
    }
    CHECK(harness.draw() == Rows{"[ 0 ]", "[ 1 ]", "[ 2 ]"});
    harness.focus(*buttons.back());
    CHECK(harness.draw() == Rows{"[ 2 ]", "[ 3 ]", "[ 4 ]"});
    CHECK(WidgetBase::of(*buttons.front()).view().screenBounds().empty());
    CHECK(WidgetBase::of(*buttons.at(1)).view().screenBounds().empty());
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Handled);
    CHECK(clicked == std::vector<int>{2});
    buttons.clear();
}

TEST_CASE("tui containers: the wheel scrolls a scroll with nothing focused in it", "[tui][containers]") {
    Harness harness{10, 3};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const column = harness.make<StackImpl>(scroll.get(), ui::Axis::Vertical);
    std::vector<std::unique_ptr<TextImpl>> lines;
    for (int i = 0; i < 5; ++i) {
        lines.push_back(harness.make<TextImpl>(column.get()));
        lines.back()->setText("line " + std::to_string(i));
    }
    static_cast<void>(harness.draw());
    static_cast<void>(harness.send(
        core::tui::MouseEvent{.type = core::tui::MouseEvent::Type::ScrollDown, .button = 0, .x = 1, .y = 1}));
    CHECK(harness.draw() == Rows{"line 1", "line 2", "line 3"});
    lines.clear();
}

TEST_CASE("tui containers: the wheel stops at the last full view and scrolls back up", "[tui][containers]") {
    Harness harness{10, 3};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const column = harness.make<StackImpl>(scroll.get(), ui::Axis::Vertical);
    std::vector<std::unique_ptr<TextImpl>> lines;
    for (int i = 0; i < 5; ++i) {
        lines.push_back(harness.make<TextImpl>(column.get()));
        lines.back()->setText("line " + std::to_string(i));
    }
    static_cast<void>(harness.draw());
    using Type = core::tui::MouseEvent::Type;
    for (int i = 0; i < 6; ++i) {
        CHECK(harness.send(mouseAt(Type::ScrollDown, {.x = 1, .y = 1})) == EventResult::Handled);
        static_cast<void>(harness.draw());
    }
    CHECK(harness.draw() == Rows{"line 2", "line 3", "line 4"});
    CHECK(harness.send(mouseAt(Type::ScrollUp, {.x = 1, .y = 1})) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"line 1", "line 2", "line 3"});
    lines.clear();
}

TEST_CASE("tui containers: a scroll over content that is not a stack on its axis leaves the wheel to others",
          "[tui][containers]") {
    Harness harness{10, 2};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const text = harness.make<TextImpl>(scroll.get());
    text->setText("a\nb\nc");
    CHECK(harness.draw() == Rows{"a", "b"});
    CHECK(harness.send(mouseAt(core::tui::MouseEvent::Type::ScrollDown, {.x = 0, .y = 0})) == EventResult::Ignored);
    CHECK(harness.draw() == Rows{"a", "b"});
}

TEST_CASE("tui containers: a horizontal scroll keeps the focused child in view", "[tui][containers]") {
    Harness harness{10, 1};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Horizontal);
    auto const row = harness.make<StackImpl>(scroll.get(), ui::Axis::Horizontal);
    std::vector<std::unique_ptr<ButtonImpl>> buttons;
    for (int i = 0; i < 3; ++i) {
        buttons.push_back(harness.make<ButtonImpl>(row.get()));
        buttons.back()->setLabel(std::to_string(i));
    }
    CHECK(harness.draw() == Rows{"[ 0 ][ 1 ]"});
    harness.focus(*buttons.back());
    CHECK(harness.draw() == Rows{"[ 1 ][ 2 ]"});
    buttons.clear();
}
