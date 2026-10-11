// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Rect.hpp>
#include <cstdint>
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
using morph::tui::testing::ResizableOutput;
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

TEST_CASE("tui containers: a panel shortens a wide or combining title by whole characters", "[tui][containers]") {
    SECTION("wide characters") {
        Harness harness{9, 3};
        auto const panel = harness.make<PanelImpl>(nullptr);
        panel->setTitle("日本語");
        CHECK(harness.draw().front() == "┌─日本──┐");
    }
    SECTION("a combining mark stays with its letter") {
        Harness harness{6, 3};
        auto const panel = harness.make<PanelImpl>(nullptr);
        panel->setTitle("e\u0301te");
        CHECK(harness.draw().front() == "┌─e\u0301t─┐");
    }
    SECTION("a title that fits is drawn whole") {
        Harness harness{8, 3};
        auto const panel = harness.make<PanelImpl>(nullptr);
        panel->setTitle("e\u0301te");
        CHECK(harness.draw().front() == "┌─e\u0301te──┐");
    }
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
    // Even with the focus forced back onto it, the hidden button takes no key.
    harness.focus(*button);
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

TEST_CASE("tui containers: collapsing a panel that holds the focus moves it to the panel",
          "[tui][containers][focus]") {
    Harness harness{14, 4};
    auto const panel = harness.make<PanelImpl>(nullptr);
    panel->setTitle("Info");
    panel->setCollapsible(true);
    std::vector<bool> toggles;
    panel->setOnToggle([&](bool collapsed) { toggles.push_back(collapsed); });
    auto const column = harness.make<StackImpl>(panel.get(), ui::Axis::Vertical);
    auto const button = harness.make<ButtonImpl>(column.get());
    button->setLabel("Go");
    static_cast<void>(harness.draw());
    harness.focus(*button);
    SECTION("by setCollapsed") {
        panel->setCollapsed(true);
        CHECK(toggles.empty());
    }
    SECTION("by a click on its title line") {
        CHECK(harness.click({.x = 3, .y = 0}) == EventResult::Handled);
        CHECK(toggles == std::vector<bool>{true});
    }
    CHECK(harness.focused(*panel));
    CHECK(harness.draw() == Rows{"▸ Info"});
    CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
    CHECK(harness.draw().at(1) == "│[ Go ]      │");
}

TEST_CASE("tui containers: collapsing a panel that cannot take the focus clears it", "[tui][containers][focus]") {
    Harness harness{14, 4};
    auto const panel = harness.make<PanelImpl>(nullptr);
    panel->setTitle("Info");
    auto const button = harness.make<ButtonImpl>(panel.get());
    button->setLabel("Go");
    static_cast<void>(harness.draw());
    harness.focus(*button);
    SECTION("one that is not collapsible") {}
    SECTION("a collapsible one that is disabled") {
        panel->setCollapsible(true);
        panel->setEnabled(false);
    }
    panel->setCollapsed(true);
    CHECK(harness.screen().focusedComponent() == nullptr);
}

TEST_CASE("tui containers: a click on a panel's body leaves the focus where it was", "[tui][containers][focus]") {
    Harness harness{14, 5};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const button = harness.make<ButtonImpl>(column.get());
    button->setLabel("B");
    auto const panel = harness.make<PanelImpl>(column.get());
    panel->setTitle("Info");
    panel->setCollapsible(true);
    std::vector<bool> toggles;
    panel->setOnToggle([&](bool collapsed) { toggles.push_back(collapsed); });
    auto const body = harness.make<TextImpl>(panel.get());
    body->setText("hi");
    CHECK(harness.draw() == Rows{"[ B ]", "┌─▾ Info─────┐", "│hi          │", "└────────────┘"});
    harness.focus(*button);
    static_cast<void>(harness.click({.x = 1, .y = 2}));
    static_cast<void>(harness.click({.x = 8, .y = 3}));
    CHECK(harness.focused(*button));
    CHECK(toggles.empty());
    CHECK(harness.click({.x = 5, .y = 1}) == EventResult::Handled);
    CHECK(harness.focused(*panel));
    CHECK(toggles == std::vector<bool>{true});
}

TEST_CASE("tui containers: a click on the title of a panel that cannot take the focus leaves the focus alone",
          "[tui][containers][focus]") {
    Harness harness{14, 5};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const button = harness.make<ButtonImpl>(column.get());
    button->setLabel("B");
    auto const panel = harness.make<PanelImpl>(column.get());
    panel->setTitle("Info");
    // A drag key makes the panel take the press, though it is not collapsible and so not focusable.
    panel->setDragKey(ui::Key{std::int64_t{1}});
    CHECK(harness.draw().at(1) == "┌─Info───────┐");
    harness.focus(*button);
    CHECK(harness.click({.x = 3, .y = 1}) == EventResult::Handled);
    CHECK(harness.focused(*button));
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

namespace {

/// A widget four rows tall that puts the cursor on one of its own cells.
class CursorProbe final : public morph::tui::detail::TuiWidget<ui::SpacerWidget> {
public:
    CursorProbe(morph::tui::detail::Context& context, core::tui::Point cell) : TuiWidget{context}, _cell{cell} {
        adopt(morph::tui::detail::makeView(*this));
    }
    [[nodiscard]] core::tui::Size naturalSize() const override { return {.width = 4, .height = 4}; }
    void paint(core::tui::Canvas& canvas) override {
        canvas.putString(0, 0, "top", canvas.theme().textNormal);
        canvas.setCursor(_cell.y, _cell.x);
    }

private:
    core::tui::Point _cell;
};

}  // namespace

TEST_CASE("tui containers: a widget scrolled partly out of view keeps its cursor on its own cell",
          "[tui][containers]") {
    Harness harness{10, 2};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    using Type = core::tui::MouseEvent::Type;
    SECTION("a cursor inside the drawn part moves with the widget") {
        auto const probe = harness.make<CursorProbe>(scroll.get(), core::tui::Point{.x = 1, .y = 2});
        CHECK(harness.send(mouseAt(Type::ScrollDown, {.x = 0, .y = 0})) == EventResult::Ignored);
        CHECK(harness.draw() == Rows{"top"});
        CHECK(harness.send(mouseAt(Type::ScrollDown, {.x = 0, .y = 0})) == EventResult::Handled);
        CHECK(harness.draw().empty());
        auto const& frame = harness.screen().renderedBuffer();
        CHECK(frame.cursorVisible());
        CHECK(frame.cursor() == core::tui::Point{.x = 1, .y = 1});
    }
    SECTION("a cursor in the part scrolled away is hidden") {
        auto const probe = harness.make<CursorProbe>(scroll.get(), core::tui::Point{.x = 1, .y = 0});
        CHECK(harness.draw() == Rows{"top"});
        CHECK(harness.screen().renderedBuffer().cursorVisible());
        CHECK(harness.send(mouseAt(Type::ScrollDown, {.x = 0, .y = 0})) == EventResult::Handled);
        static_cast<void>(harness.draw());
        CHECK_FALSE(harness.screen().renderedBuffer().cursorVisible());
    }
}

TEST_CASE("tui containers: a wide character cut at a scroll's left edge leaves a blank", "[tui][containers]") {
    Harness harness{3, 1};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Horizontal);
    auto const text = harness.make<TextImpl>(scroll.get());
    text->setText("日本");
    CHECK(harness.draw() == Rows{"日"});
    CHECK(harness.send(mouseAt(core::tui::MouseEvent::Type::ScrollDown, {.x = 0, .y = 0})) == EventResult::Handled);
    CHECK(harness.draw() == Rows{" 本"});
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

TEST_CASE("tui containers: the wheel scrolls past the focused child, and moving the focus brings it back",
          "[tui][containers]") {
    Harness harness{10, 2};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const column = harness.make<StackImpl>(scroll.get(), ui::Axis::Vertical);
    std::vector<std::unique_ptr<ButtonImpl>> buttons;
    for (int i = 0; i < 4; ++i) {
        buttons.push_back(harness.make<ButtonImpl>(column.get()));
        buttons.back()->setLabel(std::to_string(i));
    }
    harness.focus(*buttons.front());
    CHECK(harness.draw() == Rows{"[ 0 ]", "[ 1 ]"});
    using Type = core::tui::MouseEvent::Type;
    CHECK(harness.send(mouseAt(Type::ScrollDown, {.x = 1, .y = 0})) == EventResult::Handled);
    CHECK(harness.send(mouseAt(Type::ScrollDown, {.x = 1, .y = 0})) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"[ 2 ]", "[ 3 ]"});
    harness.focus(*buttons.at(1));
    CHECK(harness.draw() == Rows{"[ 1 ]", "[ 2 ]"});
    buttons.clear();
}

TEST_CASE("tui containers: a scroll keeps the focused child in view when the terminal shrinks", "[tui][containers]") {
    auto output = std::make_unique<ResizableOutput>(core::tui::Size{.width = 10, .height = 6});
    auto& terminal = *output;
    Harness harness{std::move(output)};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const column = harness.make<StackImpl>(scroll.get(), ui::Axis::Vertical);
    std::vector<std::unique_ptr<ButtonImpl>> buttons;
    std::vector<int> clicked;
    for (int i = 0; i < 12; ++i) {
        buttons.push_back(harness.make<ButtonImpl>(column.get()));
        buttons.back()->setLabel(std::to_string(i));
        buttons.back()->setOnClick([&clicked, i] { clicked.push_back(i); });
    }
    harness.focus(*buttons.at(10));
    CHECK(harness.draw().back() == "[ 10 ]");

    terminal.resize({.width = 10, .height = 2});
    static_cast<void>(harness.send(core::tui::ResizeEvent{.columns = 10, .rows = 2}));
    auto const rows = harness.draw();
    REQUIRE(rows.size() == 2);
    CHECK(rows.back() == "[ 10 ]");
    CHECK(harness.click({.x = 1, .y = 1}) == EventResult::Handled);
    CHECK(clicked == std::vector<int>{10});
    buttons.clear();
}

TEST_CASE("tui containers: a scroll keeps the focused child in view when the content above it grows",
          "[tui][containers]") {
    Harness harness{10, 3};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const column = harness.make<StackImpl>(scroll.get(), ui::Axis::Vertical);
    auto const text = harness.make<TextImpl>(column.get());
    text->setText("t");
    auto const button = harness.make<ButtonImpl>(column.get());
    button->setLabel("B");
    harness.focus(*button);
    CHECK(harness.draw() == Rows{"t", "[ B ]"});
    text->setText("1\n2\n3\n4");
    CHECK(harness.draw() == Rows{"3", "4", "[ B ]"});
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
    for (int i = 0; i < 2; ++i) {
        CHECK(harness.send(mouseAt(Type::ScrollDown, {.x = 1, .y = 1})) == EventResult::Handled);
        static_cast<void>(harness.draw());
    }
    // At the end of the content the wheel is left to whatever lies around the scroll.
    CHECK(harness.send(mouseAt(Type::ScrollDown, {.x = 1, .y = 1})) == EventResult::Ignored);
    CHECK(harness.draw() == Rows{"line 2", "line 3", "line 4"});
    CHECK(harness.send(mouseAt(Type::ScrollUp, {.x = 1, .y = 1})) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"line 1", "line 2", "line 3"});
    lines.clear();
}

