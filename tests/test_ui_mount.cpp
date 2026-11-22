// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/testing/owner_probe_recorder.hpp>
#include <morph/ui/backend.hpp>
#include <morph/table/view_change.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <span>
#include <numeric>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "test_support.hpp"
#include "ui_echoing_backend.hpp"
#include "ui_fake_rows.hpp"
#include "ui_test_support.hpp"

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

// Mutation: call `setMinColumnWidth` whatever the key (a plain grid records it as 0), or for a grid with tracks (which
// size their own columns).
TEST_CASE("ui::Mounted: a grid's least column width reaches only a grid of equal columns that gives one", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const mounted{
        runtime, backend,
        ui::column({.children = {ui::grid({.columns = 3, .minColumnWidth = 25}), ui::grid({.columns = 3}),
                                 ui::grid({.tracks = {{.width = ui::Sizing::fixed(5)}, {}}, .minColumnWidth = 25})}})};
    std::vector<int> const grids = backend.all("Grid");
    REQUIRE(grids.size() == 3);
    CHECK(backend.prop(grids.at(0), "minColumnWidth") == "25");
    CHECK_FALSE(backend.hasProp(grids.at(1), "minColumnWidth"));
    CHECK_FALSE(backend.hasProp(grids.at(2), "minColumnWidth"));
}

TEST_CASE("ui::Mounted: a gap or padding reaches the backend as a spacing step in 0..5", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{
        runtime, backend,
        ui::column({.children = {ui::row({.children = {ui::text({.text = "a"})}, .gap = -3}),
                                 ui::grid({.columns = 1, .cells = {{.node = ui::text({.text = "b"})}}, .gap = 7}),
                                 ui::panel({.title = "c", .padding = 6, .child = ui::text({.text = "d"})}),
                                 ui::panel({.title = "e", .padding = 3, .child = ui::text({.text = "f"})})},
                    .gap = 99})};
    CHECK(backend.dump() ==
          "Column#1 gap=5\n"
          "  Row#2 gap=0\n"
          "    Text#3 role=Normal text=a\n"
          "  Grid#4 columns=1 gap=5\n"
          "    Text#5 role=Normal text=b\n"
          "  Panel#6 collapsible=false padding=5 title=c\n"
          "    Text#7 role=Normal text=d\n"
          "  Panel#8 collapsible=false padding=3 title=e\n"
          "    Text#9 role=Normal text=f\n");
}

TEST_CASE("ui::Mounted: a button's emphasis reaches the backend only when it is not Normal", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{
        runtime, backend,
        ui::row({.children = {ui::button({.label = "Plain"}),
                              ui::button({.label = "Go", .emphasis = ui::ButtonEmphasis::Primary}),
                              ui::button({.label = "Drop", .emphasis = ui::ButtonEmphasis::Danger}),
                              ui::button({.label = "Remove", .emphasis = ui::ButtonEmphasis::Ghost})}})};
    CHECK(backend.dump() ==
          "Row#1 gap=0\n"
          "  Button#2 label=Plain\n"
          "  Button#3 emphasis=Primary label=Go\n"
          "  Button#4 emphasis=Danger label=Drop\n"
          "  Button#5 emphasis=Ghost label=Remove\n");
}

// Mutation: skip checkLayout (every node below mounts).
TEST_CASE("ui::Mounted: bounds reach the backend, and the loader's bound rules hold for a C++ tree", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    auto const sized = [](ui::LayoutHints layout) {
        ui::Text text{.text = "x"};
        text.common.layout = layout;
        text.common.testId = "sized";
        return ui::text(std::move(text));
    };
    {
        ui::Mounted const view{runtime, backend,
                               sized({.width = ui::Sizing::stretch(), .maxWidth = 44, .minHeight = 1})};
        CHECK(backend.prop(backend.find("Text", "testId", "sized").value(), "layout") ==
              "stretch(1) max=44/content min=1");
    }
    for (ui::LayoutHints const& refused :
         {ui::LayoutHints{.minWidth = -1}, ui::LayoutHints{.maxHeight = -2},
          ui::LayoutHints{.minWidth = 50, .maxWidth = 40}, ui::LayoutHints{.minHeight = 5, .maxHeight = 4},
          ui::LayoutHints{.width = ui::Sizing::fixed(50), .maxWidth = 40},
          ui::LayoutHints{.height = ui::Sizing::fixed(2), .minHeight = 3}}) {
        CHECK_THROWS_WITH((ui::Mounted{runtime, backend, sized(refused)}),
                          Catch::Matchers::ContainsSubstring("on the node `sized`"));
    }
}

// Mutation: drop the checks in mountKind(KeyValue) (a width above stacked captions, or below 1, mounts).
TEST_CASE("ui::Mounted: a key-value refuses a caption width with captions above, or below 1", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::KeyValue const beside{.captionWidth = 12};
    {
        ui::Mounted const view{runtime, backend, ui::keyValue(beside)};
        CHECK(backend.find("KeyValue", "captionWidth", "12").has_value());
    }
    ui::KeyValue const above{.captions = ui::CaptionPlacement::Above, .captionWidth = 12};
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, ui::keyValue(above)}),
                      Catch::Matchers::ContainsSubstring("needs the captions beside their values"));
    ui::KeyValue const none{.captionWidth = 0};
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, ui::keyValue(none)}),
                      Catch::Matchers::ContainsSubstring("at least 1 unit"));
}

// Mutation: skip checkOverlayLayout in mountKind(Dialog)/mountKind(Drawer) (both still mount).
TEST_CASE("ui::Mounted: a dialog or drawer refuses a stretching width or height, for a C++ tree", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Dialog dialog{.open = true, .title = "D"};
    dialog.common.testId = "overlay";
    {
        ui::Mounted const view{runtime, backend, ui::dialog(dialog)};
        CHECK(backend.find("Dialog", "testId", "overlay").has_value());
    }
    ui::Dialog stretchingWidth = dialog;
    stretchingWidth.common.layout.width = ui::Sizing::stretch();
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, ui::dialog(stretchingWidth)}),
                      Catch::Matchers::ContainsSubstring("a dialog is sized by the frontend") &&
                          Catch::Matchers::ContainsSubstring("width cannot stretch") &&
                          Catch::Matchers::ContainsSubstring("node `overlay`"));
    ui::Dialog stretchingHeight = dialog;
    stretchingHeight.common.layout.height = ui::Sizing::stretch();
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, ui::dialog(stretchingHeight)}),
                      Catch::Matchers::ContainsSubstring("a dialog is sized by the frontend") &&
                          Catch::Matchers::ContainsSubstring("height cannot stretch"));
    ui::Drawer drawer{.open = true, .title = "R"};
    drawer.common.testId = "side";
    drawer.common.layout.width = ui::Sizing::stretch();
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, ui::drawer(drawer)}),
                      Catch::Matchers::ContainsSubstring("a drawer is sized by the frontend") &&
                          Catch::Matchers::ContainsSubstring("width cannot stretch") &&
                          Catch::Matchers::ContainsSubstring("node `side`"));
    // A fixed size, the lever this design actually provides, is unaffected.
    ui::Dialog fixedWidth = dialog;
    fixedWidth.common.layout.width = ui::Sizing::fixed(80);
    ui::Mounted const fixed{runtime, backend, ui::dialog(fixedWidth)};
    CHECK(backend.find("Dialog", "testId", "overlay").has_value());
}

// Mutation: never call setAlign/setJustify in mountStack and the grid's mountKind (the first dump loses them).
TEST_CASE("ui::Mounted: an alignment reaches the backend only when it is not the default", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Text placed{.text = "c"};
    placed.common.layout.alignSelf = ui::Align::End;
    {
        ui::Mounted const view{
            runtime, backend,
            ui::column(
                {.children = {ui::row({.children = {ui::text({.text = "a"})},
                                       .align = ui::Align::Center,
                                       .justify = ui::Justify::Between}),
                              ui::grid({.cells = {{.node = ui::text(std::move(placed))}}, .align = ui::Align::Start})},
                 .justify = ui::Justify::End})};
        CHECK(backend.dump() ==
              "Column#1 gap=0 justify=end\n"
              "  Row#2 align=center gap=0 justify=between\n"
              "    Text#3 role=Normal text=a\n"
              "  Grid#4 align=start columns=1 gap=0\n"
              "    Text#5 layout=content/content self=end role=Normal text=c\n");
    }
    // The default-only guard: a stack and a grid that say nothing of alignment record as they did without it.
    RecordingBackend plain;
    ui::Mounted const view{runtime, plain,
                           ui::column({.children = {ui::row({.children = {ui::text({.text = "a"})}}),
                                                    ui::grid({.cells = {{.node = ui::text({.text = "b"})}}})}})};
    CHECK(plain.dump() ==
          "Column#1 gap=0\n"
          "  Row#2 gap=0\n"
          "    Text#3 role=Normal text=a\n"
          "  Grid#4 columns=1 gap=0\n"
          "    Text#5 role=Normal text=b\n");
}

