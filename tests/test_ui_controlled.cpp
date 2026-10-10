// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/testing/owner_probe_recorder.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
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
using ui::testing::RecordingBackend;
using Owner = morph::testing::StepExecutor;
using Lines = std::vector<std::string>;

std::string upper(std::string text) {
    for (char& character : text) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return text;
}

int only(RecordingBackend const& backend, std::string_view kind) {
    std::vector<int> const ids = backend.all(kind);
    REQUIRE(ids.size() == 1);
    return ids.front();
}

}  // namespace

// Mutation: in Mounter::reasserter, do not post the turn (the field keeps "ab").
TEST_CASE("ui controlled: an edit the slot refuses snaps back once the event's turn has run", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> const name{runtime, "a"};
    Lines changes;
    ui::Mounted const view{runtime, backend,
                           ui::textInput({.value = [&] { return name.get(); },
                                          .onChange = [&](std::string text) { changes.push_back(std::move(text)); }})};
    backend.clearLog();
    backend.edit(1, "ab");
    CHECK(changes == Lines{"ab"});
    CHECK(backend.prop(1, "text") == "ab");  // the field shows the request until the turn runs
    owner.runAll();
    CHECK(backend.prop(1, "text") == "a");
    CHECK(backend.log() == Lines{"set TextInput#1 text=a"});
}

// Mutations: in Mounter::showSlot, show the slot whatever the widget shows; in Mounter::controlled, send a value the
// widget already shows. Either adds a setText the field does not need.
TEST_CASE("ui controlled: an accepted edit costs no setter call", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> name{runtime, "a"};
    ui::Mounted const view{
        runtime, backend,
        ui::textInput({.value = [&] { return name.get(); }, .onChange = [&](std::string text) { name.set(text); }})};
    backend.clearLog();
    backend.edit(1, "ab");
    owner.runAll();
    CHECK(name.peek() == "ab");
    CHECK(backend.prop(1, "text") == "ab");
    CHECK(backend.log().empty());
}

// Mutation: in Mounter::reasserter, run the check inside the event instead of posting it (the field shows "AB"
// before the flush).
TEST_CASE("ui controlled: a transformed edit shows the raw text for one turn, then the slot's", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> name{runtime, "A"};
    ui::Mounted const view{runtime, backend,
                           ui::textInput({.value = [&] { return name.get(); },
                                          .onChange = [&](std::string text) { name.set(upper(std::move(text))); }})};
    backend.clearLog();
    backend.edit(1, "ab");
    CHECK(backend.prop(1, "text") == "ab");
    owner.runAll();
    CHECK(backend.prop(1, "text") == "AB");
    CHECK(backend.log() == Lines{"set TextInput#1 text=AB"});
}

// The cursor rule is what keeps that turn harmless: a re-assertion of the text the field shows leaves the cursor.
// Mutation: in Mounter::controlled, send a value the widget already shows, and in FakeTextInput::setText move the
// cursor whatever the text (the cursor jumps to the end).
TEST_CASE("ui controlled: an accepted edit keeps the cursor where the user left it", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> name{runtime, ""};
    ui::Mounted const view{
        runtime, backend,
        ui::textInput({.value = [&] { return name.get(); }, .onChange = [&](std::string text) { name.set(text); }})};
    backend.edit(1, "abc");
    backend.moveCursor(1, 1);
    owner.runAll();
    CHECK(backend.prop(1, "text") == "abc");
    CHECK(backend.cursor(1) == 1);
}

// Mutation: in Mounter::controlled, return a re-assertion for a constant too (the field snaps back to "fixed").
TEST_CASE("ui controlled: a constant value is not a slot, and the field keeps what the user typed", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Lines changes;
    ui::Mounted const view{
        runtime, backend,
        ui::textInput({.value = "fixed", .onChange = [&](std::string text) { changes.push_back(std::move(text)); }})};
    backend.clearLog();
    backend.edit(1, "typed");
    owner.runAll();
    CHECK(changes == Lines{"typed"});
    CHECK(backend.prop(1, "text") == "typed");
    CHECK(backend.log().empty());
}

