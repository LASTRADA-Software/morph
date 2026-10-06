// SPDX-License-Identifier: Apache-2.0
//
// Drag-and-drop driven by SGR mouse reports, decoded by core-cpp's parser:
// CSI < b ; x ; y M is a press (b = 0) or, with bit 32 set, a motion with the
// button held; a final m is the release. Coordinates are 1-based.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <core/tui/Rect.hpp>
#include <cstdint>
#include <memory>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <vector>

#include "tui/container_widgets.hpp"
#include "tui/drag.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui/widget.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using morph::tui::detail::ButtonImpl;
using morph::tui::detail::DialogImpl;
using morph::tui::detail::SliderImpl;
using morph::tui::detail::StackImpl;
using morph::tui::detail::TextImpl;
using morph::tui::detail::WidgetBase;
using morph::tui::testing::Harness;

namespace {

ui::Key key(std::int64_t value) { return ui::Key{value}; }

/// Two 10-cell columns two cells apart; "task" in the first carries key 7.
struct Board {
    explicit Board(Harness& harness)
        : board{harness.make<StackImpl>(nullptr, ui::Axis::Horizontal)},
          todo{harness.make<StackImpl>(board.get(), ui::Axis::Vertical)},
          done{harness.make<StackImpl>(board.get(), ui::Axis::Vertical)},
          card{harness.make<TextImpl>(todo.get())} {
        board->setGap(2);
        todo->setLayout({.width = ui::Sizing::fixed(10), .height = {}});
        done->setLayout({.width = ui::Sizing::fixed(10), .height = {}});
        card->setText("task");
        card->setDragKey(key(7));
    }

    std::unique_ptr<StackImpl> board;
    std::unique_ptr<StackImpl> todo;
    std::unique_ptr<StackImpl> done;
    std::unique_ptr<TextImpl> card;
};

bool contains(std::vector<std::string> const& rows, std::string const& text) {
    return std::ranges::any_of(rows, [&text](std::string const& row) { return row.contains(text); });
}

/// One SGR mouse report for the left button at 0-based viewport @p cell.
std::string report(int code, ::core::tui::Point cell, char final) {
    return "\x1b[<" + std::to_string(code) + ";" + std::to_string(cell.x + 1) + ";" + std::to_string(cell.y + 1) +
           final;
}

void press(Harness& harness, ::core::tui::Point cell) { harness.sgr(report(0, cell, 'M')); }

void motion(Harness& harness, ::core::tui::Point cell) { harness.sgr(report(32, cell, 'M')); }

void release(Harness& harness, ::core::tui::Point cell) { harness.sgr(report(0, cell, 'm')); }

/// The widget owning the component the pointer would hit at @p cell, or null.
WidgetBase const* ownerAt(Harness& harness, ::core::tui::Point cell) {
    return harness.context().ownerOf(harness.screen().componentAt(cell.y, cell.x));
}

/// The first cell of where @p widget was drawn last.
::core::tui::Point cornerOf(ui::Widget const& widget) {
    return WidgetBase::of(widget).view().screenBounds().position();
}

}  // namespace

TEST_CASE("tui drag: a card dragged onto an accepting column is dropped there", "[tui][drag]") {
    Harness harness{30, 6};
    Board board{harness};
    std::vector<ui::Key> dropped;
    // An accept-everything predicate, as the mount passes for a node without `accepts`.
    board.done->setDropHandler([](ui::Key const&) { return true; },
                               [&](ui::Key dropKey) { dropped.push_back(std::move(dropKey)); });
    static_cast<void>(harness.draw());

    harness.sgr("\x1b[<0;1;1M");    // press on the card
    harness.sgr("\x1b[<32;14;1M");  // move over the second column, button held
    auto const during = harness.draw();
    CHECK(contains(during, "[task]"));
    CHECK(contains(during, "╔"));
    CHECK(harness.context().drag->target() == &WidgetBase::of(*board.done));

    harness.sgr("\x1b[<0;14;1m");  // release
    CHECK(dropped == std::vector<ui::Key>{key(7)});
    auto const after = harness.draw();
    CHECK_FALSE(contains(after, "[task]"));
    CHECK_FALSE(contains(after, "╔"));
}

TEST_CASE("tui drag: a target whose accepts refuses the key is not highlighted and gets no drop", "[tui][drag]") {
    Harness harness{30, 6};
    Board board{harness};
    int dropped = 0;
    board.done->setDropHandler([](ui::Key const&) { return false; }, [&](ui::Key const&) { ++dropped; });
    static_cast<void>(harness.draw());
    harness.sgr("\x1b[<0;1;1M");
    harness.sgr("\x1b[<32;14;1M");
    CHECK(harness.context().drag->target() == nullptr);
    harness.sgr("\x1b[<0;14;1m");
    CHECK(dropped == 0);
}

