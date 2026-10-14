// SPDX-License-Identifier: Apache-2.0
//
// Custom components (spec 5 §10): a backend that loads the component gets its props, bound like any property, and
// hands its events back; one that loads nothing shows the fallback. Both through the view tree and through a
// document's `custom` node.

#include <catch2/catch_test_macros.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_support.hpp"
#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;

namespace {

using morph::reactive::Runtime;
using morph::reactive::Signal;
using ui::testing::RecordingBackend;
using Owner = morph::testing::StepExecutor;

/// A chart node with a text prop, a bound bool prop and a number prop, whose events land in @p events.
///
/// The props are appended one at a time rather than listed in a braced initializer: copying a `CustomProp` out of
/// an initializer list makes GCC 15 at -O2 report the `std::function` inside a bound `Prop` as maybe uninitialized.
ui::Node chart(Signal<bool>& live, std::vector<std::pair<std::string, std::string>>& events) {
    ui::Custom custom{.name = "Chart",
                      .fallback = ui::text({.text = "no chart", .common = {.testId = "fallback"}}),
                      .props = {},
                      .minVersion = 0,
                      .onEvent = [&events](std::string const& event, std::string const& payload) {
                          events.emplace_back(event, payload);
                      }};
    custom.props.push_back(ui::CustomProp{.name = "title", .value = ui::Prop<std::string>{"Trend"}});
    custom.props.push_back(ui::CustomProp{.name = "live", .value = ui::Prop<bool>{[&live] { return live.get(); }}});
    custom.props.push_back(ui::CustomProp{.name = "zoom", .value = ui::Prop<double>{2.5}});
    return ui::custom(std::move(custom));
}

}  // namespace

// Mutation: mount the fallback without asking the backend (the loaded component never appears).
TEST_CASE("ui::Custom: a backend that loads the component gets its props, bound, and hands back its events", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    backend.loadsComponents({"Chart"});
    Signal<bool> live{runtime, false};
    std::vector<std::pair<std::string, std::string>> events;
    ui::Mounted const view{runtime, backend, chart(live, events)};
    owner.runAll();

    std::optional<int> const loaded = backend.find("Custom", "component", "Chart");
    REQUIRE(loaded.has_value());
    CHECK_FALSE(backend.find("Text", "testId", "fallback").has_value());
    CHECK(backend.prop(*loaded, "prop.title") == "Trend");
    CHECK(backend.prop(*loaded, "prop.live") == "false");
    CHECK(backend.prop(*loaded, "prop.zoom") == "2.5");

    // Mutation: bind a slot prop once instead of reactively (live stays false).
    live.set(true);
    owner.runAll();
    CHECK(backend.prop(*loaded, "prop.live") == "true");

    // Mutation: drop setOnEvent (no event arrives).
    backend.raiseEvent(*loaded, "picked", "i:42");
    REQUIRE(events.size() == 1);
    CHECK(events.front() == std::pair<std::string, std::string>{"picked", "i:42"});
}

// Mutation: show the loaded component when the backend returned null (the mount dereferences null).
TEST_CASE("ui::Custom: a backend that loads nothing shows the fallback", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    backend.loadsComponents({"SomethingElse"});
    Signal<bool> live{runtime, false};
    std::vector<std::pair<std::string, std::string>> events;
    ui::Mounted const view{runtime, backend, chart(live, events)};
    owner.runAll();
    CHECK(backend.find("Text", "testId", "fallback").has_value());
    CHECK_FALSE(backend.find("Custom", "component", "Chart").has_value());
}

