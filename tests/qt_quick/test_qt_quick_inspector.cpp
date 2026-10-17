// SPDX-License-Identifier: Apache-2.0
//
// The development inspector's window on Qt Quick (spec 6 §21; spec 6 §12, "Inspector"): Ctrl+Shift+I in the
// application's window opens it over the focused screen, and it lists every mounted node with its evaluated
// properties and every query with its key, state and age. A release build contains none of it; that is measured on
// the built binaries, not here.

#include <QQuickWindow>
#include <QTest>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <morph/qt_quick/inspector.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/ui/inspector.hpp>
#include <morph/ui/interpreter.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "qt_quick_harness.hpp"
#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using morph::qt_quick::InspectorWindow;
using morph::qt_quick::testing::Scene;
using ui::testing::corpusCatalog;
using ui::testing::loadOrFail;
using ui::testing::RecordingHost;
using ui::testing::ScriptedDispatcher;

namespace {

constexpr std::string_view kInspected = R"({
  "vocab": "ui/1", "id": "inspected",
  "state": { "filter": { "type": "string", "init": "all" } },
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "info": { "model": "sample", "action": "Lims_Info", "staleTime": 60000 } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "title", "text": { "fn": "format", "args": [ "filter {0}", { "ref": "filter" } ] } },
    { "kind": "button", "id": "go", "testId": "go", "label": "Go", "onClick": [ { "set": "filter", "value": "open" } ] }
  ] } })";

bool contains(std::string const& text, std::string_view part) { return text.contains(part); }

}  // namespace

// Mutation: leave the shortcut filter uninstalled in attachTo — Ctrl+Shift+I does not open the window.
// Mutation: drop the nodes section from refresh — the node lines are missing.
TEST_CASE("Inspector window: Ctrl+Shift+I opens it over the focused screen, listing its nodes and queries",
          "[qt_quick][inspector]") {
    Scene scene;
    morph::reactive::testing::ManualScheduler scheduler;
    ui::Session session{scene.runtime(), ui::SessionOptions{.scheduler = &scheduler,
                                                            .clock = [&scheduler] { return scheduler.now(); },
                                                            .keepUnused = std::chrono::minutes{5},
                                                            .catalog = corpusCatalog(),
                                                            .classify = {},
                                                            .developmentTools = ui::DevelopmentTools::On}};
    ScriptedDispatcher dispatcher{scene.owner()};
    RecordingHost host;
    ui::Interpreter interpreter{session, dispatcher, ui::InterpreterOptions{.host = &host}};
    auto mounted = interpreter.mount(loadOrFail(kInspected, corpusCatalog()), {}, scene.backend());
    REQUIRE(mounted.has_value());
    std::unique_ptr<ui::Screen> screen = std::move(*mounted);
    scene.settle();
    dispatcher.reply(0, R"({"name":"one"})");
    scene.settle();
    scheduler.advance(std::chrono::milliseconds{250});
    scene.settle();

    bool shown = true;
    InspectorWindow inspector{[&](std::function<void(ui::Inspector&)> const& visit) {
        if (shown) {
            std::optional<ui::Inspector> focused = ui::Inspector::over(*screen);
            REQUIRE(focused.has_value());
            visit(*focused);
        }
    }};
    inspector.attachTo(scene.window());
    CHECK_FALSE(inspector.isVisible());

    QTest::keyClick(&scene.window(), Qt::Key_I, Qt::ControlModifier | Qt::ShiftModifier);
    scene.settle();
    REQUIRE(inspector.isVisible());

    std::string const text = inspector.text();
    CAPTURE(text);
    // Every mounted node, with its evaluated properties.
    CHECK(contains(text, "/view  column"));
    CHECK(contains(text, "/view/children/0  text [title]  text=\"filter all\""));
    CHECK(contains(text, "/view/children/1  button #go [go]  label=\"Go\""));
    // Every query, with its key, state and age.
    CHECK(contains(text, "info (screen query) Lims_SampleModel/Lims_Info  fresh  age 250 ms  key "));
    CHECK(contains(text, "flushes "));

    // The same chord hides it again.
    QTest::keyClick(&scene.window(), Qt::Key_I, Qt::ControlModifier | Qt::ShiftModifier);
    scene.settle();
    CHECK_FALSE(inspector.isVisible());

    // The refetch button runs the document's refetch.
    std::size_t const calls = dispatcher.calls.size();
    inspector.refetch("info");
    scene.settle();
    CHECK(dispatcher.calls.size() == calls + 1);
    CHECK(contains(inspector.text(), "info (screen query) Lims_SampleModel/Lims_Info  pending"));
    CHECK(contains(inspector.text(), "/inspector: refetch info;"));

    // With no screen shown, it says so and lists nothing.
    shown = false;
    inspector.refresh();
    CHECK(contains(inspector.text(), "no screen is shown"));
    CHECK_FALSE(contains(inspector.text(), "/view  column"));
}