// Mutation: drop the setStyle call in the button's mountKind (the item records no style).
TEST_CASE("ui::Mounted: an item button records its style and its selection; a plain button neither", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> current{runtime, false};
    {
        ui::Mounted const view{
            runtime, backend,
            ui::column({.children = {ui::button({.label = "Plain"}),
                                     ui::button({.label = "Here", .style = ui::ButtonStyle::Item, .selected = true}),
                                     ui::button({.label = "There",
                                                 .style = ui::ButtonStyle::Item,
                                                 .selected = [&current] { return current.get(); }}),
                                     ui::button({.label = "Else", .style = ui::ButtonStyle::Item})}})};
        CHECK(backend.dump() ==
              "Column#1 gap=0\n"
              "  Button#2 label=Plain\n"
              "  Button#3 label=Here selected=true style=Item\n"
              "  Button#4 label=There selected=false style=Item\n"
              "  Button#5 label=Else style=Item\n");
        current.set(true);
        owner.runAll();
        CHECK(backend.prop(4, "selected") == "true");
    }
}

// Mutation: tell `selected` to an item only (the push button records none).
TEST_CASE("ui::Mounted: a push button with a bound selection records it and follows it", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> picked{runtime, true};
    ui::Mounted const view{runtime, backend,
                           ui::button({.label = "A1", .selected = [&picked] { return picked.get(); }})};
    CHECK(backend.dump() == "Button#1 label=A1 selected=true\n");
    picked.set(false);
    owner.runAll();
    CHECK(backend.prop(1, "selected") == "false");
}

// Mutation: skip checkPlacement (the panel's child mounts).
TEST_CASE("ui::Mounted: alignSelf outside a row, column or grid throws, as the loader refuses it", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    auto const placed = [] {
        ui::Text text{.text = "x"};
        text.common.layout.alignSelf = ui::Align::Center;
        text.common.testId = "placed";
        return ui::text(std::move(text));
    };
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, ui::panel({.child = placed()})}),
                      Catch::Matchers::ContainsSubstring("`alignSelf` places a node in its row, column or grid, "
                                                         "and the node `placed` is not in one"));
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, placed()}),
                      Catch::Matchers::ContainsSubstring("the node `placed` is not in one"));
    CHECK_NOTHROW(ui::Mounted{runtime, backend, ui::row({.children = {placed()}})});
}

// Mutation: skip checkShare (the percent child of a column mounts), or leave the row out of the share parents (the
// percent child of a row throws).
TEST_CASE("ui::Mounted: a percent width is a row's or a horizontal splitter's pane only, as the loader refuses it",
          "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    auto const shared = [](ui::LayoutHints layout) {
        ui::Text text{.text = "x"};
        text.common.layout = layout;
        text.common.testId = "shared";
        return ui::text(std::move(text));
    };
    ui::LayoutHints const share{.width = ui::Sizing::percent(30)};
    CHECK_NOTHROW(ui::Mounted{runtime, backend, ui::row({.children = {shared(share)}})});
    CHECK_NOTHROW(ui::Mounted{runtime, backend, ui::splitter({.children = {shared(share), shared({})}})});
    auto const refused =
        Catch::Matchers::ContainsSubstring("`percent` sizes a width along a row or a horizontal splitter");
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, ui::column({.children = {shared(share)}})}), refused);
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, ui::grid({.columns = 1, .cells = {{.node = shared(share)}}})}),
                      refused);
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, ui::row({.children = {shared(share)}, .wrap = true})}), refused);
    CHECK_THROWS_WITH(
        (ui::Mounted{runtime, backend,
                     ui::splitter({.children = {shared(share), shared({})}, .orientation = ui::Axis::Vertical})}),
        refused);
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, shared(share)}), refused);
    CHECK_THROWS_WITH(
        (ui::Mounted{runtime, backend, ui::row({.children = {shared({.height = ui::Sizing::percent(10)})}})}),
        Catch::Matchers::ContainsSubstring("a percent height"));
    CHECK_THROWS_WITH(
        (ui::Mounted{runtime, backend, ui::row({.children = {shared({.width = ui::Sizing::percent(101)})}})}),
        Catch::Matchers::ContainsSubstring("a percent width outside 1 to 100"));
    CHECK_THROWS_WITH(
        (ui::Mounted{runtime, backend, ui::row({.children = {shared({.width = ui::Sizing::percent(0)})}})}),
        Catch::Matchers::ContainsSubstring("a percent width outside 1 to 100"));
    ui::Dialog dialog{.open = true, .title = "D"};
    dialog.common.layout.width = ui::Sizing::percent(20);
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, ui::row({.children = {ui::dialog(dialog)}})}),
                      Catch::Matchers::ContainsSubstring("its width takes no share"));
    CHECK_THROWS_WITH(
        (ui::Mounted{runtime, backend,
                     ui::detail::makeNode(ui::Table{.columns = {{.label = "A", .width = ui::Sizing::percent(10)}}})}),
        Catch::Matchers::ContainsSubstring("a table column is sized by the table"));
    CHECK_THROWS_WITH(
        (ui::Mounted{runtime, backend, ui::grid({.tracks = {{.width = ui::Sizing::percent(10)}}, .cells = {}})}),
        Catch::Matchers::ContainsSubstring("a grid track is sized by the grid"));
    // The share reaches the backend as written.
    ui::Mounted const view{runtime, backend, ui::row({.children = {shared(share)}})};
    CHECK(backend.prop(backend.find("Text", "testId", "shared").value(), "layout") == "percent(30)/content");
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

// A submenu entry is reached by its path. Mutation: resolve the path one level only in itemAt (keep the first index).
TEST_CASE("ui::Mounted: a menu entry inside a submenu runs its own action", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Lines ran;
    ui::Mounted const view{
        runtime, backend,
        ui::menu({.items = {{.label = "File",
                             .items = {{.label = "Open", .onSelect = [&] { ran.emplace_back("open"); }},
                                       {.label = "Recent",
                                        .items = {{.label = "a.txt", .onSelect = [&] { ran.emplace_back("a"); }},
                                                  {.label = "b.txt", .onSelect = [&] { ran.emplace_back("b"); }}}}}},
                            {.label = "Quit", .onSelect = [&] { ran.emplace_back("quit"); }}}})};
    CHECK(backend.prop(1, "items") == "[File[Open,Recent[a.txt,b.txt]],Quit]");
    backend.chooseMenuEntry(1, {0, 1, 1});
    backend.chooseMenuEntry(1, {0, 0});
    backend.chooseMenuEntry(1, {0, 1, 2});  // out of range: nothing runs
    backend.chooseMenuEntry(1, {0, 1});     // opens the submenu: nothing runs
    CHECK(ran == Lines{"b", "open"});
}

// A hidden entry is not sent, and a path counts only what was sent. Mutation: map the path through every item, not
// only the shown ones (choosing the entry after the hidden one then runs the hidden one).
TEST_CASE("ui::Mounted: a hidden menu entry is not sent, and the shown one after it runs its own action", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> shown{runtime, false};
    Lines ran;
    ui::Mounted const view{runtime, backend,
                           ui::menu({.items = {{.label = "Undo", .onSelect = [&] { ran.emplace_back("undo"); }},
                                               {.label = "Redo",
                                                .onSelect = [&] { ran.emplace_back("redo"); },
                                                .visible = [&] { return shown.get(); }},
                                               {.label = "Cut", .onSelect = [&] { ran.emplace_back("cut"); }}}})};
    CHECK(backend.prop(1, "items") == "[Undo,Cut]");
    backend.chooseMenuEntry(1, {1});
    CHECK(ran == Lines{"cut"});
    shown.set(true);
    owner.runAll();
    CHECK(backend.prop(1, "items") == "[Undo,Redo,Cut]");
    backend.chooseMenuEntry(1, {1});
    CHECK(ran == Lines{"cut", "redo"});
}

