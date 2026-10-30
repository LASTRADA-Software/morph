// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "gallery_harness.hpp"
#include "lims_gallery/screens.hpp"

using lims_gallery::testing::GalleryScreen;

namespace {

constexpr char const* kScreen = "lims-gallery.review";

// The queue entry of @p worksheet.
int queueEntry(GalleryScreen& screen, std::string const& worksheet) {
    return screen.backend().find("Button", "label", worksheet).value_or(0);
}

// The workspace's tab bar: the shell's, which holds every open screen.
std::string workspaceTabs(GalleryScreen& screen) {
    auto const bars = screen.backend().all("Tabs");
    return bars.empty() ? std::string{} : screen.backend().prop(bars.front(), "tabs");
}

// Whether the shell insets the page: the first of its three inset spacers shows.
bool pageInset(GalleryScreen& screen) {
    int const page = screen.find("Column", "shell_page").value();
    return screen.backend().prop(screen.backend().children(page).at(0), "visible") != "false";
}

}  // namespace

TEST_CASE("review, expanded: the queue beside the worksheet, its flagged results", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.requireNoProblems();
    CHECK(screen.shows("Splitter", "split"));
    CHECK(screen.text("queue-title") == "Awaiting review · 4");
    CHECK(screen.backend().prop(queueEntry(screen, "WS-2026-0412 · 2 h 10 min"), "selected") == "true");
    CHECK(screen.text("sheet-title") == "Assay and impurities · Paracetamol 500 mg");
    CHECK(screen.prop("Select", "show", "options") == "[\"all\":All 7,\"flagged\":Flagged 2]");
    CHECK(screen.backend().find("Badge", "text", "Out of trend: −0.7 % vs. last 5 lots").has_value());
    CHECK(screen.prop("Tabs", "record-tabs", "tabs") == "[Results,Previous lots,Audit trail · 3,Comments · 1]");
    CHECK_FALSE(screen.shows("Banner", "self-review"));
    CHECK(screen.prop("Button", "approve", "enabled") == "true");
}

TEST_CASE("review: self-review is blocked with its reason", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.backend().click(queueEntry(screen, "WS-2026-0409 · 1 h 05 min"));
    CHECK(screen.waitFor([&screen] { return screen.shows("Banner", "self-review"); }));
    CHECK(screen.prop("Button", "approve", "enabled") == "false");
    CHECK(screen.text("approve-note") == "Another reviewer must approve");
    // Picking another worksheet mounts its own tabs, whose labels count its entries.
    CHECK(screen.prop("Tabs", "record-tabs", "tabs") == "[Results,Previous lots,Audit trail · 0,Comments · 0]");
    screen.requireNoProblems();
}

TEST_CASE("review: returning a worksheet needs a reason, and takes it out of the queue", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.click("return");
    CHECK(screen.waitFor([&screen] { return screen.prop("Dialog", "decision", "open") == "true"; }));
    CHECK(screen.prop("Dialog", "decision", "title") == "Return WS-2026-0412 to the analyst");
    CHECK(screen.prop("Button", "action_confirm", "enabled") == "false");
    screen.edit("decision-reason", "Attach the reintegrated chromatogram to the worksheet as well.");
    CHECK(screen.waitFor([&screen] { return screen.prop("Button", "action_confirm", "enabled") == "true"; }));
    screen.click("action_confirm");
    CHECK(screen.waitForText("queue-title", "Awaiting review · 3"));
    CHECK(screen.text("sheet-title") == "Water (KF) · Lactose monohydrate");
    CHECK(screen.prop("Dialog", "decision", "open") == "false");
    screen.requireNoProblems();
}

// `sizes` is written into the document and reaches the splitter.
TEST_CASE("review: the splitter's sizes reach the splitter", "[lims-gallery]") {
    auto const written = lims_gallery::reviewScreen().write();
    REQUIRE(written.has_value());
    auto const text = written.value_or(morph::ui::author::Emitted{}).text;
    CHECK(text.contains(R"("sizes":{"lit":[29,74]})"));
    GalleryScreen screen{kScreen, "expanded"};
    CHECK(screen.prop("Splitter", "split", "sizes") == "[29,74]");
}

