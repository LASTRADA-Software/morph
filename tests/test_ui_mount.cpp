// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "owner_probe_recorder.hpp"
#include "test_support.hpp"
#include "ui_echoing_backend.hpp"

namespace ui = morph::ui;

namespace {

using morph::reactive::Runtime;
using morph::reactive::Signal;
using ui::testing::RecordingBackend;
using Owner = morph::testing::StepExecutor;
using Probe = morph::testing::OwnerProbeRecorder;
using Lines = std::vector<std::string>;
using morph::testing::EchoingBackend;

}  // namespace

static_assert(!std::is_copy_constructible_v<ui::Mounted> && !std::is_move_constructible_v<ui::Mounted>);

TEST_CASE("ui::Mounted: a constant tree is built once and makes no reactive node", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const
        view{runtime, backend,
             ui::column(
                 {
                     .children =
                         {
                             ui::text({.text = "Title", .role = ui::TextRole::Heading}),
                             ui::row({.children = {ui::button({.label = "Go"}),
                                                   ui::spacer(
                                                       {.common = {.layout = {.width = ui::Sizing::stretch()}}})},
                                      .gap = 2}),
                             ui::textInput(
                                 {.value = "abc", .placeholder = "name", .mode = ui::TextInputMode::Password}),
                             ui::checkbox({.label = "Agree", .checked = true}),
                             ui::select({.options =
                                             std::vector<ui::SelectOption>{
                                                 {.key = ui::Key{std::int64_t{1}}, .label = "One"},
                                                 {.key = ui::Key{std::string{"b"}}, .label = "Bee"}},
                                         .selected = std::optional<ui::Key>{ui::Key{std::string{"b"}}},
                                         .style = ui::SelectStyle::Radio}),
                             ui::menu({.items = {{.label = "Open"}, {.label = "Quit"}}}),
                             ui::grid(
                                 {.columns = 2, .cells = {{.node = ui::text({.text = "wide"}), .span = 2}}, .gap = 1}),
                             ui::panel({.title = "Box",
                                        .padding = 1,
                                        .child = ui::busy({.active = true, .label = "wait"}),
                                        .collapsible = true,
                                        .collapsed = false}),
                             ui::scroll({.child = ui::slider({.value = 5, .minimum = 0, .maximum = 10, .step = 1}),
                                         .axis = ui::Axis::Horizontal}),
                             ui::dateTimeInput(
                                 {.value = std::nullopt, .mode = ui::DateMode::Date, .offsetMinutes = 60}),
                             ui::filePicker({.path = "/tmp/x", .mode = ui::FilePickerMode::Save}),
                         },
                     .gap = 1,
                 })};
    CHECK(backend.dump() ==
          "Column#1 gap=1\n"
          "  Text#2 role=Heading text=Title\n"
          "  Row#3 gap=2\n"
          "    Button#4 label=Go\n"
          "    Spacer#5 layout=stretch(1)/content\n"
          "  TextInput#6 mode=Password placeholder=name text=abc\n"
          "  Checkbox#7 checked=true label=Agree\n"
          "  Select#8 options=[1:One,\"b\":Bee] selected=\"b\" style=Radio\n"
          "  Menu#9 items=[Open,Quit]\n"
          "  Grid#10 columns=2 gap=1\n"
          "    Text#11 role=Normal span=2 text=wide\n"
          "  Panel#12 collapsed=false collapsible=true padding=1 title=Box\n"
          "    Busy#13 active=true label=wait\n"
          "  Scroll#14 axis=Horizontal\n"
          "    Slider#15 range=0..10/1 value=5\n"
          "  DateTimeInput#16 mode=Date offset=60 value=none\n"
          "  FilePicker#17 mode=Save path=/tmp/x\n");
    CHECK(runtime.core()->liveNodes() == 0);
    CHECK(owner.pending() == 0);
}