// Mutation: send every separator (the line the hidden first entry leaves is then sent first).
TEST_CASE("ui::Mounted: a separator hidden entries leave at an end or beside another is not sent", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Lines ran;
    ui::Mounted const view{runtime, backend,
                           ui::menu({.items = {{.label = "Hidden", .visible = false},
                                               {.separator = true},
                                               {.label = "Print", .onSelect = [&] { ran.emplace_back("print"); }},
                                               {.separator = true},
                                               {.label = "Gone", .visible = false},
                                               {.separator = true},
                                               {.label = "Export", .description = std::string{"as PDF"}},
                                               {.separator = true}},
                                     .style = ui::MenuStyle::Button,
                                     .label = std::string{"More"}})};
    CHECK(backend.prop(1, "items") == "[Print,;separator,Export;description=as PDF]");
    CHECK(backend.prop(1, "style") == "Button");
    CHECK(backend.prop(1, "label") == "More");
    backend.chooseMenuEntry(1, {1});  // the separator: nothing runs
    backend.chooseMenuEntry(1, {0});
    CHECK(ran == Lines{"print"});
}

// Mutation: drop the default in the badge's mount (the control is then named by an empty label).
TEST_CASE("ui::Mounted: a removable badge names its control after its text, and removing it runs once", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> text{runtime, std::string{"Result"}};
    int removals = 0;
    ui::Mounted const view{
        runtime, backend,
        ui::column(
            {.children = {ui::badge({.text = [&] { return text.get(); }, .onRemove = [&removals] { ++removals; }}),
                          ui::badge({.text = std::string{"Plain"}})}})};
    CHECK(backend.prop(2, "removable") == "Remove Result");
    CHECK_FALSE(backend.hasProp(3, "removable"));
    text.set("Sample");
    owner.runAll();
    CHECK(backend.prop(2, "removable") == "Remove Sample");
    backend.dismiss(2);
    CHECK(removals == 1);
    CHECK_THROWS_AS(backend.dismiss(3), std::logic_error);
}

// Mutations: skip the enabled check (on the entry, or on an entry above it) in RecordingBackend::chooseMenuEntry.
TEST_CASE("ui::Mounted: a disabled menu entry, or one inside a disabled entry, runs nothing", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> enabled{runtime, false};
    Lines ran;
    ui::Mounted const view{
        runtime, backend,
        ui::menu({.items = {{.label = "Delete",
                             .onSelect = [&] { ran.emplace_back("delete"); },
                             .enabled = [&] { return enabled.get(); }},
                            {.label = "Export",
                             .enabled = [&] { return enabled.get(); },
                             .items = {{.label = "CSV", .onSelect = [&] { ran.emplace_back("csv"); }}}}}})};
    CHECK(backend.prop(1, "items") == "[Delete;enabled=false,Export;enabled=false[CSV]]");
    backend.chooseMenuEntry(1, {0});
    backend.chooseMenuEntry(1, {1, 0});
    CHECK(ran.empty());
    enabled.set(true);
    owner.runAll();
    CHECK(backend.prop(1, "items") == "[Delete,Export[CSV]]");
    backend.chooseMenuEntry(1, {0});
    backend.chooseMenuEntry(1, {1, 0});
    CHECK(ran == Lines{"delete", "csv"});
}

// Mutation: drop the `item->onSelect` check in the menu's dispatcher (the empty action then throws, and the probe
// sees a reported callback failure).
TEST_CASE("ui::Mounted: a menu entry without an action runs nothing when chosen", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{runtime, backend, ui::menu({.items = {{.label = "About"}}})};
    backend.chooseIndex(1, 0);
    CHECK(probe.count(ui::detail::site::kCallbackThrew) == 0);
}

// Mutation: stop anyBound at the top level (drop its recursion): a binding only a submenu entry has is then read
// once and never followed.
TEST_CASE("ui::Mounted: a binding inside a submenu updates the menu", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<int> count{runtime, 1};
    ui::Mounted const view{
        runtime, backend,
        ui::menu({.items = {{.label = "File",
                             .items = {{.label = [&] { return "Recent (" + std::to_string(count.get()) + ")"; }}}}}})};
    CHECK(backend.prop(1, "items") == "[File[Recent (1)]]");
    count.set(2);
    owner.runAll();
    CHECK(backend.prop(1, "items") == "[File[Recent (2)]]");
}

// The check mark is the binding's: choosing the entry runs its action, and only a change to the binding moves the
// mark. Icon and shortcut text are set once. Mutations: drop `checked` from entriesOf, or `checked.isBound()` from
// anyBound (the mark then never follows).
TEST_CASE("ui::Mounted: a checkable menu entry shows the mark its binding gives", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> wrap{runtime, false};
    ui::Mounted const view{runtime, backend,
                           ui::menu({.items = {{.label = "Wrap",
                                                .onSelect = [&] { wrap.set(!wrap.get()); },
                                                .checked = [&] { return std::optional<bool>{wrap.get()}; }},
                                               {.label = "Save", .icon = "save", .keys = "Ctrl+S"}}})};
    CHECK(backend.prop(1, "items") == "[Wrap;checked=false,Save;icon=save;keys=Ctrl+S]");
    backend.chooseIndex(1, 0);
    owner.runAll();
    CHECK(backend.prop(1, "items") == "[Wrap;checked=true,Save;icon=save;keys=Ctrl+S]");
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

// Mutations: in Mounter::applyCommon, apply `enabled` only when bound, or `dragKey` only when bound.
TEST_CASE("ui::Mounted: a constant disabled or draggable widget says so once", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{runtime, backend,
                           ui::text({.text = "t", .common = {.enabled = false, .dragKey = ui::Key{std::int64_t{4}}}})};
    CHECK(backend.prop(1, "enabled") == "false");
    CHECK(backend.prop(1, "dragKey") == "4");
    CHECK(runtime.core()->liveNodes() == 0);
}

// Mutation: in the Menu's dispatcher, call the entry without checking it has a handler or exists.
TEST_CASE("ui::Mounted: a menu entry without a handler, or past the last, runs nothing", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    int runs = 0;
    ui::Mounted const view{
        runtime, backend,
        ui::menu({.items = {{.label = "quiet"}, {.label = "loud", .onSelect = [&runs] { ++runs; }}}})};
    backend.chooseIndex(1, 0);
    backend.chooseIndex(1, 5);
    backend.chooseIndex(1, 1);
    owner.runAll();
    CHECK(runs == 1);
}

// Mutation: in Mounter's Tabs, treat a constant index as bound (no page is shown).
TEST_CASE("ui::Mounted: a constant Tabs index shows its page and leaves the user's pick on the bar", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    std::vector<std::size_t> picks;
    ui::Mounted const view{runtime, backend,
                           ui::tabs({.tabs = {{.label = "A", .node = ui::text({.text = "a"})},
                                              {.label = "B", .node = ui::text({.text = "b"})}},
                                     .selected = std::size_t{1},
                                     .onSelect = [&picks](std::size_t index) { picks.push_back(index); }})};
    CHECK(backend.dump() ==
          "Tabs#1 selected=1 tabs=[A,B]\n  Slot#2 layout=stretch(1)/stretch(1) scrollable=true\n    Text#3 "
          "role=Normal text=b\n");
    backend.chooseIndex(1, 0);
    owner.runAll();
    CHECK(picks == std::vector<std::size_t>{0});
    CHECK(backend.prop(1, "selected") == "0");  // not a slot: nothing re-asserts it
}

// Mutation: in Mounter::mountRows, dereference the model without checking it.
TEST_CASE("ui::Mounted: a ForEach without a model shows no rows", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{runtime, backend, ui::detail::makeNode(ui::ForEach{})};
    CHECK(backend.dump() == "Column#1 gap=0\n");
}

// Mutation: in Mounter::dismissFailed, call the dismissal without checking there is one.
TEST_CASE("ui::Mounted: a Dialog without onDismiss whose content fails stays closed", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    EchoingBackend backend;
    std::optional<int> spare;
    backend.setOnCreate(morph::testing::failWhenSpent(spare));
    Signal<bool> open{runtime, false};
    ui::Mounted const view{runtime, backend,
                           ui::dialog({.open = [&open] { return open.get(); }, .child = ui::text({.text = "x"})})};
    spare = 0;
    open.set(true);
    owner.runAll();
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
    CHECK(backend.recording().prop(1, "open") == "false");
}

// The first of several row failures in one run is the one reported. Mutation: in Mounter::reconcile, keep the last
// failure instead of the first.
TEST_CASE("ui::Mounted: two rows that fail in one run are one report, and neither is left behind", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    EchoingBackend backend;
    std::optional<int> spare;
    backend.setOnCreate(morph::testing::failWhenSpent(spare));
    Signal<std::vector<std::int64_t>> rows{runtime, std::vector<std::int64_t>{}};
    int builds = 0;
    ui::Mounted const view{runtime, backend,
                           ui::forEach<std::int64_t>(
                               rows, [](std::int64_t row) { return ui::Key{row}; },
                               [&builds](Signal<std::int64_t> const& row) {
                                   if (++builds > 2) {
                                       throw std::runtime_error{"row " + std::to_string(row.peek())};
                                   }
                                   return ui::text({.text = "r"});
                               })};
    builds = 2;  // every row view from here on throws
    rows.set({1, 2});
    owner.runAll();
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
    CHECK(backend.recording().dump() == "Column#1 gap=0\n");
}

// Mutation: drop the text's description or label hint check (a described or hinted text then mounts), or skip the
// mount of `labelHint` (the input records no `captionHint`).
TEST_CASE(
    "ui::Mounted: a C++ tree's text takes no description, a push button no content, and a text area "
    "no emphasis",
    "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Text described{.text = "x"};
    described.field.description = "a line";
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, ui::text(described)}),
                      Catch::Matchers::ContainsSubstring("a text is not described"));
    ui::Text hinted{.text = "x"};
    hinted.field.labelHint = "a hint";
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, ui::text(hinted)}),
                      Catch::Matchers::ContainsSubstring("a text has no caption to hint after"));
    ui::Button push{.label = "Go"};
    push.child = ui::text({.text = "inside"});
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, ui::button(push)}),
                      Catch::Matchers::ContainsSubstring("a push button has a label"));
    ui::Button entry = push;
    entry.style = ui::ButtonStyle::Item;
    {
        ui::Mounted const view{runtime, backend, ui::button(entry)};
        CHECK(backend.find("Text", "text", "inside").has_value());
    }
    ui::TextInput area{.mode = ui::TextInputMode::Multiline};
    area.emphasis = ui::InputEmphasis::Prominent;
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, ui::textInput(area)}),
                      Catch::Matchers::ContainsSubstring("a multiline text input's size is its own"));
    ui::TextInput focal{};
    focal.emphasis = ui::InputEmphasis::Prominent;
    ui::Mounted const view{runtime, backend, ui::textInput(focal)};
    CHECK(backend.find("TextInput", "emphasis", "Prominent").has_value());
    ui::TextInput captioned{};
    captioned.field.labelHint = "range 2-8";
    ui::Mounted const hintedView{runtime, backend, ui::textInput(captioned)};
    CHECK(backend.find("TextInput", "captionHint", "range 2-8").has_value());
}

