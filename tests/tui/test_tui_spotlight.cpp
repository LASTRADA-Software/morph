// SPDX-License-Identifier: Apache-2.0
//
// The spotlight on the terminal (docs/spec/tui/frontend.md, "The spotlight"): the card is a box under the target, or
// over it when there is no room below; the target is drawn in reverse video and nothing else is dimmed; Tab stays in
// the card, and on a waiting step reaches the target.

#include <catch2/catch_test_macros.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <cstddef>
#include <memory>
#include <morph/ui/backend.hpp>
#include <string>
#include <vector>

#include "tui/container_widgets.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui/widget.hpp"
#include "tui_harness.hpp"

namespace {

namespace detail = morph::tui::detail;
namespace ui = morph::ui;
using morph::tui::testing::Harness;

/// The row a text first shows on, or npos.
std::size_t rowOf(std::vector<std::string> const& rows, std::string const& text) {
    for (std::size_t row = 0; row < rows.size(); ++row) {
        if (rows.at(row).find(text) != std::string::npos) {
            return row;
        }
    }
    return std::string::npos;
}

/// A column of a spacer @p above rows tall and a button `TARGET`, and an open spotlight on it with a card of two
/// buttons, `BACK` and `NEXT`.
struct Rig {
    explicit Rig(int above) {
        column = harness.make<detail::StackImpl>(nullptr, ui::Axis::Vertical);
        spacer = harness.make<detail::SpacerImpl>(column.get());
        spacer->setLayout({.width = {}, .height = ui::Sizing::fixed(above)});
        target = harness.make<detail::ButtonImpl>(column.get());
        target->setLabel("TARGET");
        target->setTestId("target");
        target->setOnClick([this] { ++clicks; });
        other = harness.make<detail::ButtonImpl>(column.get());
        other->setLabel("OTHER");
        spotlight = harness.make<detail::SpotlightImpl>(nullptr);
        back = harness.make<detail::ButtonImpl>(spotlight.get());
        back->setLabel("BACK");
        next = harness.make<detail::ButtonImpl>(spotlight.get());
        next->setLabel("NEXT");
        spotlight->setTarget(std::string{"target"});
        spotlight->setOpen(true);
        // The first frame draws the target, whose rectangle the next one places the card by.
        static_cast<void>(harness.draw());
        rows = harness.draw();
    }
    Harness harness{40, 12};
    std::unique_ptr<detail::StackImpl> column;
    std::unique_ptr<detail::SpacerImpl> spacer;
    std::unique_ptr<detail::ButtonImpl> target;
    std::unique_ptr<detail::ButtonImpl> other;
    std::unique_ptr<detail::SpotlightImpl> spotlight;
    std::unique_ptr<detail::ButtonImpl> back;
    std::unique_ptr<detail::ButtonImpl> next;
    std::vector<std::string> rows;
    int clicks = 0;
};

}  // namespace

// Mutation: leave `framePosition` centring the box (its row is the screen's middle, not under the target).
TEST_CASE("tui spotlight: the card is a box under the target, or over it when there is no room below",
          "[tui][tours]") {
    Rig const top{0};
    std::size_t const targetRow = rowOf(top.rows, "TARGET");
    REQUIRE(targetRow != std::string::npos);
    // The box's top border is the row under the target, so the first button is one row further.
    CHECK(rowOf(top.rows, "BACK") == targetRow + 2);
    CHECK(top.spotlight->targetShown());
    Rig const bottom{10};
    std::size_t const lowRow = rowOf(bottom.rows, "TARGET");
    REQUIRE(lowRow != std::string::npos);
    // Two buttons and the bottom border sit between the first button and the target.
    CHECK(rowOf(bottom.rows, "BACK") + 3 == lowRow);
}

// Mutation: drop the `spotlit` branch of `WidgetBase::render` (the target is drawn as before).
TEST_CASE("tui spotlight: the target is drawn in reverse video and nothing else is dimmed", "[tui][tours]") {
    Rig rig{0};
    std::size_t const row = rowOf(rig.rows, "TARGET");
    REQUIRE(row != std::string::npos);
    int const column = static_cast<int>(rig.rows.at(row).find("TARGET"));
    auto const& buffer = rig.harness.screen().renderedBuffer();
    CHECK(buffer.cellAt({.x = column, .y = static_cast<int>(row)})->style.inverse);
    CHECK_FALSE(buffer.cellAt({.x = 39, .y = 11})->style.inverse);
    rig.spotlight->setOpen(false);
    // Closing hands the focus to the first widget, which a button draws in reverse video too: move it elsewhere.
    rig.harness.focus(*rig.other);
    rig.harness.draw();
    CHECK_FALSE(
        rig.harness.screen().renderedBuffer().cellAt({.x = column, .y = static_cast<int>(row)})->style.inverse);
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