TEST_CASE("ui::Mounted: a binding calls its setter once per real change", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> name{runtime, "ada"};
    Signal<int> count{runtime, 1};
    ui::Mounted const greeting{runtime, backend, ui::text({.text = [&] { return "Hi " + name.get(); }})};
    ui::Mounted const parity{runtime, backend,
                             ui::text({.text = [&] { return count.get() % 2 == 0 ? "even" : "odd"; }})};
    backend.clearLog();

    name.set("bob");
    owner.runAll();
    CHECK(backend.log() == Lines{"set Text#1 text=Hi bob"});

    backend.clearLog();
    name.set("bob");
    CHECK(owner.pending() == 0);

    count.set(3);  // the binding re-runs and yields "odd" again: no setter call
    owner.runAll();
    CHECK(backend.log().empty());
}

TEST_CASE("ui::Mounted: visible and enabled follow their props", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> shown{runtime, true};
    ui::Mounted const view{runtime, backend,
                           ui::column({.children = {
                                           ui::button({.label = "Bound",
                                                       .common = {.visible = [&] { return shown.get(); },
                                                                  .enabled = [&] { return shown.get(); }}}),
                                           ui::button({.label = "Hidden", .common = {.visible = false}}),
                                           ui::button({.label = "Plain"}),
                                       }})};
    CHECK(backend.dump() ==
          "Column#1 gap=0\n"
          "  Button#2 enabled=true label=Bound visible=true\n"
          "  Button#3 label=Hidden visible=false\n"
          "  Button#4 label=Plain\n");
    backend.clearLog();
    shown.set(false);
    owner.runAll();
    CHECK(backend.log() == Lines{"set Button#2 visible=false", "set Button#2 enabled=false"});
}

TEST_CASE("ui::Mounted: a pending flush does not run inside a click handler", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<int> clicks{runtime, 0};
    std::string labelInHandler;
    ui::Mounted const view{runtime, backend,
                           ui::button({.label = [&] { return "clicked " + std::to_string(clicks.get()); },
                                       .onClick =
                                           [&] {
                                               CHECK(owner.runOne());  // a nested event loop pumps the owner
                                               labelInHandler = backend.prop(1, "label");
                                           }})};
    clicks.set(1);  // posts a flush that would update the label
    backend.click(1);
    CHECK(labelInHandler == "clicked 0");
    CHECK(probe.count(morph::reactive::detail::site::kFlushInWidgetEvent) == 1);
    owner.runAll();
    CHECK(backend.prop(1, "label") == "clicked 1");
}

TEST_CASE("ui::Mounted: typing reaches onChange, and a programmatic value is not echoed", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> name{runtime, "a"};
    Lines changes;
    Lines submits;
    ui::Mounted const view{runtime, backend,
                           ui::textInput({.value = [&] { return name.get(); },
                                          .onChange =
                                              [&](std::string text) {
                                                  changes.push_back(text);
                                                  name.set(std::move(text));
                                              },
                                          .onSubmit = [&](std::string text) { submits.push_back(std::move(text)); },
                                          .placeholder = "name"})};
    backend.edit(1, "ab");
    owner.runAll();
    CHECK(changes == Lines{"ab"});
    CHECK(backend.prop(1, "text") == "ab");

    name.set("xyz");
    owner.runAll();
    CHECK(backend.prop(1, "text") == "xyz");
    CHECK(changes == Lines{"ab"});

    backend.submit(1, "xyz");
    CHECK(submits == Lines{"xyz"});
}

TEST_CASE("ui::Mounted: a menu entry runs its own action and its label may be bound", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<int> count{runtime, 1};
    Lines ran;
    ui::Mounted const view{runtime, backend,
                           ui::menu({.items = {{.label = [&] { return "Count " + std::to_string(count.get()); },
                                                .onSelect = [&] { ran.emplace_back("count"); }},
                                               {.label = "Quit", .onSelect = [&] { ran.emplace_back("quit"); }}}})};
    CHECK(backend.prop(1, "items") == "[Count 1,Quit]");
    backend.chooseIndex(1, 1);
    backend.chooseIndex(1, 7);  // out of range: nothing runs
    CHECK(ran == Lines{"quit"});
    backend.clearLog();
    count.set(2);
    owner.runAll();
    CHECK(backend.log() == Lines{"set Menu#1 items=[Count 2,Quit]"});
}