TEST_CASE("tui drag: a press and release without motion is a click, not a drag", "[tui][drag]") {
    Harness harness{30, 2};
    auto const button = harness.make<ButtonImpl>(nullptr);
    int clicks = 0;
    button->setLabel("Card");
    button->setDragKey(key(3));
    button->setOnClick([&] { ++clicks; });
    static_cast<void>(harness.draw());
    harness.sgr("\x1b[<0;2;1M");
    harness.sgr("\x1b[<0;2;1m");
    CHECK(clicks == 1);
    CHECK_FALSE(harness.context().drag->dragging());
}

TEST_CASE("tui drag: destroying the drag source mid-drag ends the gesture", "[tui][drag]") {
    Harness harness{30, 6};
    Board board{harness};
    int dropped = 0;
    board.done->setDropHandler([](ui::Key const&) { return true; }, [&](ui::Key const&) { ++dropped; });
    static_cast<void>(harness.draw());
    harness.sgr("\x1b[<0;1;1M");
    harness.sgr("\x1b[<32;14;1M");
    REQUIRE(harness.context().drag->dragging());

    board.card.reset();  // the card was moved elsewhere and remounted
    CHECK(harness.context().drag->source() == nullptr);
    CHECK_FALSE(harness.context().drag->dragging());
    auto const rows = harness.draw();
    CHECK_FALSE(contains(rows, "[task]"));
    CHECK_FALSE(contains(rows, "╔"));
    harness.sgr("\x1b[<0;14;1m");
    CHECK(dropped == 0);
}

TEST_CASE("tui drag: the label and the outline let the pointer through to what lies under them", "[tui][drag]") {
    Harness harness{30, 6};
    Board board{harness};
    board.done->setDropHandler([](ui::Key const&) { return true; }, [](ui::Key const&) {});
    static_cast<void>(harness.draw());
    press(harness, {.x = 0, .y = 0});
    motion(harness, {.x = 13, .y = 0});
    auto const rows = harness.draw();
    REQUIRE(rows.at(0).contains("[task]"));
    // The label starts two cells right of the pointer, and the outline runs along the column's edge. Neither is hit,
    // so the hover state core::tui keeps never points at a component the drag owns.
    CHECK(ownerAt(harness, {.x = 15, .y = 0}) == &WidgetBase::of(*board.done));
    CHECK(ownerAt(harness, {.x = 12, .y = 0}) == &WidgetBase::of(*board.done));
    CHECK(ownerAt(harness, {.x = 12, .y = 3}) == &WidgetBase::of(*board.done));
    release(harness, {.x = 13, .y = 0});
}

TEST_CASE("tui drag: a source inside a container disabled two levels up does not drag", "[tui][drag][gating]") {
    Harness harness{30, 6};
    auto const row = harness.make<StackImpl>(nullptr, ui::Axis::Horizontal);
    auto const left = harness.make<StackImpl>(row.get(), ui::Axis::Vertical);
    auto const inner = harness.make<StackImpl>(left.get(), ui::Axis::Vertical);
    auto const card = harness.make<TextImpl>(inner.get());
    auto const done = harness.make<StackImpl>(row.get(), ui::Axis::Vertical);
    row->setGap(2);
    left->setLayout({.width = ui::Sizing::fixed(10), .height = {}});
    done->setLayout({.width = ui::Sizing::fixed(10), .height = {}});
    card->setText("task");
    card->setDragKey(key(7));
    int dropped = 0;
    done->setDropHandler([](ui::Key const&) { return true; }, [&](ui::Key const&) { ++dropped; });

    left->setEnabled(false);
    static_cast<void>(harness.draw());
    press(harness, {.x = 0, .y = 0});
    motion(harness, {.x = 13, .y = 0});
    CHECK_FALSE(harness.context().drag->dragging());
    CHECK_FALSE(contains(harness.draw(), "[task]"));
    release(harness, {.x = 13, .y = 0});
    CHECK(dropped == 0);

    left->setEnabled(true);
    static_cast<void>(harness.draw());
    press(harness, {.x = 0, .y = 0});
    motion(harness, {.x = 13, .y = 0});
    release(harness, {.x = 13, .y = 0});
    CHECK(dropped == 1);
}

