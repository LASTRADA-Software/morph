// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Rect.hpp>
#include <cstdint>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <vector>

#include "tui/container_widgets.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using core::tui::EventResult;
using core::tui::KeyCode;
using morph::tui::detail::ButtonImpl;
using morph::tui::detail::CheckboxImpl;
using morph::tui::detail::Direction;
using morph::tui::detail::SlotImpl;
using morph::tui::detail::SpacerImpl;
using morph::tui::detail::StackImpl;
using morph::tui::detail::TextImpl;
using morph::tui::testing::Harness;
using morph::tui::testing::ResizableOutput;
using Rows = std::vector<std::string>;

TEST_CASE("tui widgets: a column renders its children top to bottom", "[tui][widgets]") {
    Harness harness{20, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const title = harness.make<TextImpl>(column.get());
    title->setText("Title");
    auto const goButton = harness.make<ButtonImpl>(column.get());
    goButton->setLabel("Go");
    CHECK(harness.draw() == Rows{"Title", "[ Go ]"});
}

TEST_CASE("tui widgets: a row lays out left to right, with its gap", "[tui][widgets]") {
    Harness harness{20, 2};
    auto const row = harness.make<StackImpl>(nullptr, ui::Axis::Horizontal);
    row->setGap(2);
    auto const left = harness.make<TextImpl>(row.get());
    left->setText("ab");
    auto const right = harness.make<TextImpl>(row.get());
    right->setText("cd");
    CHECK(harness.draw() == Rows{"ab  cd"});
}

TEST_CASE("tui widgets: a spacer takes the space its siblings leave", "[tui][widgets]") {
    Harness harness{20, 1};
    auto const row = harness.make<StackImpl>(nullptr, ui::Axis::Horizontal);
    auto const left = harness.make<TextImpl>(row.get());
    left->setText("L");
    auto const spacer = harness.make<SpacerImpl>(row.get());
    auto const right = harness.make<TextImpl>(row.get());
    right->setText("R");
    CHECK(harness.draw() == Rows{"L" + std::string(18, ' ') + "R"});
}

TEST_CASE("tui widgets: a hidden child takes no space", "[tui][widgets]") {
    Harness harness{10, 3};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const first = harness.make<TextImpl>(column.get());
    first->setText("a");
    auto const second = harness.make<TextImpl>(column.get());
    second->setText("b");
    auto const third = harness.make<TextImpl>(column.get());
    third->setText("c");
    second->setVisible(false);
    CHECK(harness.draw() == Rows{"a", "c"});
}

TEST_CASE("tui widgets: a slot stacks its children with no gap, and a hidden one takes no space", "[tui][widgets]") {
    Harness harness{10, 3};
    auto const slot = harness.make<SlotImpl>(nullptr);
    auto const first = harness.make<TextImpl>(slot.get());
    first->setText("a");
    auto const second = harness.make<ButtonImpl>(slot.get());
    second->setLabel("b");
    auto const third = harness.make<TextImpl>(slot.get());
    third->setText("c");
    CHECK(harness.draw() == Rows{"a", "[ b ]", "c"});
    second->setVisible(false);
    CHECK(harness.draw() == Rows{"a", "c"});
}

TEST_CASE("tui widgets: a hidden child in a row takes no gap either", "[tui][widgets]") {
    Harness harness{10, 1};
    auto const row = harness.make<StackImpl>(nullptr, ui::Axis::Horizontal);
    row->setGap(1);
    auto const first = harness.make<TextImpl>(row.get());
    first->setText("a");
    auto const second = harness.make<TextImpl>(row.get());
    second->setText("b");
    auto const third = harness.make<TextImpl>(row.get());
    third->setText("c");
    second->setVisible(false);
    CHECK(harness.draw() == Rows{"a c"});
}

TEST_CASE("tui widgets: multi-line text takes one row per line", "[tui][widgets]") {
    Harness harness{10, 3};
    auto const text = harness.make<TextImpl>(nullptr);
    text->setText("one\ntwo");
    CHECK(harness.draw() == Rows{"one", "two"});
}

TEST_CASE("tui widgets: moveChild reorders both rendering and focus order", "[tui][widgets]") {
    Harness harness{10, 2};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const first = harness.make<ButtonImpl>(column.get());
    first->setLabel("A");
    auto const second = harness.make<ButtonImpl>(column.get());
    second->setLabel("B");
    column->moveChild(*second, 0);
    CHECK(harness.draw() == Rows{"[ B ]", "[ A ]"});
    morph::tui::detail::moveFocus(harness.context(), Direction::Forward);
    CHECK(harness.focused(*second));
}

TEST_CASE("tui widgets: Enter, Space and a click activate a button; a disabled one ignores them", "[tui][widgets]") {
    Harness harness{20, 2};
    auto const button = harness.make<ButtonImpl>(nullptr);
    int clicks = 0;
    button->setLabel("Go");
    button->setOnClick([&] { ++clicks; });
    harness.focus(*button);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
    CHECK(harness.type(" ") == EventResult::Handled);
    CHECK(clicks == 2);
    static_cast<void>(harness.draw());
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Handled);
    CHECK(clicks == 3);

    button->setEnabled(false);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
    static_cast<void>(harness.click({.x = 1, .y = 0}));
    CHECK(clicks == 3);
}

