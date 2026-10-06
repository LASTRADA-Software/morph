// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Rect.hpp>
#include <cstdint>
#include <limits>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <vector>

#include "tui/container_widgets.hpp"
#include "tui/field_widgets.hpp"
#include "tui/leaf_widgets.hpp"
#include "tui/list_widgets.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using core::tui::EventResult;
using core::tui::KeyCode;
using morph::tui::detail::BusyImpl;
using morph::tui::detail::ButtonImpl;
using morph::tui::detail::DialogImpl;
using morph::tui::detail::Direction;
using morph::tui::detail::DropdownSelectImpl;
using morph::tui::detail::FilePickerImpl;
using morph::tui::detail::moveFocus;
using morph::tui::detail::SliderImpl;
using morph::tui::detail::StackImpl;
using morph::tui::detail::TextImpl;
using morph::tui::detail::WidgetBase;
using morph::tui::testing::Harness;
using morph::tui::testing::ResizableOutput;
using Rows = std::vector<std::string>;

namespace {

/// A column with a "Behind" button, and a dialog titled "Confirm" holding the given buttons.
struct Scene {
    explicit Scene(Harness& harness, std::vector<std::string> const& labels)
        : column{harness.make<StackImpl>(nullptr, ui::Axis::Vertical)},
          behind{harness.make<ButtonImpl>(column.get())},
          dialog{harness.make<DialogImpl>(column.get())},
          body{harness.make<StackImpl>(dialog.get(), ui::Axis::Vertical)} {
        behind->setLabel("Behind");
        dialog->setTitle("Confirm");
        for (auto const& label : labels) {
            buttons.push_back(harness.make<ButtonImpl>(body.get()));
            buttons.back()->setLabel(label);
        }
    }
    ~Scene() { buttons.clear(); }
    Scene(Scene const&) = delete;
    Scene& operator=(Scene const&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(Scene&&) = delete;

    /// Destroys the dialog as the mount does: its content first.
    void destroyDialog() {
        buttons.clear();
        body.reset();
        dialog.reset();
    }

    /// Destroys everything, children before parents.
    void destroyAll() {
        destroyDialog();
        behind.reset();
        column.reset();
    }

    std::unique_ptr<StackImpl> column;
    std::unique_ptr<ButtonImpl> behind;
    std::unique_ptr<DialogImpl> dialog;
    std::unique_ptr<StackImpl> body;
    std::vector<std::unique_ptr<ButtonImpl>> buttons;
};

/// A left-button press on a 0-based viewport cell, with no release.
core::tui::MouseEvent pressAt(core::tui::Point cell) {
    return core::tui::MouseEvent{
        .type = core::tui::MouseEvent::Type::Press, .button = 0, .x = cell.x + 1, .y = cell.y + 1};
}

/// Esc, as the terminal reports it.
core::tui::InputEvent escape() { return core::tui::test::specialKey(KeyCode::Escape, core::tui::Modifier::None); }

/// Where @p widget was drawn in the last frame.
core::tui::Rect drawnAt(ui::Widget const& widget) { return WidgetBase::of(widget).view().screenBounds(); }

/// The middle cell of @p area.
core::tui::Point middleOf(core::tui::Rect area) {
    return {.x = area.x + (area.width / 2), .y = area.y + (area.height / 2)};
}

}  // namespace

TEST_CASE("tui dialog: an open dialog is a centred box over the tree, and takes the focus", "[tui][dialog]") {
    Harness harness{30, 10};
    Scene scene{harness, {}};
    auto const text = harness.make<TextImpl>(scene.body.get());
    text->setText("Sure?");
    scene.buttons.push_back(harness.make<ButtonImpl>(scene.body.get()));
    scene.buttons.back()->setLabel("OK");
    scene.dialog->setOpen(true);
    CHECK(harness.draw() == Rows{"[ Behind ]", "", "", "         ┌─Confirm─┐", "         │ Sure?   │",
                                 "         │ [ OK ]  │", "         └─────────┘"});
    CHECK(harness.focused(*scene.buttons.back()));
    scene.buttons.clear();
}

TEST_CASE("tui dialog: Esc anywhere inside calls onDismiss", "[tui][dialog]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK"}};
    int dismissed = 0;
    scene.dialog->setOnDismiss([&] { ++dismissed; });
    scene.dialog->setOpen(true);
    static_cast<void>(harness.key(KeyCode::Escape));
    CHECK(dismissed == 1);
}