TEST_CASE("tui drag: a target inside a container disabled two levels up takes no drop, and the search goes past it",
          "[tui][drag][gating]") {
    Harness harness{30, 6};
    Board board{harness};
    auto const inner = harness.make<StackImpl>(board.done.get(), ui::Axis::Vertical);
    auto const slot = harness.make<StackImpl>(inner.get(), ui::Axis::Vertical);
    inner->setLayout({.width = {}, .height = ui::Sizing::fixed(3)});
    slot->setLayout({.width = {}, .height = ui::Sizing::fixed(3)});
    int slotDrops = 0;
    int boardDrops = 0;
    slot->setDropHandler([](ui::Key const&) { return true; }, [&](ui::Key const&) { ++slotDrops; });
    board.board->setDropHandler([](ui::Key const&) { return true; }, [&](ui::Key const&) { ++boardDrops; });

    board.done->setEnabled(false);
    static_cast<void>(harness.draw());
    press(harness, {.x = 0, .y = 0});
    motion(harness, {.x = 13, .y = 1});
    CHECK(harness.context().drag->target() == &WidgetBase::of(*board.board));
    release(harness, {.x = 13, .y = 1});
    CHECK(slotDrops == 0);
    CHECK(boardDrops == 1);

    board.done->setEnabled(true);
    static_cast<void>(harness.draw());
    press(harness, {.x = 0, .y = 0});
    motion(harness, {.x = 13, .y = 1});
    CHECK(harness.context().drag->target() == &WidgetBase::of(*slot));
    release(harness, {.x = 13, .y = 1});
    CHECK(slotDrops == 1);
    CHECK(boardDrops == 1);
}

TEST_CASE("tui drag: a target or a source the user loses mid-drag takes part in no drop", "[tui][drag][gating]") {
    Harness harness{30, 6};
    Board board{harness};
    int dropped = 0;
    board.done->setDropHandler([](ui::Key const&) { return true; }, [&](ui::Key const&) { ++dropped; });
    static_cast<void>(harness.draw());

    SECTION("the target's container is disabled") {
        press(harness, {.x = 0, .y = 0});
        motion(harness, {.x = 13, .y = 0});
        REQUIRE(harness.context().drag->target() == &WidgetBase::of(*board.done));
        board.board->setEnabled(false);
        CHECK_FALSE(contains(harness.draw(), "╔"));
        release(harness, {.x = 13, .y = 0});
    }
    SECTION("the source's container is disabled") {
        press(harness, {.x = 0, .y = 0});
        motion(harness, {.x = 13, .y = 0});
        board.todo->setEnabled(false);
        motion(harness, {.x = 14, .y = 0});
        CHECK_FALSE(harness.context().drag->dragging());
        CHECK_FALSE(contains(harness.draw(), "[task]"));
        board.todo->setEnabled(true);
        release(harness, {.x = 14, .y = 0});
    }
    SECTION("the source's container is disabled right before the release") {
        press(harness, {.x = 0, .y = 0});
        motion(harness, {.x = 13, .y = 0});
        board.todo->setEnabled(false);
        release(harness, {.x = 13, .y = 0});
    }
    CHECK(dropped == 0);
    CHECK_FALSE(harness.context().drag->dragging());
}

TEST_CASE("tui drag: a source inside an open dialog drops only inside it", "[tui][drag][gating]") {
    Harness harness{30, 10};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const behind = harness.make<StackImpl>(column.get(), ui::Axis::Vertical);
    auto const dialog = harness.make<DialogImpl>(column.get());
    auto const body = harness.make<StackImpl>(dialog.get(), ui::Axis::Vertical);
    auto const card = harness.make<TextImpl>(body.get());
    auto const inside = harness.make<StackImpl>(body.get(), ui::Axis::Vertical);
    behind->setLayout({.width = {}, .height = ui::Sizing::fixed(2)});
    inside->setLayout({.width = ui::Sizing::fixed(6), .height = ui::Sizing::fixed(1)});
    card->setText("task");
    card->setDragKey(key(7));
    int behindDrops = 0;
    int insideDrops = 0;
    behind->setDropHandler([](ui::Key const&) { return true; }, [&](ui::Key const&) { ++behindDrops; });
    inside->setDropHandler([](ui::Key const&) { return true; }, [&](ui::Key const&) { ++insideDrops; });
    dialog->setOpen(true);
    static_cast<void>(harness.draw());
    auto const from = cornerOf(*card);
    auto const into = cornerOf(*inside);
    REQUIRE(from.y >= 2);
    REQUIRE(ownerAt(harness, {.x = 0, .y = 0}) == &WidgetBase::of(*behind));

    press(harness, from);
    motion(harness, {.x = 0, .y = 0});
    CHECK(harness.context().drag->target() == nullptr);
    release(harness, {.x = 0, .y = 0});
    CHECK(behindDrops == 0);

    press(harness, from);
    motion(harness, into);
    CHECK(harness.context().drag->target() == &WidgetBase::of(*inside));
    release(harness, into);
    CHECK(insideDrops == 1);
    CHECK(behindDrops == 0);
}

