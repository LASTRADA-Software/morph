// SPDX-License-Identifier: Apache-2.0
//
// The spotlight on the terminal (docs/spec/tui/frontend.md, "The spotlight"): the card is a box under the target, or
// over it when there is no room below; the target is drawn in reverse video and nothing else is dimmed; Tab stays in
// the card, and on a waiting step reaches the target.

#include <catch2/catch_test_macros.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/ui/backend.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "tui/container_widgets.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui/list_widgets.hpp"
#include "tui/widget.hpp"
#include "tui_harness.hpp"

namespace {

namespace detail = morph::tui::detail;
namespace ui = morph::ui;
using morph::tui::testing::Harness;

/// The row a text first shows on, or npos.
std::size_t rowOf(std::vector<std::string> const& rows, std::string const& text) {
    for (std::size_t row = 0; row < rows.size(); ++row) {
        if (rows.at(row).contains(text)) {
            return row;
        }
    }
    return std::string::npos;
}

/// A column of a button `OTHER`, a spacer @p above rows tall and a button `TARGET`, with an open spotlight on the
/// target whose card is a stack of two buttons, `BACK` and `NEXT`. One draw has been made, as the first frame of a
/// session; `settle` draws on while the box is misplaced, as the session does. With a @p surface, the column is that
/// surface.
struct Rig {
    explicit Rig(int above, std::string_view surface = {}) {
        column = harness.make<detail::StackImpl>(nullptr, ui::Axis::Vertical);
        column->setSurface(surface);
        other = harness.make<detail::ButtonImpl>(column.get());
        other->setLabel("OTHER");
        spacer = harness.make<detail::SpacerImpl>(column.get());
        spacer->setLayout({.width = {}, .height = ui::Sizing::fixed(above)});
        target = harness.make<detail::ButtonImpl>(column.get());
        target->setLabel("TARGET");
        target->setTestId("target");
        target->setOnClick([this] { ++clicks; });
        spotlight = harness.make<detail::SpotlightImpl>(nullptr);
        spotlight->setKeys({"Escape"}, [this](std::string const&) { ++ended; });
        card = harness.make<detail::StackImpl>(spotlight.get(), ui::Axis::Vertical);
        card->setKeys({"Right"}, [this](std::string const&) { ++advanced; });
        back = harness.make<detail::ButtonImpl>(card.get());
        back->setLabel("BACK");
        next = harness.make<detail::ButtonImpl>(card.get());
        next->setLabel("NEXT");
        spotlight->setTarget(std::string{"target"});
        spotlight->setOpen(true);
        rows = harness.draw();
    }
    /// Draws again for as long as the box asks to be placed again, and keeps the rows of the last frame.
    void settle() {
        for (int pass = 0; pass < 3 && harness.context().spotlightMisplaced(); ++pass) {
            rows = harness.draw();
        }
    }
    /// Whether the cell of @p text's first character, on the last drawn frame, is in reverse video.
    [[nodiscard]] bool inverseAt(std::string const& text) {
        std::size_t const row = rowOf(rows, text);
        REQUIRE(row != std::string::npos);
        int const x = static_cast<int>(rows.at(row).find(text));
        return harness.screen().renderedBuffer().cellAt({.x = x, .y = static_cast<int>(row)})->style.inverse;
    }
    Harness harness{40, 12};
    std::unique_ptr<detail::StackImpl> column;
    std::unique_ptr<detail::SpacerImpl> spacer;
    std::unique_ptr<detail::ButtonImpl> target;
    std::unique_ptr<detail::ButtonImpl> other;
    std::unique_ptr<detail::SpotlightImpl> spotlight;
    std::unique_ptr<detail::StackImpl> card;
    std::unique_ptr<detail::ButtonImpl> back;
    std::unique_ptr<detail::ButtonImpl> next;
    std::vector<std::string> rows;
    int clicks = 0;
    int ended = 0;
    int advanced = 0;
};

}  // namespace

// Mutation: leave `framePosition` centring the box (its row is the screen's middle, not under the target).
TEST_CASE("tui spotlight: the card is a box under the target, or over it when there is no room below",
          "[tui][tours]") {
    Rig top{0};
    top.settle();
    std::size_t const targetRow = rowOf(top.rows, "TARGET");
    REQUIRE(targetRow != std::string::npos);
    // The box's top border is the row under the target, so the first button is one row further.
    CHECK(rowOf(top.rows, "BACK") == targetRow + 2);
    CHECK(top.spotlight->targetShown());
    Rig bottom{9};
    bottom.settle();
    std::size_t const lowRow = rowOf(bottom.rows, "TARGET");
    REQUIRE(lowRow != std::string::npos);
    // Two buttons and the bottom border sit between the first button and the target.
    CHECK(rowOf(bottom.rows, "BACK") + 3 == lowRow);
}

