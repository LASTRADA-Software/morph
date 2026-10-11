// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
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
using ui::testing::RecordingBackend;
using Owner = morph::testing::StepExecutor;
using Probe = morph::testing::OwnerProbeRecorder;
using Lines = std::vector<std::string>;

}  // namespace

// Mutations: leave out setDismissible (dismiss then refuses the banner); call the click handler for the action.
TEST_CASE("ui::banner: tone, text and action follow their bindings; action and dismissal reach their handlers",
          "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<int> failures{runtime, 1};
    Lines ran;
    ui::Mounted const view{runtime, backend,
                           ui::banner({.tone = [&] { return failures.get() > 1 ? ui::Tone::Err : ui::Tone::Warn; },
                                       .text = [&] { return std::to_string(failures.get()) + " failed"; },
                                       .action = {.label = [&] { return failures.get() > 1 ? "Retry all" : "Retry"; },
                                                  .onClick = [&] { ran.emplace_back("retry"); }},
                                       .dismissible = true,
                                       .onDismiss = [&] { ran.emplace_back("dismiss"); }})};
    CHECK(backend.dump() == "Banner#1 actionLabel=Retry dismissible=true text=1 failed tone=Warn\n");
    backend.clearLog();
    failures.set(2);
    owner.runAll();
    CHECK(backend.log() ==
          Lines{"set Banner#1 tone=Err", "set Banner#1 text=2 failed", "set Banner#1 actionLabel=Retry all"});
    backend.click(1);
    backend.dismiss(1);
    CHECK(ran == Lines{"retry", "dismiss"});
    CHECK(backend.prop(1, "visible").empty());  // the banner stays: the application hides it
}

// Mutation: drop the dismissible check in RecordingBackend::dismiss, or the action-label check in click.
TEST_CASE("RecordingBackend: a banner without a dismissal or an action button takes neither", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{runtime, backend, ui::banner({.text = "Saved"})};
    CHECK(backend.prop(1, "dismissible") == "false");
    CHECK(backend.prop(1, "tone") == "Info");
    CHECK_THROWS_AS(backend.dismiss(1), std::logic_error);
    CHECK_THROWS_AS(backend.click(1), std::logic_error);
}

// Mutations: leave out setIcon; bind the tone once.
TEST_CASE("ui::badge: tone and text follow their bindings, the icon is set once", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<int> count{runtime, 0};
    ui::Mounted const view{runtime, backend,
                           ui::badge({.tone = [&] { return count.get() == 0 ? ui::Tone::Neutral : ui::Tone::Info; },
                                      .text = [&] { return std::to_string(count.get()); },
                                      .icon = "mail"})};
    CHECK(backend.dump() == "Badge#1 icon=mail text=0 tone=Neutral\n");
    backend.clearLog();
    count.set(3);
    owner.runAll();
    CHECK(backend.log() == Lines{"set Badge#1 tone=Info", "set Badge#1 text=3"});
}

// Mutation: print an indeterminate value as 0.
TEST_CASE("ui::progress: the value and label follow their bindings; no value is indeterminate", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::optional<double>> done{runtime, std::nullopt};
    ui::Mounted const view{runtime, backend,
                           ui::progress({.value = [&] { return done.get(); },
                                         .label = [&] { return done.get().has_value() ? "Uploading" : "Waiting"; }})};
    CHECK(backend.dump() == "Progress#1 label=Waiting value=none\n");
    backend.clearLog();
    done.set(0.25);
    owner.runAll();
    CHECK(backend.log() == Lines{"set Progress#1 value=0.25", "set Progress#1 label=Uploading"});
}