TEST_CASE("ui::Mounted: a collapsible panel reports a toggle and follows its collapsed prop", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> collapsed{runtime, false};
    ui::Mounted const view{runtime, backend,
                           ui::column({.children = {
                                           ui::panel({.title = "Advanced",
                                                      .child = ui::text({.text = "inside"}),
                                                      .collapsible = true,
                                                      .collapsed = [&] { return collapsed.get(); },
                                                      .onToggle = [&](bool value) { collapsed.set(value); }}),
                                           ui::panel({.title = "Fixed", .collapsed = true}),
                                       }})};
    CHECK(backend.prop(4, "collapsed").empty());  // not collapsible: collapsed is never set
    backend.collapse(2, true);
    owner.runAll();
    CHECK(collapsed.peek());
    CHECK(backend.prop(2, "collapsed") == "true");
}

TEST_CASE("ui::Mounted: date-time, slider and file picker report what the user entered", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    using morph::time::Timestamp;
    Signal<std::optional<Timestamp>> due{runtime, std::nullopt};
    Signal<std::int64_t> volume{runtime, 3};
    Signal<std::string> path{runtime, ""};
    ui::Mounted const view{
        runtime, backend,
        ui::column({.children = {
                        ui::dateTimeInput({.value = [&] { return due.get(); },
                                           .onChange = [&](std::optional<Timestamp> value) { due.set(value); }}),
                        ui::slider({.value = [&] { return volume.get(); },
                                    .minimum = 0,
                                    .maximum = 10,
                                    .step = 1,
                                    .onChange = [&](std::int64_t value) { volume.set(value); }}),
                        ui::filePicker({.path = [&] { return path.get(); },
                                        .onPicked = [&](std::string value) { path.set(std::move(value)); }}),
                    }})};
    Timestamp const when{morph::time::DateTime{std::chrono::year{2026}, std::chrono::month{10}, std::chrono::day{4},
                                               std::chrono::hours{9}, std::chrono::minutes{30},
                                               std::chrono::seconds{0}}};
    backend.setDateTime(2, when);
    backend.slide(3, 7);
    backend.pick(4, "/tmp/report.csv");
    owner.runAll();
    CHECK(due.peek() == std::optional<Timestamp>{when});
    CHECK(volume.peek() == 7);
    CHECK(path.peek() == "/tmp/report.csv");
    CHECK(backend.dump() ==
          "Column#1 gap=0\n"
          "  DateTimeInput#2 mode=DateTime offset=0 value=2026-10-04T09:30:00.000Z\n"
          "  Slider#3 range=0..10/1 value=7\n"
          "  FilePicker#4 mode=Open path=/tmp/report.csv\n");
}

TEST_CASE("ui::Mounted: a drag key reaches a drop target that accepts it", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    std::vector<ui::Key> dropped;
    auto const keep = [&dropped](ui::Key key) { dropped.push_back(std::move(key)); };
    ui::Mounted const view{
        runtime, backend,
        ui::column(
            {.children = {
                 ui::text({.text = "card", .common = {.dragKey = std::optional<ui::Key>{ui::Key{std::int64_t{7}}}}}),
                 ui::panel(
                     {.title = "Numbers",
                      .common = {.accepts =
                                     [](ui::Key const& key) { return std::holds_alternative<std::int64_t>(key); },
                                 .onDrop = keep}}),
                 ui::panel({.title = "Words",
                            .common = {.accepts = [](ui::Key const&
                                                         key) { return std::holds_alternative<std::string>(key); },
                                       .onDrop = keep}}),
                 ui::panel({.title = "Anything", .common = {.onDrop = keep}}),
                 ui::text({.text = "not a target"}),
             }})};
    CHECK(backend.prop(2, "dragKey") == "7");
    CHECK(backend.drag(2, 3));
    CHECK_FALSE(backend.drag(2, 4));
    CHECK(backend.drag(2, 5));  // no accepts: every key is accepted
    CHECK_FALSE(backend.drag(2, 6));
    CHECK_FALSE(backend.drag(6, 3));
    CHECK(dropped == std::vector<ui::Key>{ui::Key{std::int64_t{7}}, ui::Key{std::int64_t{7}}});
}

