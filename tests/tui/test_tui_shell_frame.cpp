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
#include <morph/ui/mount.hpp>
#include <morph/ui/shell_frame.hpp>
#include <morph/ui/view.hpp>
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
    FramedPage wide{160};
    CHECK(wide.backend.widthClass() == "expanded");
    CHECK(wide.pageColumn() == 30);
}

namespace {

// The row of the first line of @p screen that holds @p text, or npos.
std::size_t rowOf(core::tui::Screen const& screen, std::string const& text) {
    std::vector<std::string> const rows = rowsOf(screen);
    for (std::size_t row = 0; row < rows.size(); ++row) {
        if (rows.at(row).contains(text)) {
            return row;
        }
    }
    return std::string::npos;
}

}  // namespace

// Control: the old two-line bar, whose head line is below the title's at every width.
TEST_CASE("tui shell frame: the head shares the title's line where it fits, and wraps under it where it does not",
          "[tui][shell]") {
    for (auto const& [columns, together] : {std::pair{100, true}, std::pair{30, false}}) {
        FramedPage page{columns};
        ui::Text lead{.text = std::string{"a subtitle of some length"}};
        ui::Text trail{.text = std::string{"ACTIONS"}};
        ui::Mounted const leadMount{page.runtime, page.backend, ui::text(std::move(lead)),
                                    &page.frame->headLeadSlot()};
        ui::Mounted const trailMount{page.runtime, page.backend, ui::text(std::move(trail)), &page.frame->headSlot()};
        page.frame->setHeadShown(true);
        static_cast<void>(page.loop.runUntilIdle());
        page.backend.fit();
        page.screen.draw();
        std::size_t const title = rowOf(page.screen, "Review");
        std::size_t const actions = rowOf(page.screen, "ACTIONS");
        REQUIRE(title != std::string::npos);
        REQUIRE(actions != std::string::npos);
        INFO("columns " << columns << ": title row " << title << ", actions row " << actions);
        CHECK((actions == title) == together);
    }
}

// Control: give the lead the in-page head's 32-unit minimum (the head's trail then wraps under the title).
TEST_CASE("tui shell frame: a short lead and a short trail stay on the title's line in a narrow bar", "[tui][shell]") {
    FramedPage page{40};
    ui::Text badge{.text = std::string{"Connected"}};
    ui::Row lead{.children = {ui::text(std::move(badge))}, .gap = 1};
    lead.common.layout.width = ui::Sizing::stretch();
    ui::Text scan{.text = std::string{"SCAN"}};
    ui::Mounted const leadMount{page.runtime, page.backend, ui::row(std::move(lead)), &page.frame->headLeadSlot()};
    ui::Mounted const trailMount{page.runtime, page.backend, ui::text(std::move(scan)), &page.frame->headSlot()};
    page.frame->setHeadShown(true);
    static_cast<void>(page.loop.runUntilIdle());
    page.backend.fit();
    page.screen.draw();
    std::size_t const title = rowOf(page.screen, "Review");
    std::size_t const scanRow = rowOf(page.screen, "SCAN");
    REQUIRE(title != std::string::npos);
    REQUIRE(scanRow != std::string::npos);
    CHECK(scanRow == title);
}

// Control: make `setPageInset` a no-op (the page's text keeps its row and column of inset).
TEST_CASE("tui shell frame: without the page's inset the content starts at the page's first row and column",
          "[tui][shell]") {
    FramedPage page{60};
    std::size_t const inset = page.pageColumn();
    std::size_t const insetRow = rowOf(page.screen, "PAGE");
    page.frame->setPageInset(false);
    static_cast<void>(page.loop.runUntilIdle());
    page.backend.fit();
    page.screen.draw();
    CHECK(page.pageColumn() < inset);
    CHECK(rowOf(page.screen, "PAGE") < insetRow);
}

