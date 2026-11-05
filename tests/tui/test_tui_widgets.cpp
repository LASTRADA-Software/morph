// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Rect.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <utility>
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

// Mutation: ignore `setSize` in ButtonImpl (the small button keeps its inner spaces).
TEST_CASE("tui widgets: a small push button drops the spaces inside its brackets", "[tui][widgets]") {
    Harness harness{20, 2};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const small = harness.make<ButtonImpl>(column.get());
    small->setLabel("S1");
    small->setSize(ui::ButtonSize::Small);
    small->setShape(ui::ControlShape::Round);
    small->setMono(true);
    auto const normal = harness.make<ButtonImpl>(column.get());
    normal->setLabel("S1");
    CHECK(harness.draw() == Rows{"[S1]", "[ S1 ]"});
}

// Mutation: ignore `setWeight` in ButtonImpl (the caption is not bold), or bold from 700 (the 600 caption is plain).
TEST_CASE("tui widgets: a button weight of 600 or more draws its caption bold, a lighter one plain",
          "[tui][widgets]") {
    Harness harness{20, 2};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const semi = harness.make<ButtonImpl>(column.get());
    semi->setLabel("S1");
    semi->setWeight(600);
    auto const light = harness.make<ButtonImpl>(column.get());
    light->setLabel("S2");
    light->setEmphasis(ui::ButtonEmphasis::Primary);
    light->setWeight(500);
    static_cast<void>(harness.draw());
    auto const& buffer = harness.screen().renderedBuffer();
    CHECK(buffer.at(0, 2).style.bold);
    CHECK_FALSE(buffer.at(1, 2).style.bold);
}

// Mutation: ignore the alignment in TextImpl::paintControl (each line starts at cell 0).
TEST_CASE("tui widgets: a text places each of its lines at its alignment within its width", "[tui][widgets][align]") {
    Harness harness{20, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const ended = harness.make<TextImpl>(column.get());
    ended->setText("abc");
    ended->setAlign(ui::TextAlign::End);
    auto const centred = harness.make<TextImpl>(column.get());
    centred->setText("abcd");
    centred->setAlign(ui::TextAlign::Center);
    auto const lines = harness.make<TextImpl>(column.get());
    lines->setText("abcdef\nab");
    lines->setAlign(ui::TextAlign::End);
    CHECK(harness.draw() ==
          Rows{"                 abc", "        abcd", "              abcdef", "                  ab"});
}

// Mutation: paint runs from column 0 whatever the alignment, or measure the line without its strong run.
TEST_CASE("tui widgets: a text with runs is placed by the width of all its runs", "[tui][widgets][align]") {
    Harness harness{20, 3};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const runs = harness.make<TextImpl>(column.get());
    runs->setSpans(
        {ui::TextSpan{.text = "a "}, ui::TextSpan{.text = "b", .strong = true}, ui::TextSpan{.text = " c"}});
    runs->setAlign(ui::TextAlign::End);
    auto const multi = harness.make<TextImpl>(column.get());
    multi->setSpans({ui::TextSpan{.text = "xyz\nx"}});
    multi->setAlign(ui::TextAlign::End);
    // "a b c" is five cells wide, so it starts at cell 15.
    CHECK(harness.draw() == Rows{"               a b c", "                 xyz", "                   x"});
}

// Mutation: place the required mark at the end of the unaligned text (cell 3, inside the aligned text's cells).
TEST_CASE("tui widgets: a required text's mark follows its aligned text", "[tui][widgets][align]") {
    Harness harness{20, 1};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const note = harness.make<TextImpl>(column.get());
    note->setText("abc");
    note->setRequired(true);
    note->setAlign(ui::TextAlign::Center);
    CHECK(harness.draw() == Rows{"        abc *"});
}

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
    // Step 3: two columns.
    row->setGap(3);
    auto const left = harness.make<TextImpl>(row.get());
    left->setText("ab");
    auto const right = harness.make<TextImpl>(row.get());
    right->setText("cd");
    CHECK(harness.draw() == Rows{"ab  cd"});
}