// Mutation: in Mounter::inputEvent, return the bare event when the application gave no handler (the edit stays).
TEST_CASE("ui controlled: a slot with no handler is read-only", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> const name{runtime, "a"};
    ui::Mounted const view{runtime, backend, ui::textInput({.value = [&] { return name.get(); }})};
    backend.edit(1, "ab");
    owner.runAll();
    CHECK(backend.prop(1, "text") == "a");
}

// A handler that destroys its own field: the event finds the check gone and posts nothing.
TEST_CASE("ui controlled: an edit whose handler unmounts the field posts no turn", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> const name{runtime, "a"};
    std::optional<ui::Mounted> view;
    view.emplace(runtime, backend,
                 ui::textInput({.value = [&] { return name.get(); }, .onChange = [&](std::string) { view.reset(); }}));
    backend.edit(1, "ab");
    CHECK_FALSE(view.has_value());
    CHECK_FALSE(backend.exists(1));
    CHECK(owner.runAll() == 0);
}

// The field goes in the flush the edit caused, after the turn was posted: the turn holds the check weakly and finds
// it gone. Mutation: capture the check strongly in the posted turn (it reads the destroyed Computed and shows a value
// on a destroyed widget: a use after free under ASan, and an out-of-range id from the recording backend here).
TEST_CASE("ui controlled: a turn posted for a field that is gone by then does nothing", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> const name{runtime, "a"};
    Signal<std::int64_t> which{runtime, 1};
    ui::Mounted const view{
        runtime, backend,
        ui::switchOf({.selector = [&] { return ui::Key{which.get()}; },
                      .cases = {{.key = ui::Key{std::int64_t{1}},
                                 .node = ui::textInput({.value = [&] { return name.get(); },
                                                        .onChange = [&](std::string) { which.set(0); }})}}})};
    int const field = backend.all("TextInput").front();
    backend.edit(field, "ab");
    CHECK_NOTHROW(owner.runAll());
    CHECK_FALSE(backend.exists(field));
    CHECK(backend.dump() == "Slot#1\n");
}