// Mutation: leave out the step state in the formatted steps; bind current once.
TEST_CASE("ui::steps: the steps and the current one follow their bindings", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::size_t> at{runtime, 0};
    ui::Mounted const view{
        runtime, backend,
        ui::steps(
            {.items =
                 [&] {
                     return std::vector<ui::Step>{
                         {.label = "Upload", .state = at.get() > 0 ? ui::StepState::Done : ui::StepState::Active},
                         {.label = "Check", .state = at.get() > 0 ? ui::StepState::Active : ui::StepState::Pending}};
                 },
             .current = [&] { return std::optional<std::size_t>{at.get()}; }})};
    CHECK(backend.dump() == "Steps#1 current=0 steps=[Upload:Active,Check:Pending]\n");
    backend.clearLog();
    at.set(1);
    owner.runAll();
    CHECK(backend.log() == Lines{"set Steps#1 steps=[Upload:Done,Check:Active]", "set Steps#1 current=1"});
}

// Mutation: bind the items once.
TEST_CASE("ui::keyValue: the pairs follow their binding, escaped", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::string> mass{runtime, "1,5 g"};
    ui::Mounted const view{runtime, backend, ui::keyValue({.items = [&] {
                               return std::vector<ui::KeyValueItem>{{.label = "Mass", .value = mass.get()},
                                                                    {.label = "Unit", .value = "g"}};
                           }})};
    CHECK(backend.prop(1, "items") == "[Mass:1\\,5 g,Unit:g]");
    backend.clearLog();
    mass.set("2 g");
    owner.runAll();
    CHECK(backend.log() == Lines{"set KeyValue#1 items=[Mass:2 g,Unit:g]"});
}

// Mutations: leave out setIcon or setOnAction from the empty state's mount.
TEST_CASE("ui::emptyState: title, text and action follow their bindings; the action reaches its handler", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> filtered{runtime, false};
    int created = 0;
    ui::Mounted const view{
        runtime, backend,
        ui::emptyState(
            {.title = [&] { return filtered.get() ? "No matches" : "No samples"; },
             .text = [&] { return filtered.get() ? "Clear the filter." : "Create the first one."; },
             .icon = "flask",
             .action = {.label = [&] { return filtered.get() ? "" : "New sample"; }, .onClick = [&] { ++created; }}})};
    CHECK(backend.dump() ==
          "EmptyState#1 actionLabel=New sample icon=flask text=Create the first one. title=No samples\n");
    backend.click(1);
    CHECK(created == 1);
    backend.clearLog();
    filtered.set(true);
    owner.runAll();
    CHECK(backend.log() == Lines{"set EmptyState#1 title=No matches", "set EmptyState#1 text=Clear the filter.",
                                 "set EmptyState#1 actionLabel="});
    CHECK_THROWS_AS(backend.click(1), std::logic_error);  // no action button any more
}

// A drawer follows a dialog's rules. Mutations: leave Drawer out of isClosedDialog (a closed drawer's button then
// takes a click); pass a fixed side to createDrawer.
TEST_CASE("ui::drawer: the content exists only while open, a closed drawer takes no input, dismiss reaches onDismiss",
          "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> open{runtime, false};
    Lines ran;
    ui::Mounted const view{
        runtime, backend,
        ui::column(
            {.children = {
                 ui::drawer({.open = [&] { return open.get(); },
                             .title = "Filters",
                             .child = ui::button({.label = "Apply", .onClick = [&] { ran.emplace_back("apply"); }}),
                             .onDismiss = [&] { ran.emplace_back("dismiss"); },
                             .side = ui::Side::Start}),
                 ui::drawer(
                     {.child = ui::button({.label = "Hidden", .onClick = [&] { ran.emplace_back("hidden"); }})})}})};
    // The second drawer is closed by a constant, so it has no content; the first holds none until opened.
    CHECK(backend.dump() ==
          "Column#1 gap=0\n  Drawer#2 open=false side=Start title=Filters\n"
          "  Drawer#3 open=false side=End title=\n");
    backend.dismiss(2);
    CHECK(ran.empty());
    open.set(true);
    owner.runAll();
    REQUIRE(backend.kindOf(4) == "Button");
    backend.click(4);
    backend.dismiss(2);
    CHECK(ran == Lines{"apply", "dismiss"});
}