TEST_CASE("tui widgets: a checkbox toggles on Enter and reports the new state; setChecked reports nothing",
          "[tui][widgets]") {
    Harness harness{20, 1};
    auto const box = harness.make<CheckboxImpl>(nullptr);
    std::vector<bool> toggles;
    box->setLabel("Done");
    box->setChecked(false);
    box->setOnToggle([&](bool checked) { toggles.push_back(checked); });
    CHECK(harness.draw() == Rows{"[ ] Done"});
    harness.focus(*box);
    static_cast<void>(harness.key(KeyCode::Enter));
    CHECK(toggles == std::vector<bool>{true});
    CHECK(harness.draw() == Rows{"[x] Done"});
    box->setChecked(false);
    CHECK(harness.draw() == Rows{"[ ] Done"});
    CHECK(toggles.size() == 1);
}

TEST_CASE("tui widgets: destroying the focused widget clears the screen's focus", "[tui][widgets]") {
    Harness harness{20, 2};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto button = harness.make<ButtonImpl>(column.get());
    button->setLabel("Go");
    harness.focus(*button);
    REQUIRE(harness.screen().focusedComponent() != nullptr);
    button.reset();
    CHECK(harness.screen().focusedComponent() == nullptr);
    CHECK(harness.draw().empty());
}

namespace {

using morph::tui::detail::WidgetBase;

/// A left-button press on a 0-based viewport cell, with no release.
core::tui::MouseEvent pressAt(core::tui::Point cell) {
    return core::tui::MouseEvent{
        .type = core::tui::MouseEvent::Type::Press, .button = 0, .x = cell.x + 1, .y = cell.y + 1};
}

/// A left-button release on a 0-based viewport cell.
core::tui::MouseEvent releaseAt(core::tui::Point cell) {
    return core::tui::MouseEvent{
        .type = core::tui::MouseEvent::Type::Release, .button = 0, .x = cell.x + 1, .y = cell.y + 1};
}

}  // namespace

