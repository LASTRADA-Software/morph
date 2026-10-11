// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/testing/owner_probe_recorder.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_support.hpp"
#include "ui_echoing_backend.hpp"
#include "ui_test_support.hpp"

namespace ui = morph::ui;

namespace {

using morph::reactive::Runtime;
using morph::reactive::Signal;
using morph::testing::EchoingBackend;
using morph::testing::failWhenSpent;
using morph::testing::intKey;
using ui::testing::RecordingBackend;
using Owner = morph::testing::StepExecutor;
using Probe = morph::testing::OwnerProbeRecorder;
using Lines = std::vector<std::string>;

}  // namespace

// Mutations: leave out any one of the five setters in applyCommon; set the role or the test id on every change of
// the name (they are set once).
TEST_CASE("ui::Common: accessibility, test id, tooltip and surface reach the widget", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> name{runtime, "Total"};
    ui::Mounted const view{runtime, backend,
                           ui::text({.text = "42",
                                     .common = {.a11y = {.name = [&] { return name.get(); }, .role = "status"},
                                                .testId = "total",
                                                .tooltip = [&] { return "The total of " + name.get(); },
                                                .surface = "sidebar"}})};
    CHECK(backend.prop(1, "a11yName") == "Total");
    CHECK(backend.prop(1, "a11yRole") == "status");
    CHECK(backend.prop(1, "testId") == "total");
    CHECK(backend.prop(1, "tooltip") == "The total of Total");
    CHECK(backend.prop(1, "surface") == "sidebar");
    backend.clearLog();
    name.set("Sum");
    owner.runAll();
    CHECK(backend.log() == Lines{"set Text#1 a11yName=Sum", "set Text#1 tooltip=The total of Sum"});
}

// Mutation: drop the `!empty()` and `isBound()` guards in applyCommon (every widget then gets five setter calls).
TEST_CASE("ui::Common: the defaults call no setter", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{runtime, backend, ui::text({.text = "plain"})};
    CHECK(backend.log() == Lines{"create Text#1 in root", "set Text#1 text=plain", "set Text#1 role=Normal"});
}

// The innermost widget declaring a chord takes it; a chord only an outer widget declares reaches it. Mutations:
// walk to the outermost declaring widget in RecordingBackend::press; call the last binding of a chord listed twice.
TEST_CASE("ui::Common: a chord goes to the innermost node that declares it", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Lines pressed;
    ui::Mounted const view{
        runtime, backend,
        ui::column(
            {.children = {ui::button(
                 {.label = "Save",
                  .common = {.keys = {{.chord = "Ctrl+S", .onPress = [&] { pressed.emplace_back("inner save"); }},
                                      {.chord = "Ctrl+S",
                                       .onPress = [&] { pressed.emplace_back("inner save again"); }}}}})},
             .common = {.keys = {{.chord = "Ctrl+S", .onPress = [&] { pressed.emplace_back("outer save"); }},
                                 {.chord = "Esc", .onPress = [&] { pressed.emplace_back("outer close"); }}}}})};
    REQUIRE(backend.kindOf(2) == "Button");
    CHECK(backend.prop(1, "keys") == "Ctrl+S,Esc");
    CHECK(backend.press(2, "Ctrl+S"));
    CHECK(backend.press(2, "Esc"));
    CHECK_FALSE(backend.press(2, "F5"));
    CHECK(pressed == Lines{"inner save", "outer close"});
}

// Mutation: drop the reachable() check in RecordingBackend::press.
TEST_CASE("ui::Common: a chord pressed inside a hidden container does nothing", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> shown{runtime, false};
    int presses = 0;
    ui::Mounted const view{
        runtime, backend,
        ui::column({.children = {ui::button(
                        {.label = "Save", .common = {.keys = {{.chord = "Ctrl+S", .onPress = [&] { ++presses; }}}}})},
                    .common = {.visible = [&] { return shown.get(); }}})};
    CHECK_FALSE(backend.press(2, "Ctrl+S"));
    CHECK(presses == 0);
    shown.set(true);
    owner.runAll();
    CHECK(backend.press(2, "Ctrl+S"));
    CHECK(presses == 1);
}

// A chord handler runs like any widget callback: an exception it throws is reported and goes no further. Mutation:
// call the handler directly instead of through runCallback.
TEST_CASE("ui::Common: a chord handler that throws is reported", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{
        runtime, backend,
        ui::button({.label = "Save", .common = {.keys = {{.chord = "Ctrl+S", .onPress = [] {
                                                              throw std::runtime_error{"save failed"};
                                                          }}}}})};
    CHECK(backend.press(1, "Ctrl+S"));
    CHECK(probe.count(ui::detail::site::kCallbackThrew) == 1);
}