TEST_CASE("ui::Mounted: unmounting destroys children first and leaves no binding", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> label{runtime, "x"};
    auto view = std::make_unique<ui::Mounted>(
        runtime, backend,
        ui::column({.children = {ui::text({.text = [&] { return label.get(); }}),
                                 ui::row({.children = {ui::button({.label = "Go"})}})}}));
    CHECK(runtime.core()->liveNodes() == 3);  // the signal, and the text binding's Computed and Effect
    backend.clearLog();
    view.reset();
    CHECK(backend.log() == Lines{"destroy Button#4", "destroy Row#3", "destroy Text#2", "destroy Column#1"});
    CHECK(runtime.core()->liveNodes() == 1);  // the signal
    label.set("y");
    CHECK(owner.pending() == 0);
}

TEST_CASE("ui::Mounted: mounts into an existing container, and refuses a null root", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    auto host = backend.createStack(nullptr, ui::Axis::Vertical);
    {
        ui::Mounted const view{runtime, backend, ui::text({.text = "inside"}), host.get()};
        CHECK(backend.dump() == "Column#1\n  Text#2 role=Normal text=inside\n");
        CHECK(backend.idOf(view.root()) == 2);
    }
    CHECK(backend.dump() == "Column#1\n");
    CHECK_THROWS_AS((ui::Mounted{runtime, backend, ui::Node{}}), std::invalid_argument);
}

TEST_CASE("ui::Mounted: a node is created, then given Common, then its own props, then its children", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> enabled{runtime, true};
    ui::Mounted const view{
        runtime, backend,
        ui::row(
            {.children = {
                 ui::button({.label = "B",
                             .common = {.visible = false,
                                        .enabled = [&] { return enabled.get(); },
                                        .layout = {.width = ui::Sizing::fixed(3)},
                                        .dragKey = std::optional<ui::Key>{ui::Key{std::string{"k"}}},
                                        .onDrop = [](ui::Key const&) {}}}),
                 ui::Node{},
                 ui::grid({.columns = 3,
                           .cells = {{.node = ui::text({.text = "one"})},
                                     {.node = ui::Node{}, .span = 2},
                                     {.node = ui::spacer(), .span = 2}}}),
                 ui::panel({.title = "P", .child = ui::text({.text = "in"}), .collapsible = true, .collapsed = true}),
             }})};
    CHECK(backend.log() ==
          Lines{"create Row#1 in root",       "set Row#1 gap=0",           "create Button#2 in Row#1",
                "set Button#2 visible=false", "set Button#2 enabled=true", "set Button#2 layout=fixed(3)/content",
                "set Button#2 dragKey=\"k\"", "set Button#2 drop=handler", "set Button#2 label=B",
                "create Grid#3 in Row#1",     "set Grid#3 columns=3",      "set Grid#3 gap=0",
                "create Text#4 in Grid#3",    "set Text#4 text=one",       "set Text#4 role=Normal",
                "create Spacer#5 in Grid#3",  "set Spacer#5 span=2",       "create Panel#6 in Row#1",
                "set Panel#6 title=P",        "set Panel#6 padding=0",     "set Panel#6 collapsible=true",
                "set Panel#6 collapsed=true", "create Text#7 in Panel#6",  "set Text#7 text=in",
                "set Text#7 role=Normal"});
}

TEST_CASE("ui::Mounted: a binding that throws is reported, and its widget keeps its last value", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<int> value{runtime, 0};
    ui::Mounted const view{runtime, backend, ui::text({.text = [&] {
                               if (value.get() % 2 == 0) {
                                   throw std::runtime_error{"even"};
                               }
                               return std::to_string(value.get());
                           }})};
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
    CHECK_FALSE(backend.hasProp(1, "text"));
    CHECK(backend.prop(1, "role") == "Normal");  // the mount went on past the failed binding

    value.set(1);
    owner.runAll();
    CHECK(backend.prop(1, "text") == "1");

    backend.clearLog();
    value.set(2);
    owner.runAll();
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 2);
    CHECK(backend.log().empty());
    CHECK(backend.prop(1, "text") == "1");

    value.set(3);
    owner.runAll();
    CHECK(backend.prop(1, "text") == "3");
}