TEST_CASE("tui widgets: a widget inside a disabled container two levels up takes no key, click or focus",
          "[tui][widgets][gating]") {
    Harness harness{20, 3};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const outer = harness.make<StackImpl>(column.get(), ui::Axis::Vertical);
    auto const inner = harness.make<StackImpl>(outer.get(), ui::Axis::Vertical);
    auto const gated = harness.make<ButtonImpl>(inner.get());
    gated->setLabel("Gated");
    auto const other = harness.make<ButtonImpl>(column.get());
    other->setLabel("Other");
    int clicks = 0;
    gated->setOnClick([&] { ++clicks; });
    static_cast<void>(harness.draw());
    harness.focus(*gated);

    outer->setEnabled(false);
    static_cast<void>(harness.draw());
    CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
    CHECK(harness.type(" ") == EventResult::Ignored);
    WidgetBase::of(*gated).activate();
    CHECK(clicks == 0);
    CHECK_FALSE(WidgetBase::of(*gated).view().focusable());
    morph::tui::detail::moveFocus(harness.context(), Direction::Forward);
    CHECK(harness.focused(*other));
    morph::tui::detail::moveFocus(harness.context(), Direction::Forward);
    CHECK(harness.focused(*other));
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Ignored);
    CHECK(harness.focused(*other));
    CHECK(clicks == 0);

    outer->setEnabled(true);
    static_cast<void>(harness.draw());
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Handled);
    CHECK(clicks == 1);
    CHECK(harness.focused(*gated));
    CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
    CHECK(clicks == 2);
}

TEST_CASE("tui widgets: a focused widget inside a hidden container takes no key until shown again",
          "[tui][widgets][gating]") {
    Harness harness{20, 3};
    auto const outer = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const inner = harness.make<StackImpl>(outer.get(), ui::Axis::Vertical);
    auto const box = harness.make<CheckboxImpl>(inner.get());
    box->setLabel("Done");
    std::vector<bool> toggles;
    box->setOnToggle([&](bool checked) { toggles.push_back(checked); });
    harness.focus(*box);

    outer->setVisible(false);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
    CHECK(toggles.empty());
    CHECK(harness.draw().empty());

    outer->setVisible(true);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
    CHECK(toggles == std::vector<bool>{true});
}

TEST_CASE("tui widgets: a checkbox inside a disabled container ignores a click", "[tui][widgets][gating]") {
    Harness harness{20, 2};
    auto const outer = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const inner = harness.make<StackImpl>(outer.get(), ui::Axis::Vertical);
    auto const box = harness.make<CheckboxImpl>(inner.get());
    box->setLabel("Done");
    std::vector<bool> toggles;
    box->setOnToggle([&](bool checked) { toggles.push_back(checked); });
    outer->setEnabled(false);
    CHECK(harness.draw() == Rows{"[ ] Done"});
    static_cast<void>(harness.click({.x = 1, .y = 0}));
    WidgetBase::of(*box).activate();
    CHECK(toggles.empty());
    CHECK(harness.draw() == Rows{"[ ] Done"});
}

namespace {

/// A focusable widget that counts the clicks the base delivers, without an `activate()` gate of its own.
class ClickProbe final : public morph::tui::detail::TuiWidget<ui::SpacerWidget> {
public:
    explicit ClickProbe(morph::tui::detail::Context& context) : TuiWidget{context} {
        adopt(morph::tui::detail::makeView(*this));
    }
    [[nodiscard]] core::tui::Size naturalSize() const override { return {.width = 4, .height = 1}; }
    [[nodiscard]] bool wantsFocus() const override { return true; }
    void click(core::tui::Point /*cell*/) override { ++_clicks; }
    [[nodiscard]] int clicks() const noexcept { return _clicks; }

private:
    int _clicks = 0;
};

}  // namespace

TEST_CASE("tui widgets: a press whose container is disabled before the release clicks nothing",
          "[tui][widgets][gating]") {
    Harness harness{20, 2};
    auto const outer = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const inner = harness.make<StackImpl>(outer.get(), ui::Axis::Vertical);
    auto const probe = harness.make<ClickProbe>(inner.get());
    static_cast<void>(harness.draw());
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Handled);
    CHECK(probe->clicks() == 1);

    CHECK(harness.send(pressAt({.x = 1, .y = 0})) == EventResult::Handled);
    outer->setEnabled(false);
    CHECK(harness.send(releaseAt({.x = 1, .y = 0})) == EventResult::Handled);
    CHECK(probe->clicks() == 1);
}