// Mutation: let every autofocus widget take the candidate (`_autofocus = &widget` unconditionally): the last wins.
TEST_CASE("ui::Common: the first autofocus widget in document order takes focus once the mount completes", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{
        runtime, backend,
        ui::column({.children = {ui::text({.text = "Name"}), ui::textInput({.common = {.autofocus = true}}),
                                 ui::textInput({.common = {.autofocus = true}})}})};
    CHECK(backend.focused() == 3);
    CHECK(std::ranges::count_if(backend.log(), [](std::string const& line) { return line.starts_with("focus "); }) ==
          1);
}

// Content a binding mounts while the root mounts belongs to the root's mount: document order decides between them.
// Content mounted later is a mount of its own and focuses its own first autofocus widget. Mutation: let every pass
// focus at its end (drop the `_passDepth == 0` check): the switch's input, focused first, loses to the later one.
TEST_CASE("ui::Common: autofocus follows document order across content the root mounts", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::int64_t> which{runtime, 0};
    ui::Mounted const view{
        runtime, backend,
        ui::column(
            {.children = {
                 ui::switchOf({.selector = [&] { return ui::Key{which.get()}; },
                               .cases = {{.key = intKey(0), .node = ui::textInput({.common = {.autofocus = true}})},
                                         {.key = intKey(1), .node = ui::textInput({.common = {.autofocus = true}})}}}),
                 ui::textInput({.common = {.autofocus = true}})}})};
    REQUIRE(backend.kindOf(3) == "TextInput");
    CHECK(backend.focused() == 3);
    which.set(1);
    owner.runAll();
    REQUIRE(backend.kindOf(5) == "TextInput");
    CHECK(backend.focused() == 5);
}

// A content mount that fails destroys what it mounted, its autofocus widget included; the mount around it must not
// focus that widget, and finds the next one instead. Mutation: keep the failed pass's candidate (do not restore it
// in mountPass's catch): the root then calls focus on a destroyed widget, and the later input is never focused.
TEST_CASE("ui::Common: an autofocus widget in content that failed to mount is not focused", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    EchoingBackend backend;
    std::optional<int> spare = 4;  // root column, switch slot, case column, case input; the case's text fails
    backend.setOnCreate(failWhenSpent(spare));
    Signal<std::int64_t> which{runtime, 0};
    ui::Mounted const view{
        runtime, backend,
        ui::column(
            {.children = {ui::switchOf({.selector = [&] { return ui::Key{which.get()}; },
                                        .cases = {{.key = intKey(0),
                                                   .node = ui::column(
                                                       {.children = {ui::textInput({.common = {.autofocus = true}}),
                                                                     ui::text({.text = "fails"})}})}}}),
                          ui::textInput({.common = {.autofocus = true}})}})};
    RecordingBackend const& recording = backend.recording();
    // The case's text failed in its factory, so it never took an id: the root's input is #5, the case's was #4.
    CHECK(recording.dump() == "Column#1 gap=0\n  Slot#2\n  TextInput#5 mode=SingleLine placeholder= text=\n");
    CHECK(recording.focused() == 5);
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
}

// Mutations: drop the `!constant()` half of the enabled guard, or the constant halves of the drag key, accessible
// name or tooltip guards (a constant then calls no setter).
TEST_CASE("ui::Common: a constant other than the default calls its setter once", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{runtime, backend,
                           ui::text({.text = "42",
                                     .common = {.enabled = false,
                                                .dragKey = std::optional<ui::Key>{intKey(7)},
                                                .a11y = {.name = "Total"},
                                                .tooltip = "The total"}})};
    CHECK(backend.log() == Lines{"create Text#1 in root", "set Text#1 enabled=false", "set Text#1 dragKey=7",
                                 "set Text#1 a11yName=Total", "set Text#1 tooltip=The total", "set Text#1 text=42",
                                 "set Text#1 role=Normal"});
}

// A chord declared without a handler is taken and runs nothing. Mutation: drop the `bound->onPress` check (the
// empty function then throws, and the probe sees a reported callback failure).
TEST_CASE("ui::Common: a chord without a handler is taken and runs nothing", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{runtime, backend, ui::button({.label = "Help", .common = {.keys = {{.chord = "F1"}}}})};
    CHECK(backend.press(1, "F1"));
    CHECK(probe.count(ui::detail::site::kCallbackThrew) == 0);
}