TEST_CASE("tui dialog: Tab stays inside the open dialog", "[tui][dialog]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK", "Cancel"}};
    scene.dialog->setOpen(true);
    static_cast<void>(harness.draw());
    CHECK(harness.focused(*scene.buttons.at(0)));
    moveFocus(harness.context(), Direction::Forward);
    CHECK(harness.focused(*scene.buttons.at(1)));
    moveFocus(harness.context(), Direction::Forward);
    CHECK(harness.focused(*scene.buttons.at(0)));
    moveFocus(harness.context(), Direction::Backward);
    CHECK(harness.focused(*scene.buttons.at(1)));
}

TEST_CASE("tui dialog: closing hands the focus back to where it was", "[tui][dialog]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK"}};
    harness.focus(*scene.behind);
    scene.dialog->setOpen(true);
    CHECK(harness.focused(*scene.buttons.at(0)));
    scene.dialog->setOpen(false);
    CHECK(harness.focused(*scene.behind));
    CHECK(harness.draw() == Rows{"[ Behind ]"});
}

// The mount's order: a Dialog's content is destroyed before `setOpen(false)`, and destroying the focused button
// clears the screen's focus on the way.
TEST_CASE("tui dialog: closing after its content is gone still hands the focus back", "[tui][dialog]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK"}};
    harness.focus(*scene.behind);
    scene.dialog->setOpen(true);
    REQUIRE(harness.focused(*scene.buttons.at(0)));
    scene.buttons.clear();
    REQUIRE(harness.screen().focusedComponent() == nullptr);
    scene.dialog->setOpen(false);
    CHECK(harness.focused(*scene.behind));
}

TEST_CASE("tui dialog: a click behind an open dialog does nothing", "[tui][dialog]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK"}};
    int behindClicks = 0;
    scene.behind->setOnClick([&] { ++behindClicks; });
    scene.dialog->setOpen(true);
    static_cast<void>(harness.draw());
    static_cast<void>(harness.click({.x = 2, .y = 0}));
    CHECK(behindClicks == 0);
}

TEST_CASE("tui busy: spins while active, one frame per animation step", "[tui][busy]") {
    Harness harness{20, 1};
    auto const busy = harness.make<BusyImpl>(nullptr);
    busy->setLabel("Loading");
    CHECK(harness.draw().empty());
    busy->setActive(true);
    CHECK(harness.context().activeBusy == 1);
    CHECK(harness.draw() == Rows{"| Loading"});
    ++harness.context().animationFrame;
    CHECK(harness.draw() == Rows{"/ Loading"});
    busy->setActive(false);
    CHECK(harness.context().activeBusy == 0);
}

TEST_CASE("tui slider: Left and Right move by step, Home and End to the ends", "[tui][slider]") {
    Harness harness{24, 1};
    auto const slider = harness.make<SliderImpl>(nullptr);
    std::vector<std::int64_t> changes;
    slider->setRange(0, 100, 10);
    slider->setValue(50);
    slider->setOnChange([&](std::int64_t value) { changes.push_back(value); });
    CHECK(harness.draw() == Rows{"[=========|---------] 50"});
    harness.focus(*slider);
    static_cast<void>(harness.key(KeyCode::Right));
    CHECK(changes == std::vector<std::int64_t>{60});
    CHECK(harness.draw() == Rows{"[==========|--------] 60"});
    static_cast<void>(harness.key(KeyCode::Home));
    CHECK(harness.draw() == Rows{"[|-------------------] 0"});
    static_cast<void>(harness.key(KeyCode::Left));
    CHECK(changes == std::vector<std::int64_t>{60, 0});
}

