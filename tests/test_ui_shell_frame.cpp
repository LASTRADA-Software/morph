// SPDX-License-Identifier: Apache-2.0
//
// The shell frame (docs/spec/ui/shell_frame.md): the sidebar with the application's name and the navigator, the top
// bar with the title, the principal and sign-out, and the page; signed out, only the page.

#include <catch2/catch_test_macros.hpp>
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
    int const topBar = backend.find("Row", "testId", "shell_topbar").value();
    int const page = backend.find("Column", "testId", "shell_page").value();
    CHECK(backend.prop(sidebar, "surface") == "sidebar");
    CHECK(backend.prop(sidebar, "layout") == "fixed(28)/stretch(1)");
    CHECK(backend.prop(topBar, "surface") == "toolbar");
    CHECK(backend.prop(page, "surface") == "page");
    CHECK(backend.prop(backend.find("Text", "testId", "shell_title").value(), "text") == "Bookmarks");
    CHECK(backend.prop(backend.find("Text", "testId", "shell_principal").value(), "text") == "alice");
    CHECK(backend.prop(backend.find("Text", "testId", "shell_application").value(), "text") == "Morph Bookmarks");

    backend.click(backend.find("Button", "testId", "nav_bookmarks.tags").value());
    CHECK(opened == std::vector<std::string>{"bookmarks.tags"});
    backend.click(backend.find("Button", "testId", "shell_signOut").value());
    CHECK(signOuts == 1);
}

// Mutation: hide only the sidebar (the top bar stays shown signed out).
TEST_CASE("shell frame: signed out, the sidebar and the top bar are hidden", "[ui][shell]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    int signOuts = 0;
    ShellFrame frame{backend, nullptr, frameOptions(opened, signOuts)};
    frame.showSignedIn(false);
    CHECK(backend.prop(backend.find("Row", "testId", "shell_sidebar").value(), "visible") == "false");
    CHECK(backend.prop(backend.find("Row", "testId", "shell_topbar").value(), "visible") == "false");
    CHECK_FALSE(backend.hasProp(backend.find("Column", "testId", "shell_page").value(), "visible"));
    frame.showSignedIn(true);
    CHECK(backend.prop(backend.find("Row", "testId", "shell_sidebar").value(), "visible") == "true");
}