// Mutation: skip the tone in the mount (the text records none), or the spans (the run's tone is not recorded).
TEST_CASE("ui::Mounted: a text's tone and its runs' tones reach the backend", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Text toned{.text = "7", .role = ui::TextRole::Display};
    toned.field.tone = ui::Tone::Err;
    ui::Mounted const text{runtime, backend, ui::text(toned)};
    CHECK(backend.prop(backend.find("Text", "role", "Display").value(), "tone") == "Err");
    ui::Text const runs{.spans = std::vector<ui::TextSpan>{{.text = "Past due "},
                                                           {.text = "7", .strong = true, .tone = ui::Tone::Err}}};
    ui::Mounted const withRuns{runtime, backend, ui::text(runs)};
    CHECK(backend.dump().contains("spans=[Past due ,7;strong;tone=Err]"));
}

// Mutation: never call setWrap in mountStack (the log loses it), or call it after the children are mounted.
TEST_CASE("ui::Mounted: a wrapping row records wrap before its first child, and a plain row records nothing", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    {
        ui::Mounted const view{
            runtime, backend, ui::row({.children = {ui::text({.text = "a"}), ui::text({.text = "b"})}, .wrap = true})};
        CHECK(backend.dump() ==
              "Row#1 gap=0 wrap=true\n"
              "  Text#2 role=Normal text=a\n"
              "  Text#3 role=Normal text=b\n");
        auto const& log = backend.log();
        auto const wrapped = std::ranges::find(log, "set Row#1 wrap=true");
        auto const firstChild =
            std::ranges::find_if(log, [](std::string const& entry) { return entry.starts_with("create Text"); });
        REQUIRE(wrapped != log.end());
        REQUIRE(firstChild != log.end());
        CHECK(wrapped < firstChild);
    }
    RecordingBackend plain;
    ui::Mounted const view{runtime, plain, ui::row({.children = {ui::text({.text = "a"})}})};
    CHECK(plain.dump() ==
          "Row#1 gap=0\n"
          "  Text#2 role=Normal text=a\n");
}

// Mutation: never call setDivided in mountStack (the log loses it), or call it after the children are mounted.
TEST_CASE("ui::Mounted: a divided column records divided before its first child, and a plain one records nothing",
          "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    {
        ui::Mounted const view{
            runtime, backend,
            ui::column({.children = {ui::text({.text = "a"}), ui::text({.text = "b"})}, .divided = true})};
        CHECK(backend.dump() ==
              "Column#1 divided=true gap=0\n"
              "  Text#2 role=Normal text=a\n"
              "  Text#3 role=Normal text=b\n");
        auto const& log = backend.log();
        auto const divided = std::ranges::find(log, "set Column#1 divided=true");
        auto const firstChild =
            std::ranges::find_if(log, [](std::string const& entry) { return entry.starts_with("create Text"); });
        REQUIRE(divided != log.end());
        REQUIRE(firstChild != log.end());
        CHECK(divided < firstChild);
    }
    RecordingBackend plain;
    ui::Mounted const view{runtime, plain, ui::column({.children = {ui::text({.text = "a"})}})};
    CHECK(plain.dump() ==
          "Column#1 gap=0\n"
          "  Text#2 role=Normal text=a\n");
}

TEST_CASE("ui::Mounted: a stack's padding reaches the backend as one step or as a step for each axis", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{
        runtime, backend,
        ui::column({.children = {ui::column({.padding = 3}), ui::row({.padding = ui::Padding{2, 4}}),
                                 ui::row({.padding = ui::Padding{9, 1}}), ui::column({})}})};
    CHECK(backend.dump() ==
          "Column#1 gap=0\n"
          "  Column#2 gap=0 padding=3\n"
          "  Row#3 gap=0 padding=2/4\n"
          "  Row#4 gap=0 padding=5/1\n"
          "  Column#5 gap=0\n");
}

TEST_CASE("ui::Mounted: a stack's selected condition reaches the backend at mount and on every change", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> chosen{runtime, false};
    ui::Mounted const view{runtime, backend,
                           ui::column({.children = {ui::column({.selected = [&chosen] { return chosen.get(); }}),
                                                    ui::row({}), ui::row({.selected = true})}})};
    CHECK(backend.dump() ==
          "Column#1 gap=0\n"
          "  Column#2 gap=0 selected=false\n"
          "  Row#3 gap=0\n"
          "  Row#4 gap=0 selected=true\n");
    chosen.set(true);
    owner.runAll();
    CHECK(backend.dump().contains("Column#2 gap=0 selected=true"));
}

// Mutation: never tell the banner its style (the dump has no actionStyle), or tell a ghost banner it too.
TEST_CASE("ui::Mounted: a banner is told its action style only when it is not the default", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{
        runtime, backend,
        ui::column({.children = {
                        ui::banner({.text = "a", .actionStyle = ui::BannerActionStyle::Link}),
                        ui::banner({.text = "b"}), ui::banner({.text = "c", .actionStretch = true}),
                        ui::row({.children = {ui::text({.text = "x"}), ui::text({.text = "y"})}, .divided = true})}})};
    std::string const dump = backend.dump();
    CHECK(dump.contains("Banner#2 actionLabel= actionStretch=false actionStyle=link"));
    CHECK_FALSE(dump.contains("Banner#3 actionStretch"));
    CHECK(dump.contains("Banner#4 actionLabel= actionStretch=true actionStyle=ghost"));
    CHECK(dump.contains("Row#5 divided=true"));
}