TEST_CASE("tui dialog: a new dialog is closed, and a closed one shows nothing and takes no input",
          "[tui][dialog][gating]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK"}};
    int dismissed = 0;
    int clicks = 0;
    scene.dialog->setOnDismiss([&] { ++dismissed; });
    scene.buttons.at(0)->setOnClick([&] { ++clicks; });
    CHECK_FALSE(scene.dialog->isOpen());

    auto const closedChecks = [&] {
        CHECK(harness.draw() == Rows{"[ Behind ]"});
        // Esc reaching the frame, whether bubbling up from a child the test focused or sent to it directly.
        harness.focus(*scene.buttons.at(0));
        CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
        CHECK(harness.key(KeyCode::Escape) == EventResult::Ignored);
        CHECK(scene.dialog->host().onEvent(escape()) == EventResult::Ignored);
        CHECK(dismissed == 0);
        CHECK(clicks == 0);
    };
    SECTION("never opened") { closedChecks(); }
    SECTION("opened and closed again") {
        scene.dialog->setOpen(true);
        static_cast<void>(harness.draw());
        scene.dialog->setOpen(false);
        closedChecks();
    }
}

TEST_CASE("tui dialog: a disabled dialog takes no Esc, and its children no input", "[tui][dialog][gating]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK"}};
    int dismissed = 0;
    int clicks = 0;
    scene.dialog->setOnDismiss([&] { ++dismissed; });
    scene.buttons.at(0)->setOnClick([&] { ++clicks; });
    scene.dialog->setOpen(true);
    REQUIRE(harness.focused(*scene.buttons.at(0)));
    scene.dialog->setEnabled(false);
    static_cast<void>(harness.key(KeyCode::Enter));
    static_cast<void>(harness.key(KeyCode::Escape));
    CHECK(dismissed == 0);
    CHECK(clicks == 0);
    scene.dialog->setEnabled(true);
    static_cast<void>(harness.key(KeyCode::Enter));
    static_cast<void>(harness.key(KeyCode::Escape));
    CHECK(dismissed == 1);
    CHECK(clicks == 1);
}

TEST_CASE("tui dialog: a widget behind an open dialog takes no key even while focused", "[tui][dialog][gating]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK"}};
    int behindClicks = 0;
    scene.behind->setOnClick([&] { ++behindClicks; });
    scene.dialog->setOpen(true);
    harness.focus(*scene.behind);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Ignored);
    CHECK(behindClicks == 0);
    scene.dialog->setOpen(false);
    harness.focus(*scene.behind);
    CHECK(harness.key(KeyCode::Enter) == EventResult::Handled);
    CHECK(behindClicks == 1);
}

TEST_CASE("tui dialog: an empty dialog focuses its frame, which takes Esc", "[tui][dialog]") {
    Harness harness{30, 10};
    Scene scene{harness, {}};
    int dismissed = 0;
    scene.dialog->setOnDismiss([&] { ++dismissed; });
    harness.focus(*scene.behind);
    scene.dialog->setOpen(true);
    CHECK(harness.screen().focusedComponent() == &scene.dialog->host());
    moveFocus(harness.context(), Direction::Forward);
    CHECK(harness.screen().focusedComponent() == &scene.dialog->host());
    CHECK(harness.key(KeyCode::Escape) == EventResult::Handled);
    CHECK(dismissed == 1);
    scene.dialog->setOpen(false);
    CHECK(harness.focused(*scene.behind));
}

TEST_CASE("tui dialog: Tab inside a dialog follows the order moveChild gives", "[tui][dialog]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK", "Cancel"}};
    scene.body->moveChild(*scene.buttons.at(1), 0);
    scene.dialog->setOpen(true);
    CHECK(harness.draw().at(4).contains("[ Cancel ]"));
    CHECK(harness.focused(*scene.buttons.at(1)));
    moveFocus(harness.context(), Direction::Forward);
    CHECK(harness.focused(*scene.buttons.at(0)));
    moveFocus(harness.context(), Direction::Forward);
    CHECK(harness.focused(*scene.buttons.at(1)));
}