// Mutation: make `SpotlightImpl::placementStale` answer false (nothing asks for the frame that places the box).
TEST_CASE("tui spotlight: the first frame asks for a second that places the box, and a moved target for another",
          "[tui][tours]") {
    Rig rig{0};
    // One frame drew the target; the box was placed before that, by nothing, and the code itself says so.
    CHECK(rig.harness.context().spotlightMisplaced());
    CHECK(rowOf(rig.rows, "BACK") != rowOf(rig.rows, "TARGET") + 2);
    rig.rows = rig.harness.draw();
    CHECK_FALSE(rig.harness.context().spotlightMisplaced());
    CHECK(rowOf(rig.rows, "BACK") == rowOf(rig.rows, "TARGET") + 2);
    rig.spacer->setLayout({.width = {}, .height = ui::Sizing::fixed(3)});
    rig.rows = rig.harness.draw();
    CHECK(rig.harness.context().spotlightMisplaced());
    rig.rows = rig.harness.draw();
    CHECK_FALSE(rig.harness.context().spotlightMisplaced());
    CHECK(rowOf(rig.rows, "BACK") == rowOf(rig.rows, "TARGET") + 2);
}

// Mutation: drop the `spotlit` branch of `WidgetBase::render` (the target is drawn as before).
TEST_CASE("tui spotlight: the target is drawn in reverse video and nothing else is dimmed", "[tui][tours]") {
    Rig rig{0};
    rig.settle();
    CHECK(rig.inverseAt("TARGET"));
    // The whole row of the other button, not one cell of it.
    std::size_t const row = rowOf(rig.rows, "OTHER");
    REQUIRE(row != std::string::npos);
    for (int column = 0; column < 40; ++column) {
        CHECK_FALSE(
            rig.harness.screen().renderedBuffer().cellAt({.x = column, .y = static_cast<int>(row)})->style.inverse);
    }
    rig.spotlight->setOpen(false);
    // Closing hands the focus to the first widget, which a button draws in reverse video too: move it elsewhere.
    rig.harness.focus(*rig.other);
    rig.rows = rig.harness.draw();
    CHECK_FALSE(rig.inverseAt("TARGET"));
}

// Mutation: make the mark `invert` (set) instead of flip in `WidgetBase::render`: the target on an inverted surface
// then looks like everything else on it.
TEST_CASE("tui spotlight: a target on an inverted surface is marked by not being inverted", "[tui][tours]") {
    Rig rig{0, "sidebar"};
    rig.settle();
    CHECK(rig.inverseAt("OTHER"));
    CHECK_FALSE(rig.inverseAt("TARGET"));
}

// Mutation: as above, for a target that is drawn inverse because it has the focus.
TEST_CASE("tui spotlight: a focused target is marked by not being inverted", "[tui][tours]") {
    Rig rig{0};
    rig.spotlight->setTargetFocusable(true);
    detail::moveFocus(rig.harness.context(), detail::Direction::Forward);
    detail::moveFocus(rig.harness.context(), detail::Direction::Forward);
    REQUIRE(rig.harness.focused(*rig.target));
    rig.rows = rig.harness.draw();
    rig.settle();
    CHECK_FALSE(rig.inverseAt("TARGET"));
    // Without the mark the focused button is the inverse one.
    rig.spotlight->setOpen(false);
    rig.harness.focus(*rig.target);
    rig.rows = rig.harness.draw();
    CHECK(rig.inverseAt("TARGET"));
}

// Mutation: make `SpotlightImpl::tabTarget` answer null (Tab never reaches the target).
TEST_CASE("tui spotlight: Tab stays in the card, and on a waiting step reaches the target", "[tui][tours]") {
    Rig rig{0};
    CHECK(rig.harness.focused(*rig.back));
    detail::moveFocus(rig.harness.context(), detail::Direction::Forward);
    CHECK(rig.harness.focused(*rig.next));
    detail::moveFocus(rig.harness.context(), detail::Direction::Forward);
    CHECK(rig.harness.focused(*rig.back));
    rig.spotlight->setTargetFocusable(true);
    detail::moveFocus(rig.harness.context(), detail::Direction::Forward);
    detail::moveFocus(rig.harness.context(), detail::Direction::Forward);
    CHECK(rig.harness.focused(*rig.target));
    static_cast<void>(rig.harness.key(core::tui::KeyCode::Enter));
    CHECK(rig.clicks == 1);
}