TEST_CASE("tui widgets: a press inside a disabled container takes neither the pointer nor focus",
          "[tui][widgets][gating]") {
    Harness harness{20, 2};
    auto const outer = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const inner = harness.make<StackImpl>(outer.get(), ui::Axis::Vertical);
    auto const probe = harness.make<ClickProbe>(inner.get());
    outer->setEnabled(false);
    static_cast<void>(harness.draw());
    CHECK(harness.send(pressAt({.x = 1, .y = 0})) == EventResult::Ignored);
    CHECK(harness.screen().pointerCapture() == nullptr);
    CHECK_FALSE(harness.focused(*probe));
    static_cast<void>(harness.send(releaseAt({.x = 1, .y = 0})));
    CHECK(probe->clicks() == 0);
}

TEST_CASE("tui widgets: a handler may destroy its own widget", "[tui][widgets][lifetime]") {
    Harness harness{20, 3};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const survivor = harness.make<ButtonImpl>(column.get());
    survivor->setLabel("Stay");
    int calls = 0;

    SECTION("a button, by Enter") {
        auto button = harness.make<ButtonImpl>(column.get());
        button->setOnClick([&button, &calls] {
            button.reset();
            ++calls;
        });
        harness.focus(*button);
        CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
        CHECK(button == nullptr);
    }
    SECTION("a button, by a click") {
        auto button = harness.make<ButtonImpl>(column.get());
        button->setLabel("Go");
        button->setOnClick([&button, &calls] {
            button.reset();
            ++calls;
        });
        static_cast<void>(harness.draw());
        CHECK(harness.click({.x = 1, .y = 1}) == EventResult::Handled);
        CHECK(button == nullptr);
    }
    SECTION("a checkbox, by Space") {
        auto box = harness.make<CheckboxImpl>(column.get());
        box->setOnToggle([&box, &calls](bool /*checked*/) {
            box.reset();
            ++calls;
        });
        harness.focus(*box);
        CHECK(harness.type(" ") == EventResult::Handled);
        CHECK(box == nullptr);
    }
    SECTION("a checkbox, by a click") {
        auto box = harness.make<CheckboxImpl>(column.get());
        box->setLabel("Done");
        box->setOnToggle([&box, &calls](bool /*checked*/) {
            box.reset();
            ++calls;
        });
        static_cast<void>(harness.draw());
        CHECK(harness.click({.x = 1, .y = 1}) == EventResult::Handled);
        CHECK(box == nullptr);
    }
    SECTION("a drop target, by its drop") {
        auto text = harness.make<TextImpl>(column.get());
        text->setDropHandler([](ui::Key const& /*key*/) { return true; },
                             [&text, &calls](ui::Key const& /*key*/) {
                                 text.reset();
                                 ++calls;
                             });
        WidgetBase::of(*text).drop(ui::Key{std::int64_t{7}});
        CHECK(text == nullptr);
    }
    SECTION("a drop target, by its accepts predicate") {
        auto text = harness.make<TextImpl>(column.get());
        text->setDropHandler(
            [&text, &calls](ui::Key const& /*key*/) {
                text.reset();
                ++calls;
                return false;
            },
            [](ui::Key const& /*key*/) {});
        CHECK_FALSE(WidgetBase::of(*text).accepts(ui::Key{std::int64_t{7}}));
        CHECK(text == nullptr);
    }

    CHECK(calls == 1);
    // The backend stays usable: the survivor still draws and takes a click.
    int survivorClicks = 0;
    survivor->setOnClick([&] { ++survivorClicks; });
    CHECK(harness.draw() == Rows{"[ Stay ]"});
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Handled);
    CHECK(survivorClicks == 1);
}