TEST_CASE("tui dialog: onDismiss may destroy the dialog or the whole view", "[tui][dialog][lifetime]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK"}};
    bool const everything = GENERATE(false, true);
    int dismissed = 0;
    // The count goes up after the destruction, so a handler destroyed while it runs reads a freed capture.
    scene.dialog->setOnDismiss([&] {
        if (everything) {
            scene.destroyAll();
        } else {
            scene.destroyDialog();
        }
        ++dismissed;
    });
    harness.focus(*scene.behind);
    scene.dialog->setOpen(true);
    static_cast<void>(harness.draw());
    CHECK(harness.key(KeyCode::Escape) == EventResult::Handled);
    CHECK(dismissed == 1);
    if (everything) {
        CHECK(harness.screen().focusedComponent() == nullptr);
        CHECK(harness.draw().empty());
    } else {
        CHECK(harness.focused(*scene.behind));
        CHECK(harness.draw() == Rows{"[ Behind ]"});
    }
    CHECK(harness.context().openDialogs.empty());
    static_cast<void>(harness.key(KeyCode::Escape));
    CHECK(dismissed == 1);
}

TEST_CASE("tui dialog: no onDismiss runs after the dialog was destroyed", "[tui][dialog][lifetime]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK"}};
    int dismissed = 0;
    scene.dialog->setOnDismiss([&] { ++dismissed; });
    scene.dialog->setOpen(true);
    static_cast<void>(harness.draw());
    auto const target = middleOf(drawnAt(*scene.buttons.at(0)));
    scene.destroyDialog();
    CHECK(harness.context().openDialogs.empty());
    static_cast<void>(harness.key(KeyCode::Escape));
    static_cast<void>(harness.click(target));
    CHECK(dismissed == 0);
    CHECK(harness.draw() == Rows{"[ Behind ]"});
}

TEST_CASE("tui slider: onChange may destroy its own slider, and none runs after destruction",
          "[tui][slider][lifetime]") {
    Harness harness{24, 1};
    auto slider = harness.make<SliderImpl>(nullptr);
    std::vector<std::int64_t> changes;
    slider->setRange(0, 10, 1);
    slider->setOnChange([&](std::int64_t value) {
        slider.reset();
        changes.push_back(value);
    });
    static_cast<void>(harness.draw());
    harness.focus(*slider);
    CHECK(harness.key(KeyCode::Right) == EventResult::Handled);
    CHECK(changes == std::vector<std::int64_t>{1});
    CHECK(slider == nullptr);
    static_cast<void>(harness.key(KeyCode::Right));
    CHECK(changes == std::vector<std::int64_t>{1});
    CHECK(harness.draw().empty());
}

TEST_CASE("tui slider: no setter calls onChange, and moves stay inside the range", "[tui][slider]") {
    Harness harness{30, 1};
    auto const slider = harness.make<SliderImpl>(nullptr);
    std::vector<std::int64_t> changes;
    slider->setOnChange([&](std::int64_t value) { changes.push_back(value); });
    slider->setRange(0, 10, 3);
    slider->setValue(20);
    CHECK(slider->probeText() == "10");
    slider->setRange(0, 5, 3);
    CHECK(slider->probeText() == "5");
    CHECK(changes.empty());

    harness.focus(*slider);
    static_cast<void>(harness.key(KeyCode::Right));
    CHECK(changes.empty());  // already at the end
    static_cast<void>(harness.key(KeyCode::Left));
    static_cast<void>(harness.key(KeyCode::Left));
    static_cast<void>(harness.key(KeyCode::Left));
    CHECK(changes == std::vector<std::int64_t>{2, 0});
    static_cast<void>(harness.key(KeyCode::End));
    CHECK(changes == std::vector<std::int64_t>{2, 0, 5});

    constexpr auto lowest = std::numeric_limits<std::int64_t>::min();
    constexpr auto highest = std::numeric_limits<std::int64_t>::max();
    slider->setRange(lowest, highest, highest);
    slider->setValue(highest - 1);
    changes.clear();
    static_cast<void>(harness.key(KeyCode::Right));
    static_cast<void>(harness.key(KeyCode::Left));
    static_cast<void>(harness.key(KeyCode::Left));
    static_cast<void>(harness.key(KeyCode::Left));
    CHECK(changes == std::vector<std::int64_t>{highest, 0, -highest, lowest});
    CHECK(harness.draw().front().starts_with("[|---"));
}