// Control: put the trail's lead under the title (the badge is then on the next row), or leave the inline slot hidden
// (the inline lead is not drawn).
TEST_CASE("tui shell frame: a lead at the end and an inline lead are drawn on the title's row", "[tui][shell]") {
    FramedPage page{120};
    // The trail as the workspace builds it for an `end` lead: the tool, the lead, the action, in one row.
    ui::Text tool{.text = std::string{"TOOL"}};
    ui::Text badge{.text = std::string{"BADGE"}};
    ui::Text action{.text = std::string{"ACTION"}};
    ui::Mounted const trailMount{page.runtime, page.backend,
                                 ui::row(ui::Row{.children = {ui::text(std::move(tool)), ui::text(std::move(badge)),
                                                              ui::text(std::move(action))},
                                                 .gap = 1}),
                                 &page.frame->headSlot()};
    ui::Text subtitle{.text = std::string{"SUBTITLE"}};
    ui::Mounted const inlineMount{page.runtime, page.backend, ui::text(std::move(subtitle)),
                                  &page.frame->headInlineSlot()};
    page.frame->setLeadInline(true);
    page.frame->setHeadShown(true);
    static_cast<void>(page.loop.runUntilIdle());
    page.backend.fit();
    page.screen.draw();
    std::size_t const title = rowOf(page.screen, "Review");
    REQUIRE(title != std::string::npos);
    CHECK(rowOf(page.screen, "BADGE") == title);
    CHECK(rowOf(page.screen, "SUBTITLE") == title);
    std::string const row = rowsOf(page.screen).at(title);
    CHECK(row.find("TOOL") < row.find("BADGE"));
    CHECK(row.find("BADGE") < row.find("ACTION"));
    CHECK(row.find("Review") < row.find("SUBTITLE"));
}

// Control: the old tree, with the inline slot beside the lead block in the top line (the trail is then the item that
// drops, and ACTION is below the title's row).
TEST_CASE("tui shell frame: an inline lead too wide for the title's row wraps under the title, the trail stays",
          "[tui][shell]") {
    FramedPage page{80};
    page.frame->setTitle("Review of a long plate title");
    ui::Text action{.text = std::string{"ACTION"}};
    ui::Mounted const trailMount{page.runtime, page.backend, ui::text(std::move(action)), &page.frame->headSlot()};
    ui::Text subtitle{.text = std::string{"SUBTITLE that is long enough not to fit beside the title"}};
    ui::Mounted const inlineMount{page.runtime, page.backend, ui::text(std::move(subtitle)),
                                  &page.frame->headInlineSlot()};
    page.frame->setLeadInline(true);
    page.frame->setHeadShown(true);
    static_cast<void>(page.loop.runUntilIdle());
    page.backend.fit();
    page.screen.draw();
    std::size_t const title = rowOf(page.screen, "Review of a long");
    REQUIRE(title != std::string::npos);
    CHECK(rowOf(page.screen, "ACTION") == title);
    CHECK(rowOf(page.screen, "SUBTITLE") > title);
}

// Control: the principal outside the trail, in the top line after it (it is then not after the action's last cell
// by the trail's gap, and with an empty slot the gap before it shows).
TEST_CASE("tui shell frame: the principal is drawn on the trail's row after the action, with no gap for an empty slot",
          "[tui][shell]") {
    FramedPage page{160};
    page.frame->setPrincipalInHead(true);
    page.frame->setPrincipal("DrMeyer");
    ui::Text action{.text = std::string{"ACTIONS"}};
    ui::Mounted const trailMount{page.runtime, page.backend, ui::text(std::move(action)), &page.frame->headSlot()};
    page.frame->setHeadShown(true);
    static_cast<void>(page.loop.runUntilIdle());
    page.backend.fit();
    page.screen.draw();
    std::size_t const title = rowOf(page.screen, "Review");
    REQUIRE(title != std::string::npos);
    REQUIRE(rowOf(page.screen, "ACTIONS") == title);
    REQUIRE(rowOf(page.screen, "DrMeyer") == title);
    std::string const row = rowsOf(page.screen).at(title);
    CHECK(row.find("ACTIONS") < row.find("DrMeyer"));

    // Nothing mounted in a shown slot: the principal starts where the trail does, and where it does with the slot
    // hidden.
    FramedPage empty{160};
    empty.frame->setPrincipalInHead(true);
    empty.frame->setPrincipal("DrMeyer");
    empty.frame->setHeadShown(true);
    static_cast<void>(empty.loop.runUntilIdle());
    empty.backend.fit();
    empty.screen.draw();
    std::size_t const shownAt = rowsOf(empty.screen).at(rowOf(empty.screen, "DrMeyer")).find("DrMeyer");
    empty.frame->setHeadShown(false);
    static_cast<void>(empty.loop.runUntilIdle());
    empty.backend.fit();
    empty.screen.draw();
    CHECK(rowsOf(empty.screen).at(rowOf(empty.screen, "DrMeyer")).find("DrMeyer") == shownAt);
}