TEST_CASE("tui containers: a scroll whose content shrinks moves back to keep its view full", "[tui][containers]") {
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
    CHECK(harness.send(mouseAt(Type::ScrollDown, {.x = 1, .y = 1})) == EventResult::Handled);
    CHECK(harness.send(mouseAt(Type::ScrollDown, {.x = 1, .y = 1})) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"line 2", "line 3", "line 4"});
    lines.pop_back();
    lines.pop_back();
    CHECK(harness.draw() == Rows{"line 0", "line 1", "line 2"});
    lines.clear();
}

TEST_CASE("tui containers: a scroll with nothing focusable inside takes the focus, and the keys scroll it",
          "[tui][containers][keys]") {
    Harness harness{10, 3};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const text = harness.make<TextImpl>(scroll.get());
    text->setText("0\n1\n2\n3\n4\n5");
    CHECK(harness.draw() == Rows{"0", "1", "2"});
    morph::tui::detail::moveFocus(harness.context(), Direction::Forward);
    REQUIRE(harness.focused(*scroll));
    CHECK(harness.key(KeyCode::PageDown) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"2", "3", "4"});
    CHECK(harness.key(KeyCode::End) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"3", "4", "5"});
    CHECK(harness.key(KeyCode::Down) == EventResult::Ignored);
    CHECK(harness.key(KeyCode::Up) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"2", "3", "4"});
    CHECK(harness.key(KeyCode::Home) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"0", "1", "2"});
    CHECK(harness.key(KeyCode::PageUp) == EventResult::Ignored);
    CHECK(harness.key(KeyCode::Down) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"1", "2", "3"});
    CHECK(harness.key(KeyCode::Right) == EventResult::Ignored);
}