TEST_CASE("tui busy: an active busy stops counting once destroyed, and counts once however often activated",
          "[tui][busy]") {
    Harness harness{20, 1};
    auto busy = harness.make<BusyImpl>(nullptr);
    busy->setActive(true);
    busy->setActive(true);
    CHECK(harness.context().activeBusy == 1);
    busy.reset();
    CHECK(harness.context().activeBusy == 0);
}

TEST_CASE("tui dialog: a press on the dialog's frame, or behind the dialog, ends a press whose release never came",
          "[tui][dialog][press]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK"}};
    int behindClicks = 0;
    scene.behind->setOnClick([&] { ++behindClicks; });
    static_cast<void>(harness.draw());
    CHECK(harness.send(pressAt({.x = 1, .y = 0})) == EventResult::Handled);
    REQUIRE(harness.context().pressed == &WidgetBase::of(*scene.behind));
    scene.dialog->setOpen(true);
    static_cast<void>(harness.draw());
    auto const frame = scene.dialog->host().screenBounds();
    REQUIRE_FALSE(frame.empty());
    SECTION("on the frame's border") {
        CHECK(harness.send(pressAt({.x = frame.x, .y = frame.y})) == EventResult::Handled);
    }
    SECTION("behind the dialog") { CHECK(harness.send(pressAt({.x = 0, .y = 9})) == EventResult::Handled); }
    CHECK(harness.context().pressed == nullptr);
    scene.dialog->setOpen(false);
    static_cast<void>(harness.draw());
    static_cast<void>(harness.send(
        core::tui::MouseEvent{.type = core::tui::MouseEvent::Type::Release, .button = 0, .x = 2, .y = 1}));
    CHECK(behindClicks == 0);
}

TEST_CASE("tui dialog: a reopened dialog takes no click at its old places until it is drawn again",
          "[tui][dialog][drawn]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK"}};
    int clicks = 0;
    scene.buttons.at(0)->setOnClick([&] { ++clicks; });
    scene.dialog->setOpen(true);
    static_cast<void>(harness.draw());
    auto const old = middleOf(drawnAt(*scene.buttons.at(0)));
    auto const shifted = [&] {
        auto added = harness.make<TextImpl>(scene.body.get());
        added->setText("Moved down");
        scene.body->moveChild(*added, 0);
        return added;
    };
    std::unique_ptr<TextImpl> note;
    SECTION("closed and opened again") {
        scene.dialog->setOpen(false);
        note = shifted();
        scene.dialog->setOpen(true);
    }
    SECTION("hidden for a frame and shown again") {
        scene.dialog->setVisible(false);
        CHECK(harness.draw() == Rows{"[ Behind ]"});
        note = shifted();
        scene.dialog->setVisible(true);
    }
    static_cast<void>(harness.click(old));
    CHECK(clicks == 0);
    static_cast<void>(harness.draw());
    auto const now = middleOf(drawnAt(*scene.buttons.at(0)));
    REQUIRE(now.y == old.y + 1);
    static_cast<void>(harness.click(now));
    CHECK(clicks == 1);
}

