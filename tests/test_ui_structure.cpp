// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "owner_probe_recorder.hpp"
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

enum class Page : std::uint8_t { Home, Settings };

}  // namespace

TEST_CASE("ui::switchOf: mounts the selected case and remounts child-first", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::int64_t> which{runtime, 0};
    ui::Mounted const view{
        runtime, backend,
        ui::switchOf({
            .selector = [&] { return ui::Key{which.get()}; },
            .cases = {{.key = intKey(0), .node = ui::column({.children = {ui::text({.text = "zero"})}})},
                      {.key = intKey(1), .node = ui::column({.children = {ui::text({.text = "one"})}})}},
        })};
    CHECK(backend.dump() == "Slot#1\n  Column#2 gap=0\n    Text#3 role=Normal text=zero\n");
    backend.clearLog();
    which.set(1);
    owner.runAll();
    CHECK(backend.log() == Lines{"destroy Text#3", "destroy Column#2", "create Column#4 in Slot#1",
                                 "set Column#4 gap=0", "create Text#5 in Column#4", "set Text#5 text=one",
                                 "set Text#5 role=Normal"});
}

TEST_CASE("ui::switchOf: an unchanged key never remounts", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::int64_t> count{runtime, 1};
    ui::Mounted const view{runtime, backend,
                           ui::switchOf({
                               .selector = [&] { return intKey(count.get() > 0 ? 1 : 0); },
                               .cases = {{.key = intKey(0), .node = ui::text({.text = "none"})},
                                         {.key = intKey(1), .node = ui::text({.text = "some"})}},
                           })};
    backend.clearLog();
    count.set(5);
    owner.runAll();
    CHECK(backend.log().empty());
}

TEST_CASE("ui::switchOf: a key with no case and no fallback mounts nothing", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::int64_t> which{runtime, 9};
    ui::Mounted const view{runtime, backend,
                           ui::switchOf({.selector = [&] { return ui::Key{which.get()}; },
                                         .cases = {{.key = intKey(0), .node = ui::text({.text = "zero"})}}})};
    CHECK(backend.dump() == "Slot#1\n");
    which.set(0);
    owner.runAll();
    CHECK(backend.dump() == "Slot#1\n  Text#2 role=Normal text=zero\n");
    which.set(9);
    owner.runAll();
    CHECK(backend.dump() == "Slot#1\n");
}

TEST_CASE("ui::switchOf: the fallback answers a key with no case", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{runtime, backend,
                           ui::switchOf({.selector = ui::Key{std::string{"missing"}},
                                         .cases = {{.key = intKey(0), .node = ui::text({.text = "zero"})}},
                                         .fallback = ui::text({.text = "other"})})};
    CHECK(backend.dump() == "Slot#1\n  Text#2 role=Normal text=other\n");
}

TEST_CASE("ui::switchOf: a case being left never runs against the state that removes it", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::int64_t> which{runtime, 0};
    // The case's text and its inner Switch both read what the outer Switch selects on, so one write wakes all
    // three; the outer one is the oldest, runs first, and removes the other two before they run.
    ui::Mounted const view{
        runtime, backend,
        ui::switchOf({
            .selector = [&] { return ui::Key{which.get()}; },
            .cases = {{.key = intKey(0),
                       .node = ui::column(
                           {.children = {ui::text({.text = [&] { return "which:" + std::to_string(which.get()); }}),
                                         ui::switchOf({
                                             .selector = [&] { return ui::Key{which.get()}; },
                                             .cases = {{.key = intKey(0), .node = ui::text({.text = "inner zero"})}},
                                             .fallback = ui::text({.text = "inner other"}),
                                         })}})},
                      {.key = intKey(1), .node = ui::text({.text = "one"})}},
        })};
    CHECK(backend.dump() ==
          "Slot#1\n"
          "  Column#2 gap=0\n"
          "    Text#3 role=Normal text=which:0\n"
          "    Slot#4\n"
          "      Text#5 role=Normal text=inner zero\n");
    backend.clearLog();
    which.set(1);
    owner.runAll();
    CHECK(backend.log() == Lines{"destroy Text#5", "destroy Slot#4", "destroy Text#3", "destroy Column#2",
                                 "create Text#6 in Slot#1", "set Text#6 text=one", "set Text#6 role=Normal"});
}