// A drawer's `open` is controlled as a dialog's is: the user's dismissal closes it, and a document that keeps it open
// opens it again over the content it still holds. Mutations: leave Drawer out of RecordingBackend::dismiss's close (it
// never shows closed); skip the re-assertion in mountOverlay (it stays closed).
TEST_CASE("ui::drawer: a dismissal the document refuses opens the drawer again", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> const open{runtime, true};
    int dismissals = 0;
    ui::Mounted const view{runtime, backend,
                           ui::drawer({.open = [&] { return open.get(); },
                                       .child = ui::text({.text = "inside"}),
                                       .onDismiss = [&] { ++dismissals; }})};
    std::string const before = backend.dump();
    backend.dismiss(1);
    CHECK(dismissals == 1);
    CHECK(backend.prop(1, "open") == "false");
    owner.runAll();
    CHECK(backend.prop(1, "open") == "true");
    CHECK(backend.dump() == before);
}

// Mutation: drop the closed-drawer check from reachable() while the drawer has content (open, then closed by a
// setter the backend received but the mount has not yet acted on).
TEST_CASE("RecordingBackend: a widget inside a closed drawer takes no input", "[ui]") {
    RecordingBackend backend;
    std::unique_ptr<ui::DrawerWidget> const drawer = backend.createDrawer(nullptr, ui::Side::End);
    std::unique_ptr<ui::ButtonWidget> const button = backend.createButton(drawer.get());
    int clicks = 0;
    button->setOnClick([&] { ++clicks; });
    backend.click(2);
    drawer->setOpen(true);
    backend.click(2);
    CHECK(clicks == 1);
}

// The drawer shares the dialog's failure rule. Mutation: mount a drawer's content without the try/catch that
// dismisses it (mountOverlay is shared, so this is the dialog's rule exercised through a drawer).
TEST_CASE("ui::drawer: content that fails to mount dismisses the drawer", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    EchoingBackend backend;
    std::optional<int> spare;
    backend.setOnCreate(failWhenSpent(spare));
    Signal<bool> open{runtime, false};
    ui::Mounted const view{runtime, backend,
                           ui::drawer({.open = [&] { return open.get(); },
                                       .child = ui::text({.text = "body"}),
                                       .onDismiss = [&] { open.set(false); }})};
    spare = 0;
    open.set(true);
    owner.runAll();
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
    CHECK_FALSE(open.peek());
    CHECK(backend.recording().dump() == "Drawer#1 open=false side=End title=\n");
}

// Mutations: bind the sizes before the panes are mounted (the first setSizes then precedes the panes); drop the
// pane-count check in RecordingBackend::resize.
TEST_CASE("ui::splitter: panes, sizes and a user's resize", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<int>> sizes{runtime, std::vector<int>{}};
    std::vector<std::vector<int>> resized;
    ui::Mounted const view{
        runtime, backend,
        ui::splitter({.children = {ui::text({.text = "list"}), nullptr, ui::text({.text = "detail"})},
                      .orientation = ui::Axis::Vertical,
                      .sizes = [&] { return sizes.get(); },
                      .onResize = [&](std::vector<int> const& each) { resized.push_back(each); }})};
    CHECK(backend.log().back() == "set Splitter#1 sizes=[]");
    CHECK(backend.dump() ==
          "Splitter#1 axis=Vertical sizes=[]\n  Text#2 role=Normal text=list\n"
          "  Text#3 role=Normal text=detail\n");
    backend.resize(1, {30, 70});
    CHECK(resized == std::vector<std::vector<int>>{{30, 70}});
    CHECK(backend.prop(1, "sizes") == "[30,70]");
    CHECK_THROWS_AS(backend.resize(1, {100}), std::logic_error);
    backend.clearLog();
    sizes.set({40, 60});
    owner.runAll();
    CHECK(backend.log() == Lines{"set Splitter#1 sizes=[40,60]"});
}