TEST_CASE("ui::Mounted: a click handler may unmount the view that holds its own button", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> label{runtime, "Close"};
    std::unique_ptr<ui::Mounted> view;
    std::string seenAfter;
    // Larger than std::function's inline buffer, so the handler's closure is on the heap, and the read after the
    // unmount is a use after free unless the mount's wrapper keeps its own copy of the handler.
    std::string const note = "read after the unmount, from the handler's own closure";
    view = std::make_unique<ui::Mounted>(
        runtime, backend,
        ui::column({.children = {ui::button({.label = [&] { return label.get(); },
                                             .onClick =
                                                 [&view, &label, &seenAfter, note = note] {
                                                     label.set("Closing");
                                                     view.reset();
                                                     seenAfter = note;
                                                 }})}}));
    backend.clearLog();
    backend.click(2);
    CHECK(view == nullptr);
    CHECK(seenAfter == note);
    owner.runAll();
    CHECK(backend.log() == Lines{"destroy Button#2", "destroy Column#1"});
    CHECK(backend.dump().empty());
    CHECK(runtime.core()->liveNodes() == 1);  // the signal
}

TEST_CASE("ui::Mounted: a handler a setter echoes does not subscribe the binding that called the setter", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    EchoingBackend backend;
    Signal<std::vector<ui::SelectOption>> options{
        runtime, std::vector<ui::SelectOption>{{.key = ui::Key{std::int64_t{1}}, .label = "One"}}};
    Signal<int> elsewhere{runtime, 0};
    std::vector<ui::Key> echoed;
    ui::Mounted const view{runtime, backend,
                           ui::select({.options = [&] { return options.get(); },
                                       .onSelect =
                                           [&](ui::Key key) {
                                               static_cast<void>(elsewhere.get());
                                               echoed.push_back(std::move(key));
                                           }})};
    options.set(std::vector<ui::SelectOption>{{.key = ui::Key{std::int64_t{2}}, .label = "Two"}});
    owner.runAll();
    CHECK(echoed == std::vector<ui::Key>{ui::Key{std::int64_t{2}}});  // called from inside the options binding

    backend.recording().clearLog();
    elsewhere.set(1);
    owner.runAll();
    CHECK(backend.recording().log().empty());
    CHECK(echoed.size() == 1);
}

TEST_CASE("ui::Mounted: a callback that throws is reported, and what it wrote before still flushes", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<int> clicks{runtime, 0};
    std::vector<ui::Key> dropped;
    ui::Mounted const view{
        runtime, backend,
        ui::column(
            {.children = {
                 ui::button({.label = [&] { return "clicked " + std::to_string(clicks.get()); },
                             .onClick =
                                 [&] {
                                     clicks.set(1);
                                     throw std::runtime_error{"the handler failed"};
                                 }}),
                 ui::text({.text = "card", .common = {.dragKey = std::optional<ui::Key>{ui::Key{"k"}}}}),
                 ui::panel({.title = "Target",
                            .common = {.accepts = [](ui::Key const&) -> bool {
                                           throw std::runtime_error{"the predicate failed"};
                                       },
                                       .onDrop = [&dropped](ui::Key key) { dropped.push_back(std::move(key)); }}}),
             }})};
    CHECK_NOTHROW(backend.click(2));
    CHECK(probe.count(ui::detail::site::kCallbackThrew) == 1);
    owner.runAll();
    CHECK(backend.prop(2, "label") == "clicked 1");

    bool accepted = true;
    CHECK_NOTHROW(accepted = backend.drag(3, 4));
    CHECK_FALSE(accepted);  // a predicate that throws refuses the drop
    CHECK(dropped.empty());
    CHECK(probe.count(ui::detail::site::kCallbackThrew) == 2);
}

TEST_CASE("ui::Mounted: mounting inside an Effect does not subscribe that Effect to what the mount reads", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    EchoingBackend backend;
    Signal<int> theme{runtime, 0};
    backend.setOnCreate([&theme] { static_cast<void>(theme.get()); });
    int hostRuns = 0;
    std::unique_ptr<ui::Mounted> view;
    morph::reactive::Effect const host{runtime, [&] {
                                           ++hostRuns;
                                           view = std::make_unique<ui::Mounted>(runtime, backend,
                                                                                ui::text({.text = "inside"}));
                                       }};
    CHECK(hostRuns == 1);
    CHECK(backend.recording().dump() == "Text#1 role=Normal text=inside\n");
    theme.set(1);
    owner.runAll();
    CHECK(hostRuns == 1);
}
