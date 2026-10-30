// SPDX-License-Identifier: Apache-2.0
//
// The shell frame (docs/spec/ui/shell_frame.md): the sidebar with the application's name, the navigator, the
// principal and sign-out, the top bar with the title and the head, and the page; signed out, only the page.

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

    // The principal and the sign-out button sit at the sidebar's foot, below the navigator.
    int const side = backend.children(sidebar).at(1);
    int const navigator = backend.find("Column", "testId", "navigator").value();
    int const principalText = backend.find("Text", "testId", "shell_principal").value();
    int const signOutButton = backend.find("Button", "testId", "shell_signOut").value();
    std::vector<int> const foot = backend.children(side);
    auto const position = [&foot](int widget) { return std::ranges::find(foot, widget) - foot.begin(); };
    CHECK(position(navigator) < position(principalText));
    CHECK(position(principalText) + 1 == position(signOutButton));
    CHECK(backend.prop(navigator, "layout") == "content/stretch(1)");

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

// Mutation: put the head slots back on a line of their own (outside `shell_topbar_main`), start them shown, start the
// top line without the wrap before its children, or leave the lead block without its minimum.
TEST_CASE("shell frame: the head shares the title's wrapping line, its slots shown only when the client says",
          "[ui][shell]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    int signOuts = 0;
    ShellFrame frame{backend, nullptr, frameOptions(opened, signOuts)};
    int const main = backend.find("Row", "testId", "shell_topbar_main").value();
    int const lead = backend.find("Column", "testId", "shell_head_lead").value();
    int const eyebrow = backend.find("Column", "testId", "shell_head_eyebrow").value();
    int const slot = backend.find("Row", "testId", "shell_head").value();
    int const title = backend.find("Text", "testId", "shell_title").value();
    int const menu = backend.find("Menu", "testId", "shell_nav_menu").value();
    // The second line and its scroll are gone.
    CHECK_FALSE(backend.find("Row", "testId", "shell_head_line").has_value());
    CHECK_FALSE(backend.find("Scroll", "testId", "shell_head_scroll").has_value());
    // The wrapping row holds the menu, the lead block (the title over the lead slot) and the trail slot.
    CHECK(backend.prop(main, "wrap") == "true");
    CHECK(backend.prop(main, "justify") == "end");
    REQUIRE(backend.children(main).size() == 3);
    CHECK(backend.children(main).at(0) == menu);
    int const block = backend.children(main).at(1);
    CHECK(backend.prop(block, "layout") == "stretch(1) min=32/content");
    CHECK(backend.children(block) == std::vector<int>{eyebrow, title, lead});
    CHECK(backend.children(main).at(2) == slot);
    // The pad spacers are outside the wrapping row, so every line of it is padded at both ends.
    CHECK(within(backend, backend.find("Row", "testId", "shell_topbar_line").value(), main));
    CHECK_FALSE(backend.hasProp(backend.find("Row", "testId", "shell_topbar_line").value(), "wrap"));

    CHECK(backend.prop(lead, "visible") == "false");
    CHECK(backend.prop(slot, "visible") == "false");
    CHECK(backend.prop(eyebrow, "visible") == "false");
    frame.setHeadShown(true);
    CHECK(backend.prop(lead, "visible") == "true");
    CHECK(backend.prop(slot, "visible") == "true");
    CHECK(backend.prop(eyebrow, "visible") == "true");
    frame.setHeadShown(false);
    CHECK(backend.prop(lead, "visible") == "false");
    CHECK(backend.prop(slot, "visible") == "false");
    CHECK(backend.prop(eyebrow, "visible") == "false");
}

// Control: route the last index to `Navigator::choose`, and sign-out is not called.
TEST_CASE("shell frame: compact, the navigation menu ends with the principal's sign-out entry", "[ui][shell]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    int signOuts = 0;
    ShellFrame frame{backend, nullptr, frameOptions(opened, signOuts)};
    frame.setPrincipal("alice");
    frame.setCompact(true);
    int const menu = backend.find("Menu", "testId", "shell_nav_menu").value();
    std::string const items = backend.prop(menu, "items");
    CHECK(items.contains("Sign out"));
    CHECK(items.contains("alice"));
    // Entries: the group, then a separator and the sign-out entry; the navigator's own path 0 is the group.
    std::size_t const last = frame.navigator().menuEntries().size() + 1;
    backend.chooseMenuEntry(menu, {last});
    CHECK(signOuts == 1);
    CHECK(opened.empty());
    backend.chooseMenuEntry(menu, {0, 1});
    CHECK(signOuts == 1);
    CHECK(opened == std::vector<std::string>{"bookmarks.tags"});
}

// Control: a frame without a sign-out handler lists no sign-out entry and shows no principal button.
TEST_CASE("shell frame: without a sign-out handler there is no sign-out button and no sign-out entry", "[ui][shell]") {
    RecordingBackend backend;
    ShellFrameOptions options;
    options.applicationName = "App";
    options.navigator.entries = {CatalogEntry{.id = "a.b", .title = "B", .group = "G"}};
    ShellFrame frame{backend, nullptr, std::move(options)};
    frame.setPrincipal("alice");
    frame.setCompact(true);
    CHECK(backend.prop(backend.find("Button", "testId", "shell_signOut").value(), "visible") == "false");
    CHECK_FALSE(backend.prop(backend.find("Menu", "testId", "shell_nav_menu").value(), "items").contains("Sign out"));
}