TEST_CASE("tui widgets: a column's gap step is rows, half as many as a row's columns", "[tui][widgets]") {
    for (auto const& [step, expected] :
         std::vector<std::pair<int, Rows>>{{2, Rows{"ab", "cd"}}, {3, Rows{"ab", "", "cd"}}}) {
        INFO("step " << step);
        Harness harness{10, 3};
        auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
        column->setGap(step);
        auto const top = harness.make<TextImpl>(column.get());
        top->setText("ab");
        auto const bottom = harness.make<TextImpl>(column.get());
        bottom->setText("cd");
        CHECK(harness.draw() == expected);
    }
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

TEST_CASE("tui widgets: a stack's own size counts a child's fixed extent", "[tui][widgets]") {
    Harness harness{10, 5};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const inner = harness.make<StackImpl>(column.get(), ui::Axis::Vertical);
    auto const tall = harness.make<TextImpl>(inner.get());
    tall->setText("t");
    tall->setLayout(ui::LayoutHints{.height = ui::Sizing::fixed(3)});
    auto const after = harness.make<TextImpl>(column.get());
    after->setText("after");
    CHECK(harness.draw() == Rows{"t", "", "", "after"});
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

// Mutation: activate the checkbox on Enter too (Enter then toggles it).
TEST_CASE("tui widgets: a checkbox toggles on Space, not Enter, and reports the new state; setChecked reports nothing",
          "[tui][widgets]") {
    Harness harness{20, 1};
    auto const box = harness.make<CheckboxImpl>(nullptr);
    std::vector<bool> toggles;
    box->setLabel("Done");
    box->setChecked(false);
    box->setOnToggle([&](bool checked) { toggles.push_back(checked); });
    CHECK(harness.draw() == Rows{"[ ] Done"});
    harness.focus(*box);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
    CHECK(toggles.empty());
    static_cast<void>(harness.type(" "));
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

/// A motion to a 0-based viewport cell with the left button held.
core::tui::MouseEvent moveTo(core::tui::Point cell) {
    return core::tui::MouseEvent{
        .type = core::tui::MouseEvent::Type::Move, .button = 0, .x = cell.x + 1, .y = cell.y + 1};
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
    CHECK(harness.type(" ") == EventResult::Ignored);
    CHECK(toggles.empty());
    CHECK(harness.draw().empty());

    outer->setVisible(true);
    CHECK(harness.type(" ") == EventResult::Handled);
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
    row->setColumnLayout({3, 3, 3}, 1, {});
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

TEST_CASE("tui widgets: a child squeezed out of a row takes no click at its old place", "[tui][widgets][drawn]") {
    Harness harness{8, 1};
    auto const row = harness.make<StackImpl>(nullptr, ui::Axis::Horizontal);
    int clicks = 0;
    auto const text = harness.make<TextImpl>(row.get());
    text->setText("ab");
    // A list item, which has no minimum width: a push button keeps its label and takes the room from the text.
    auto const button = harness.make<ButtonImpl>(row.get());
    button->setStyle(ui::ButtonStyle::Item);
    button->setLabel("B");
    button->setOnClick([&] { ++clicks; });
    CHECK(harness.draw().front().starts_with("ab B"));
    text->setText("abcdefgh");
    CHECK(harness.draw() == Rows{"abcdefgh"});
    CHECK(WidgetBase::of(*button).view().screenBounds().empty());
    static_cast<void>(harness.click({.x = 4, .y = 0}));
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

// The second press hits no widget's view: the only root is hidden, so it reaches the screen's own root. core::tui
// still ends the first press's pointer capture.
TEST_CASE("tui widgets: a press that reaches no widget ends a press that lost its release", "[tui][widgets]") {
    Harness harness{10, 2};
    auto const button = harness.make<ButtonImpl>(nullptr);
    button->setLabel("A");
    int clicks = 0;
    button->setOnClick([&] { ++clicks; });
    static_cast<void>(harness.draw());

    CHECK(harness.send(pressAt({.x = 1, .y = 0})) == EventResult::Handled);
    button->setVisible(false);
    CHECK(harness.send(pressAt({.x = 1, .y = 0})) == EventResult::Ignored);
    REQUIRE(harness.screen().pointerCapture() == nullptr);
    button->setVisible(true);
    SECTION("a frame drawn in between ends it") { static_cast<void>(harness.draw()); }
    SECTION("a motion over the widget ends it") { static_cast<void>(harness.send(moveTo({.x = 2, .y = 0}))); }
    static_cast<void>(harness.send(releaseAt({.x = 1, .y = 0})));
    CHECK(clicks == 0);
    CHECK(harness.click({.x = 1, .y = 0}) == EventResult::Handled);
    CHECK(clicks == 1);
}

TEST_CASE("tui widgets: a primary button is bold, a danger one bold in the error colour, a ghost one plain",
          "[tui][widgets]") {
    Harness harness{30, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const plain = harness.make<ButtonImpl>(column.get());
    plain->setLabel("Plain");
    auto const primary = harness.make<ButtonImpl>(column.get());
    primary->setLabel("Go");
    primary->setEmphasis(ui::ButtonEmphasis::Primary);
    auto const danger = harness.make<ButtonImpl>(column.get());
    danger->setLabel("Drop");
    danger->setEmphasis(ui::ButtonEmphasis::Danger);
    auto const ghost = harness.make<ButtonImpl>(column.get());
    ghost->setLabel("Remove");
    ghost->setEmphasis(ui::ButtonEmphasis::Ghost);
    static_cast<void>(harness.draw());
    auto const& buffer = harness.screen().renderedBuffer();
    CHECK_FALSE(buffer.at(0, 2).style.bold);
    CHECK(buffer.at(1, 2).style.bold);
    CHECK(buffer.at(2, 2).style.bold);
    CHECK(buffer.at(2, 2).style.fg == harness.screen().theme().error.fg);
    CHECK_FALSE(buffer.at(3, 2).style.bold);
}

// Mutation: map Section to Normal in roleStyle (the section is neither bold nor muted).
TEST_CASE("tui widgets: a section text is bold and muted, a step below a heading", "[tui][widgets]") {
    Harness harness{20, 2};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const section = harness.make<TextImpl>(column.get());
    section->setText("Lab");
    section->setRole(ui::TextRole::Section);
    auto const heading = harness.make<TextImpl>(column.get());
    heading->setText("App");
    heading->setRole(ui::TextRole::Heading);
    static_cast<void>(harness.draw());
    auto const& buffer = harness.screen().renderedBuffer();
    auto const& theme = harness.screen().theme();
    CHECK(buffer.at(0, 0).style.bold);
    CHECK(buffer.at(0, 0).style.fg == theme.textMuted.fg);
    CHECK(buffer.at(1, 0).style.bold);
    CHECK_FALSE(theme.textMuted.fg == theme.textBold.fg);
}

// Mutation: map Display to Normal in roleStyle (the figure is not bold).
TEST_CASE("tui widgets: a display text is bold as a heading is, and a small text is normal", "[tui][widgets]") {
    Harness harness{20, 2};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const display = harness.make<TextImpl>(column.get());
    display->setText("7");
    display->setRole(ui::TextRole::Display);
    auto const small = harness.make<TextImpl>(column.get());
    small->setText("note");
    small->setRole(ui::TextRole::Small);
    static_cast<void>(harness.draw());
    auto const& buffer = harness.screen().renderedBuffer();
    CHECK(buffer.at(0, 0).style.bold);
    CHECK_FALSE(buffer.at(0, 1).style.bold);
    CHECK(buffer.at(0, 1).style.fg == harness.screen().theme().textNormal.fg);
}

// Mutation: map Headline to Normal in roleStyle (the title is not bold, and differs from a heading).
TEST_CASE("tui widgets: a headline text is drawn as a heading is: a cell has one size", "[tui][widgets]") {
    Harness harness{20, 3};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const headline = harness.make<TextImpl>(column.get());
    headline->setText("Instrument");
    headline->setRole(ui::TextRole::Headline);
    auto const heading = harness.make<TextImpl>(column.get());
    heading->setText("Instrument");
    heading->setRole(ui::TextRole::Heading);
    auto const normal = harness.make<TextImpl>(column.get());
    normal->setText("Instrument");
    static_cast<void>(harness.draw());
    auto const& buffer = harness.screen().renderedBuffer();
    CHECK(buffer.at(0, 0).style.bold);
    CHECK(buffer.at(0, 0).style.fg == buffer.at(1, 0).style.fg);
    CHECK(buffer.at(0, 0).style.bold == buffer.at(1, 0).style.bold);
    CHECK_FALSE(buffer.at(2, 0).style.bold);
}

// Mutation: draw an item as a push button (its line shows `[ ... ]`).
TEST_CASE("tui widgets: an item button paints its label unbracketed across its line, selected in the list's style",
          "[tui][widgets]") {
    Harness harness{20, 3};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const push = harness.make<ButtonImpl>(column.get());
    push->setLabel("Push");
    auto const item = harness.make<ButtonImpl>(column.get());
    item->setLabel("Samples");
    item->setStyle(ui::ButtonStyle::Item);
    auto const other = harness.make<ButtonImpl>(column.get());
    other->setLabel("Results");
    other->setStyle(ui::ButtonStyle::Item);
    other->setSelected(true);
    Rows const rows = harness.draw();
    REQUIRE(rows.size() == 3);
    CHECK(rows.at(0).starts_with("[ Push ]"));
    CHECK(rows.at(1).starts_with(" Samples"));
    CHECK_FALSE(rows.at(1).contains('['));
    CHECK_FALSE(rows.at(2).contains('['));
    auto const& buffer = harness.screen().renderedBuffer();
    auto const& theme = harness.screen().theme();
    auto const same = [](::core::tui::Style const& lhs, ::core::tui::Style const& rhs) {
        return lhs.fg == rhs.fg && lhs.bg == rhs.bg && lhs.bold == rhs.bold && lhs.inverse == rhs.inverse;
    };
    // The selected item's style runs to the end of its line, past its label.
    CHECK(same(buffer.at(2, 19).style, theme.listItemSelected));
    CHECK(same(buffer.at(1, 19).style, theme.textNormal));
    CHECK_FALSE(same(theme.listItemSelected, theme.textNormal));
}

// Mutation: draw a selected push button's caption in the button's own style.
TEST_CASE("tui widgets: a selected push button draws its caption in the list's selected style, as wide as another",
          "[tui][widgets]") {
    Harness harness{20, 2};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const plain = harness.make<ButtonImpl>(column.get());
    plain->setLabel("A1");
    auto const picked = harness.make<ButtonImpl>(column.get());
    picked->setLabel("A2");
    picked->setSelected(true);
    Rows const rows = harness.draw();
    REQUIRE(rows.size() == 2);
    CHECK(rows.at(0).starts_with("[ A1 ]"));
    CHECK(rows.at(1).starts_with("[ A2 ]"));
    CHECK(plain->naturalSize().width == picked->naturalSize().width);
    auto const& buffer = harness.screen().renderedBuffer();
    auto const& theme = harness.screen().theme();
    auto const same = [](::core::tui::Style const& lhs, ::core::tui::Style const& rhs) {
        return lhs.fg == rhs.fg && lhs.bg == rhs.bg && lhs.bold == rhs.bold && lhs.inverse == rhs.inverse;
    };
    CHECK(same(buffer.at(1, 2).style, theme.listItemSelected));
    CHECK(same(buffer.at(1, 3).style, theme.listItemSelected));
    CHECK_FALSE(same(buffer.at(1, 0).style, theme.listItemSelected));
    CHECK_FALSE(same(buffer.at(0, 2).style, theme.listItemSelected));
}

TEST_CASE("tui widgets: a sidebar surface draws its subtree inverse; a card draws nothing", "[tui][widgets]") {
    Harness harness{20, 3};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const sidebar = harness.make<StackImpl>(column.get(), ui::Axis::Vertical);
    sidebar->setSurface("sidebar");
    auto const inside = harness.make<TextImpl>(sidebar.get());
    inside->setText("inside");
    auto const card = harness.make<StackImpl>(column.get(), ui::Axis::Vertical);
    card->setSurface("card");
    auto const carded = harness.make<TextImpl>(card.get());
    carded->setText("card");
    auto const outside = harness.make<TextImpl>(column.get());
    outside->setText("outside");
    CHECK(harness.draw() == Rows{"inside", "card", "outside"});
    auto const& buffer = harness.screen().renderedBuffer();
    CHECK(buffer.at(0, 0).style.inverse);
    CHECK_FALSE(buffer.at(1, 0).style.inverse);
    CHECK_FALSE(buffer.at(2, 0).style.inverse);
}

// Mutation: ignore `strong` in paintRuns (the strong run is not bold).
TEST_CASE("tui widgets: a text's runs draw in order, a strong run bold and its neighbours not, a line feed breaking",
          "[tui][widgets]") {
    Harness harness{30, 3};
    auto const text = harness.make<TextImpl>(nullptr);
    text->setSpans({{.text = "Lot "}, {.text = "L-26", .strong = true}, {.text = " ok\nnext", .mono = true}});
    CHECK(harness.draw() == Rows{"Lot L-26 ok", "next"});
    auto const& buffer = harness.screen().renderedBuffer();
    CHECK_FALSE(buffer.at(0, 3).style.bold);
    CHECK(buffer.at(0, 4).style.bold);
    CHECK(buffer.at(0, 7).style.bold);
    CHECK_FALSE(buffer.at(0, 8).style.bold);
    CHECK(text->probeText() == "Lot L-26 ok\nnext");
    text->setText("plain");
    CHECK(harness.draw() == Rows{"plain"});
}

// Mutation: draw a mono text with no ellipsis (it is cut like any text), or elide every role (the normal text ends
// in one too).
TEST_CASE("tui widgets: a mono text wider than its row ends in an ellipsis, and a normal text is cut",
          "[tui][widgets]") {
    Harness harness{8, 2};
    auto const text = harness.make<TextImpl>(nullptr);
    text->setRole(ui::TextRole::Mono);
    text->setText("S-26-104170");
    CHECK(harness.draw() == Rows{"S-26-10…"});
    CHECK(text->probeText() == "S-26-104170");
    text->setRole(ui::TextRole::Normal);
    CHECK(harness.draw() == Rows{"S-26-104"});
}

// Mutation: ignore a run's tone in paintRuns (the run keeps the text's colour), or let a text's tone replace its
// boldness (a heading is no longer bold).
TEST_CASE("tui widgets: a toned run takes the tone's colour, and a toned text keeps its role's boldness",
          "[tui][widgets]") {
    Harness harness{30, 3};
    auto const& theme = harness.screen().theme();
    auto const text = harness.make<TextImpl>(nullptr);
    text->setSpans({{.text = "Past due "}, {.text = "7", .tone = ui::Tone::Err}});
    CHECK(harness.draw() == Rows{"Past due 7"});
    auto const& buffer = harness.screen().renderedBuffer();
    CHECK_FALSE(buffer.at(0, 0).style.fg == theme.error.fg);
    CHECK(buffer.at(0, 9).style.fg == theme.error.fg);
    text->setText("Past due 7");
    text->setRole(ui::TextRole::Heading);
    text->setTone(ui::Tone::Err);
    static_cast<void>(harness.draw());
    auto const& drawn = harness.screen().renderedBuffer();
    CHECK(drawn.at(0, 0).style.fg == theme.error.fg);
    CHECK(drawn.at(0, 0).style.bold);
}

// Mutation: leave the mark's cells out of the text's natural size (the mark then overwrites its last characters).
TEST_CASE("tui widgets: a required text draws its mark after its last line, at its natural width in a row",
          "[tui][widgets]") {
    Harness harness{40, 2};
    auto const row = harness.make<StackImpl>(nullptr, ui::Axis::Horizontal);
    auto const text = harness.make<TextImpl>(row.get());
    text->setText("Condition on receipt");
    text->setRequired(true);
    auto const after = harness.make<TextImpl>(row.get());
    after->setText("|");
    CHECK(harness.draw().at(0).starts_with("Condition on receipt *"));
}

// Mutation: make dangerOutline bold (it then reads as danger).
TEST_CASE("tui widgets: a danger outlined button is red and not bold, a danger one red and bold", "[tui][widgets]") {
    Harness harness{30, 2};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const outlined = harness.make<ButtonImpl>(column.get());
    outlined->setLabel("Reject");
    outlined->setEmphasis(ui::ButtonEmphasis::DangerOutline);
    auto const danger = harness.make<ButtonImpl>(column.get());
    danger->setLabel("Drop");
    danger->setEmphasis(ui::ButtonEmphasis::Danger);
    static_cast<void>(harness.draw());
    auto const& buffer = harness.screen().renderedBuffer();
    CHECK(buffer.at(0, 2).style.fg == harness.screen().theme().error.fg);
    CHECK_FALSE(buffer.at(0, 2).style.bold);
    CHECK(buffer.at(1, 2).style.fg == harness.screen().theme().error.fg);
    CHECK(buffer.at(1, 2).style.bold);
}

// Mutation: leave out the fill of the line's style under the content (its rows lose the selected line's colour).
TEST_CASE("tui widgets: a selected list entry with content paints its line on every row, and takes a click on any",
          "[tui][widgets]") {
    Harness harness{30, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const item = harness.make<ButtonImpl>(column.get());
    item->setLabel("WS-2026-0412");
    item->setStyle(ui::ButtonStyle::Item);
    item->setSelected(true);
    int clicks = 0;
    item->setOnClick([&clicks] { ++clicks; });
    auto& content = item->content();
    auto const lines = harness.make<StackImpl>(&content, ui::Axis::Vertical);
    auto const first = harness.make<TextImpl>(lines.get());
    first->setText("WS-2026-0412");
    auto const second = harness.make<TextImpl>(lines.get());
    second->setText("Assay");
    auto const third = harness.make<TextImpl>(lines.get());
    third->setText("Tags");
    CHECK(harness.draw() == Rows{" WS-2026-0412", " Assay", " Tags"});
    auto const& buffer = harness.screen().renderedBuffer();
    // The line's style is under every row: in the leading cell and past the content's text.
    auto const& selected = harness.screen().theme().listItemSelected;
    for (int row = 0; row < 3; ++row) {
        INFO("row " << row);
        for (int const cell : {0, 20}) {
            CHECK(buffer.at(row, cell).style.fg == selected.fg);
            CHECK(buffer.at(row, cell).style.bold == selected.bold);
        }
    }
    static_cast<void>(harness.click({.x = 3, .y = 2}));
    CHECK(clicks == 1);
}

// Mutation: draw `square` as `default` ([ ‹ ]).
TEST_CASE("tui widgets: a square push button drops the spaces inside its brackets", "[tui][widgets]") {
    Harness harness{8, 1};
    auto const button = harness.make<ButtonImpl>(nullptr);
    button->setLabel("\xE2\x80\xB9");
    CHECK(harness.draw() == Rows{"[ \xE2\x80\xB9 ]"});
    button->setShape(ui::ControlShape::Square);
    CHECK(harness.draw() == Rows{"[\xE2\x80\xB9]"});
}

// Mutation: map Caption to Normal in roleStyle (the note is drawn in the normal text style).
TEST_CASE("tui widgets: a caption text is drawn in the muted style", "[tui][widgets]") {
    Harness harness{20, 1};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const caption = harness.make<TextImpl>(column.get());
    caption->setText("Needs a reason");
    caption->setRole(ui::TextRole::Caption);
    CHECK(harness.draw() == Rows{"Needs a reason"});
    auto const& buffer = harness.screen().renderedBuffer();
    CHECK(buffer.at(0, 0).style.fg == harness.screen().theme().textMuted.fg);
    CHECK_FALSE(buffer.at(0, 0).style.bold);
}

// Guard: a label is the normal text style on the terminal. It passes without the role too (an unknown role falls
// through to the normal style), and says what the terminal does with it.
TEST_CASE("tui widgets: a label text is drawn in the normal style", "[tui][widgets]") {
    Harness harness{20, 1};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const label = harness.make<TextImpl>(column.get());
    label->setText("Sample type");
    label->setRole(ui::TextRole::Label);
    CHECK(harness.draw() == Rows{"Sample type"});
    auto const& buffer = harness.screen().renderedBuffer();
    CHECK(buffer.at(0, 0).style.fg == harness.screen().theme().textNormal.fg);
    CHECK_FALSE(buffer.at(0, 0).style.bold);
}

// Mutation: map Overline to Normal in roleStyle (the label is neither bold nor muted).
TEST_CASE("tui widgets: an overline text is bold and muted, written as it reads", "[tui][widgets]") {
    Harness harness{20, 1};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const overline = harness.make<TextImpl>(column.get());
    overline->setText("Record type");
    overline->setRole(ui::TextRole::Overline);
    CHECK(harness.draw() == Rows{"Record type"});
    auto const& buffer = harness.screen().renderedBuffer();
    CHECK(buffer.at(0, 0).style.bold);
    CHECK(buffer.at(0, 0).style.fg == harness.screen().theme().textMuted.fg);
}

// Mutation: draw `[  ]` for an icon-only button whose icon has no glyph (the caption ignores the accessible name).
TEST_CASE("tui widgets: an icon-only button with no glyph draws its accessible name", "[tui][widgets]") {
    Harness harness{20, 3};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const glyph = harness.make<ButtonImpl>(column.get());
    glyph->setIcon("nav.back");
    glyph->setAccessibleName("Previous page");
    auto const named = harness.make<ButtonImpl>(column.get());
    named->setIcon("scan");
    named->setAccessibleName("Scan");
    auto const labelled = harness.make<ButtonImpl>(column.get());
    labelled->setIcon("scan");
    labelled->setLabel("Go");
    labelled->setAccessibleName("Start");
    CHECK(harness.draw() == Rows{"[ ‹  ]", "[ Scan ]", "[ Go ]"});
}

// Mutation: leave `toolbarJoined` out of the inverted surfaces (the joined top bar then draws plain).
TEST_CASE("tui widgets: the toolbar surfaces, joined to the head's block or not, draw their subtree inverse",
          "[tui][widgets]") {
    Harness harness{20, 3};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const toolbar = harness.make<StackImpl>(column.get(), ui::Axis::Vertical);
    toolbar->setSurface("toolbar");
    auto const toolbarText = harness.make<TextImpl>(toolbar.get());
    toolbarText->setText("toolbar");
    auto const joined = harness.make<StackImpl>(column.get(), ui::Axis::Vertical);
    joined->setSurface("toolbarJoined");
    auto const joinedText = harness.make<TextImpl>(joined.get());
    joinedText->setText("joined");
    auto const bar = harness.make<StackImpl>(column.get(), ui::Axis::Vertical);
    bar->setSurface("bar");
    auto const barText = harness.make<TextImpl>(bar.get());
    barText->setText("bar");
    CHECK(harness.draw() == Rows{"toolbar", "joined", "bar"});
    auto const& buffer = harness.screen().renderedBuffer();
    CHECK(buffer.at(0, 0).style.inverse);
    CHECK(buffer.at(1, 0).style.inverse);
    CHECK_FALSE(buffer.at(2, 0).style.inverse);
}

namespace {

/// What @p naturals ask for as stretch children of the weights @p weights: the weights' sum times the largest natural
/// per weight, rounded up to a whole cell.
int stretchNaturalOf(std::vector<int> const& naturals, std::vector<int> const& weights) {
    int sum = 0;
    for (int const weight : weights) {
        sum += weight;
    }
    int asked = 0;
    for (std::size_t index = 0; index < naturals.size(); ++index) {
        asked = std::max(asked, ((naturals.at(index) * sum) + weights.at(index) - 1) / weights.at(index));
    }
    return asked;
}

}  // namespace

// Mutation: sum the stretching children's naturals in `stackNaturalSize` (a row of two stretch texts asks for
// less than every share needs, so the first case's 20 is 11).
TEST_CASE("tui widgets: a row of stretch children asks for the weights' sum times the largest natural per weight",
          "[tui][widgets]") {
    struct Case {
        std::vector<int> naturals;
        std::vector<int> weights;
        int expected;
    };
    // The expected widths are written out beside the formula, which computes them again.
    for (auto const& testCase : {Case{.naturals = {1, 10}, .weights = {1, 1}, .expected = 20},
                                 Case{.naturals = {10, 10}, .weights = {1, 2}, .expected = 30},
                                 Case{.naturals = {10, 10}, .weights = {2, 1}, .expected = 30},
                                 Case{.naturals = {4, 10}, .weights = {1, 2}, .expected = 15},
                                 Case{.naturals = {10}, .weights = {3}, .expected = 10}}) {
        INFO(testCase.expected);
        Harness harness{80, 1};
        auto const row = harness.make<StackImpl>(nullptr, ui::Axis::Horizontal);
        std::vector<std::unique_ptr<TextImpl>> texts;
        for (std::size_t index = 0; index < testCase.naturals.size(); ++index) {
            texts.push_back(harness.make<TextImpl>(row.get()));
            texts.back()->setText(std::string(static_cast<std::size_t>(testCase.naturals.at(index)), 'x'));
            texts.back()->setLayout(ui::LayoutHints{.width = ui::Sizing::stretch(testCase.weights.at(index))});
        }
        CHECK(stretchNaturalOf(testCase.naturals, testCase.weights) == testCase.expected);
        CHECK(row->naturalSize().width == testCase.expected);
    }
}

// Mutation: leave a spacer out of `stretchSizingOf` (the row asks for the text's 4 cells, not 8), or drop the gaps.
TEST_CASE("tui widgets: a spacer counts as a stretch child of weight 1 and no natural size in a stack's natural size",
          "[tui][widgets]") {
    Harness harness{80, 1};
    auto const row = harness.make<StackImpl>(nullptr, ui::Axis::Horizontal);
    row->setGap(1);
    auto const text = harness.make<TextImpl>(row.get());
    text->setText("abcd");
    text->setLayout(ui::LayoutHints{.width = ui::Sizing::stretch()});
    auto const spacer = harness.make<SpacerImpl>(row.get());
    auto const fixed = harness.make<TextImpl>(row.get());
    fixed->setText("zz");
    // 2 stretching children of weight 1, the larger natural 4: 8 cells, then the fixed text and two gaps.
    CHECK(row->naturalSize().width == 8 + 2 + (2 * row->gap()));
}

// Mutation: leave `contentMinimumWidth` off ButtonImpl (a stretch push button is narrowed below its label).
TEST_CASE("tui widgets: a push button's minimum width is its label's, and a list item's is none", "[tui][widgets]") {
    Harness harness{40, 1};
    auto const row = harness.make<StackImpl>(nullptr, ui::Axis::Horizontal);
    auto const push = harness.make<ButtonImpl>(row.get());
    push->setLabel("Approve");
    auto const item = harness.make<ButtonImpl>(row.get());
    item->setLabel("Approve");
    item->setStyle(ui::ButtonStyle::Item);
    CHECK(push->contentMinimumWidth() == push->naturalSize().width);
    CHECK(push->contentMinimumWidth() > 7);
    CHECK(item->contentMinimumWidth() == 0);
}

// Guard: a text is cut at its width on the terminal, which never wraps it, so it keeps no minimum. It passes with or
// without a minimum on a text that wraps on another frontend.
TEST_CASE("tui widgets: a text keeps no minimum width", "[tui][widgets]") {
    Harness harness{40, 1};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const text = harness.make<TextImpl>(column.get());
    text->setText("Assay 95.0-105.0 and a longer word");
    CHECK(text->contentMinimumWidth() == 0);
}