namespace {

// A horizontal ForEach of two buttons whose own gap is 0.
ui::Node actionRows(Runtime& runtime) {
    static_cast<void>(runtime);
    return ui::forEach<std::int64_t>(
        [] { return std::vector<std::int64_t>{1, 2}; }, [](std::int64_t row) { return ui::Key{row}; },
        [](Signal<std::int64_t> const& row) { return ui::button({.label = std::to_string(row.peek())}); },
        ui::Axis::Horizontal, 0);
}

// The ids of the stacks the recording backend holds, with the property set on them.
std::vector<int> actionRowsOf(RecordingBackend const& backend) {
    std::vector<int> found;
    for (int const row : backend.all("Row")) {
        if (backend.prop(row, "actionRow") == "true") {
            found.push_back(row);
        }
    }
    return found;
}

}  // namespace

// Mutation: mount the bar without `setWrap`, or leave out the ForEach's action-row marking.
TEST_CASE("ui::Mounted: a wrapping action bar wraps, and its direct horizontal ForEach is part of the row", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::ActionBar bar{.children = {actionRows(runtime)}, .align = ui::Align::Center, .justify = ui::Justify::Start};
    bar.wrap = true;
    ui::Mounted const view{runtime, backend, ui::actionBar(bar)};
    std::vector<int> const rows = actionRowsOf(backend);
    REQUIRE(rows.size() == 2);
    CHECK(backend.prop(rows.front(), "wrap") == "true");
    CHECK(backend.prop(rows.front(), "gap") == "2");
    CHECK(backend.prop(rows.back(), "gap") == "2");  // the ForEach said 0
    CHECK(backend.prop(rows.back(), "wrap") != "true");
}

// Mutation: a plain Row's ForEach gets the marking, or an ActionBar's non-wrapping row gets `wrap`.
TEST_CASE("ui::Mounted: the same ForEach in a plain row keeps its own gap, and a bar without wrap does not wrap",
          "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    {
        ui::Mounted const view{runtime, backend, ui::row(ui::Row{.children = {actionRows(runtime)}})};
        CHECK(actionRowsOf(backend).empty());
        std::vector<int> const rows = backend.all("Row");
        REQUIRE(rows.size() == 2);
        CHECK(backend.prop(rows.back(), "gap") == "0");
    }
    RecordingBackend plain;
    ui::Mounted const view{runtime, plain, ui::actionBar(ui::ActionBar{.children = {actionRows(runtime)}})};
    std::vector<int> const rows = actionRowsOf(plain);
    REQUIRE(rows.size() == 2);
    CHECK(plain.prop(rows.front(), "wrap") != "true");
}

// Mutation: the check becomes "any ActionBar is mounting" instead of "my parent is that bar".
TEST_CASE("ui::Mounted: a ForEach in a row in an action bar is not part of the bar's row", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Node const nested = ui::row(ui::Row{.children = {actionRows(runtime)}});
    ui::Mounted const view{runtime, backend, ui::actionBar(ui::ActionBar{.children = {nested}})};
    std::vector<int> const rows = actionRowsOf(backend);
    CHECK(rows.size() == 1);  // the bar alone: the ForEach's parent is the row, not the bar
}

// Mutation: drop the footer refusal (the wrapping bar mounts in the dialog's footer).
TEST_CASE("ui::Mounted: a wrapping action bar in a dialog's content is refused, and mounts outside it", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::ActionBar bar{.children = {ui::button({.label = "Go"})}};
    bar.wrap = true;
    ui::Dialog const dialog{.open = true, .title = "D", .child = ui::actionBar(bar)};
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, ui::dialog(dialog)}),
                      Catch::Matchers::ContainsSubstring("an overlay's footer is one row of actions"));
    ui::Mounted const outside{runtime, backend, ui::actionBar(bar)};
    CHECK(actionRowsOf(backend).size() == 1);
}

// Mutation: drop the multiline check (the centered multiline input mounts), or refuse the single-line one.
TEST_CASE("ui::Mounted: a multiline text input's text starts at the leading edge", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::TextInput input{.mode = ui::TextInputMode::Multiline};
    input.align = ui::Align::Center;
    CHECK_THROWS_WITH((ui::Mounted{runtime, backend, ui::textInput(std::move(input))}),
                      Catch::Matchers::ContainsSubstring("a multiline text input's text starts at the leading edge"));
    ui::TextInput single{.mode = ui::TextInputMode::SingleLine};
    single.align = ui::Align::Center;
    CHECK_NOTHROW(ui::Mounted{runtime, backend, ui::textInput(std::move(single))});
}

// Mutation: drop the binding of a constant label that differs from the default, or send the default as well.
TEST_CASE("ui::Mounted: a tab bar's overflow label reaches the backend when it is not the default", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    {
        RecordingBackend backend;
        ui::Tabs tabs{.tabs = {{.label = "A", .node = ui::text({.text = "a"})}}};
        tabs.moreLabel = std::string{"Others"};
        ui::Mounted const view{runtime, backend, ui::tabs(std::move(tabs))};
        CHECK(backend.dump().contains("moreLabel=Others"));
    }
    {
        RecordingBackend backend;
        ui::Mounted const view{runtime, backend,
                               ui::tabs({.tabs = {{.label = "A", .node = ui::text({.text = "a"})}}})};
        CHECK_FALSE(backend.dump().contains("moreLabel"));
    }
    {
        RecordingBackend backend;
        Signal<std::string> label{runtime, "Elsewhere"};
        ui::Tabs tabs{.tabs = {{.label = "A", .node = ui::text({.text = "a"})}}};
        tabs.moreLabel = [&label] { return label.get(); };
        ui::Mounted const view{runtime, backend, ui::tabs(std::move(tabs))};
        CHECK(backend.dump().contains("moreLabel=Elsewhere"));
        label.set("Else");
        owner.runAll();
        CHECK(backend.dump().contains("moreLabel=Else"));
    }
}

// ── The column window ───────────────────────────────────────────────────────

namespace {

using ui::CellRef;
using ui::ColumnExtents;
using ui::FocusMove;
using ui::Key;
using ui::MountStats;
using ui::TableArrangement;
using ui::Value;
using ui::Virtualization;
using ui::windowingOf;
namespace table = morph::table;

using morph::testing::FakeRows;

struct WindowOptions {
    std::size_t pinned = 0;
    std::vector<std::size_t> custom;
};

// The text of a record's field, or nothing.
std::string fieldText(Value const& record, std::string const& field) {
    Value const* const value = record.field(field);
    return value != nullptr && value->asString() != nullptr ? *value->asString() : std::string{};
}

// One table over `FakeRows` on a recording backend: every column 100 wide with the default text cell, but the
// `custom` ones, which have a cell of their own; a viewport of 30 rows by 2,000 (twenty columns), reported at once.
struct WindowFixture {
    WindowFixture(std::int64_t rowCount, std::size_t columnCount, WindowOptions const& options = {})
        : rows{std::make_shared<FakeRows>(rowCount, columnCount, runtime.owner())},
          arrangement{runtime, TableArrangement{.visible = {}, .pinned = options.pinned, .widths = {}}},
          focus{runtime, std::nullopt},
          highlight{runtime, std::nullopt} {
        std::vector<ui::TableColumn> columns;
        for (std::size_t column = 0; column < columnCount; ++column) {
            columns.push_back(ui::TableColumn{.id = "c" + std::to_string(column),
                                              .label = "C" + std::to_string(column),
                                              .width = ui::Sizing::fixed(100)});
        }
        ui::TableSource source;
        source.rows = rows;
        source.ownCell.assign(columnCount, false);
        for (std::size_t const column : options.custom) {
            source.ownCell.at(column) = true;
        }
        source.defaultCellView = [](Signal<std::size_t> const& column, Signal<Value> const& row) {
            return ui::text(
                {.text = [&column, &row] { return fieldText(row.get(), "c" + std::to_string(column.get())); }});
        };
        source.cellView = [this](std::size_t column, Signal<Value> const& row) {
            ++cellViewCalls;
            if (throwing) {
                throw std::runtime_error{"the cell does not build"};
            }
            return ui::text({.text = [&row, column] { return fieldText(row.get(), "c" + std::to_string(column)); }});
        };
        ui::TableOptions tableOptions;
        tableOptions.arrangement = [this] { return arrangement.get(); };
        tableOptions.focus = [this] { return focus.get(); };
        tableOptions.highlight = [this] { return highlight.get(); };
        tableOptions.onFocusCell = [this](std::optional<CellRef> cell) {
            if (onFocusCell) {
                onFocusCell(std::move(cell));
            }
        };
        view = std::make_unique<ui::Mounted>(runtime, backend,
                                             ui::table(std::move(columns), std::move(source), std::move(tableOptions)));
        table = backend.all("Table").front();
        scrollColumns(0);
        scrollRows(0);
    }