TEST_CASE(
    "tui containers: a scroll with a focusable widget inside is no Tab stop, and a key the widget leaves "
    "scrolls it",
    "[tui][containers][keys]") {
    Harness harness{10, 2};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const column = harness.make<StackImpl>(scroll.get(), ui::Axis::Vertical);
    auto const button = harness.make<ButtonImpl>(column.get());
    button->setLabel("B");
    auto const text = harness.make<TextImpl>(column.get());
    text->setText("a\nb\nc");
    CHECK(harness.draw() == Rows{"[ B ]", "a"});
    CHECK_FALSE(WidgetBase::of(*scroll).view().focusable());
    morph::tui::detail::moveFocus(harness.context(), Direction::Forward);
    morph::tui::detail::moveFocus(harness.context(), Direction::Forward);
    REQUIRE(harness.focused(*button));
    CHECK(harness.key(KeyCode::PageDown) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"a", "b"});

    // Once nothing inside can take the focus, the scroll takes it itself.
    button->setEnabled(false);
    CHECK(WidgetBase::of(*scroll).view().focusable());
}

TEST_CASE("tui containers: a horizontal scroll moves on Left and Right", "[tui][containers][keys]") {
    Harness harness{3, 1};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Horizontal);
    auto const text = harness.make<TextImpl>(scroll.get());
    text->setText("abcde");
    CHECK(harness.draw() == Rows{"abc"});
    harness.focus(*scroll);
    CHECK(harness.key(KeyCode::Down) == EventResult::Ignored);
    CHECK(harness.key(KeyCode::Right) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"bcd"});
    CHECK(harness.key(KeyCode::Left) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"abc"});
}

