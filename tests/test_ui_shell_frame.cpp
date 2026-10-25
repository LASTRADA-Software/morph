// SPDX-License-Identifier: Apache-2.0
//
// The shell frame (docs/spec/ui/shell_frame.md): the sidebar with the application's name and the navigator, the top
// bar with the title, the principal and sign-out, and the page; signed out, only the page.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <iterator>
#include <morph/ui/bundle.hpp>
#include <morph/ui/shell_frame.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <string>
#include <vector>

using morph::ui::CatalogEntry;
using morph::ui::ShellFrame;
using morph::ui::ShellFrameOptions;
using morph::ui::testing::RecordingBackend;

namespace {

ShellFrameOptions frameOptions(std::vector<std::string>& opened, int& signOuts) {
    ShellFrameOptions options;
    options.applicationName = "Morph Bookmarks";
    options.navigator.entries = {CatalogEntry{.id = "bookmarks.main", .title = "Bookmarks", .group = "Library"},
                                 CatalogEntry{.id = "bookmarks.tags", .title = "Tags", .group = "Library"}};
    options.navigator.onOpen = [&opened](std::string const& id) { opened.push_back(id); };
    options.onSignOut = [&signOuts] { ++signOuts; };
    return options;
}

}  // namespace

// Mutation: put the top bar in the sidebar (its surface reads `sidebar` above the title).
TEST_CASE("shell frame: a sidebar with the navigator, a top bar and the page, on their surfaces", "[ui][shell]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    int signOuts = 0;
    ShellFrame frame{backend, nullptr, frameOptions(opened, signOuts)};
    frame.setTitle("Bookmarks");
    frame.setPrincipal("alice");

    int const sidebar = backend.find("Row", "testId", "shell_sidebar").value();
    int const topBar = backend.find("Column", "testId", "shell_topbar").value();
    int const page = backend.find("Column", "testId", "shell_page").value();
    CHECK(backend.prop(sidebar, "surface") == "sidebar");
    CHECK(backend.prop(sidebar, "layout") == "fixed(28)/stretch(1)");
    CHECK(backend.prop(topBar, "surface") == "toolbar");
    CHECK(backend.prop(page, "surface") == "page");
    CHECK(backend.prop(backend.find("Text", "testId", "shell_title").value(), "text") == "Bookmarks");
    CHECK(backend.prop(backend.find("Text", "testId", "shell_principal").value(), "text") == "alice");
    CHECK(backend.prop(backend.find("Text", "testId", "shell_application").value(), "text") == "Morph Bookmarks");

    // The head slot (#1027): its own stretch, independent of whatever is mounted inside it, so it keeps claiming
    // room in the top bar even while an active tab's head row is hidden.
    int const headSlot = backend.find("Row", "testId", "shell_head").value();
    CHECK(backend.prop(headSlot, "layout") == "stretch(1)/content");

    backend.click(backend.find("Button", "testId", "nav_bookmarks.tags").value());
    CHECK(opened == std::vector<std::string>{"bookmarks.tags"});
    backend.click(backend.find("Button", "testId", "shell_signOut").value());
    CHECK(signOuts == 1);
}

// Mutation: never show the toast the first time (showToast is a no-op), or stop restarting its dismiss interval on
// a second call (its shown count stops advancing).
TEST_CASE("shell frame: showToast makes the toast widget on first use, under the main content area", "[ui][shell]") {
    using morph::ui::Tone;
    RecordingBackend backend;
    std::vector<std::string> opened;
    int signOuts = 0;
    ShellFrame frame{backend, nullptr, frameOptions(opened, signOuts)};
    CHECK(backend.all("Toast").empty());
    frame.showToast("Saved", Tone::Ok);
    auto const toasts = backend.all("Toast");
    REQUIRE(toasts.size() == 1);
    int const toast = toasts.front();
    CHECK(backend.prop(toast, "text") == "Saved");
    CHECK(backend.prop(toast, "tone") == "Ok");
    CHECK(backend.prop(toast, "shown") == "1");
    // A second call while the first is still showing replaces its text and tone, and restarts the interval (the
    // shown count advances again) rather than being a no-op.
    frame.showToast("Saved again", Tone::Warn);
    CHECK(backend.all("Toast").size() == 1);  // made once, not once per call
    CHECK(backend.prop(toast, "text") == "Saved again");
    CHECK(backend.prop(toast, "tone") == "Warn");
    CHECK(backend.prop(toast, "shown") == "2");
}

// Mutation: hide only the sidebar (the top bar stays shown signed out).
TEST_CASE("shell frame: signed out, the sidebar and the top bar are hidden", "[ui][shell]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    int signOuts = 0;
    ShellFrame frame{backend, nullptr, frameOptions(opened, signOuts)};
    frame.showSignedIn(false);
    CHECK(backend.prop(backend.find("Row", "testId", "shell_sidebar").value(), "visible") == "false");
    CHECK(backend.prop(backend.find("Column", "testId", "shell_topbar").value(), "visible") == "false");
    CHECK_FALSE(backend.hasProp(backend.find("Column", "testId", "shell_page").value(), "visible"));
    frame.showSignedIn(true);
    CHECK(backend.prop(backend.find("Row", "testId", "shell_sidebar").value(), "visible") == "true");
}

namespace {

/// Whether @p node is @p ancestor or one of its descendants.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the ancestor, then the node, as a containment reads.
bool within(RecordingBackend const& backend, int ancestor, int node) {
    std::vector<int> pending{ancestor};
    while (!pending.empty()) {
        int const at = pending.back();
        pending.pop_back();
        if (at == node) {
            return true;
        }
        std::ranges::copy(backend.children(at), std::back_inserter(pending));
    }
    return false;
}

}  // namespace

// Mutation: put the head slot back in the title's line (it is then beside the sign-out button, outside
// `shell_head_line`), or start the head line shown.
TEST_CASE("shell frame: the head slot has the top bar's second line, shown only when the client says", "[ui][shell]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    int signOuts = 0;
    ShellFrame frame{backend, nullptr, frameOptions(opened, signOuts)};
    int const line = backend.find("Row", "testId", "shell_head_line").value();
    int const main = backend.find("Row", "testId", "shell_topbar_main").value();
    int const slot = backend.find("Row", "testId", "shell_head").value();
    int const signOut = backend.find("Button", "testId", "shell_signOut").value();
    CHECK(within(backend, line, slot));
    CHECK_FALSE(within(backend, main, slot));
    CHECK(within(backend, main, signOut));
    // The slot sits in a scroll along the line, so a head wider than the bar scrolls sideways.
    int const scroll = backend.find("Scroll", "testId", "shell_head_scroll").value();
    CHECK(within(backend, scroll, slot));

    CHECK(backend.prop(line, "visible") == "false");
    frame.setHeadShown(true);
    CHECK(backend.prop(line, "visible") == "true");
    frame.setHeadShown(false);
    CHECK(backend.prop(line, "visible") == "false");
}