    void settle() {
        while (owner.runAll() > 0) {
        }
    }

    void scrollRows(std::size_t first) {
        firstRow = first;
        backend.scrollTable(table, first, 30);
        settle();
    }

    void scrollColumns(int scrollX) {
        backend.scrollTableColumns(table, scrollX, 2000);
        settle();
    }

    // Scrolls down over @p n rows a viewport at a time, settling one owner turn at a time, and keeps the largest
    // number of cells built or rebound in one turn.
    void flingRows(std::size_t n) {
        auto const work = [this] { return stats().cellsRebound + stats().cellsCreated; };
        for (std::size_t moved = 0; moved < n; moved += 30) {
            std::size_t before = work();
            firstRow += 30;
            backend.scrollTable(table, firstRow, 30);
            maxRebindsPerTurn = std::max(maxRebindsPerTurn, work() - before);
            for (;;) {
                before = work();
                if (!owner.runOne()) {
                    break;
                }
                maxRebindsPerTurn = std::max(maxRebindsPerTurn, work() - before);
            }
        }
    }

    [[nodiscard]] MountStats const& stats() const { return view->stats(); }
    [[nodiscard]] std::size_t realizedRows() const { return backend.children(table).size(); }

    [[nodiscard]] std::size_t cellsAlive() const {
        std::size_t alive = 0;
        for (int const row : backend.children(table)) {
            for (int const cell : backend.children(row)) {
                static_cast<void>(cell);
                ++alive;
            }
        }
        return alive;
    }

    [[nodiscard]] std::size_t headerCellsAlive() { return backend.headerCells(table); }