// Control: a no-op setCompact (the sidebar stays shown and the navigation menu hidden); or leave the menu's items
// stale after a count (its first entry keeps the label without it).
TEST_CASE("shell frame: compact, the sidebar and the principal give way to a navigation menu", "[ui][shell]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    int signOuts = 0;
    ShellFrame frame{backend, nullptr, frameOptions(opened, signOuts)};
    frame.showSignedIn(true);
    int const sidebar = backend.find("Row", "testId", "shell_sidebar").value();
    int const menu = backend.find("Menu", "testId", "shell_nav_menu").value();
    CHECK(backend.prop(menu, "visible") == "false");
    CHECK(backend.prop(menu, "icon") == "nav.menu");
    CHECK(backend.prop(menu, "a11yName") == "Navigation");
    // First in the top bar's wrapping line.
    int const line = backend.find("Row", "testId", "shell_topbar_main").value();
    CHECK(backend.children(line).at(0) == menu);

    frame.setCompact(true);
    CHECK(frame.compact());
    CHECK(backend.prop(sidebar, "visible") == "false");
    CHECK(backend.prop(menu, "visible") == "true");
    frame.navigator().setBadge("bookmarks.tags", morph::ui::NavigatorBadge{.text = "2", .tone = {}});
    CHECK(backend.prop(menu, "items").contains("Tags · 2"));
    backend.chooseMenuEntry(menu, {0, 1});
    CHECK(opened == std::vector<std::string>{"bookmarks.tags"});

    // Signed out, the top bar goes, and the sidebar stays hidden when the frame is no longer compact.
    frame.showSignedIn(false);
    frame.setCompact(false);
    CHECK(backend.prop(sidebar, "visible") == "false");
    frame.showSignedIn(true);
    CHECK(backend.prop(sidebar, "visible") == "true");
    CHECK(backend.prop(menu, "visible") == "false");
}

// Control: ignore the runs (`shell_title` keeps the plain title), or keep them after they are cleared.
TEST_CASE("shell frame: the title draws a screen's runs in place of its plain title, until they are cleared",
          "[ui][shell]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    int signOuts = 0;
    ShellFrame frame{backend, nullptr, frameOptions(opened, signOuts)};
    int const title = backend.find("Text", "testId", "shell_title").value();
    frame.setTitle("Plate PL-0193");
    CHECK(backend.prop(title, "text") == "Plate PL-0193");
    CHECK_FALSE(backend.hasProp(title, "spans"));

    frame.setTitleSpans({{.text = "Plate "}, {.text = "PL-0193", .mono = true}});
    CHECK(backend.prop(title, "spans") == "[Plate ,PL-0193;mono]");
    // A new caption while the runs show does not replace them.
    frame.setTitle("Plate PL-0200");
    CHECK(backend.prop(title, "spans") == "[Plate ,PL-0193;mono]");

    frame.setTitleSpans({});
    CHECK(backend.prop(title, "text") == "Plate PL-0200");
}

// Control: make `setPageInset` a no-op (the spacers stay shown).
TEST_CASE("shell frame: a frame insets its page until it is told the screen insets itself", "[ui][shell]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    int signOuts = 0;
    ShellFrame frame{backend, nullptr, frameOptions(opened, signOuts)};
    int const page = backend.find("Column", "testId", "shell_page").value();
    REQUIRE(backend.children(page).size() == 2);
    int const top = backend.children(page).at(0);
    int const padded = backend.children(page).at(1);
    REQUIRE(backend.children(padded).size() == 3);
    int const left = backend.children(padded).at(0);
    int const right = backend.children(padded).at(2);

    CHECK_FALSE(backend.hasProp(top, "visible"));
    frame.setPageInset(false);
    for (int const spacer : {top, left, right}) {
        CHECK(backend.prop(spacer, "visible") == "false");
    }
    frame.setPageInset(true);
    for (int const spacer : {top, left, right}) {
        CHECK(backend.prop(spacer, "visible") == "true");
    }
}

// Control: give the heading the `Section` role (the recording reads `Section`).
TEST_CASE("shell frame: the navigator's group headings are overlines", "[ui][shell]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    int signOuts = 0;
    ShellFrame frame{backend, nullptr, frameOptions(opened, signOuts)};
    int const heading = backend.find("Text", "testId", "navigator_group_/Library").value();
    CHECK(backend.prop(heading, "role") == "Overline");
}

// Mutation: leave the gaps alone (the compact top line keeps steps 3 and 3).
TEST_CASE("shell frame: the compact top line pads its ends by step 1 and spaces its items by step 2", "[ui][shell]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    int signOuts = 0;
    ShellFrame frame{backend, nullptr, frameOptions(opened, signOuts)};
    int const line = backend.find("Row", "testId", "shell_topbar_line").value();
    int const main = backend.find("Row", "testId", "shell_topbar_main").value();
    CHECK(backend.prop(line, "gap") == "3");
    CHECK(backend.prop(main, "gap") == "3");

    frame.setCompact(true);
    CHECK(backend.prop(line, "gap") == "1");
    CHECK(backend.prop(main, "gap") == "2");

    frame.setCompact(false);
    CHECK(backend.prop(line, "gap") == "3");
    CHECK(backend.prop(main, "gap") == "3");
}