TEST_CASE("ui::switchOf: a case's own button may switch that case away", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::int64_t> which{runtime, 0};
    // Larger than std::function's inline buffer, so the handler's closure lives on the heap, where ASan watches it.
    std::string const note(64, 'n');
    bool aliveAfterWrite = false;
    std::string seenAfter;
    ui::Mounted const view{
        runtime, backend,
        ui::switchOf({
            .selector = [&] { return ui::Key{which.get()}; },
            .cases = {{.key = intKey(0),
                       .node = ui::button({.label = "Next",
                                           .onClick =
                                               [&, note = note] {
                                                   which.set(1);
                                                   aliveAfterWrite = backend.exists(2);
                                                   seenAfter = note;
                                               }})},
                      {.key = intKey(1), .node = ui::button({.label = "Back", .onClick = [&] { which.set(0); }})}},
        })};
    std::size_t const mountedNodes = runtime.core()->liveNodes();

    backend.clearLog();
    backend.click(2);
    CHECK(aliveAfterWrite);
    CHECK(seenAfter == note);
    owner.runAll();
    CHECK(backend.log() == Lines{"destroy Button#2", "create Button#3 in Slot#1", "set Button#3 label=Back"});

    backend.click(3);
    owner.runAll();
    CHECK(backend.dump() == "Slot#1\n  Button#4 label=Next\n");
    CHECK(runtime.core()->liveNodes() == mountedNodes);
}

TEST_CASE("ui::switchOf: a case's button survives a remount that is due while its handler runs", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::int64_t> which{runtime, 0};
    std::string const note(64, 'n');
    bool aliveInHandler = false;
    std::string seenInHandler;
    ui::Mounted const view{runtime, backend,
                           ui::switchOf({
                               .selector = [&] { return ui::Key{which.get()}; },
                               .cases = {{.key = intKey(0),
                                          .node = ui::button({.label = "Next",
                                                              .onClick =
                                                                  [&, note = note] {
                                                                      CHECK(owner.runOne());  // a modal loop
                                                                      aliveInHandler = backend.exists(2);
                                                                      seenInHandler = note;
                                                                  }})},
                                         {.key = intKey(1), .node = ui::text({.text = "one"})}},
                           })};
    which.set(1);  // a write from elsewhere: its posted flush would unmount the button
    backend.click(2);
    CHECK(aliveInHandler);
    CHECK(seenInHandler == note);
    CHECK(probe.count(morph::reactive::detail::site::kFlushInWidgetEvent) == 1);
    owner.runAll();
    CHECK(backend.dump() == "Slot#1\n  Text#3 role=Normal text=one\n");
}

TEST_CASE("ui::switchOf: a case that fails to mount leaves nothing behind, and mounts when selected again", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    EchoingBackend backend;
    RecordingBackend& recording = backend.recording();
    std::optional<int> spare;
    backend.setOnCreate(failWhenSpent(spare));
    Signal<std::int64_t> which{runtime, 0};
    ui::Mounted const view{
        runtime, backend,
        ui::switchOf({
            .selector = [&] { return ui::Key{which.get()}; },
            .cases = {{.key = intKey(0), .node = ui::text({.text = "zero"})},
                      {.key = intKey(1),
                       .node =
                           ui::column({.children = {ui::text({.text = [&] { return std::to_string(which.get()); }}),
                                                    ui::text({.text = "last"})}})}},
        })};
    std::size_t const mountedNodes = runtime.core()->liveNodes();

    recording.clearLog();
    spare = 2;  // the Column and the bound Text; the last Text fails
    which.set(1);
    owner.runAll();
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
    CHECK(recording.log() == Lines{"destroy Text#2", "create Column#3 in Slot#1", "set Column#3 gap=0",
                                   "create Text#4 in Column#3", "set Text#4 text=1", "set Text#4 role=Normal",
                                   "destroy Text#4", "destroy Column#3"});
    CHECK(recording.dump() == "Slot#1\n");
    CHECK(runtime.core()->liveNodes() == mountedNodes);

    which.set(0);
    owner.runAll();
    which.set(1);
    owner.runAll();
    CHECK(recording.dump() ==
          "Slot#1\n  Column#6 gap=0\n    Text#7 role=Normal text=1\n    Text#8 role=Normal text=last\n");
}