TEST_CASE("tui drag: a drop handler may destroy its own target, or the source", "[tui][drag][lifetime]") {
    Harness harness{30, 6};
    Board board{harness};
    int drops = 0;
    SECTION("its own target") {
        // `drops` is read through the closure after the reset: a handler called in place would read freed memory.
        board.done->setDropHandler([](ui::Key const&) { return true; },
                                   [&board, &drops](ui::Key const&) {
                                       board.done.reset();
                                       ++drops;
                                   });
    }
    SECTION("the source") {
        board.done->setDropHandler([](ui::Key const&) { return true; },
                                   [&board, &drops](ui::Key const&) {
                                       board.card.reset();
                                       ++drops;
                                   });
    }
    static_cast<void>(harness.draw());
    press(harness, {.x = 0, .y = 0});
    motion(harness, {.x = 13, .y = 0});
    release(harness, {.x = 13, .y = 0});
    CHECK(drops == 1);
    CHECK_FALSE(harness.context().drag->dragging());
    CHECK(harness.context().drag->source() == nullptr);
    CHECK(harness.context().drag->target() == nullptr);
    CHECK_FALSE(contains(harness.draw(), "╔"));
}

TEST_CASE("tui drag: an accepts predicate may destroy its own widget, or the source", "[tui][drag][lifetime]") {
    Harness harness{30, 6};
    Board board{harness};
    int asked = 0;
    int dropped = 0;
    bool sourceGone = false;
    SECTION("its own widget") {
        board.done->setDropHandler(
            [&board, &asked](ui::Key const&) {
                board.done.reset();
                ++asked;
                return true;
            },
            [&](ui::Key const&) { ++dropped; });
    }
    SECTION("the source") {
        sourceGone = true;
        board.done->setDropHandler(
            [&board, &asked](ui::Key const&) {
                board.card.reset();
                ++asked;
                return true;
            },
            [&](ui::Key const&) { ++dropped; });
    }
    static_cast<void>(harness.draw());
    press(harness, {.x = 0, .y = 0});
    motion(harness, {.x = 13, .y = 0});
    CHECK(asked == 1);
    CHECK(harness.context().drag->target() == nullptr);
    CHECK(harness.context().drag->dragging() == !sourceGone);
    static_cast<void>(harness.draw());
    release(harness, {.x = 13, .y = 0});
    CHECK(dropped == 0);
    CHECK_FALSE(harness.context().drag->dragging());
}

TEST_CASE("tui drag: the board reordering under a drag keeps the gesture, and the outline follows the target",
          "[tui][drag]") {
    Harness harness{30, 6};
    Board board{harness};
    auto const other = harness.make<TextImpl>(board.todo.get());
    other->setText("other");
    std::vector<ui::Key> dropped;
    board.done->setDropHandler([](ui::Key const&) { return true; },
                               [&](ui::Key dropKey) { dropped.push_back(std::move(dropKey)); });
    static_cast<void>(harness.draw());
    auto const& card = WidgetBase::of(*board.card).view();

    press(harness, {.x = 0, .y = 0});
    motion(harness, {.x = 13, .y = 0});
    REQUIRE(harness.context().drag->target() == &WidgetBase::of(*board.done));
    board.todo->moveChild(*board.card, 1);
    board.board->moveChild(*board.done, 0);
    auto const rows = harness.draw();
    CHECK(harness.screen().pointerCapture() == &card);
    CHECK(harness.context().drag->dragging());
    CHECK(cornerOf(*board.card).x == 12);
    CHECK(cornerOf(*board.card).y == 1);
    CHECK(rows.at(0).starts_with("╔"));

    motion(harness, {.x = 3, .y = 2});
    CHECK(harness.context().drag->target() == &WidgetBase::of(*board.done));
    release(harness, {.x = 3, .y = 2});
    CHECK(dropped == std::vector<ui::Key>{key(7)});
    CHECK_FALSE(harness.context().drag->dragging());
}