TEST_CASE("tui containers: a scroll scrolls a long text line by line", "[tui][containers]") {
    Harness harness{10, 2};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const text = harness.make<TextImpl>(scroll.get());
    text->setText("a\nb\nc");
    CHECK(harness.draw() == Rows{"a", "b"});
    CHECK(harness.send(mouseAt(core::tui::MouseEvent::Type::ScrollDown, {.x = 0, .y = 0})) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"b", "c"});
    CHECK(harness.send(mouseAt(core::tui::MouseEvent::Type::ScrollDown, {.x = 0, .y = 0})) == EventResult::Ignored);
    CHECK(harness.draw() == Rows{"b", "c"});
}

TEST_CASE("tui containers: Tab into a scroll brings a button deep inside it into view", "[tui][containers]") {
    Harness harness{12, 4};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const panel = harness.make<PanelImpl>(scroll.get());
    panel->setTitle("P");
    auto const column = harness.make<StackImpl>(panel.get(), ui::Axis::Vertical);
    std::vector<std::unique_ptr<ButtonImpl>> buttons;
    std::vector<int> clicked;
    for (int i = 0; i < 6; ++i) {
        buttons.push_back(harness.make<ButtonImpl>(column.get()));
        buttons.back()->setLabel(std::to_string(i));
        buttons.back()->setOnClick([&clicked, i] { clicked.push_back(i); });
    }
    CHECK(harness.draw() == Rows{"┌─P────────┐", "│[ 0 ]     │", "│[ 1 ]     │", "│[ 2 ]     │"});
    auto const tab = [&] { morph::tui::detail::moveFocus(harness.context(), Direction::Forward); };
    for (int i = 0; i < 6; ++i) {
        tab();
    }
    REQUIRE(harness.focused(*buttons.back()));
    CHECK(harness.draw() == Rows{"│[ 2 ]     │", "│[ 3 ]     │", "│[ 4 ]     │", "│[ 5 ]     │"});
    CHECK(WidgetBase::of(*buttons.front()).view().screenBounds().empty());
    CHECK(harness.click({.x = 2, .y = 0}) == EventResult::Handled);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
    CHECK(clicked == std::vector<int>{2, 2});

    harness.focus(*buttons.back());
    static_cast<void>(harness.draw());
    tab();
    REQUIRE(harness.focused(*buttons.front()));
    CHECK(harness.draw() == Rows{"│[ 0 ]     │", "│[ 1 ]     │", "│[ 2 ]     │", "│[ 3 ]     │"});
    buttons.clear();
}