// The buttons are the dialog's own children, so their views lie right under its frame: a stale place inside the
// frame is reached by the pointer only when nothing between keeps it out.
TEST_CASE("tui dialog: a child squeezed out of the dialog keeps no drawn bounds", "[tui][dialog][drawn]") {
    auto output = std::make_unique<ResizableOutput>(core::tui::Size{.width = 30, .height = 6});
    auto& terminal = *output;
    Harness harness{std::move(output)};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const dialog = harness.make<DialogImpl>(column.get());
    std::vector<std::unique_ptr<ButtonImpl>> buttons;
    for (auto const* const label : {"A", "B", "C"}) {
        buttons.push_back(harness.make<ButtonImpl>(dialog.get()));
        buttons.back()->setLabel(label);
    }
    int clicks = 0;
    buttons.back()->setOnClick([&] { ++clicks; });
    dialog->setOpen(true);
    static_cast<void>(harness.draw());
    auto const old = middleOf(drawnAt(*buttons.back()));

    terminal.resize({.width = 30, .height = 4});
    static_cast<void>(harness.send(core::tui::ResizeEvent{.columns = 30, .rows = 4}));
    static_cast<void>(harness.draw());
    REQUIRE(dialog->host().screenBounds().contains(old.x, old.y));
    CHECK(drawnAt(*buttons.back()).empty());
    static_cast<void>(harness.click(old));
    CHECK(clicks == 0);
    buttons.clear();
}

TEST_CASE("tui dialog: a root hidden for a frame takes no click at its old places until it is drawn again",
          "[tui][drawn]") {
    Harness harness{30, 4};
    auto const column = harness.make<StackImpl>(nullptr, ui::Axis::Vertical);
    auto const button = harness.make<ButtonImpl>(column.get());
    button->setLabel("OK");
    int clicks = 0;
    button->setOnClick([&] { ++clicks; });
    static_cast<void>(harness.draw());
    auto const old = middleOf(drawnAt(*button));
    column->setVisible(false);
    CHECK(harness.draw().empty());
    auto const note = harness.make<TextImpl>(column.get());
    note->setText("Moved down");
    column->moveChild(*note, 0);
    column->setVisible(true);
    static_cast<void>(harness.click(old));
    CHECK(clicks == 0);
    static_cast<void>(harness.draw());
    static_cast<void>(harness.click(middleOf(drawnAt(*button))));
    CHECK(clicks == 1);
}

TEST_CASE("tui dialog: closing closes an open dropdown list inside it", "[tui][dialog][popup]") {
    Harness harness{30, 10};
    Scene scene{harness, {}};
    auto const select = harness.make<DropdownSelectImpl>(scene.body.get());
    select->setOptions(
        {{.key = ui::Key{std::int64_t{1}}, .label = "Red"}, {.key = ui::Key{std::int64_t{2}}, .label = "Green"}});
    int chosen = 0;
    select->setOnSelect([&](ui::Key const&) { ++chosen; });
    harness.focus(*scene.behind);
    scene.dialog->setOpen(true);
    static_cast<void>(harness.draw());
    REQUIRE(harness.focused(*select));
    static_cast<void>(harness.key(KeyCode::Enter));
    REQUIRE(select->isOpen());
    scene.dialog->setOpen(false);
    CHECK_FALSE(select->isOpen());
    CHECK(harness.context().popups.empty());
    CHECK(harness.focused(*scene.behind));
    CHECK(harness.draw() == Rows{"[ Behind ]"});
    CHECK(chosen == 0);
}

namespace {

/// What a commit handler destroys while opening a dialog moves the focus off a field behind it.
enum class Victim : std::uint8_t { FirstButton, Dialog, Everything };

}  // namespace

