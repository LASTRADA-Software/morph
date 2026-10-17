// SPDX-License-Identifier: Apache-2.0
//
// The app explorer on the terminal: it renders its list, and the keyboard reaches an action and shows it.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/MockTerminalOutput.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/TestHelpers.hpp>
#include <map>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/tui/backend.hpp>
#include <morph/tui/loop_executor.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/explorer.hpp>
#include <string>
#include <vector>

#include "tui_harness.hpp"

namespace ui = morph::ui;
using core::tui::KeyCode;
using morph::tui::testing::rowsOf;

namespace {

struct Live {
    void settle() {
        static_cast<void>(loop.runUntilIdle());
        backend.fit();
        screen.draw();
    }

    // Tab moves the focus as the terminal session does; any other key goes to the focused widget.
    void key(KeyCode code) {
        if (code == KeyCode::Tab) {
            backend.focusNext();
        } else {
            static_cast<void>(screen.dispatchEvent(core::tui::test::specialKey(code)));
        }
        settle();
    }

    [[nodiscard]] bool shows(std::string const& text) const {
        return std::ranges::any_of(rowsOf(screen), [&text](std::string const& row) { return row.contains(text); });
    }

    core::net::PlatformLoop loop;
    morph::tui::LoopExecutor executor{loop};
    morph::reactive::Runtime runtime{executor};
    core::tui::Terminal terminal{std::make_unique<core::tui::MockTerminalOutput>(100, 40)};
    core::tui::Screen screen{terminal};
    morph::tui::Backend backend{screen};
};

ui::ExplorerOptions oneModel() {
    ui::ExplorerOptions options;
    options.catalog.entries = {ui::CatalogEntry{.id = "samples", .title = "Samples"}};
    options.catalog.models = std::map<std::string, ui::ModelInfo, std::less<>>{{"SampleModel", ui::ModelInfo{}}};
    options.schemas = ui::SchemasByModel{{"SampleModel", std::string{R"({"Confirm":{"type":"object","properties":{
           "sampleId":{"type":"integer","x-order":0}},"required":["sampleId"]}})"}}};
    return options;
}

}  // namespace

// Mutation: leave the action buttons out of the model's collapsible (the keyboard reaches no action).
TEST_CASE("tui explorer: the list renders, and the keyboard opens a model and shows its action", "[tui][explorer]") {
    Live live;
    auto const page = live.backend.createStack(nullptr, ui::Axis::Vertical);
    page->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::stretch()});
    auto const explorer = std::make_unique<ui::Explorer>(live.runtime, live.backend, *page, oneModel());
    live.settle();
    CHECK(live.shows("Screens (1)"));
    CHECK(live.shows("Models (1)"));
    CHECK(live.shows("SampleModel"));

    // Tab through the list, activating each stop, until the action is shown: the model opens on the way.
    for (int step = 0; step < 20 && explorer->selected().empty(); ++step) {
        live.key(KeyCode::Tab);
        live.key(KeyCode::Enter);
    }
    CHECK(explorer->selected() == "Confirm");
    CHECK(live.shows("SampleModel / Confirm"));
    CHECK(live.shows("sampleId"));
    CHECK(live.shows("Not offered here."));
}