// Its header stays usable while it is closed; its content does not. Mutations: leave out setHeader (the header is
// then hidden with the content); drop closedAround from reachable(); let expand skip the open property.
TEST_CASE("ui::collapsible: a closed section hides its content but not its header", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> open{runtime, false};
    Lines ran;
    std::vector<bool> toggles;
    ui::Mounted const view{
        runtime, backend,
        ui::collapsible({.title = "Advanced",
                         .open = [&] { return open.get(); },
                         .onToggle = [&](bool wanted) { toggles.push_back(wanted); },
                         .header = ui::button({.label = "Reset", .onClick = [&] { ran.emplace_back("reset"); }}),
                         .child = ui::button({.label = "Apply", .onClick = [&] { ran.emplace_back("apply"); }})})};
    CHECK(backend.dump() ==
          "Collapsible#1 header=2 open=false title=Advanced\n  Button#2 label=Reset\n"
          "  Button#3 label=Apply\n");
    backend.click(2);
    backend.click(3);
    CHECK(ran == Lines{"reset"});
    backend.expand(1, true);
    CHECK(toggles == std::vector<bool>{true});
    CHECK(backend.prop(1, "open") == "true");
    backend.click(3);
    CHECK(ran == Lines{"reset", "apply"});
}

// Mutation: let FakeCollapsible::setHeader take a widget that is not its child.
TEST_CASE("RecordingBackend: a collapsible's header must be its child", "[ui]") {
    RecordingBackend backend;
    std::unique_ptr<ui::CollapsibleWidget> const section = backend.createCollapsible(nullptr);
    std::unique_ptr<ui::TextWidget> const elsewhere = backend.createText(nullptr);
    CHECK_THROWS_AS(section->setHeader(*elsewhere), std::logic_error);
}

// Mutations: compare extensions with case; let a single-file zone take several files; take a drop with one refused
// file among accepted ones (any_of for all_of).
TEST_CASE("ui::dropZone: a drop is taken whole or refused whole", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    std::vector<std::vector<std::string>> drops;
    ui::Mounted const view{
        runtime, backend,
        ui::dropZone({.accept = {".csv", ".tsv"},
                      .onDrop = [&](std::vector<std::string> const& paths) { drops.push_back(paths); },
                      .child = ui::text({.text = "Drop results here"})})};
    CHECK(backend.dump() ==
          "DropZone#1 accept=[.csv,.tsv] multiple=false\n  Text#2 role=Normal text=Drop results here\n");
    CHECK(backend.dropFiles(1, {"/in/RUN.CSV"}));
    CHECK_FALSE(backend.dropFiles(1, {"/in/notes.txt"}));
    CHECK_FALSE(backend.dropFiles(1, {"/in/a.csv", "/in/b.csv"}));
    CHECK_THROWS_AS(backend.dropFiles(1, {}), std::logic_error);
    CHECK(drops == std::vector<std::vector<std::string>>{{"/in/RUN.CSV"}});
}

// Mutation: let a zone that takes several files take a drop that also carries a refused one.
TEST_CASE("ui::dropZone: a zone that takes several files refuses a drop with one it does not accept", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    std::vector<std::vector<std::string>> drops;
    ui::Mounted const view{
        runtime, backend,
        ui::dropZone({.accept = {".csv"}, .multiple = true, .onDrop = [&](std::vector<std::string> const& paths) {
                          drops.push_back(paths);
                      }})};
    CHECK_FALSE(backend.dropFiles(1, {"/in/a.csv", "/in/b.txt"}));
    CHECK(backend.dropFiles(1, {"/in/a.csv", "/in/b.csv"}));
    CHECK(drops == std::vector<std::vector<std::string>>{{"/in/a.csv", "/in/b.csv"}});
}

// Mutation: mount the fallback as a root of its own instead of in the node's place.
TEST_CASE("ui::custom: Mounted shows the fallback in the node's place", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{
        runtime, backend,
        ui::column({.children = {ui::custom({.name = "ConcentrationChart", .fallback = ui::text({.text = "chart"})}),
                                 ui::text({.text = "after"})}})};
    CHECK(backend.dump() == "Column#1 gap=0\n  Text#2 role=Normal text=chart\n  Text#3 role=Normal text=after\n");
}