// Every input kind whose value is a slot snaps back to it when the application ignores the user. Mutation, per kind:
// mount its value with `bind` instead of `controlled` (or, for Tabs and Dialog, drop the reasserter), and that kind's
// check after the turn fails.
TEST_CASE("ui controlled: every input kind shows its slot again after a request the application ignores", "[ui]") {
    using morph::time::Timestamp;
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> const checked{runtime, false};
    Signal<std::optional<ui::Key>> const chosen{runtime, ui::Key{std::int64_t{1}}};
    Signal<std::int64_t> const level{runtime, 5};
    Signal<std::optional<Timestamp>> const when{runtime, std::nullopt};
    Signal<std::string> const path{runtime, "/a"};
    Signal<bool> const collapsed{runtime, false};
    Signal<std::size_t> const page{runtime, 0};
    Signal<bool> const open{runtime, true};
    Signal<std::vector<ui::Key>> const picked{runtime, {}};
    int requests = 0;
    auto const ignore = [&requests](auto&&...) { ++requests; };
    ui::Mounted const view{
        runtime, backend,
        ui::column(
            {.children = {
                 ui::checkbox({.label = "c", .checked = [&] { return checked.get(); }, .onToggle = ignore}),
                 ui::select(
                     {.options = std::vector<ui::SelectOption>{{.key = ui::Key{std::int64_t{1}}, .label = "one"},
                                                               {.key = ui::Key{std::int64_t{2}}, .label = "two"}},
                      .selected = [&] { return chosen.get(); },
                      .onSelect = ignore}),
                 ui::slider({.value = [&] { return level.get(); }, .onChange = ignore}),
                 ui::dateTimeInput({.value = [&] { return when.get(); }, .onChange = ignore}),
                 ui::filePicker({.path = [&] { return path.get(); }, .onPicked = ignore}),
                 ui::panel({.title = "p",
                            .collapsible = true,
                            .collapsed = [&] { return collapsed.get(); },
                            .onToggle = ignore}),
                 ui::tabs({.tabs = {{.label = "A", .node = ui::text({.text = "a"})},
                                    {.label = "B", .node = ui::text({.text = "b"})}},
                           .selected = [&] { return page.get(); },
                           .onSelect = ignore}),
                 ui::dialog({.open = [&] { return open.get(); },
                             .child = ui::text({.text = "inside"}),
                             .onDismiss = [&requests] { ++requests; }}),
                 ui::table<std::int64_t>(
                     {{.label = "n"}}, [] { return std::vector<std::int64_t>{1, 2}; },
                     [](std::int64_t row) { return ui::Key{row}; },
                     [](Signal<std::int64_t> const& row) {
                         return std::vector<ui::Node>{
                             ui::text({.text = [&row] { return std::to_string(row.get()); }})};
                     },
                     {.selectionMode = ui::SelectionMode::Single,
                      .selection = [&] { return picked.get(); },
                      .onSelectionChange = ignore}),
             }})};
    int const checkbox = only(backend, "Checkbox");
    int const select = only(backend, "Select");
    int const slider = only(backend, "Slider");
    int const dateTime = only(backend, "DateTimeInput");
    int const picker = only(backend, "FilePicker");
    int const panel = only(backend, "Panel");
    int const tabBar = only(backend, "Tabs");
    int const dialog = only(backend, "Dialog");
    int const table = only(backend, "Table");
    std::string const before = backend.dump();

    backend.toggle(checkbox);
    backend.choose(select, ui::Key{std::int64_t{2}});
    backend.slide(slider, 9);
    backend.setDateTime(dateTime, Timestamp{morph::time::DateTime{std::chrono::year{2026}, std::chrono::month{10},
                                                                  std::chrono::day{4}, std::chrono::hours{9},
                                                                  std::chrono::minutes{30}, std::chrono::seconds{0}}});
    backend.pick(picker, "/b");
    backend.collapse(panel, true);
    backend.chooseIndex(tabBar, 1);
    backend.dismiss(dialog);
    backend.selectRows(table, {ui::Key{std::int64_t{2}}});
    CHECK(requests == 9);
    CHECK(backend.prop(checkbox, "checked") == "true");
    CHECK(backend.prop(select, "selected") == "2");
    CHECK(backend.prop(slider, "value") == "9");
    CHECK(backend.prop(dateTime, "value") != "none");
    CHECK(backend.prop(picker, "path") == "/b");
    CHECK(backend.prop(panel, "collapsed") == "true");
    CHECK(backend.prop(tabBar, "selected") == "1");
    CHECK(backend.prop(dialog, "open") == "false");
    CHECK(backend.prop(table, "selection") == "[2]");

    owner.runAll();
    CHECK(backend.prop(checkbox, "checked") == "false");
    CHECK(backend.prop(select, "selected") == "1");
    CHECK(backend.prop(slider, "value") == "5");
    CHECK(backend.prop(dateTime, "value") == "none");
    CHECK(backend.prop(picker, "path") == "/a");
    CHECK(backend.prop(panel, "collapsed") == "false");
    CHECK(backend.prop(tabBar, "selected") == "0");
    CHECK(backend.prop(dialog, "open") == "true");
    CHECK(backend.prop(table, "selection") == "[]");
    CHECK(backend.dump() == before);
}

// A dialog whose content failed to mount reports itself closed through onDismiss. A document that keeps `open` true
// then has its dialog re-asserted, but there is no content to show, so it stays closed rather than opening empty, and
// the re-assertion does not try the mount again. Mutation: in Mounter's Dialog re-assertion, call setOpen whatever
// the content (the dialog opens with nothing in it).
TEST_CASE("ui controlled: a dialog whose content failed stays closed when the document keeps it open", "[ui]") {
    Owner owner;
    morph::testing::OwnerProbeRecorder const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    EchoingBackend backend;
    RecordingBackend& recording = backend.recording();
    std::optional<int> spare;
    backend.setOnCreate(failWhenSpent(spare));
    Signal<bool> open{runtime, false};
    int dismissals = 0;
    ui::Mounted const view{runtime, backend,
                           ui::dialog({.open = [&] { return open.get(); },
                                       .child = ui::text({.text = "inside"}),
                                       .onDismiss = [&] { ++dismissals; }})};
    spare = 0;  // the content's Text fails
    open.set(true);
    owner.runAll();
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
    CHECK(dismissals == 1);
    CHECK(recording.prop(1, "open") == "false");
    CHECK(recording.children(1).empty());
}