namespace {

using ui::testing::Harness;

/// A host component with one prop of each type and two events, and a screen that binds and handles them.
constexpr std::string_view kPicker = R"({
  "vocab": "ui/1", "id": "picker",
  "state": { "order": { "init": 9007199254740993 }, "limit": { "init": { "lit": { "num": 25, "den": 2, "dp": 2 } } },
             "live": { "init": true }, "picked": { "init": null }, "note": { "init": "-" } },
  "components": { "Picker": {
      "host": "OrderPicker", "version": 3,
      "props": { "title": "string", "orderId": "int", "limit": "exact", "live": "bool", "zoom": "number",
                 "rows": "json", "unused": "string" },
      "events": { "picked": "int", "noted": "string" },
      "fallback": { "kind": "text", "testId": "fallback", "text": { "ref": "props.title" } } } },
  "view": { "kind": "column", "children": [
    { "kind": "custom", "name": "Picker",
      "props": { "title": "Orders", "orderId": { "ref": "order" }, "limit": { "ref": "limit" },
                 "live": { "ref": "live" }, "zoom": { "lit": { "num": 3, "den": 2, "dp": 1 } }, "rows": { "list": [ 1, "a" ] } },
      "on": { "picked": [ { "set": "picked", "value": { "ref": "event" } } ],
              "noted": [ { "set": "note", "value": { "ref": "event" } } ] } },
    { "kind": "text", "testId": "picked", "text": { "fn": "format", "args": [ "{0}", { "ref": "picked" } ] } },
    { "kind": "text", "testId": "note", "text": { "ref": "note" } } ] } })";

}  // namespace

// Mutations: give the component no props (every prop check fails); send an `int` through its locale text (the id
// above 2^53 gains group separators or loses digits); send `exact` without its places ("12.5").
TEST_CASE("custom node: a loaded component gets every declared prop in its type's form", "[ui][interpreter]") {
    Harness h;
    // A locale that groups digits and writes a decimal comma: `int` and `exact` must not cross in it.
    ui::FormatContext german;
    german.locale = "de";
    german.numeric.decimalSeparator = ",";
    german.numeric.groupSeparator = ".";
    h.session.setFormatting(std::move(german));
    h.backend.loadsComponents({"Picker"});
    auto const screen = h.mount(kPicker);
    std::optional<int> const loaded = h.backend.find("Custom", "component", "Picker");
    REQUIRE(loaded.has_value());
    CHECK(h.diagnostics.empty());
    CHECK_FALSE(h.backend.find("Text", "testId", "fallback").has_value());
    CHECK(h.backend.prop(*loaded, "prop.title") == "Orders");
    CHECK(h.backend.prop(*loaded, "prop.orderId") == "9007199254740993");
    CHECK(h.backend.prop(*loaded, "prop.limit") == "12.50");
    CHECK(h.backend.prop(*loaded, "prop.live") == "true");
    CHECK(h.backend.prop(*loaded, "prop.zoom") == "1.5");
    CHECK(h.backend.prop(*loaded, "prop.rows") == R"([1,"a"])");
    CHECK(h.backend.prop(*loaded, "prop.unused").empty());  // declared, not bound: its type's empty value
    // Mutation: ask the backend for version 0 (a host component below the document's version would load).
    CHECK(h.backend.prop(*loaded, "minVersion") == "3");
}

// Mutations: run the commands with a null payload (picked stays null); look the commands up under the bare event
// name (nothing runs).
TEST_CASE("custom node: a loaded component's events run the node's commands with the typed payload",
          "[ui][interpreter]") {
    Harness h;
    h.backend.loadsComponents({"Picker"});
    auto const screen = h.mount(kPicker);
    std::optional<int> const picker = h.backend.find("Custom", "component", "Picker");
    REQUIRE(picker.has_value());
    h.backend.raiseEvent(*picker, "picked", "i:9007199254740993");
    h.settle();
    CHECK(h.text("picked") == "9007199254740993");
    h.backend.raiseEvent(*picker, "noted", "s:checked twice");
    h.settle();
    CHECK(h.text("note") == "checked twice");
    h.backend.raiseEvent(*picker, "undeclared", "i:1");  // no commands for it: nothing runs, nothing is reported
    h.settle();
    CHECK(h.diagnostics.empty());
}

// Mutation: mount nothing in the node's place when the backend loads nothing (the fallback's text is missing).
TEST_CASE("custom node: a backend that loads nothing shows the fallback with the props it reads",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(kPicker);
    CHECK_FALSE(h.backend.find("Custom", "component", "Picker").has_value());
    CHECK(h.text("fallback") == "Orders");
}
