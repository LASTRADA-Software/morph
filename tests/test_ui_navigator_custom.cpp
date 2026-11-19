// SPDX-License-Identifier: Apache-2.0
//
// The navigator's Custom section (docs/spec/ui/workspace.md, "Navigator and pins"): a person's custom screens after
// the application's groups, each marked, naming who saved it when that was someone else, and badged when broken.

#include <catch2/catch_test_macros.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/navigator.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
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
    CHECK(menu.back().items[1].label == "Custom: Shared");
    CHECK(menu.back().items[1].description == "by bob");
    CHECK(menu.back().items[0].description.empty());
    CHECK(menu.back().items[2].description == "the action Lims_Verify is not permitted");
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

// Mutation: route a custom title, an author or a menu label through `options.label` or `options.titles`.
TEST_CASE("navigator: custom titles and author names are shown as written, not translated",
          "[ui][navigator][custom-screens]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    NavigatorOptions made = options(opened);
    made.label = [](std::string_view text) { return "T(" + std::string{text} + ")"; };
    made.titles = [](CatalogEntry const& entry) { return std::vector<std::string>{"T(" + entry.id + ")"}; };
    Navigator navigator{backend, nullptr, std::move(made)};
    navigator.setCustom({NavigatorCustomEntry{
        .id = "custom:dd", .title = "app.key", .savedBy = "<i>eve</i>", .mine = false, .broken = {}}});
    CHECK(backend.prop(widget(backend, "Text", "nav_title_custom:dd"), "text") == "app.key");
    CHECK(backend.prop(widget(backend, "Text", "nav_author_custom:dd"), "text") == "by <i>eve</i>");
    CHECK(navigator.menuEntries().back().items[0].label == "Custom: app.key");
    CHECK(navigator.menuEntries().back().items[0].description == "by <i>eve</i>");
    CHECK(backend.prop(widget(backend, "Text", "nav_title_notes.home"), "text") == "T(notes.home)");
}

// Mutation: drop the mark from the menu label (a custom "Home" reads as the group's "Home").
TEST_CASE("navigator: a custom entry never passes for an application entry in the menu",
          "[ui][navigator][custom-screens]") {
    RecordingBackend backend;
    NavigatorOptions made;
    made.entries = {CatalogEntry{.id = "notes.home", .title = "Home", .group = "Custom"}};
    Navigator navigator{backend, nullptr, std::move(made)};
    navigator.setCustom({NavigatorCustomEntry{.id = "custom:aa", .title = "Home", .savedBy = "ann"}});
    auto const menu = navigator.menuEntries();
    REQUIRE(menu.size() == 2);
    CHECK(menu[0].label == "Custom");
    CHECK(menu[0].items[0].label == "Home");
    CHECK(menu[1].label == "Custom");
    CHECK(menu[1].items[0].label == "Custom: Home");
    CHECK(menu[0].items[0].label != menu[1].items[0].label);
}

// Mutation: leave the current custom entry unchecked in the menu.
TEST_CASE("navigator: the current custom entry is checked in the menu, grouped by module or not",
          "[ui][navigator][custom-screens]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    Navigator navigator{backend, nullptr, options(opened)};
    navigator.setCustom(customs());
    navigator.setCurrent("custom:bb");
    for (bool const grouped : {false, true}) {
        navigator.setModuleGroups(grouped);
        auto const menu = navigator.menuEntries();
        CHECK(menu.back().label == "Custom");
        REQUIRE(menu.back().items.size() == 3);
        CHECK(menu.back().items[1].checked == std::optional<bool>{true});
        CHECK_FALSE(menu.back().items[0].checked.has_value());
        CHECK(navigator.choose({menu.size() - 1, 0}));
        CHECK(opened.back() == "custom:aa");
    }
}

// Mutation: forget the pending id (`setCurrent` before `setCustom` marks nothing).
TEST_CASE("navigator: a custom screen opened before the list arrives is marked when it does",
          "[ui][navigator][custom-screens]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    Navigator navigator{backend, nullptr, options(opened)};
    navigator.setCurrent("custom:bb");
    CHECK(navigator.current().empty());
    navigator.setCustom(customs());
    CHECK(navigator.current() == "custom:bb");
    CHECK(backend.prop(widget(backend, "Button", "nav_custom:bb"), "selected") == "true");
}

// Mutations: accept an id without the prefix; accept a repeated id.
TEST_CASE("navigator: custom entries without the custom prefix or with a repeated id are skipped",
          "[ui][navigator][custom-screens]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    Navigator navigator{backend, nullptr, options(opened)};
    navigator.setCustom({NavigatorCustomEntry{.id = "notes.home", .title = "Spoof"},
                         NavigatorCustomEntry{.id = "custom:aa", .title = "First"},
                         NavigatorCustomEntry{.id = "custom:aa", .title = "Second"}});
    REQUIRE(navigator.custom().size() == 1);
    CHECK(navigator.custom().front().title == "First");
    CHECK(backend.prop(widget(backend, "Text", "nav_title_custom:aa"), "text") == "First");
}

// Mutation: keep the section shown when no custom title matches the search.
TEST_CASE("navigator: the section hides when the search matches no custom title, and ids with odd characters work",
          "[ui][navigator][custom-screens]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    Navigator navigator{backend, nullptr, options(opened)};
    navigator.setCustom({NavigatorCustomEntry{.id = "custom:a b/c\"d", .title = "Odd"}});
    int const section = widget(backend, "Column", "navigator_custom_section");
    int const search = widget(backend, "TextInput", "navigator_search");
    backend.edit(search, "zzz");
    CHECK(backend.prop(section, "visible") == "false");
    backend.edit(search, "odd");
    CHECK(backend.prop(section, "visible") == "true");
    backend.click(widget(backend, "Button", "nav_custom:a b/c\"d"));
    CHECK(opened == std::vector<std::string>{"custom:a b/c\"d"});
}

// Mutation: use the closure's capture after `onOpen` relists (a synchronous `setCustom` destroys the closure).
TEST_CASE("navigator: an open handler may relist the custom screens", "[ui][navigator][custom-screens]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    NavigatorOptions made = options(opened);
    Navigator* self = nullptr;
    made.onOpen = [&opened, &self](std::string const& id) {
        self->setCustom({});
        opened.push_back(id);
    };
    Navigator navigator{backend, nullptr, std::move(made)};
    self = &navigator;
    navigator.setCustom(customs());
    backend.click(widget(backend, "Button", "nav_custom:bb"));
    CHECK(opened == std::vector<std::string>{"custom:bb"});
}

// Mutation: clear the pending id after the early return (`setCurrent("")` leaves it pending).
TEST_CASE("navigator: closing the screen cancels a custom screen still waiting for its list",
          "[ui][navigator][custom-screens]") {
    RecordingBackend backend;
    std::vector<std::string> opened;
    Navigator navigator{backend, nullptr, options(opened)};
    navigator.setCurrent("custom:bb");
    navigator.setCurrent("");
    navigator.setCustom(customs());
    CHECK(navigator.current().empty());
    CHECK(backend.prop(widget(backend, "Button", "nav_custom:bb"), "selected") != "true");
}