TEST_CASE("ui::switchOn: each enumerator selects its case", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<Page> page{runtime, Page::Home};
    ui::Mounted const view{
        runtime, backend,
        ui::switchOn<Page>([&] { return page.get(); }, {{Page::Home, ui::text({.text = "home"})},
                                                        {Page::Settings, ui::text({.text = "settings"})}})};
    CHECK(backend.dump() == "Slot#1\n  Text#2 role=Normal text=home\n");
    page.set(Page::Settings);
    owner.runAll();
    CHECK(backend.dump() == "Slot#1\n  Text#3 role=Normal text=settings\n");
}

TEST_CASE("ui::switchOn: an empty selector is refused where it is written", "[ui]") {
    CHECK_THROWS_AS(ui::switchOn<Page>(std::function<Page()>{}, {{Page::Home, ui::text({.text = "home"})}}),
                    std::invalid_argument);
}

TEST_CASE("ui::tabs: pages mount on first selection and are kept, hidden, afterwards", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::size_t> selected{runtime, 0};
    ui::Mounted const view{runtime, backend,
                           ui::tabs({
                               .tabs = {{.label = "Dashboard", .node = ui::text({.text = "dash"})},
                                        {.label = "Stats", .node = ui::text({.text = "stats"})}},
                               .selected = [&] { return selected.get(); },
                               .onSelect = [&](std::size_t index) { selected.set(index); },
                           })};
    CHECK(backend.dump() == "Tabs#1 selected=0 tabs=[Dashboard,Stats]\n  Slot#2\n    Text#3 role=Normal text=dash\n");

    backend.clearLog();
    backend.chooseIndex(1, 1);
    owner.runAll();
    // The new page is complete before the old one is hidden.
    CHECK(backend.log() == Lines{"create Slot#4 in Tabs#1", "create Text#5 in Slot#4", "set Text#5 text=stats",
                                 "set Text#5 role=Normal", "set Slot#2 visible=false", "set Tabs#1 selected=1"});

    backend.clearLog();
    backend.chooseIndex(1, 0);
    owner.runAll();
    CHECK(backend.log() == Lines{"set Slot#4 visible=false", "set Slot#2 visible=true", "set Tabs#1 selected=0"});
}

TEST_CASE("ui::tabs: an index past the last page shows none, and a null page is an empty slot", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::size_t> selected{runtime, 5};
    ui::Mounted const view{runtime, backend,
                           ui::tabs({
                               .tabs = {{.label = "A", .node = ui::text({.text = "a"})}, {.label = "B", .node = {}}},
                               .selected = [&] { return selected.get(); },
                           })};
    CHECK(backend.dump() == "Tabs#1 selected=5 tabs=[A,B]\n");

    selected.set(1);
    owner.runAll();
    CHECK(backend.dump() == "Tabs#1 selected=1 tabs=[A,B]\n  Slot#2\n");

    backend.clearLog();
    selected.set(7);
    owner.runAll();
    CHECK(backend.log() == Lines{"set Slot#2 visible=false", "set Tabs#1 selected=7"});

    backend.clearLog();
    selected.set(0);
    owner.runAll();
    CHECK(backend.log() == Lines{"create Slot#3 in Tabs#1", "create Text#4 in Slot#3", "set Text#4 text=a",
                                 "set Text#4 role=Normal", "set Tabs#1 selected=0"});
}