TEST_CASE("tui widgets: no handler runs after its widget was destroyed", "[tui][widgets][lifetime]") {
    Harness harness{20, 2};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    int calls = 0;
    SECTION("a button") {
        auto button = harness.make<ButtonImpl>(column.get());
        button->setLabel("Go");
        button->setOnClick([&] { ++calls; });
        static_cast<void>(harness.draw());
        harness.focus(*button);
        button.reset();
    }
    SECTION("a checkbox") {
        auto box = harness.make<CheckboxImpl>(column.get());
        box->setLabel("Done");
        box->setOnToggle([&](bool /*checked*/) { ++calls; });
        static_cast<void>(harness.draw());
        harness.focus(*box);
        box.reset();
    }
    CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Ignored);
    CHECK(calls == 0);
}

TEST_CASE("tui widgets: with column extents, a hidden child keeps its slot", "[tui][widgets]") {
    Harness harness{20, 1};
    auto const row = harness.make<StackImpl>(nullptr, ui::Axis::Horizontal);
    row->setColumnLayout({3, 3, 3}, 1);
    auto const first = harness.make<TextImpl>(row.get());
    first->setText("a");
    auto const second = harness.make<TextImpl>(row.get());
    second->setText("b");
    auto const third = harness.make<TextImpl>(row.get());
    third->setText("c");
    CHECK(harness.draw() == Rows{"a   b   c"});
    first->setVisible(false);
    CHECK(harness.draw() == Rows{"    b   c"});
    second->setVisible(false);
    CHECK(harness.draw() == Rows{"        c"});
}

TEST_CASE("tui widgets: moveChild keeps a pressed child's pointer capture and focus", "[tui][widgets]") {
    Harness harness{10, 3};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const first = harness.make<ButtonImpl>(column.get());
    first->setLabel("A");
    auto const second = harness.make<ButtonImpl>(column.get());
    second->setLabel("B");
    int clicks = 0;
    first->setOnClick([&] { ++clicks; });
    CHECK(harness.draw() == Rows{"[ A ]", "[ B ]"});

    CHECK(harness.send(pressAt({.x = 1, .y = 0})) == EventResult::Handled);
    REQUIRE(harness.screen().pointerCapture() == &WidgetBase::of(*first).view());
    column->moveChild(*second, 0);
    CHECK(harness.screen().pointerCapture() == &WidgetBase::of(*first).view());
    column->moveChild(*first, 0);
    CHECK(harness.screen().pointerCapture() == &WidgetBase::of(*first).view());
    CHECK(harness.focused(*first));
    CHECK(harness.send(releaseAt({.x = 1, .y = 0})) == EventResult::Handled);
    CHECK(clicks == 1);
}

TEST_CASE("tui widgets: Tab order follows the children's order after moves in both directions", "[tui][widgets]") {
    Harness harness{10, 3};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const first = harness.make<ButtonImpl>(column.get());
    auto const second = harness.make<ButtonImpl>(column.get());
    auto const third = harness.make<ButtonImpl>(column.get());
    column->moveChild(*third, 0);
    column->moveChild(*first, 7);
    // Order now: third, second, first.
    auto const forward = [&] { morph::tui::detail::moveFocus(harness.context(), Direction::Forward); };
    auto const backward = [&] { morph::tui::detail::moveFocus(harness.context(), Direction::Backward); };
    forward();
    CHECK(harness.focused(*third));
    forward();
    CHECK(harness.focused(*second));
    forward();
    CHECK(harness.focused(*first));
    forward();
    CHECK(harness.focused(*third));
    backward();
    CHECK(harness.focused(*first));
    backward();
    CHECK(harness.focused(*second));
}