    [[nodiscard]] std::optional<int> cellOf(Key const& key, std::size_t column) const {
        for (int const row : backend.children(table)) {
            if (backend.prop(row, "rowKey") != ui::testing::detail::formatKey(key)) {
                continue;
            }
            for (int const cell : backend.children(row)) {
                if (backend.prop(cell, "column") == std::to_string(column)) {
                    return cell;
                }
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] ColumnExtents const& extents() const { return *view->columnExtents(); }

    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    std::shared_ptr<FakeRows> rows;
    Signal<TableArrangement> arrangement;
    Signal<std::optional<CellRef>> focus;
    Signal<std::optional<ui::TableHighlight>> highlight;
    std::function<void(std::optional<CellRef>)> onFocusCell;
    std::size_t cellViewCalls = 0;
    // Every own cell's build throws while set.
    bool throwing = false;
    std::size_t maxRebindsPerTurn = 0;
    std::size_t firstRow = 0;
    std::unique_ptr<ui::Mounted> view;
    int table = 0;
};

// The indices 0 to n - 1 without @p left.
std::vector<std::size_t> allColumnsBut(std::size_t n, std::size_t left) {
    std::vector<std::size_t> out;
    for (std::size_t column = 0; column < n; ++column) {
        if (column != left) {
            out.push_back(column);
        }
    }
    return out;
}

}  // namespace

// Mutation: `windowingOf` leaving `cellThreshold` out of the columns (the 1,000 × 30 line's `.columns` is false).
TEST_CASE("windowing: the three thresholds", "[ui][mount]") {
    Virtualization const v{};
    CHECK(windowingOf(v, 1999, 300, false).rows);
    CHECK(windowingOf(v, 1999, 300, false).columns);
    CHECK_FALSE(windowingOf(v, 1999, 10, false).rows);
    CHECK_FALSE(windowingOf(v, 1999, 10, false).columns);
    CHECK(windowingOf(v, 2000, 10, false).rows);
    CHECK_FALSE(windowingOf(v, 2000, 10, false).columns);
    CHECK(windowingOf(v, 100, 41, false).columns);
    CHECK_FALSE(windowingOf(v, 100, 41, false).rows);
    CHECK_FALSE(windowingOf(v, 100, 40, false).columns);
    CHECK(windowingOf(v, 10, 10, true).rows);
    CHECK_FALSE(windowingOf(v, 10, 10, true).columns);
    CHECK(windowingOf(v, 1000, 30, false).columns);  // 30,000 cells in 30 columns: by cellThreshold alone
    CHECK_FALSE(windowingOf(v, 600, 30, false).columns);
}

// Mutation: the column window never on (`windowColumns` is all 300).  Gate F2.
TEST_CASE("table window: 1,999 × 300 realizes only the windows", "[ui][mount]") {
    WindowFixture f{1'999, 300, {.pinned = 2}};
    f.scrollRows(500);
    f.scrollColumns(10'000);  // columns 100 to 119 in view
    CHECK(f.realizedRows() == 50);  // 30 and overscan 10 each side
    CHECK(f.backend.windowColumns(f.table).size() == 2 + 20 + 2 * 4);
    CHECK(f.cellsAlive() == f.realizedRows() * f.backend.windowColumns(f.table).size());
    CHECK(f.headerCellsAlive() == f.backend.windowColumns(f.table).size());
    CHECK(f.backend.totalWidth(f.table) == 300 * 100);
    CHECK(f.rows->lastVisible.columns.size() == 30);  // the window's columns go to `visible`
}

// Mutations: a leaving row's slot not reused (cellsCreated grows on the second fling); no frame
// budget (maxRebindsPerTurn is 720, thirty rows of twenty-four cells).  Gate F8.
TEST_CASE("table window: a fling creates no cell once the pools are warm", "[ui][mount]") {
    WindowFixture f{100'000, 300, {.pinned = 2}};
    f.flingRows(2'000);
    auto const created = f.stats().cellsCreated;
    f.flingRows(2'000);
    CHECK(f.stats().cellsCreated == created);
    CHECK(f.maxRebindsPerTurn <= 240);
    CHECK(f.stats().placeholders > 0);  // the budget left cells blank for a turn
}

// Mutation: rebinding a cell by building it again through cellView (cellViewCalls grows on the second fling).
TEST_CASE("table window: a pooled cell is rebound by its row signal", "[ui][mount]") {
    WindowFixture f{100'000, 300, {.pinned = 2, .custom = {5}}};
    f.flingRows(2'000);
    auto const views = f.cellViewCalls;
    auto const rebound = f.stats().cellsRebound;
    f.flingRows(2'000);
    CHECK(f.cellViewCalls == views);
    CHECK(f.stats().cellsRebound > rebound);
    f.scrollRows(40'000);
    f.settle();
    REQUIRE(f.cellOf(Key{std::int64_t{40'000}}, 5).has_value());
    CHECK(f.backend.prop(*f.cellOf(Key{std::int64_t{40'000}}, 5), "text") == "r40000c5");  // rebound to its new row
}

// Mutation: no anchoring (`horizontalOffset` is never set).
TEST_CASE("table window: a column growing left of the view keeps the columns in view where they are", "[ui][mount]") {
    WindowFixture f{1'999, 300, {.pinned = 0}};
    f.scrollColumns(10'000);  // column 100 is the first in view
    f.backend.measureColumn(f.table, 98, 160);
    f.settle();  // an overscan column grows by 60
    CHECK(f.backend.horizontalOffset(f.table) == 10'060);
    CHECK(f.extents().offsetOf(100) == 10'060);
    f.backend.measureColumn(f.table, 98, 120);
    f.settle();  // narrower: a width only grows
    CHECK(f.extents().widthOf(98) == 160);
    CHECK(f.backend.horizontalOffset(f.table) == 10'060);
    f.backend.measureColumn(f.table, 110, 180);
    f.settle();  // inside the view: no anchoring
    CHECK(f.backend.horizontalOffset(f.table) == 10'060);
}

// Mutations: pinned columns dropped from the window when scrolled away; no display unpinning (`pinnedShown` 12);
// the pins not given back when the viewport widens (`pinnedShown` stays 10).
TEST_CASE("table window: pinned columns stay realized, and a narrow table unpins for display only", "[ui][mount]") {
    WindowFixture f{1'999, 300, {.pinned = 2}};
    f.scrollColumns(20'000);
    std::vector<std::size_t> const window = f.backend.windowColumns(f.table);
    CHECK(window.at(0) == 0);
    CHECK(window.at(1) == 1);  // still the first two
    CHECK(f.cellOf(Key{std::int64_t{0}}, 1).has_value());
    TableArrangement wide = f.arrangement.peek();
    wide.pinned = 12;  // 1,200 of a 2,000 viewport: over half
    f.arrangement.set(wide);
    f.settle();
    CHECK(f.backend.pinnedShown(f.table) == 10);  // the last two scroll with the rest
    CHECK(f.arrangement.peek().pinned == 12);  // display only
    f.backend.scrollTableColumns(f.table, 20'000, 4'000);  // 1,200 of 4,000: the pins come back
    f.settle();
    CHECK(f.backend.pinnedShown(f.table) == 12);
}

// Mutation: a pass that reads every column's width (columnReads grows by 300 a pass).  Gate F9.
TEST_CASE("table window: a pass reads no width outside the column window", "[ui][mount]") {
    WindowFixture f{1'999, 300, {.pinned = 2}};
    f.scrollColumns(10'000);
    std::size_t const before = f.extents().columnReads();
    for (int x = 10'000; x < 12'000; x += 100) {
        f.scrollColumns(x);  // twenty passes, one column each
    }
    std::size_t const window = 2 + 20 + 2 * 4;
    CHECK(f.extents().columnReads() - before <= 20 * window);  // the window's columns, per pass
}

// Mutations: the rows rebuilt on a layout change (cellsCreated grows); a hide that pools nothing (the destroyed
// cell, still among its row's children, makes the next pass throw).  Gate D8.
TEST_CASE("table window: hiding and moving a column create no cell widgets", "[ui][mount]") {
    WindowFixture f{1'999, 300, {.pinned = 2}};
    auto const before = f.stats().cellsCreated;
    std::size_t const alive = f.cellsAlive();
    TableArrangement hidden = f.arrangement.peek();
    hidden.visible = allColumnsBut(300, 5);  // column 5 was in the window
    f.arrangement.set(hidden);
    f.settle();
    CHECK(f.stats().cellsCreated == before);
    CHECK(f.cellsAlive() == alive);  // the entering column took the pooled cells
    CHECK_FALSE(f.cellOf(Key{std::int64_t{0}}, 5).has_value());
    CHECK(f.headerCellsAlive() == f.backend.windowColumns(f.table).size());
    TableArrangement moved = hidden;
    std::swap(moved.visible.at(3), moved.visible.at(4));
    f.arrangement.set(moved);
    f.settle();
    CHECK(f.stats().cellsCreated == before);
    REQUIRE(f.cellOf(Key{std::int64_t{0}}, 3).has_value());
    CHECK(f.backend.prop(*f.cellOf(Key{std::int64_t{0}}, 3), "text") == "r0c3");  // every cell still its own
    CHECK(f.rows->lastVisible.first == 0);  // rows and their order untouched
}

// Mutations: the focused row not kept realized (the focused cell is gone after the scroll); the focused row counted
// in `setRealizedWindow`'s contiguous range (`9990+51`).  Gate F10.
TEST_CASE("table focus: the focused cell survives a scroll of 10,000 rows and 200 columns", "[ui][mount]") {
    WindowFixture f{100'000, 300, {.pinned = 2}};
    f.focus.set(CellRef{.key = Key{std::int64_t{7}}, .column = "c5"});
    f.settle();
    f.scrollRows(10'000);
    f.scrollColumns(20'000);
    REQUIRE(f.cellOf(Key{std::int64_t{7}}, 5).has_value());  // its row and its column stay realized
    CHECK(f.backend.focusCell(f.table) == std::pair{std::size_t{7}, std::size_t{5}});
    CHECK(f.realizedRows() == 51);  // one row beyond the window at most
    CHECK(f.backend.prop(f.table, "realizedWindow") == "9990+50/100000");  // the contiguous rows; row 7 apart
    CHECK(f.backend.lastCounts(f.table) == std::pair{std::optional<std::int64_t>{100'000}, std::size_t{300}});
}

// Mutation: a slot's index told only when it is built (a reused slot keeps the view row of the row it showed before).
TEST_CASE("table window: every realized row tells the backend its view row, the focused one beyond the window too",
          "[ui][mount]") {
    WindowFixture f{100'000, 300, {.pinned = 0}};
    f.focus.set(CellRef{.key = Key{std::int64_t{7}}, .column = "c5"});
    f.settle();
    f.scrollRows(10'000);
    std::vector<std::string> told;
    for (int const row : f.backend.children(f.table)) {
        told.push_back(f.backend.prop(row, "viewRow"));
    }
    REQUIRE(told.size() == 51);
    CHECK(told.front() == "7");   // the focused row, before the window
    CHECK(told.at(1) == "9990");  // the window: the rows in view and `overscan` above them
    CHECK(told.back() == "10039");
}

// Mutation: `Last` clamped to the rows held (focus stays on row 199).
TEST_CASE("table focus: Ctrl+End over rows not fetched lands on the last row when it arrives", "[ui][mount]") {
    WindowFixture f{100'000, 300, {.pinned = 0}};
    std::vector<std::optional<CellRef>> reported;
    f.onFocusCell = [&reported](std::optional<CellRef> cell) { reported.push_back(std::move(cell)); };
    f.focus.set(CellRef{.key = Key{std::int64_t{0}}, .column = "c0"});
    f.settle();
    f.rows->holdFrom(200);
    f.backend.focusMove(f.table, FocusMove::Last);
    f.settle();
    CHECK(f.backend.focusCell(f.table).first == 99'999);  // the placeholder holds focus
    CHECK(f.rows->lastVisible.first + f.rows->lastVisible.count >= 100'000);  // and is asked for
    f.rows->arrive();
    f.settle();
    REQUIRE_FALSE(reported.empty());
    CHECK(reported.back() == CellRef{.key = Key{std::int64_t{99'999}}, .column = "c0"});
}

// Mutations: the realized row count reported (50); a lower bound reported as exact.
TEST_CASE("table focus: the reported row count is the total, or unknown for a lower bound", "[ui][mount]") {
    WindowFixture f{9'093, 300, {.pinned = 0}};
    CHECK(f.backend.lastCounts(f.table) == std::pair{std::optional<std::int64_t>{9'093}, std::size_t{300}});
    f.rows->setTotal(100'000, table::TotalKind::AtLeast);
    f.settle();
    CHECK(f.backend.lastCounts(f.table) == std::pair{std::optional<std::int64_t>{}, std::size_t{300}});
    TableArrangement fewer = f.arrangement.peek();
    fewer.visible = allColumnsBut(300, 5);
    f.arrangement.set(fewer);
    f.settle();
    CHECK(f.backend.lastCounts(f.table).second == 299);  // the visible columns
    f.rows->holdFrom(200);
    f.focus.set(CellRef{.key = Key{std::int64_t{0}}, .column = "c0"});
    f.settle();
    f.backend.focusMove(f.table, FocusMove::Last);
    f.settle();
    CHECK(f.backend.focusCell(f.table).first == 199);  // AtLeast: the last row known
}

// Mutation: the column window ignoring `cellThreshold` (thirty columns are under `columnThreshold`, so all 30 are in
// the window).  Gate F2.
TEST_CASE("table window: 1,000 × 30 is windowed by its cells alone", "[ui][mount]") {
    WindowFixture f{1'000, 30};
    CHECK(f.backend.windowColumns(f.table).size() == 20 + 4);  // twenty in view, four overscan on the right
    CHECK(f.cellsAlive() == f.realizedRows() * 24);
}

// Mutations: a reset followed by view index (focus moves to row 7's place and is reported; the mark stays on 12);
// focus taken by the row now at its index although its key is still in the view.
TEST_CASE("table focus: a new order finds focus and the highlight by key", "[ui][mount]") {
    WindowFixture f{100'000, 30};
    std::vector<std::optional<CellRef>> reported;
    f.onFocusCell = [&reported](std::optional<CellRef> cell) { reported.push_back(std::move(cell)); };
    f.focus.set(CellRef{.key = Key{std::int64_t{7}}, .column = "c3"});
    f.highlight.set(ui::TableHighlight{.key = Key{std::int64_t{12}}, .row = 12});
    f.settle();
    REQUIRE(f.backend.focusCell(f.table) == std::pair{std::size_t{7}, std::size_t{3}});
    REQUIRE(f.backend.prop(f.table, "highlightedRow") == "12");
    // Every row one place down: key k at view index k + 1.
    f.rows->reorder([](std::int64_t index) { return index == 0 ? std::int64_t{99'999} : index - 1; });
    f.settle();
    CHECK(f.backend.focusCell(f.table) == std::pair{std::size_t{8}, std::size_t{3}});
    CHECK(f.backend.prop(f.table, "highlightedRow") == "13");
    CHECK(reported.empty());  // the user moved nothing
    // Reversed: keys 7 and 12 are far from the rows realized.
    f.rows->reorder([](std::int64_t index) { return 99'999 - index; });
    f.settle();
    CHECK(reported.empty());  // focus stays on its key; the row now at its place does not take it
    CHECK(f.backend.prop(f.table, "highlightedRow") == "none");  // until the key is located again
}

// Mutations: the located index ignored, the fallback taken instead (focus stays on view row 7 and the row there is
// reported); an answer that the view lacks the key leaving focus looking for it (nothing is reported).
TEST_CASE("table focus: a new order follows the focused key off-screen and only its absence moves focus",
          "[ui][mount]") {
    WindowFixture f{100'000, 30};
    std::vector<std::optional<CellRef>> reported;
    f.onFocusCell = [&reported](std::optional<CellRef> cell) { reported.push_back(std::move(cell)); };
    f.focus.set(CellRef{.key = Key{std::int64_t{7}}, .column = "c3"});
    f.settle();
    REQUIRE(f.backend.focusCell(f.table) == std::pair{std::size_t{7}, std::size_t{3}});
    // Reversed: key 7 is at view row 99,992, far from the rows realized; the rows are asked where it is.
    std::size_t const asked = f.rows->locateCalls;
    f.rows->reorder([](std::int64_t index) { return 99'999 - index; });
    f.settle();
    CHECK(f.rows->locateCalls == asked + 1);
    CHECK(f.backend.focusCell(f.table) == std::pair{std::size_t{99'992}, std::size_t{3}});
    CHECK(f.cellOf(Key{std::int64_t{7}}, 3).has_value());  // its row realized where it is
    CHECK(reported.empty());                               // the same cell: nothing to report
    // A new order without key 7: the row now at its place takes focus, and is reported.
    f.rows->reorder([](std::int64_t index) { return index == 99'992 ? std::int64_t{100'000} : 99'999 - index; });
    f.settle();
    CHECK(f.backend.focusCell(f.table) == std::pair{std::size_t{99'992}, std::size_t{3}});
    REQUIRE(reported.size() == 1);
    CHECK(reported.back() == CellRef{.key = Key{std::int64_t{100'000}}, .column = "c3"});
}

// Mutation: no pooled cell dropped once a row has nothing left to realize (the widgets alive grow with every column
// scrolled past).  Gate F2.
TEST_CASE("table window: cells of their own columns scrolled across stay bounded", "[ui][mount]") {
    std::vector<std::size_t> every(300);
    std::iota(every.begin(), every.end(), std::size_t{0});
    WindowFixture f{1'999, 300, {.pinned = 0, .custom = every}};
    for (int x = 0; x <= 20'000; x += 1'000) {
        f.scrollColumns(x);
        CHECK(f.cellsAlive() == f.realizedRows() * f.backend.windowColumns(f.table).size());
    }
}

// Mutation: a pass that realized nothing posts the next anyway (the owner never goes idle).
TEST_CASE("table window: cells whose build throws on every pass stop being retried", "[ui][mount]") {
    std::vector<std::size_t> every(300);
    std::iota(every.begin(), every.end(), std::size_t{0});
    WindowFixture f{1'999, 300, {.pinned = 0, .custom = every}};
    Probe const probe{f.owner.coreExecutor()};  // each failed pass is reported, not asserted
    f.throwing = true;
    f.backend.scrollTableColumns(f.table, 10'000, 2'000);  // twenty-eight new columns of their own, none builds
    std::size_t turns = 0;
    while (f.owner.runOne() && ++turns < 1'000) {
    }
    CHECK(turns < 1'000);
}

// Mutations: a fixed width taken as the backend's measure (every extent is 10); the padding left out (78, not 84).
TEST_CASE("table window: widths are in the backend's measure", "[ui][mount]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    backend.setTableMeasure(ui::TableMeasure{.unit = 7.8, .monoUnit = 10.12, .contentEstimate = 64, .padding = 6});
    auto rows = std::make_shared<FakeRows>(50, 4, runtime.owner());
    std::vector<ui::TableColumn> columns{
        {.id = "c0", .label = "A", .width = ui::Sizing::fixed(10)},
        {.id = "c1", .label = "B", .width = ui::Sizing::fixed(10), .role = ui::TextRole::Mono},
        {.id = "c2", .label = "C"},
        {.id = "c3", .label = "D", .width = ui::Sizing::fixed(10)},
    };
    ui::TableSource source;
    source.rows = rows;
    source.cellView = [](std::size_t column, Signal<Value> const& row) {
        return ui::text({.text = [&row, column] { return fieldText(row.get(), "c" + std::to_string(column)); }});
    };
    ui::TableOptions options;
    options.arrangement = TableArrangement{.visible = {}, .pinned = 0, .widths = {{3, 5}}};  // the user's 5 units
    ui::Mounted const view{runtime, backend, ui::table(std::move(columns), std::move(source), std::move(options))};
    owner.runAll();
    ColumnExtents const& extents = *view.columnExtents();
    CHECK(extents.widthOf(0) == 84);   // 10 units of 7.8, rounded, and the padding
    CHECK(extents.widthOf(1) == 108);  // 10 monospace characters of 10.12, rounded up, and the padding
    CHECK(extents.widthOf(2) == 64);   // the backend's estimate
    CHECK(extents.widthOf(3) == 45);
}

// Mutations: the mount sending the rectangle once at its first positions (the sort leaves "r0+2,c0+2" recorded);
// the mount reading a reported position as a `Table::columns` index (with `a` hidden, position 0 reports column a).
TEST_CASE("mount: a cell selection is shown by its corners' positions now", "[ui][mount][cells]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    auto rows = std::make_shared<FakeRows>(3, 3, runtime.owner());
    std::vector<std::int64_t> order{1, 2, 3};
    rows->reorder([&order](std::int64_t index) { return order.at(static_cast<std::size_t>(index)); });
    std::vector<ui::TableColumn> columns{{.id = "a", .label = "A"}, {.id = "b", .label = "B"}, {.id = "c", .label = "C"}};
    ui::TableSource source;
    source.rows = rows;
    source.cellView = [](std::size_t column, Signal<Value> const& row) {
        return ui::text({.text = [&row, column] { return fieldText(row.get(), "c" + std::to_string(column)); }});
    };
    auto const cell = [](std::int64_t key, std::string column) { return CellRef{Key{key}, std::move(column)}; };
    Signal<std::optional<ui::CellRange>> selection{runtime, ui::CellRange{cell(1, "a"), cell(2, "b")}};
    Signal<TableArrangement> arrangement{runtime, TableArrangement{}};
    std::vector<std::optional<ui::CellRange>> reported;
    ui::TableOptions options;
    options.arrangement = [&arrangement] { return arrangement.get(); };
    options.cellSelection = ui::Prop<std::optional<ui::CellRange>>{[&selection] { return selection.get(); }};
    options.onCellSelect = [&reported](std::optional<ui::CellRange> range) { reported.push_back(std::move(range)); };
    ui::Mounted const view{runtime, backend, ui::table(std::move(columns), std::move(source), std::move(options))};
    owner.runAll();
    int const table = backend.all("Table").front();
    CHECK(backend.prop(table, "cellSelection") == "r0+2,c0+2");
    order = {3, 1, 2};
    rows->reorder([&order](std::int64_t index) { return order.at(static_cast<std::size_t>(index)); });
    owner.runAll();
    CHECK(backend.prop(table, "cellSelection") == "r1+2,c0+2");
    backend.selectCells(table, 0, 1, 2, 2);
    owner.runAll();
    REQUIRE(reported.size() == 1);
    CHECK(reported.back() == ui::CellRange{cell(3, "b"), cell(2, "c")});
    arrangement.set(TableArrangement{.visible = {1, 2}, .pinned = 0, .widths = {}});
    owner.runAll();
    CHECK(backend.prop(table, "cellSelection").empty());   // a corner's column is hidden; the range is kept
    backend.selectCells(table, 0, 0, 0, 1);
    owner.runAll();
    REQUIRE(reported.size() == 2);
    CHECK(reported.back() == ui::CellRange{cell(3, "b"), cell(3, "c")});   // positions, not indices
    arrangement.set(TableArrangement{});
    owner.runAll();
    CHECK(backend.prop(table, "cellSelection") == "r1+2,c0+2");
    selection.set(std::nullopt);
    owner.runAll();
    CHECK(backend.prop(table, "cellSelection").empty());
}