TEST_CASE("ui::tabs: a page that fails to mount leaves the previous page shown and selected", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    EchoingBackend backend;
    RecordingBackend& recording = backend.recording();
    std::optional<int> spare;
    backend.setOnCreate(failWhenSpent(spare));
    Signal<std::size_t> selected{runtime, 0};
    ui::Mounted const view{
        runtime, backend,
        ui::tabs({
            .tabs = {{.label = "A", .node = ui::text({.text = "a"})},
                     {.label = "B",
                      .node = ui::column({.children = {ui::text({.text = [] { return std::string{"b1"}; }}),
                                                       ui::text({.text = "b2"})}})}},
            .selected = [&] { return selected.get(); },
            .onSelect = [&](std::size_t index) { selected.set(index); },
        })};
    std::string const before = recording.dump();
    REQUIRE(before == "Tabs#1 selected=0 tabs=[A,B]\n  Slot#2\n    Text#3 role=Normal text=a\n");
    std::size_t const mountedNodes = runtime.core()->liveNodes();

    recording.clearLog();
    spare = 3;  // the Slot, the Column and the first Text; the second Text fails
    recording.chooseIndex(1, 1);
    owner.runAll();
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
    // The bar's highlight, which the user's pick moved, goes back to the page still shown, and the application hears
    // of it through onSelect, so its index matches what the widget shows. Its write lands inside the binding's own
    // run, so the binding does not run again for it.
    CHECK(recording.log() == Lines{"create Slot#4 in Tabs#1", "create Column#5 in Slot#4", "set Column#5 gap=0",
                                   "create Text#6 in Column#5", "set Text#6 text=b1", "set Text#6 role=Normal",
                                   "destroy Text#6", "destroy Column#5", "destroy Slot#4", "set Tabs#1 selected=0"});
    CHECK(selected.peek() == 0);
    CHECK(recording.dump() == before);
    CHECK(runtime.core()->liveNodes() == mountedNodes);

    // Selecting the failed tab again is therefore a change, and its page mounts this time.
    recording.chooseIndex(1, 1);
    owner.runAll();
    CHECK(recording.dump() ==
          "Tabs#1 selected=1 tabs=[A,B]\n"
          "  Slot#2 visible=false\n"
          "    Text#3 role=Normal text=a\n"
          "  Slot#7\n"
          "    Column#8 gap=0\n"
          "      Text#9 role=Normal text=b1\n"
          "      Text#10 role=Normal text=b2\n");
}

TEST_CASE("ui::tabs: the onSelect that reports a failed page may unmount the view", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    EchoingBackend backend;
    std::optional<int> spare;
    backend.setOnCreate(failWhenSpent(spare));
    Signal<std::size_t> selected{runtime, 0};
    std::size_t const before = runtime.core()->liveNodes();
    std::vector<std::size_t> reported;
    std::unique_ptr<ui::Mounted> view;
    view = std::make_unique<ui::Mounted>(
        runtime, backend,
        ui::tabs({
            .tabs = {{.label = "A", .node = ui::text({.text = "a"})}, {.label = "B", .node = ui::text({.text = "b"})}},
            .selected = [&] { return selected.get(); },
            .onSelect =
                [&](std::size_t index) {
                    reported.push_back(index);
                    selected.set(index);
                    if (index == 0) {  // only the report: the user picks the failing page
                        view.reset();
                    }
                },
        }));
    spare = 1;  // the Slot; the Text fails
    backend.recording().chooseIndex(1, 1);
    owner.runAll();
    CHECK(reported == std::vector<std::size_t>{1, 0});
    CHECK(view == nullptr);
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
    CHECK(backend.recording().dump().empty());
    CHECK(runtime.core()->liveNodes() == before);
}

TEST_CASE("ui::dialog: the content exists only while open, and dismiss reaches onDismiss", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> open{runtime, false};
    ui::Mounted const view{runtime, backend,
                           ui::dialog({.open = [&] { return open.get(); },
                                       .title = "Confirm",
                                       .child = ui::column({.children = {ui::text({.text = "Sure?"})}}),
                                       .onDismiss = [&] { open.set(false); }})};
    CHECK(backend.dump() == "Dialog#1 open=false title=Confirm\n");

    backend.clearLog();
    open.set(true);
    owner.runAll();
    CHECK(backend.log() == Lines{"create Column#2 in Dialog#1", "set Column#2 gap=0", "create Text#3 in Column#2",
                                 "set Text#3 text=Sure?", "set Text#3 role=Normal", "set Dialog#1 open=true"});

    backend.clearLog();
    backend.dismiss(1);
    owner.runAll();
    CHECK(backend.log() == Lines{"destroy Text#3", "destroy Column#2", "set Dialog#1 open=false"});
}

TEST_CASE("ui::dialog: a button inside may close the dialog, which reopens with new content", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> open{runtime, true};
    int dismissed = 0;
    ui::Mounted const view{runtime, backend,
                           ui::dialog({.open = [&] { return open.get(); },
                                       .title = "Done",
                                       .child = ui::button({.label = "Close", .onClick = [&] { open.set(false); }}),
                                       .onDismiss = [&] { ++dismissed; }})};
    REQUIRE(backend.kindOf(2) == "Button");
    backend.click(2);
    owner.runAll();
    CHECK_FALSE(backend.exists(2));
    CHECK(backend.dump() == "Dialog#1 open=false title=Done\n");

    backend.dismiss(1);  // a closed dialog is not shown, so there is nothing to dismiss
    CHECK(dismissed == 0);

    open.set(true);
    owner.runAll();
    CHECK(backend.dump() == "Dialog#1 open=true title=Done\n  Button#3 label=Close\n");
    backend.dismiss(1);
    CHECK(dismissed == 1);
}