TEST_CASE("tui drag: a gesture whose pointer capture ends without its release is over", "[tui][drag]") {
    Harness harness{30, 6};
    Board board{harness};
    int dropped = 0;
    board.done->setDropHandler([](ui::Key const&) { return true; }, [&](ui::Key const&) { ++dropped; });
    static_cast<void>(harness.draw());
    press(harness, {.x = 0, .y = 0});
    motion(harness, {.x = 13, .y = 0});
    REQUIRE(harness.context().drag->dragging());

    SECTION("by the next frame") {
        harness.screen().releasePointer();
        CHECK_FALSE(contains(harness.draw(), "[task]"));
        CHECK_FALSE(harness.context().drag->dragging());
    }
    SECTION("at the next press, wherever it lands") {
        press(harness, {.x = 13, .y = 3});
        CHECK_FALSE(harness.context().drag->dragging());
        CHECK_FALSE(contains(harness.draw(), "[task]"));
    }
    motion(harness, {.x = 14, .y = 0});
    release(harness, {.x = 14, .y = 0});
    CHECK(dropped == 0);
}

TEST_CASE("tui drag: a source whose drag key is cleared mid-drag stops; a changed key is the one dropped",
          "[tui][drag]") {
    Harness harness{30, 6};
    Board board{harness};
    std::vector<ui::Key> dropped;
    board.done->setDropHandler([](ui::Key const&) { return true; },
                               [&](ui::Key dropKey) { dropped.push_back(std::move(dropKey)); });
    static_cast<void>(harness.draw());
    press(harness, {.x = 0, .y = 0});
    motion(harness, {.x = 13, .y = 0});

    SECTION("cleared") {
        board.card->setDragKey(std::nullopt);
        CHECK_FALSE(harness.context().drag->dragging());
        release(harness, {.x = 13, .y = 0});
        CHECK(dropped.empty());
    }
    SECTION("changed") {
        board.card->setDragKey(key(7));
        CHECK(harness.context().drag->dragging());
        board.card->setDragKey(key(8));
        release(harness, {.x = 13, .y = 0});
        CHECK(dropped == std::vector<ui::Key>{key(8)});
    }
}

TEST_CASE("tui drag: a source dropped back on itself is no click, and its container may take it", "[tui][drag]") {
    Harness harness{30, 3};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const button = harness.make<ButtonImpl>(column.get());
    int clicks = 0;
    int ownDrops = 0;
    std::vector<ui::Key> dropped;
    button->setLabel("Card");
    button->setDragKey(key(3));
    button->setOnClick([&] { ++clicks; });
    button->setDropHandler([](ui::Key const&) { return true; }, [&](ui::Key const&) { ++ownDrops; });
    column->setDropHandler([](ui::Key const&) { return true; },
                           [&](ui::Key dropKey) { dropped.push_back(std::move(dropKey)); });
    static_cast<void>(harness.draw());
    press(harness, {.x = 1, .y = 0});
    motion(harness, {.x = 1, .y = 2});
    motion(harness, {.x = 2, .y = 0});
    CHECK(harness.context().drag->target() == &WidgetBase::of(*column));
    release(harness, {.x = 2, .y = 0});
    CHECK(clicks == 0);
    CHECK(ownDrops == 0);
    CHECK(dropped == std::vector<ui::Key>{key(3)});
}

TEST_CASE("tui drag: a draggable slider takes its press, and the motion drags it rather than its thumb",
          "[tui][drag]") {
    Harness harness{30, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const slider = harness.make<SliderImpl>(column.get());
    auto const done = harness.make<StackImpl>(column.get(), ui::Axis::Vertical);
    slider->setLayout({.width = ui::Sizing::fixed(24), .height = {}});
    done->setLayout({.width = {}, .height = ui::Sizing::fixed(2)});
    slider->setRange(0, 22, 1);
    std::vector<std::int64_t> values;
    std::vector<ui::Key> dropped;
    slider->setOnChange([&](std::int64_t value) { values.push_back(value); });
    done->setDropHandler([](ui::Key const&) { return true; },
                         [&](ui::Key dropKey) { dropped.push_back(std::move(dropKey)); });
    static_cast<void>(harness.draw());

    SECTION("without a drag key the thumb follows the pointer") {
        press(harness, {.x = 6, .y = 0});
        motion(harness, {.x = 11, .y = 1});
        release(harness, {.x = 11, .y = 1});
        CHECK(values == std::vector<std::int64_t>{6, 12});
        CHECK(dropped.empty());
    }
    SECTION("with a drag key the slider is dragged") {
        slider->setDragKey(key(5));
        press(harness, {.x = 6, .y = 0});
        motion(harness, {.x = 11, .y = 1});
        CHECK(harness.context().drag->target() == &WidgetBase::of(*done));
        release(harness, {.x = 11, .y = 1});
        CHECK(values == std::vector<std::int64_t>{6});
        CHECK(dropped == std::vector<ui::Key>{key(5)});
    }
}