TEST_CASE("tui widgets: a child squeezed out of a column takes no click at its old place", "[tui][widgets][drawn]") {
    Harness harness{10, 2};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const text = harness.make<TextImpl>(column.get());
    text->setText("t");
    auto const row = harness.make<StackImpl>(column.get(), ui::Axis::Horizontal);
    auto const button = harness.make<ButtonImpl>(row.get());
    button->setLabel("B");
    int clicks = 0;
    button->setOnClick([&] { ++clicks; });
    CHECK(harness.draw() == Rows{"t", "[ B ]"});

    text->setText("1\n2\n3");
    CHECK(harness.draw() == Rows{"1", "2"});
    CHECK(WidgetBase::of(*button).view().screenBounds().empty());
    static_cast<void>(harness.click({.x = 1, .y = 1}));
    CHECK(clicks == 0);

    text->setText("t");
    CHECK(harness.draw() == Rows{"t", "[ B ]"});
    CHECK(harness.click({.x = 1, .y = 1}) == EventResult::Handled);
    CHECK(clicks == 1);
}

TEST_CASE("tui widgets: a child skipped or squeezed out of a row takes no click at its old place",
          "[tui][widgets][drawn]") {
    Harness harness{8, 1};
    auto const row = harness.make<StackImpl>(nullptr, ui::Axis::Horizontal);
    int clicks = 0;
    SECTION("skipped, as by a scroll") {
        auto const button = harness.make<ButtonImpl>(row.get());
        button->setLabel("A");
        button->setOnClick([&] { ++clicks; });
        auto const text = harness.make<TextImpl>(row.get());
        text->setText("t");
        CHECK(harness.draw() == Rows{"[ A ]t"});
        row->setSkip(1);
        CHECK(harness.draw() == Rows{"t"});
        CHECK(WidgetBase::of(*button).view().screenBounds().empty());
        static_cast<void>(harness.click({.x = 3, .y = 0}));
    }
    SECTION("squeezed by a sibling that grew") {
        auto const text = harness.make<TextImpl>(row.get());
        text->setText("ab");
        auto const button = harness.make<ButtonImpl>(row.get());
        button->setLabel("B");
        button->setOnClick([&] { ++clicks; });
        CHECK(harness.draw() == Rows{"ab[ B ]"});
        text->setText("abcdefgh");
        CHECK(harness.draw() == Rows{"abcdefgh"});
        CHECK(WidgetBase::of(*button).view().screenBounds().empty());
        static_cast<void>(harness.click({.x = 4, .y = 0}));
    }
    CHECK(clicks == 0);
}

TEST_CASE("tui widgets: a child that falls off a shrinking terminal keeps no drawn bounds", "[tui][widgets][drawn]") {
    auto output = std::make_unique<ResizableOutput>(core::tui::Size{.width = 10, .height = 2});
    auto& terminal = *output;
    Harness harness{std::move(output)};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const text = harness.make<TextImpl>(column.get());
    text->setText("t");
    auto const button = harness.make<ButtonImpl>(column.get());
    button->setLabel("B");
    CHECK(harness.draw() == Rows{"t", "[ B ]"});
    REQUIRE_FALSE(WidgetBase::of(*button).view().screenBounds().empty());

    terminal.resize({.width = 10, .height = 1});
    static_cast<void>(harness.send(core::tui::ResizeEvent{.columns = 10, .rows = 1}));
    CHECK(harness.draw() == Rows{"t"});
    CHECK(WidgetBase::of(*button).view().screenBounds().empty());
    CHECK(harness.screen().componentAt(1, 1) == nullptr);
}

TEST_CASE("tui widgets: a press that lost its release is ended by the next press", "[tui][widgets]") {
    Harness harness{10, 2};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const button = harness.make<ButtonImpl>(column.get());
    button->setLabel("A");
    auto const text = harness.make<TextImpl>(column.get());
    text->setText("text");
    int clicks = 0;
    button->setOnClick([&] { ++clicks; });
    CHECK(harness.draw() == Rows{"[ A ]", "text"});

    CHECK(harness.send(pressAt({.x = 1, .y = 0})) == EventResult::Handled);
    static_cast<void>(harness.send(pressAt({.x = 1, .y = 1})));
    static_cast<void>(harness.send(releaseAt({.x = 1, .y = 0})));
    CHECK(clicks == 0);
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Handled);
    CHECK(clicks == 1);
}
