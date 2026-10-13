// SPDX-License-Identifier: Apache-2.0
//
// A `form` document mounted on the terminal backend: the forms engine's view tree drawn on a headless screen, typed
// into through key events, submitting the exact body through the model alias.

// First, ahead of every header that brings view.hpp: GCC's -Wshadow reports value.hpp's enumerator `Type::Key` when
// view.hpp's `ui::Key` was declared before it.
// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/net/PlatformLoop.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/MockTerminalOutput.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/TestHelpers.hpp>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/tui/backend.hpp>
#include <morph/tui/loop_executor.hpp>
#include <morph/ui/interpreter.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "tui_harness.hpp"
#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using core::tui::KeyCode;
using morph::tui::testing::rowsOf;

namespace {

/// One client on the terminal backend: a loop-owned runtime, a session, a scripted dispatcher and a headless screen.
struct TuiClient {
    core::net::PlatformLoop loop;
    morph::tui::LoopExecutor executor{loop};
    morph::reactive::Runtime runtime{executor};
    morph::reactive::testing::ManualScheduler scheduler;
    ui::Session session{runtime, ui::SessionOptions{.scheduler = &scheduler,
                                                    .clock = [this] { return scheduler.now(); },
                                                    .keepUnused = std::chrono::minutes{5},
                                                    .catalog = ui::testing::corpusCatalog(),
                                                    .classify = {}}};
    ui::testing::ScriptedDispatcher dispatcher{executor};
    core::tui::Terminal terminal{std::make_unique<core::tui::MockTerminalOutput>(60, 30)};
    core::tui::Screen screen{terminal};
    morph::tui::Backend backend{screen};
    ui::Interpreter interpreter{session, dispatcher, ui::InterpreterOptions{}};

    void settle() {
        static_cast<void>(loop.runUntilIdle());
        backend.fit();
        screen.draw();
    }

    void type(std::string_view text) {
        for (char const character : text) {
            static_cast<void>(screen.dispatchEvent(core::tui::test::charKey(character)));
            settle();
        }
    }

    void key(KeyCode code) {
        static_cast<void>(screen.dispatchEvent(core::tui::test::specialKey(code)));
        settle();
    }

    [[nodiscard]] bool shows(std::string const& text) {
        auto const rows = rowsOf(screen);
        return std::ranges::any_of(rows, [&](std::string const& row) { return row.contains(text); });
    }
};

}  // namespace

TEST_CASE("tui forms: a form document draws on the terminal and submits what is typed", "[tui][forms]") {
    TuiClient client;
    client.dispatcher.autoReply.emplace("Lims_CaptureConcentration", R"({"ok":true})");
    auto screen = client.interpreter.mount(ui::testing::loadOrFail(R"({
      "vocab": "ui/1", "id": "capture",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "view": { "kind": "form", "id": "cap", "model": "sample", "action": "Lims_CaptureConcentration",
                "fields": { "note": { "label": "Remarks" } } } })",
                                                                   ui::testing::corpusCatalog()),
                                           {}, client.backend);
    REQUIRE(screen.has_value());
    client.settle();
    CHECK(client.shows("Sample *"));
    CHECK(client.shows("Concentration *"));
    CHECK(client.shows("Remarks"));
    client.backend.focusFirst();
    client.settle();
    client.type("9007199254740993");
    client.backend.focusNext();  // what the frontend does on Tab
    client.settle();
    client.type("12.5");
    std::string drawn;
    for (std::string const& row : rowsOf(client.screen)) {
        drawn += row + "\n";
    }
    INFO(drawn);
    REQUIRE(client.dispatcher.count("Lims_CaptureConcentration") >= 1);
    auto const& call = client.dispatcher.calls.at(client.dispatcher.last("Lims_CaptureConcentration"));
    CHECK(call.body == R"({"sampleId":9007199254740993,"concentration":{"num":1250,"den":100,"dp":2}})");
}
