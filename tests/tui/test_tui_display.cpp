// SPDX-License-Identifier: Apache-2.0
//
// The display widgets: banner, badge, progress, steps, key-value and empty state.

#include <catch2/catch_test_macros.hpp>
#include <core/tui/KeyCode.hpp>
#include <limits>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <vector>

#include "tui/display_widgets.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using morph::tui::detail::BadgeImpl;
using morph::tui::detail::BannerImpl;
using morph::tui::detail::EmptyStateImpl;
using morph::tui::detail::KeyValueImpl;
using morph::tui::detail::ProgressImpl;
using morph::tui::detail::StepsImpl;
using morph::tui::testing::Harness;
using Rows = std::vector<std::string>;

// Mutation: leave the action out of the row (the click on it then reaches nothing).
TEST_CASE("tui banner: the text, the action and the dismissal on one row; a click on each runs its own",
          "[tui][display]") {
    Harness harness{40, 3};
    auto const banner = harness.make<BannerImpl>(nullptr);
    int actions = 0;
    int dismissals = 0;
    banner->setTone(ui::Tone::Warn);
    banner->setText("Offline");
    banner->setActionLabel("Retry");
    banner->setDismissible(true);
    banner->setOnAction([&] { ++actions; });
    banner->setOnDismiss([&] { ++dismissals; });
    CHECK(harness.draw() == Rows{"Offline  [ Retry ] [x]"});
    static_cast<void>(harness.click({.x = 11, .y = 0}));
    CHECK(actions == 1);
    CHECK(dismissals == 0);
    static_cast<void>(harness.click({.x = 20, .y = 0}));
    CHECK(dismissals == 1);
    static_cast<void>(harness.click({.x = 2, .y = 0}));  // the text: nothing
    CHECK(actions == 1);
    CHECK(dismissals == 1);
    CHECK(harness.draw() == Rows{"Offline  [ Retry ] [x]"});  // a dismissal leaves it shown
}

// Mutation: dismiss on Esc whether or not the banner is dismissible (Esc then reaches onDismiss first).
TEST_CASE("tui banner: Enter runs the action; Esc dismisses only a dismissible banner", "[tui][display]") {
    Harness harness{40, 3};
    auto const banner = harness.make<BannerImpl>(nullptr);
    int actions = 0;
    int dismissals = 0;
    banner->setText("Saved");
    banner->setActionLabel("Undo");
    banner->setOnAction([&] { ++actions; });
    banner->setOnDismiss([&] { ++dismissals; });
    static_cast<void>(harness.draw());
    harness.focus(*banner);
    CHECK(harness.key(core::tui::KeyCode::Enter) == core::tui::EventResult::Handled);
    CHECK(harness.key(core::tui::KeyCode::Escape) == core::tui::EventResult::Ignored);
    CHECK(actions == 1);
    CHECK(dismissals == 0);
    banner->setDismissible(true);
    CHECK(harness.key(core::tui::KeyCode::Escape) == core::tui::EventResult::Handled);
    CHECK(dismissals == 1);
}

// Mutation: take the focus without an action or a dismissal (a plain banner then sits in the Tab order).
TEST_CASE("tui banner: a banner with nothing to do takes no focus", "[tui][display]") {
    Harness harness{40, 3};
    auto const banner = harness.make<BannerImpl>(nullptr);
    banner->setText("Note");
    CHECK_FALSE(banner->wantsFocus());
    banner->setActionLabel("Open");
    CHECK(banner->wantsFocus());
}

TEST_CASE("tui badge: the text in brackets", "[tui][display]") {
    Harness harness{20, 2};
    auto const badge = harness.make<BadgeImpl>(nullptr);
    badge->setTone(ui::Tone::Ok);
    badge->setText("paid");
    badge->setIcon("check");
    CHECK(harness.draw() == Rows{"[paid]"});
    CHECK(badge->probeText() == "paid");
}

// Mutation: leave the framework names undrawn (the badge shows `[paid]`).
TEST_CASE("tui badge: a framework icon is its glyph before the text", "[tui][display]") {
    Harness harness{20, 2};
    auto const badge = harness.make<BadgeImpl>(nullptr);
    badge->setText("paid");
    badge->setIcon("tone.ok");
    CHECK(harness.draw() == Rows{"[✓ paid]"});
}