TEST_CASE("ui::dialog: without onDismiss, failed content leaves the dialog closed until open turns true again",
          "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    EchoingBackend backend;
    RecordingBackend& recording = backend.recording();
    std::optional<int> spare;
    backend.setOnCreate(failWhenSpent(spare));
    Signal<bool> open{runtime, false};
    ui::Mounted const view{
        runtime, backend,
        ui::dialog({.open = [&] { return open.get(); },
                    .title = "T",
                    .child = ui::column({.children = {ui::button({.label = [] { return std::string{"OK"}; }}),
                                                      ui::text({.text = "body"})}})})};
    std::size_t const mountedNodes = runtime.core()->liveNodes();

    recording.clearLog();
    spare = 2;  // the Column and the Button; the Text fails
    open.set(true);
    owner.runAll();
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
    CHECK(recording.log() == Lines{"create Column#2 in Dialog#1", "set Column#2 gap=0", "create Button#3 in Column#2",
                                   "set Button#3 label=OK", "destroy Button#3", "destroy Column#2"});
    CHECK(recording.dump() == "Dialog#1 open=false title=T\n");
    CHECK(runtime.core()->liveNodes() == mountedNodes);

    // `open` is still true, so the mount is retried the next time it turns true.
    open.set(false);
    owner.runAll();
    open.set(true);
    owner.runAll();
    CHECK(recording.dump() ==
          "Dialog#1 open=true title=T\n  Column#4 gap=0\n    Button#5 label=OK\n    Text#6 role=Normal text=body\n");
}

TEST_CASE("ui::dialog: content that fails to mount dismisses the dialog, so the next open mounts again", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    EchoingBackend backend;
    RecordingBackend& recording = backend.recording();
    std::optional<int> spare;
    backend.setOnCreate(failWhenSpent(spare));
    Signal<bool> open{runtime, false};
    int dismissed = 0;
    ui::Mounted const view{runtime, backend,
                           ui::dialog({.open = [&] { return open.get(); },
                                       .title = "T",
                                       .child = ui::column({.children = {ui::text({.text = "body"})}}),
                                       .onDismiss =
                                           [&] {
                                               ++dismissed;
                                               open.set(false);
                                           }})};
    std::size_t const mountedNodes = runtime.core()->liveNodes();

    recording.clearLog();
    spare = 1;  // the Column; the Text fails
    open.set(true);
    owner.runAll();
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
    CHECK(recording.log() == Lines{"create Column#2 in Dialog#1", "set Column#2 gap=0", "destroy Column#2"});
    CHECK(recording.dump() == "Dialog#1 open=false title=T\n");
    CHECK(runtime.core()->liveNodes() == mountedNodes);
    // The application hears of it through onDismiss, so its `open` follows the widget.
    CHECK(dismissed == 1);
    CHECK_FALSE(open.peek());

    // Opening it again is therefore a change, and the content mounts this time.
    open.set(true);
    owner.runAll();
    CHECK(recording.dump() == "Dialog#1 open=true title=T\n  Column#3 gap=0\n    Text#4 role=Normal text=body\n");
    CHECK(dismissed == 1);
}

TEST_CASE("ui::dialog: the onDismiss that reports failed content may unmount the view", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    EchoingBackend backend;
    std::optional<int> spare;
    backend.setOnCreate(failWhenSpent(spare));
    Signal<bool> open{runtime, false};
    std::size_t const before = runtime.core()->liveNodes();
    std::unique_ptr<ui::Mounted> view;
    view = std::make_unique<ui::Mounted>(runtime, backend,
                                         ui::dialog({.open = [&] { return open.get(); },
                                                     .title = "T",
                                                     .child = ui::text({.text = "body"}),
                                                     .onDismiss = [&] { view.reset(); }}));
    spare = 0;  // the Text fails
    open.set(true);
    owner.runAll();
    CHECK(view == nullptr);
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
    CHECK(backend.recording().dump().empty());
    CHECK(runtime.core()->liveNodes() == before);
}