// Control: map step 1 to a row (the compact bar's title would move down from row 0), or draw the end inset as a
// column of its own (the action leaves the title's row at 40 columns).
TEST_CASE("tui shell frame: the compact bar's padding is no row, and the end inset keeps the trail on the title's row",
          "[tui][shell]") {
    FramedPage page{40};
    CHECK(page.backend.widthClass() == "compact");
    ui::Text action{.text = std::string{"SCAN"}};
    ui::Mounted const trailMount{page.runtime, page.backend, ui::text(std::move(action)), &page.frame->headSlot()};
    page.frame->setHeadShown(true);
    // An end that pads itself has no inset: the trail's end is where the line's is.
    page.frame->setTrailEnd(morph::ui::HeadTrailEnd::Unframed);
    static_cast<void>(page.loop.runUntilIdle());
    page.backend.fit();
    page.screen.draw();
    std::size_t const title = rowOf(page.screen, "Review");
    CHECK(title == 0);
    std::string const before = rowsOf(page.screen).at(title);

    // A framed end also takes the larger bar padding, step 3, which is a row above the line on the terminal: the
    // title and the trail move down together and stay on one row.
    page.frame->setTrailEnd(morph::ui::HeadTrailEnd::Framed);
    static_cast<void>(page.loop.runUntilIdle());
    page.backend.fit();
    page.screen.draw();
    std::size_t const padded = rowOf(page.screen, "Review");
    REQUIRE(padded == title + 1);
    REQUIRE(rowOf(page.screen, "SCAN") == padded);
    std::string const after = rowsOf(page.screen).at(padded);
    CHECK(after.find("SCAN") < before.find("SCAN"));
}

// Control: the frame draws no part (a default view), or the terminal drops the switcher's detail.
TEST_CASE("tui shell frame: the declared parts are drawn, each usable on the terminal's rows", "[tui][shell]") {
    FramedPage wide{160};
    ui::ShellPartsView view;
    view.context =
        ui::ShellContextView{.shown = true,
                             .options = {ui::ShellOptionView{.label = "Koeln", .detail = "EUR", .group = {}}},
                             .chosen = 0,
                             .enabled = true};
    view.search = ui::ShellSearchView{.shown = true, .hint = "Search samples", .sources = {}};
    view.inbox = ui::ShellInboxView{.shown = true, .title = "Approvals", .count = 7, .failed = false};
    view.principal = ui::ShellPrincipalView{.shown = true, .name = "Anna Meyer", .subtitle = "Lab director"};
    wide.frame->setParts(view);
    static_cast<void>(wide.loop.runUntilIdle());
    wide.backend.fit();
    wide.screen.draw();
    std::vector<std::string> const rows = rowsOf(wide.screen);
    auto const shown = [&rows](std::string const& text) {
        return std::ranges::any_of(rows, [&text](std::string const& row) { return row.contains(text); });
    };
    CHECK(shown("Koeln · EUR"));
    CHECK(shown("Search samples (Ctrl K)"));
    CHECK(shown("Approvals"));
    CHECK(shown("7"));
    CHECK(shown("Anna Meyer · Lab director"));

    // The results are the rows under the field, in flow, and a hit is a row.
    ui::ShellPartsView found = view;
    found.search.sources = {
        ui::ShellSourceView{.title = "Lab",
                            .failed = false,
                            .pending = false,
                            .hits = {ui::ShellHitView{.kind = "Sample", .title = "S-7", .detail = {}}}}};
    wide.frame->setParts(found);
    wide.frame->setSearchText("S-");
    static_cast<void>(wide.loop.runUntilIdle());
    wide.backend.fit();
    wide.screen.draw();
    std::vector<std::string> const after = rowsOf(wide.screen);
    CHECK(std::ranges::any_of(after,
                              [](std::string const& row) { return row.contains("Sample") && row.contains("S-7"); }));
}
