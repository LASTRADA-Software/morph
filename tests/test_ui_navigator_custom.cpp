// SPDX-License-Identifier: Apache-2.0
//
// The navigator's Custom section (docs/spec/ui/workspace.md, "Navigator and pins"): a person's custom screens after
// the application's groups, each marked, naming who saved it when that was someone else, and badged when broken.

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/navigator.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <string>
#include <vector>

using morph::ui::CatalogEntry;
using morph::ui::Navigator;
using morph::ui::NavigatorCustomEntry;
using morph::ui::NavigatorOptions;
using morph::ui::testing::RecordingBackend;

namespace {

NavigatorOptions options(std::vector<std::string>& opened) {
    NavigatorOptions made;
    made.entries = {CatalogEntry{.id = "notes.home", .title = "Home", .group = "Notes"}};
    made.onOpen = [&opened](std::string const& id) { opened.push_back(id); };
    return made;
}

std::vector<NavigatorCustomEntry> customs() {
    return {
        NavigatorCustomEntry{.id = "custom:aa", .title = "Mine", .savedBy = "ann", .mine = true, .broken = {}},
        NavigatorCustomEntry{.id = "custom:bb", .title = "Shared", .savedBy = "bob", .mine = false, .broken = {}},
        NavigatorCustomEntry{.id = "custom:cc",
                             .title = "Old",
                             .savedBy = "ann",
                             .mine = true,
                             .broken = "the action Lims_Verify is not permitted"},
    };
}

int widget(RecordingBackend const& backend, std::string const& kind, std::string const& testId) {
    auto const found = backend.find(kind, "testId", testId);
    REQUIRE(found.has_value());
    return *found;
}

}  // namespace

// Mutations: show the author of every entry, mine included (`nav_author_custom:aa` shows); leave the section shown
// when the list is emptied.
TEST_CASE("navigator: custom screens are a section after the groups, marked, with their author and breakage",
          "[ui][navigator][custom-screens]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    Navigator navigator{backend, nullptr, options(opened)};
    int const section = widget(backend, "Column", "navigator_custom_section");
    CHECK(backend.prop(section, "visible") == "false");

    navigator.setCustom(customs());
    CHECK(backend.prop(section, "visible") == "true");
    CHECK(backend.children(widget(backend, "Column", "navigator")).back() == section);
    CHECK(backend.prop(widget(backend, "Text", "navigator_custom"), "text") == "Custom");
    for (std::string const id : {"custom:aa", "custom:bb", "custom:cc"}) {
        CHECK(backend.prop(widget(backend, "Badge", "nav_mark_" + id), "text") == "Custom");
    }
    CHECK(backend.prop(widget(backend, "Text", "nav_author_custom:aa"), "visible") == "false");
    int const author = widget(backend, "Text", "nav_author_custom:bb");
    CHECK(backend.prop(author, "visible") == "true");
    CHECK(backend.prop(author, "text") == "by bob");
    CHECK(backend.prop(widget(backend, "Badge", "nav_broken_custom:cc"), "visible") == "true");
    CHECK(backend.prop(widget(backend, "Button", "nav_custom:cc"), "tooltip") ==
          "the action Lims_Verify is not permitted");
    CHECK(backend.prop(widget(backend, "Badge", "nav_broken_custom:aa"), "visible") == "false");

    navigator.setCustom({});
    CHECK(backend.prop(section, "visible") == "false");
}

// Mutation: drop the custom branch of `choose` (the menu's custom entry opens nothing).
TEST_CASE("navigator: a custom entry opens by its id from the sidebar and from the menu",
          "[ui][navigator][custom-screens]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    Navigator navigator{backend, nullptr, options(opened)};
    navigator.setCustom(customs());
    backend.click(widget(backend, "Button", "nav_custom:bb"));
    CHECK(opened == std::vector<std::string>{"custom:bb"});

    auto const menu = navigator.menuEntries();
    REQUIRE_FALSE(menu.empty());
    CHECK(menu.back().label == "Custom");
    REQUIRE(menu.back().items.size() == 3);
    CHECK(menu.back().items[1].label == "Shared · bob");
    CHECK(navigator.choose({menu.size() - 1, 2}));
    CHECK(opened.back() == "custom:cc");
}

// Mutation: leave custom rows out of the search (`Shared` still shows while searching "min").
TEST_CASE("navigator: the search filters custom entries, and the open custom screen is marked",
          "[ui][navigator][custom-screens]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    Navigator navigator{backend, nullptr, options(opened)};
    navigator.setCustom(customs());
    int const search = widget(backend, "TextInput", "navigator_search");
    backend.edit(search, "min");
    CHECK(backend.prop(widget(backend, "Row", "nav_row_custom:aa"), "visible") == "true");
    CHECK(backend.prop(widget(backend, "Row", "nav_row_custom:bb"), "visible") == "false");
    backend.edit(search, "");

    navigator.setCurrent("custom:bb");
    CHECK(navigator.current() == "custom:bb");
    CHECK(backend.prop(widget(backend, "Button", "nav_custom:bb"), "selected") == "true");
}

// Mutation: read a title as markup or as a message key (it would render other than as written).
TEST_CASE("navigator: custom titles and author names render as written", "[ui][navigator][custom-screens]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    Navigator navigator{backend, nullptr, options(opened)};
    navigator.setCustom({NavigatorCustomEntry{
        .id = "custom:dd", .title = "<b>app.key</b>", .savedBy = "<i>eve</i>", .mine = false, .broken = {}}});
    CHECK(backend.prop(widget(backend, "Text", "nav_title_custom:dd"), "text") == "<b>app.key</b>");
    CHECK(backend.prop(widget(backend, "Text", "nav_author_custom:dd"), "text") == "by <i>eve</i>");
}