TEST_CASE("ui::dialog: a Dialog's button survives a nested loop in its own handler", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> open{runtime, true};
    bool aliveInHandler = false;
    ui::Mounted const view{runtime, backend,
                           ui::dialog({.open = [&] { return open.get(); },
                                       .title = "Confirm",
                                       .child = ui::button({.label = "Close", .onClick = [&] {
                                                                CHECK(owner.runOne());  // a modal loop pumps the owner
                                                                aliveInHandler = backend.exists(2);
                                                            }})})};
    REQUIRE(backend.kindOf(2) == "Button");
    open.set(false);  // a write from elsewhere: its posted flush would unmount the button
    backend.click(2);
    CHECK(aliveInHandler);
    CHECK(probe.count(morph::reactive::detail::site::kFlushInWidgetEvent) == 1);
    owner.runAll();
    CHECK_FALSE(backend.exists(2));
    CHECK(backend.prop(1, "open") == "false");
}

TEST_CASE("ui structure: unmounting tears content down before its host, the newest page first", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::size_t> selected{runtime, 1};
    std::size_t const before = runtime.core()->liveNodes();
    auto view = std::make_unique<ui::Mounted>(
        runtime, backend,
        ui::column({.children = {
                        ui::tabs({
                            .tabs = {{.label = "A", .node = ui::text({.text = "a"})},
                                     {.label = "B", .node = ui::text({.text = "b"})}},
                            .selected = [&] { return selected.get(); },
                        }),
                        ui::dialog({.open = true, .title = "D", .child = ui::text({.text = "body"})}),
                        ui::switchOf({.selector = intKey(0),
                                      .cases = {{.key = intKey(0), .node = ui::text({.text = "case"})}}}),
                    }}));
    // The second tab's page is shown first, so the order pages were shown in is not the order of the tabs.
    selected.set(0);
    owner.runAll();
    CHECK(backend.dump() ==
          "Column#1 gap=0\n"
          "  Tabs#2 selected=0 tabs=[A,B]\n"
          "    Slot#3 visible=false\n"
          "      Text#4 role=Normal text=b\n"
          "    Slot#9\n"
          "      Text#10 role=Normal text=a\n"
          "  Dialog#5 open=true title=D\n"
          "    Text#6 role=Normal text=body\n"
          "  Slot#7\n"
          "    Text#8 role=Normal text=case\n");

    backend.clearLog();
    view.reset();
    CHECK(backend.log() == Lines{"destroy Text#8", "destroy Slot#7", "destroy Text#6", "destroy Dialog#5",
                                 "destroy Text#10", "destroy Slot#9", "destroy Text#4", "destroy Slot#3",
                                 "destroy Tabs#2", "destroy Column#1"});
    CHECK(backend.dump().empty());
    CHECK(runtime.core()->liveNodes() == before);
}

TEST_CASE("ui structure: what a backend reads while content mounts subscribes no structure binding", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    EchoingBackend backend;
    Signal<int> noise{runtime, 0};
    Signal<std::int64_t> which{runtime, 0};
    Signal<std::size_t> selected{runtime, 0};
    Signal<bool> open{runtime, false};
    backend.setOnCreate([&noise] { static_cast<void>(noise.get()); });
    ui::Mounted const view{
        runtime, backend,
        ui::column({.children = {
                        ui::switchOf({.selector = [&] { return ui::Key{which.get()}; },
                                      .cases = {{.key = intKey(0), .node = ui::text({.text = "zero"})},
                                                {.key = intKey(1), .node = ui::text({.text = "one"})}}}),
                        ui::tabs({.tabs = {{.label = "A", .node = ui::text({.text = "a"})},
                                           {.label = "B", .node = ui::text({.text = "b"})}},
                                  .selected = [&] { return selected.get(); }}),
                        ui::dialog({.open = [&] { return open.get(); }, .child = ui::text({.text = "body"})}),
                    }})};
    // Each binding mounts content from inside its own Effect here.
    which.set(1);
    selected.set(1);
    open.set(true);
    owner.runAll();
    REQUIRE(backend.recording().all("Text").size() == 4);

    backend.recording().clearLog();
    noise.set(1);
    owner.runAll();
    CHECK(backend.recording().log().empty());
}