// Mutations: drop the null check in ui::custom; drop the one in the custom node's mount.
TEST_CASE("ui::custom: a custom node without a fallback is refused", "[ui]") {
    CHECK_THROWS_AS(ui::custom({.name = "Chart"}), std::invalid_argument);
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Node const bare = std::make_shared<ui::NodeData const>(ui::NodeData{.kind = ui::Custom{.name = "Chart"}});
    CHECK_THROWS_AS((ui::Mounted{runtime, backend, bare}), std::invalid_argument);
}

// Mutations: drop the reachable() check from expand, resize or dropFiles (each then acts on a hidden widget).
TEST_CASE("RecordingBackend: expand, resize and dropFiles leave a hidden widget alone", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Lines ran;
    ui::Mounted const view{
        runtime, backend,
        ui::column(
            {.children = {ui::collapsible({.onToggle = [&](bool) { ran.emplace_back("toggle"); }}),
                          ui::splitter({.children = {ui::spacer()},
                                        .onResize = [&](std::vector<int> const&) { ran.emplace_back("resize"); }}),
                          ui::dropZone(
                              {.onDrop = [&](std::vector<std::string> const&) { ran.emplace_back("drop"); }})},
             .common = {.visible = false}})};
    REQUIRE(backend.kindOf(2) == "Collapsible");
    REQUIRE(backend.kindOf(3) == "Splitter");
    REQUIRE(backend.kindOf(5) == "DropZone");
    backend.expand(2, false);
    backend.resize(3, {10});
    CHECK_FALSE(backend.dropFiles(5, {"/in/a.csv"}));
    CHECK(ran.empty());
    CHECK(backend.prop(2, "open") == "true");
    CHECK(backend.prop(3, "sizes") == "[]");
}

// Mutations: let an empty accept list refuse files; compare a suffix longer than the path (out of range).
TEST_CASE("RecordingBackend: a drop zone without accept takes any file, and a short name matches no extension",
          "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    int drops = 0;
    ui::Mounted const any{runtime, backend,
                          ui::dropZone({.onDrop = [&](std::vector<std::string> const&) { ++drops; }})};
    ui::Mounted const csv{
        runtime, backend,
        ui::dropZone({.accept = {".csv"}, .onDrop = [&](std::vector<std::string> const&) { ++drops; }})};
    CHECK(backend.dropFiles(1, {"notes"}));
    CHECK_FALSE(backend.dropFiles(2, {"sv"}));
    CHECK(drops == 1);
}

// Mutation: drop the `index >= level->size()` check in chooseMenuEntry (the too-deep path then reads past the end).
TEST_CASE("RecordingBackend: an empty or too deep menu path chooses nothing", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    int chosen = 0;
    ui::Mounted const view{runtime, backend, ui::menu({.items = {{.label = "Open", .onSelect = [&] { ++chosen; }}}})};
    backend.chooseMenuEntry(1, {});
    backend.chooseMenuEntry(1, {0, 0});
    CHECK(chosen == 0);
    backend.chooseMenuEntry(1, {0});
    CHECK(chosen == 1);
}

// Mutation: treat a closed collapsible without a header as open (return false when `header` is absent).
TEST_CASE("RecordingBackend: a closed collapsible without a header hides all its content", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    int clicks = 0;
    ui::Mounted const view{
        runtime, backend,
        ui::collapsible({.open = false, .child = ui::button({.label = "Go", .onClick = [&] { ++clicks; }})})};
    REQUIRE(backend.kindOf(2) == "Button");
    backend.click(2);
    CHECK(clicks == 0);
}

// Mutation: print an unset current step as its position 0.
TEST_CASE("ui::steps: no current step shows none", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const view{runtime, backend, ui::steps({.items = std::vector<ui::Step>{{.label = "Only"}}})};
    CHECK(backend.prop(1, "current") == "none");
    CHECK(backend.prop(1, "steps") == "[Only:Pending]");
}