// Mutation: make `WidgetBase::spotlightChord` answer Ignored when no spotlight is around the widget (the target's
// focus cuts off the card's keys).
TEST_CASE("tui spotlight: the card's keys reach it while the target has the focus", "[tui][tours]") {
    Rig rig{0};
    rig.spotlight->setTargetFocusable(true);
    detail::moveFocus(rig.harness.context(), detail::Direction::Forward);
    detail::moveFocus(rig.harness.context(), detail::Direction::Forward);
    REQUIRE(rig.harness.focused(*rig.target));
    static_cast<void>(rig.harness.key(core::tui::KeyCode::Escape));
    CHECK(rig.ended == 1);
    static_cast<void>(rig.harness.key(core::tui::KeyCode::Right));
    CHECK(rig.advanced == 1);
    // Enter stays with the target.
    static_cast<void>(rig.harness.key(core::tui::KeyCode::Enter));
    CHECK(rig.clicks == 1);
}

// Mutation: drop the `passesThrough` branch of `WidgetBase::blockedByDialog` (the open box swallows the press).
TEST_CASE("tui spotlight: a press on the target reaches it, and one on anything else does not", "[tui][tours]") {
    Rig rig{0};
    rig.settle();
    std::size_t const targetRow = rowOf(rig.rows, "TARGET");
    std::size_t const otherRow = rowOf(rig.rows, "OTHER");
    REQUIRE(targetRow != std::string::npos);
    REQUIRE(otherRow != std::string::npos);
    static_cast<void>(rig.harness.click({.x = 2, .y = static_cast<int>(targetRow)}));
    CHECK(rig.clicks == 1);
    int otherClicks = 0;
    rig.other->setOnClick([&otherClicks] { ++otherClicks; });
    static_cast<void>(rig.harness.click({.x = 2, .y = static_cast<int>(otherRow)}));
    CHECK(otherClicks == 0);
}

// The control for the list the target opens: the list is its own focused component, so it takes Escape before any
// chord. Mutation: drop `Escape` from the popup list's `onEvent` (the key then reaches the card and ends the tour).
TEST_CASE("tui spotlight: a key the target's list takes does not reach the card", "[tui][tours]") {
    Harness harness{40, 12};
    auto column = harness.make<detail::StackImpl>(nullptr, ui::Axis::Vertical);
    auto menu = harness.make<detail::MenuButtonImpl>(column.get());
    menu->setTestId("target");
    menu->setTrigger(ui::MenuTrigger{.label = "TARGET"});
    menu->setItems({ui::MenuEntry{.label = "First"}, ui::MenuEntry{.label = "Second"}});
    auto spotlight = harness.make<detail::SpotlightImpl>(nullptr);
    int ended = 0;
    spotlight->setKeys({"Escape"}, [&ended](std::string const&) { ++ended; });
    auto card = harness.make<detail::StackImpl>(spotlight.get(), ui::Axis::Vertical);
    auto next = harness.make<detail::ButtonImpl>(card.get());
    next->setLabel("NEXT");
    spotlight->setTarget(std::string{"target"});
    spotlight->setOpen(true);
    static_cast<void>(harness.draw());
    harness.focus(*menu);
    static_cast<void>(harness.key(core::tui::KeyCode::Enter));
    REQUIRE(menu->isOpen());
    static_cast<void>(harness.key(core::tui::KeyCode::Escape));
    CHECK_FALSE(menu->isOpen());
    CHECK(ended == 0);
    // With the list closed the target ignores Escape, and the card's chord runs.
    harness.focus(*menu);
    static_cast<void>(harness.key(core::tui::KeyCode::Escape));
    CHECK(ended == 1);
}

// Mutation: make `WidgetBase::handle` ask `targetChord` before the target's own `key` (Right moves the card on
// instead of the slider's knob).
TEST_CASE("tui spotlight: a key the target's slider takes does not reach the card", "[tui][tours]") {
    Harness harness{40, 12};
    auto column = harness.make<detail::StackImpl>(nullptr, ui::Axis::Vertical);
    auto slider = harness.make<detail::SliderImpl>(column.get());
    slider->setTestId("target");
    slider->setRange(0, 10, 1);
    std::int64_t value = 0;
    slider->setOnChange([&value](std::int64_t changed) { value = changed; });
    auto spotlight = harness.make<detail::SpotlightImpl>(nullptr);
    int advanced = 0;
    int ended = 0;
    spotlight->setKeys({"Right", "Escape"},
                       [&advanced, &ended](std::string const& chord) { (chord == "Right" ? advanced : ended)++; });
    auto card = harness.make<detail::StackImpl>(spotlight.get(), ui::Axis::Vertical);
    auto next = harness.make<detail::ButtonImpl>(card.get());
    next->setLabel("NEXT");
    spotlight->setTarget(std::string{"target"});
    spotlight->setOpen(true);
    static_cast<void>(harness.draw());
    harness.focus(*slider);
    static_cast<void>(harness.key(core::tui::KeyCode::Right));
    CHECK(value == 1);
    CHECK(advanced == 0);
    // Escape is nothing to the slider, so the card's chord runs.
    static_cast<void>(harness.key(core::tui::KeyCode::Escape));
    CHECK(ended == 1);
}