// The review's `navigate` asks for a dialog, and the generic client draws one over the review: no tab is added. The
// signature answers with its meaning, which marks the worksheet signed.
TEST_CASE("review: approve and sign opens the e-signature over the review, and its signature answers it",
          "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    CHECK_FALSE(screen.shows("Banner", "signed"));
    screen.click("approve");
    CHECK(screen.waitFor(
        [&screen] { return screen.backend().find("Dialog", "title", "Sign to approve results").has_value(); }));
    CHECK(workspaceTabs(screen) == "[Review and approval]");
    CHECK(screen.mounted().size() == 1);
    CHECK(screen.find("Column", "fields").has_value());
    CHECK(screen.prop("Button", "overlay_action_sign", "enabled") == "true");
    screen.edit("password", "meyer-2026");
    screen.click("overlay_action_sign");
    CHECK(screen.waitFor([&screen] { return screen.backend().all("Dialog").size() == 1; }));
    CHECK(screen.waitFor([&screen] { return screen.shows("Banner", "signed"); }));
    CHECK(screen.prop("Banner", "signed", "text") == "WS-2026-0412 signed: approved.");
    screen.requireNoProblems();
}

TEST_CASE("review: cancelling the signature leaves the worksheet unsigned", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.click("approve");
    REQUIRE(screen.waitFor(
        [&screen] { return screen.backend().find("Dialog", "title", "Sign to approve results").has_value(); }));
    screen.click("overlay_action_cancel");
    CHECK(screen.waitFor(
        [&screen] { return !screen.backend().find("Dialog", "title", "Sign to approve results").has_value(); }));
    CHECK_FALSE(screen.shows("Banner", "signed"));
    screen.requireNoProblems();
}

TEST_CASE("review, compact: the worksheet as its own page, the queue behind it", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    screen.requireNoProblems();
    CHECK_FALSE(screen.find("Splitter", "split").has_value());
    CHECK(screen.prop("Button", "back", "label") == "‹ Awaiting review · 4");
    CHECK(screen.prop("Tabs", "record-tabs", "tabs") == "[Results,Previous,Audit 3,Comments 1]");
    CHECK(screen.find("Column", "results").has_value());
    // The out-of-specification result's card has the tint its table row has.
    CHECK(screen.backend().find("Column", "surface", "outOfSpec").has_value());
    screen.click("back");
    CHECK(screen.waitFor([&screen] { return screen.find("Column", "queue").has_value(); }));
    screen.backend().click(queueEntry(screen, "WS-2026-0405 · 48 min"));
    CHECK(screen.waitFor(
        [&screen] { return screen.prop("Text", "shell_title", "spans") == "[Dissolution · Ibuprofen 400 mg]"; }));
    screen.requireNoProblems();
}

// Mutation: drop `pageEdge` from the expanded root (the shell insets the page again, and the panes stop short of the
// window's edges), or the queue's divided entries (they are padded back from the pane's edges).
TEST_CASE("review, expanded: the splitter fills the page, with the queue's entries edge to edge", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "expanded"};
    screen.requireNoProblems();
    CHECK(screen.waitFor([&screen] { return !pageInset(screen); }));
    // The queue pane's head is padded by step 3, and its entries are a divided list with no padding of their own.
    int const queue = screen.find("Column", "queue").value();
    CHECK(screen.backend().prop(queue, "divided") == "true");
    CHECK_FALSE(screen.backend().hasProp(queue, "padding"));
    CHECK(screen.backend().find("Column", "padding", "3").has_value());
}

// Mutation: leave the compact roots without `pageEdge` (the shell keeps its inset around the pinned worksheet).
TEST_CASE("review, compact: the pinned worksheet and queue pages take the page's edges", "[lims-gallery]") {
    GalleryScreen screen{kScreen, "compact"};
    screen.requireNoProblems();
    CHECK(screen.waitFor([&screen] { return !pageInset(screen); }));
    screen.click("back");
    REQUIRE(screen.waitFor([&screen] { return screen.find("Column", "queue").has_value(); }));
    CHECK_FALSE(pageInset(screen));
}