TEST_CASE("tui dialog: a handler run by the focus leaving for an opening dialog may destroy what it opens",
          "[tui][dialog][lifetime]") {
    auto const victim = GENERATE(Victim::FirstButton, Victim::Dialog, Victim::Everything);
    Harness harness{30, 10};
    Scene scene{harness, {"OK", "Cancel"}};
    auto picker = harness.make<FilePickerImpl>(scene.column.get(), ui::FilePickerMode::Save);
    int picked = 0;
    picker->setOnPicked([&](std::string const& /*path*/) {
        ++picked;
        if (victim == Victim::FirstButton) {
            scene.buttons.at(0).reset();
            return;
        }
        if (victim == Victim::Dialog) {
            scene.destroyDialog();
            return;
        }
        picker.reset();
        scene.destroyAll();
    });
    static_cast<void>(harness.draw());
    harness.focus(*picker);
    static_cast<void>(harness.type("/p"));
    scene.dialog->setOpen(true);
    CHECK(picked == 1);
    // A dialog destroyed while it opens hands the focus back as any dialog does when it goes.
    if (victim == Victim::FirstButton) {
        CHECK(harness.focused(*scene.buttons.at(1)));
    } else if (victim == Victim::Dialog) {
        CHECK(harness.focused(*picker));
    } else {
        CHECK(harness.screen().focusedComponent() == nullptr);
    }
    static_cast<void>(harness.draw());
    static_cast<void>(harness.key(KeyCode::Escape));
    CHECK(picked == 1);
}

TEST_CASE("tui dialog: closing after the widget it would hand the focus back to is gone focuses nothing",
          "[tui][dialog][lifetime]") {
    Harness harness{30, 10};
    Scene scene{harness, {"OK"}};
    harness.focus(*scene.behind);
    scene.dialog->setOpen(true);
    scene.behind.reset();
    scene.dialog->setOpen(false);
    CHECK(harness.screen().focusedComponent() == nullptr);
    CHECK(harness.draw().empty());
}

TEST_CASE("tui dialog: a dialog inside a closed one shows nothing until that one opens, then lies on top",
          "[tui][dialog][nested]") {
    Harness harness{30, 10};
    Scene scene{harness, {"Outer"}};
    // The mount's order: the outer dialog's content, the inner dialog among it, is complete before the outer
    // dialog opens.
    auto inner = harness.make<DialogImpl>(scene.body.get());
    inner->setTitle("Inner");
    auto const innerButton = harness.make<ButtonImpl>(inner.get());
    innerButton->setLabel("In");
    int innerDismissed = 0;
    int outerDismissed = 0;
    inner->setOnDismiss([&] { ++innerDismissed; });
    scene.dialog->setOnDismiss([&] { ++outerDismissed; });
    harness.focus(*scene.behind);
    inner->setOpen(true);
    CHECK(harness.focused(*scene.behind));
    CHECK(harness.draw() == Rows{"[ Behind ]"});

    scene.dialog->setOpen(true);
    CHECK(harness.focused(*innerButton));
    auto const rows = harness.draw();
    CHECK(rows.at(3).contains("Inner"));
    CHECK(rows.at(4).contains("[ In ]"));
    moveFocus(harness.context(), Direction::Forward);
    CHECK(harness.focused(*innerButton));

    static_cast<void>(harness.key(KeyCode::Escape));
    CHECK(innerDismissed == 1);
    CHECK(outerDismissed == 0);
    inner->setOpen(false);
    CHECK(harness.focused(*scene.buttons.at(0)));
    scene.dialog->setOpen(false);
    CHECK(harness.focused(*scene.behind));
    inner->setOpen(false);
}

TEST_CASE("tui dialog: a dialog closing under another open one hands the focus into the one on top",
          "[tui][dialog][nested]") {
    Harness harness{30, 10};
    Scene lower{harness, {"Low"}};
    auto upper = harness.make<DialogImpl>(lower.column.get());
    auto upperButton = harness.make<ButtonImpl>(upper.get());
    upperButton->setLabel("Up");
    harness.focus(*lower.behind);
    lower.dialog->setOpen(true);
    upper->setOpen(true);
    REQUIRE(harness.focused(*upperButton));
    // The upper dialog's content goes, which clears the focus, and the lower dialog closes before anything else.
    upperButton.reset();
    REQUIRE(harness.screen().focusedComponent() == nullptr);
    lower.dialog->setOpen(false);
    CHECK(harness.screen().focusedComponent() == &upper->host());
    upper->setOpen(false);
    CHECK(harness.screen().focusedComponent() == nullptr);
}
