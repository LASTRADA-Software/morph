// SPDX-License-Identifier: Apache-2.0
//
// The shell frame on the terminal: compact, the sidebar gives way to the navigation menu, so the page starts at the
// page's own padding instead of after the sidebar's 28 columns.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/tui/MockTerminalOutput.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Terminal.hpp>
#include <cstddef>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/tui/backend.hpp>
#include <morph/tui/loop_executor.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/shell_frame.hpp>
#include <string>
#include <vector>

#include "tui_harness.hpp"

namespace ui = morph::ui;
using morph::tui::testing::rowsOf;

namespace {

// A signed-in frame on a terminal @p columns wide, compact as the generic client makes it below `expanded`, with the
// text `PAGE` on its page.
struct FramedPage {
    explicit FramedPage(int columns) : terminal{std::make_unique<core::tui::MockTerminalOutput>(columns, 20)} {
        ui::ShellFrameOptions options;
        options.applicationName = "Lab";
        options.navigator.entries = {ui::CatalogEntry{.id = "lims.review", .title = "Review", .group = "Lab"}};
        frame = std::make_unique<ui::ShellFrame>(backend, nullptr, std::move(options));
        frame->showSignedIn(true);
        frame->setTitle("Review");
        frame->setCompact(backend.widthClass() != "expanded");
        content = backend.createText(&frame->page());
        content->setText("PAGE");
        static_cast<void>(loop.runUntilIdle());
        backend.fit();
        screen.draw();
    }

    // The column the page's text starts at, or npos when it is not drawn.
    [[nodiscard]] std::size_t pageColumn() const {
        for (std::string const& row : rowsOf(screen)) {
            if (std::size_t const at = row.find("PAGE"); at != std::string::npos) {
                return at;
            }
        }
        return std::string::npos;
    }

    core::net::PlatformLoop loop;
    morph::tui::LoopExecutor executor{loop};
    morph::reactive::Runtime runtime{executor};
    core::tui::Terminal terminal;
    core::tui::Screen screen{terminal};
    morph::tui::Backend backend{screen};
    std::unique_ptr<ui::ShellFrame> frame;
    std::unique_ptr<ui::TextWidget> content;
};

}  // namespace

// Control: a no-op setCompact (the page starts at column 30 on the narrow terminals too).
TEST_CASE("tui shell frame: below expanded the page starts at its padding, and beside the sidebar when expanded",
          "[tui][shell]") {
    FramedPage const narrow{60};
    CHECK(narrow.backend.widthClass() == "compact");
    CHECK(narrow.pageColumn() == 2);
    CHECK(std::ranges::any_of(rowsOf(narrow.screen), [](std::string const& row) { return row.contains("☰"); }));
    FramedPage const medium{80};
    CHECK(medium.backend.widthClass() == "medium");
    CHECK(medium.pageColumn() == 2);
    FramedPage const wide{160};
    CHECK(wide.backend.widthClass() == "expanded");
    CHECK(wide.pageColumn() == 30);
}