// Mutation: leave the value unclamped (1.7 then fills past the bar).
TEST_CASE("tui progress: a bar and the percentage, clamped to its ends; none for an indeterminate one",
          "[tui][display]") {
    Harness harness{40, 2};
    auto const progress = harness.make<ProgressImpl>(nullptr);
    progress->setLabel("Upload");
    progress->setValue(0.5);
    CHECK(harness.draw() == Rows{"[##########..........] 50% Upload"});
    progress->setValue(1.7);
    CHECK(harness.draw() == Rows{"[####################] 100% Upload"});
    progress->setValue(-1.0);
    CHECK(harness.draw() == Rows{"[....................] 0% Upload"});
    progress->setValue(std::numeric_limits<double>::quiet_NaN());
    CHECK(harness.draw() == Rows{"[....................] 0% Upload"});
    progress->setValue(std::nullopt);
    CHECK(harness.draw() == Rows{"[<=>.................] Upload"});
}

// Mutation: mark every step the same (the states are then not told apart).
TEST_CASE("tui steps: one row per step with its state's mark", "[tui][display]") {
    Harness harness{30, 5};
    auto const steps = harness.make<StepsImpl>(nullptr);
    steps->setSteps({{.label = "Plan", .state = ui::StepState::Done},
                     {.label = "Build", .state = ui::StepState::Active},
                     {.label = "Ship", .state = ui::StepState::Pending},
                     {.label = "Undo", .state = ui::StepState::Failed}});
    steps->setCurrent(1);
    CHECK(harness.draw() == Rows{"[x] Plan", "[>] Build", "[ ] Ship", "[!] Undo"});
    CHECK(steps->probeText() == "Build");
    steps->setCurrent(9);
    CHECK(steps->probeText().empty());
}

// Mutation: put each value right after its own caption (the values then do not line up).
TEST_CASE("tui key-value: the values line up after the widest caption", "[tui][display]") {
    Harness harness{30, 3};
    auto const pairs = harness.make<KeyValueImpl>(nullptr);
    pairs->setItems({{.label = "Name", .value = "Ada"}, {.label = "Department", .value = "Math"}});
    CHECK(harness.draw() == Rows{"Name:       Ada", "Department: Math"});
}

// Mutation: run the action on a click anywhere (a click on the title then runs it).
TEST_CASE("tui empty state: title, text and an action that Enter and a click on it run", "[tui][display]") {
    Harness harness{30, 5};
    auto const empty = harness.make<EmptyStateImpl>(nullptr);
    int actions = 0;
    empty->setTitle("No tasks");
    empty->setText("Nothing is due.");
    empty->setIcon("inbox");
    empty->setActionLabel("Add task");
    empty->setOnAction([&] { ++actions; });
    CHECK(harness.draw() == Rows{"No tasks", "Nothing is due.", "[ Add task ]"});
    static_cast<void>(harness.click({.x = 1, .y = 0}));
    CHECK(actions == 0);
    static_cast<void>(harness.click({.x = 3, .y = 2}));
    CHECK(actions == 1);
    harness.focus(*empty);
    CHECK(harness.key(core::tui::KeyCode::Enter) == core::tui::EventResult::Handled);
    CHECK(actions == 2);
}

// Mutation: draw `_text` whatever the runs (the strong run is not bold).
TEST_CASE("tui banner: a banner's runs draw on its row, a strong run bold", "[tui][display]") {
    Harness harness{40, 3};
    auto const banner = harness.make<BannerImpl>(nullptr);
    banner->setTone(ui::Tone::Err);
    banner->setSpans({{.text = "Refused.", .strong = true}, {.text = " 3 left"}});
    CHECK(harness.draw() == Rows{"Refused. 3 left"});
    auto const& buffer = harness.screen().renderedBuffer();
    CHECK(buffer.at(0, 0).style.bold);
    CHECK_FALSE(buffer.at(0, 10).style.bold);
    CHECK(banner->probeText() == "Refused. 3 left");
}

// Mutation: ignore the placement (the stacked pair takes one row).
TEST_CASE("tui key-value: a stacked pair takes two rows, and a toned value is bold in its tone", "[tui][display]") {
    Harness harness{30, 5};
    auto const kv = harness.make<KeyValueImpl>(nullptr);
    kv->setCaptionPlacement(ui::CaptionPlacement::Above);
    kv->setItems({{.label = "Serial", .spans = {{.text = "D-9", .mono = true}}},
                  {.label = "Containers", .value = "180 needed", .tone = ui::Tone::Err}});
    CHECK(harness.draw() == Rows{"Serial", "D-9", "Containers", "180 needed"});
    auto const& buffer = harness.screen().renderedBuffer();
    CHECK(buffer.at(3, 0).style.bold);
    CHECK(buffer.at(3, 0).style.fg == harness.screen().theme().error.fg);
    CHECK_FALSE(buffer.at(1, 0).style.bold);
    CHECK(kv->probeText() == "Serial: D-9\nContainers: 180 needed");
}