TEST_CASE("tui containers: a horizontal scroll moves a column's lines sideways", "[tui][containers]") {
    Harness harness{5, 2};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Horizontal);
    auto const column = harness.make<StackImpl>(scroll.get(), ui::Axis::Vertical);
    auto const wide = harness.make<TextImpl>(column.get());
    wide->setText("abcdefg");
    auto const narrow = harness.make<TextImpl>(column.get());
    narrow->setText("xy");
    CHECK(harness.draw() == Rows{"abcde", "xy"});
    using Type = core::tui::MouseEvent::Type;
    CHECK(harness.send(mouseAt(Type::ScrollDown, {.x = 0, .y = 0})) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"bcdef", "y"});
    CHECK(harness.send(mouseAt(Type::ScrollDown, {.x = 0, .y = 0})) == EventResult::Handled);
    CHECK(harness.send(mouseAt(Type::ScrollDown, {.x = 0, .y = 0})) == EventResult::Ignored);
    CHECK(harness.draw() == Rows{"cdefg"});
}

TEST_CASE("tui containers: the wheel an inner scroll cannot use moves the scroll around it", "[tui][containers]") {
    Harness harness{10, 3};
    auto const outer = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const column = harness.make<StackImpl>(outer.get(), ui::Axis::Vertical);
    auto const top = harness.make<TextImpl>(column.get());
    top->setText("top");
    auto const inner = harness.make<ScrollImpl>(column.get(), ui::Axis::Vertical);
    inner->setLayout(ui::LayoutHints{.height = ui::Sizing::fixed(2)});
    auto const lines = harness.make<TextImpl>(inner.get());
    lines->setText("a\nb\nc");
    auto const tail = harness.make<TextImpl>(column.get());
    tail->setText("t1\nt2\nt3");
    CHECK(harness.draw() == Rows{"top", "a", "b"});
    using Type = core::tui::MouseEvent::Type;
    CHECK(harness.send(mouseAt(Type::ScrollDown, {.x = 0, .y = 1})) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"top", "b", "c"});
    CHECK(harness.send(mouseAt(Type::ScrollDown, {.x = 0, .y = 1})) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"b", "c", "t1"});
}

TEST_CASE("tui containers: a click on a panel scrolled partly out of view lands where the panel shows it",
          "[tui][containers]") {
    Harness harness{12, 3};
    auto const scroll = harness.make<ScrollImpl>(nullptr, ui::Axis::Vertical);
    auto const panel = harness.make<PanelImpl>(scroll.get());
    panel->setTitle("P");
    panel->setCollapsible(true);
    std::vector<bool> toggles;
    panel->setOnToggle([&](bool collapsed) { toggles.push_back(collapsed); });
    auto const body = harness.make<TextImpl>(panel.get());
    body->setText("a\nb\nc\nd");
    CHECK(harness.draw().front() == "┌─▾ P──────┐");
    CHECK(harness.send(mouseAt(core::tui::MouseEvent::Type::ScrollDown, {.x = 5, .y = 1})) == EventResult::Handled);
    CHECK(harness.draw() == Rows{"│a         │", "│b         │", "│c         │"});
    // The top row now shows the panel's second line, not its title line.
    static_cast<void>(harness.click({.x = 5, .y = 0}));
    CHECK(toggles.empty());
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
