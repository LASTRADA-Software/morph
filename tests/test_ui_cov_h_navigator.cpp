// SPDX-License-Identifier: Apache-2.0

// The navigator's pins, menu and module groups at the calls a person cannot make from the sidebar alone: pins the
// store holds that no longer fit, entries that cannot be opened, modules that are not listed, and a catalog whose
// modules interleave.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <morph/ui/bundle.hpp>
#include <morph/ui/navigator.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using morph::ui::CatalogEntry;
using morph::ui::Navigator;
using morph::ui::NavigatorBadge;
using morph::ui::NavigatorCustomEntry;
using morph::ui::NavigatorOptions;
using morph::ui::testing::RecordingBackend;

CatalogEntry entry(std::string id, std::string module, std::string group, bool available = true) {
    return CatalogEntry{.id = std::move(id),
                        .title = {},
                        .module = std::move(module),
                        .group = std::move(group),
                        .icon = {},
                        .singleton = false,
                        .available = available,
                        .reason = available ? std::nullopt : std::optional<std::string>{"not yours"},
                        .whenDenied = "hide",
                        .isPublic = false,
                        .host = std::nullopt,
                        .reuseEmpty = false};
}

// Module A holds two groups and an entry that cannot be opened, B and C one entry each.
std::vector<CatalogEntry> catalog() {
    return {entry("a.one", "A", "First"), entry("a.two", "A", "First"), entry("a.three", "A", "Second", false),
            entry("b.only", "B", ""), entry("c.only", "C", "")};
}

struct Fixture {
    explicit Fixture(std::vector<std::string> pins = {}, std::size_t maxPins = 12,
                     std::vector<CatalogEntry> entries = catalog()) {
        NavigatorOptions options;
        options.entries = std::move(entries);
        options.pins = std::move(pins);
        options.maxPins = maxPins;
        options.onOpen = [this](std::string const& id) { opened.push_back(id); };
        options.onPrefetch = [this](std::string const& id) { prefetched.push_back(id); };
        options.onPins = [this](std::vector<std::string> const& now) { stored = now; };
        navigator.emplace(backend, nullptr, std::move(options));
    }

    int widget(std::string const& kind, std::string const& testId) const {
        auto const found = backend.find(kind, "testId", testId);
        REQUIRE(found.has_value());
        return *found;
    }

    // The path of the menu entry whose label is @p label, searching one level into the submenus.
    std::vector<std::size_t> pathOf(std::string const& label) const {
        auto const menu = navigator->menuEntries();
        for (std::size_t at = 0; at < menu.size(); ++at) {
            if (menu[at].label == label) {
                return {at};
            }
            for (std::size_t inner = 0; inner < menu[at].items.size(); ++inner) {
                if (menu[at].items[inner].label == label) {
                    return {at, inner};
                }
            }
        }
        FAIL("no menu entry " << label);
        return {};
    }

    RecordingBackend backend;
    std::vector<std::string> opened;
    std::vector<std::string> prefetched;
    std::vector<std::string> stored;
    std::optional<Navigator> navigator;
};

}  // namespace

TEST_CASE("cov h navigator: stored pins keep the known, once each, up to the most", "[covh][ui][navigator]") {
    Fixture h{{"a.one", "nowhere", "a.one", "b.only", "a.two"}, 2};
    CHECK(h.navigator->pins() == std::vector<std::string>{"a.one", "b.only"});
    CHECK(h.stored.empty());  // nothing the person did
    CHECK(h.widget("Button", "pin_a.one") != 0);
    CHECK_FALSE(h.backend.find("Button", "testId", "pin_a.two").has_value());
}

TEST_CASE("cov h navigator: a pin is refused for an unknown screen, a repeat and a full row",
          "[covh][ui][navigator]") {
    Fixture h{{"a.one"}, 2};
    CHECK_FALSE(h.navigator->pin("nowhere"));
    CHECK_FALSE(h.navigator->pin("a.one"));
    CHECK(h.navigator->pin("a.two"));
    CHECK(h.stored == std::vector<std::string>{"a.one", "a.two"});
    CHECK_FALSE(h.navigator->pin("b.only"));  // the row is full
    CHECK_FALSE(h.navigator->unpin("b.only"));
    CHECK(h.navigator->unpin("a.one"));
    CHECK(h.stored == std::vector<std::string>{"a.two"});
    CHECK(h.navigator->pin("b.only"));
}

TEST_CASE("cov h navigator: choosing in the menu opens only an available screen at the end of a path",
          "[covh][ui][navigator]") {
    Fixture h{{"a.one"}};
    auto const menu = h.navigator->menuEntries();
    REQUIRE(menu.size() >= 4);
    CHECK_FALSE(h.navigator->choose({}));
    CHECK_FALSE(h.navigator->choose({99}));
    CHECK_FALSE(h.navigator->choose({0, 5}));
    CHECK_FALSE(h.navigator->choose({1}));                  // the separator names no screen
    CHECK_FALSE(h.navigator->choose(h.pathOf("First")));    // a submenu
    CHECK_FALSE(h.navigator->choose(h.pathOf("a.three")));  // not available
    CHECK(h.opened.empty());
    CHECK(h.navigator->choose({0}));  // the pin
    CHECK(h.navigator->choose(h.pathOf("a.two")));
    CHECK(h.opened == std::vector<std::string>{"a.one", "a.two"});
}

TEST_CASE("cov h navigator: a hover prefetches and a click opens an available entry, a disabled one does neither",
          "[covh][ui][navigator]") {
    Fixture h;
    h.backend.hover(h.widget("Button", "nav_a.one"));
    h.backend.click(h.widget("Button", "nav_a.one"));
    CHECK(h.prefetched == std::vector<std::string>{"a.one"});
    CHECK(h.opened == std::vector<std::string>{"a.one"});
    h.backend.click(h.widget("Button", "nav_a.three"));
    CHECK(h.opened.size() == 1);
    CHECK(h.backend.prop(h.widget("Button", "nav_a.three"), "tooltip") == "not yours");
    CHECK(h.backend.prop(h.widget("Button", "nav_a.three"), "enabled") == "false");
}

TEST_CASE("cov h navigator: a badge is set, repeated and cleared on a listed screen only", "[covh][ui][navigator]") {
    Fixture h;
    CHECK_FALSE(h.navigator->setBadge("nowhere", NavigatorBadge{.text = "3", .tone = morph::ui::Tone::Info}));
    CHECK_FALSE(h.navigator->badgeOf("nowhere").has_value());
    NavigatorBadge const badge{.text = "3", .tone = morph::ui::Tone::Warn};
    CHECK(h.navigator->setBadge("a.one", badge));
    CHECK(h.navigator->setBadge("a.one", badge));
    CHECK(h.navigator->badgeOf("a.one") == std::optional<NavigatorBadge>{badge});
    CHECK(h.backend.prop(h.widget("Badge", "nav_badge_a.one"), "visible") == "true");
    h.navigator->relabel();  // a shown badge keeps its count in the button's name
    CHECK(h.navigator->setBadge("a.one", std::nullopt));
    CHECK(h.backend.prop(h.widget("Badge", "nav_badge_a.one"), "visible") == "false");
    CHECK_FALSE(h.navigator->badgeOf("a.one").has_value());
}

TEST_CASE("cov h navigator: the current screen moves between screens and custom screens", "[covh][ui][navigator]") {
    Fixture h;
    h.navigator->setCustom({NavigatorCustomEntry{.id = "custom:aa", .title = "Mine"},
                            NavigatorCustomEntry{.id = "custom:bb", .title = "Other"}});
    h.navigator->setCurrent("custom:aa");
    CHECK(h.backend.prop(h.widget("Button", "nav_custom:aa"), "selected") == "true");
    h.navigator->setCurrent("custom:bb");
    CHECK(h.backend.prop(h.widget("Button", "nav_custom:aa"), "selected") == "false");
    CHECK(h.backend.prop(h.widget("Button", "nav_custom:bb"), "selected") == "true");
    h.navigator->setCurrent("a.one");
    CHECK(h.backend.prop(h.widget("Button", "nav_custom:bb"), "selected") == "false");
    CHECK(h.backend.prop(h.widget("Button", "nav_a.one"), "selected") == "true");
    h.navigator->setCurrent("a.one");      // no change
    h.navigator->setCurrent("custom:zz");  // not listed yet: the current screen is none
    CHECK(h.navigator->current().empty());
    h.navigator->setCurrent("custom:bb");
    // A list that no longer holds the open custom screen leaves no current screen.
    h.navigator->setCustom({NavigatorCustomEntry{.id = "custom:aa", .title = "Mine"}});
    CHECK(h.navigator->current().empty());
}

TEST_CASE("cov h navigator: modules are opened, revealed and badged only where the catalog lists them",
          "[covh][ui][navigator]") {
    Fixture h;
    CHECK_FALSE(h.navigator->setOpenModule("Z"));
    CHECK_FALSE(h.navigator->setModuleBadge("Z", NavigatorBadge{.text = "1", .tone = morph::ui::Tone::Info}));
    CHECK_FALSE(h.navigator->moduleBadgeOf("Z").has_value());
    CHECK_FALSE(h.navigator->reveal("nowhere"));
    h.navigator->setModuleGroups(true);
    CHECK(h.navigator->moduleGroups());
    h.navigator->setModuleGroups(true);
    NavigatorBadge const badge{.text = "2", .tone = morph::ui::Tone::Warn};
    CHECK(h.navigator->setModuleBadge("A", badge));
    CHECK(h.navigator->setModuleBadge("A", badge));  // unchanged
    CHECK(h.navigator->moduleBadgeOf("A") == std::optional<NavigatorBadge>{badge});
    CHECK(h.navigator->setModuleBadge("A", badge, true));
    CHECK(h.navigator->setOpenModule("A"));
    CHECK(h.navigator->setOpenModule("A"));
    CHECK(h.navigator->openModule() == "A");
    h.backend.click(h.widget("Button", "nav_module_A"));  // an open module closes
    CHECK(h.navigator->openModule().empty());
    h.backend.click(h.widget("Button", "nav_module_A"));
    CHECK(h.navigator->openModule() == "A");
    CHECK(h.navigator->setOpenModule("B"));
    CHECK(h.navigator->reveal("a.one"));
    CHECK(h.navigator->openModule() == "A");
    h.navigator->setModuleTexts("closed", "open", "failed");
    h.navigator->relabel();
    h.navigator->setModuleGroups(false);
    CHECK(h.navigator->reveal("b.only"));
    CHECK(h.navigator->openModule() == "A");  // a flat list opens no module
}

TEST_CASE("cov h navigator: the module menu lists lone entries, headings between groups and module badges",
          "[covh][ui][navigator]") {
    Fixture h;
    h.navigator->setModuleGroups(true);
    CHECK(h.navigator->setModuleBadge("A", NavigatorBadge{.text = "2", .tone = morph::ui::Tone::Warn}, true));
    auto const menu = h.navigator->menuEntries();
    REQUIRE(menu.size() == 3);
    CHECK(menu[0].label == "First · 2 · !");
    REQUIRE(menu[0].items.size() == 4);
    CHECK(menu[0].items[0].label == "a.one");
    CHECK(menu[0].items[2].heading);
    CHECK(menu[0].items[2].label == "Second");
    CHECK_FALSE(menu[0].items[2].enabled);
    CHECK(menu[1].label == "b.only");
    CHECK(menu[2].label == "c.only");
    CHECK_FALSE(h.navigator->choose({0, 2}));  // a heading
    CHECK(h.navigator->choose({1}));
    CHECK(h.opened == std::vector<std::string>{"b.only"});
    h.navigator->setCurrent("a.two");
    auto const checked = h.navigator->menuEntries();
    CHECK(checked[0].items[1].checked == std::optional<bool>{true});
}

TEST_CASE("cov h navigator: modules that interleave in the catalog keep its order until they are grouped",
          "[covh][ui][navigator]") {
    Fixture h{{}, 12, {entry("a.one", "A", "G"), entry("b.x", "B", "G"), entry("a.two", "A", "H")}};
    auto const flat = h.navigator->menuEntries();
    REQUIRE(flat.size() == 3);
    CHECK(flat[0].label == "G");
    CHECK(flat[1].label == "G");
    CHECK(flat[2].label == "H");
    h.navigator->setModuleGroups(true);
    h.navigator->setModuleGroups(false);
    CHECK(h.navigator->menuEntries().size() == 3);
    auto const search = h.widget("TextInput", "navigator_search");
    h.backend.edit(search, "two");
    CHECK(h.backend.prop(h.widget("Row", "nav_row_a.one"), "visible") == "false");
    CHECK(h.backend.prop(h.widget("Row", "nav_row_a.two"), "visible") == "true");
    h.navigator->setSearchShown(false);
    CHECK(h.backend.prop(h.widget("Row", "nav_row_a.one"), "visible") == "true");
    CHECK(h.backend.prop(search, "visible") == "false");
    h.navigator->setSearchShown(true);
    CHECK(h.backend.prop(search, "visible") == "true");
}

TEST_CASE("cov h navigator: a custom list that did not change is not rebuilt", "[covh][ui][navigator]") {
    Fixture h;
    std::vector<NavigatorCustomEntry> const list{NavigatorCustomEntry{.id = "custom:aa", .title = "Mine"}};
    h.navigator->setCustom(list);
    int const row = h.widget("Row", "nav_row_custom:aa");
    h.navigator->setCustom(list);
    CHECK(h.widget("Row", "nav_row_custom:aa") == row);
    h.backend.click(h.widget("Button", "nav_custom:aa"));
    CHECK(h.opened == std::vector<std::string>{"custom:aa"});
}

TEST_CASE("cov h navigator: with no handlers set, choosing, hovering and pinning only change the navigator",
          "[covh][ui][navigator]") {
    RecordingBackend backend;
    NavigatorOptions options;
    options.entries = {entry("a.one", "A", "First"), entry("a.two", "A", "First")};
    Navigator navigator{backend, nullptr, std::move(options)};
    int const button = *backend.find("Button", "testId", "nav_a.one");
    backend.hover(button);
    backend.click(button);
    CHECK(navigator.choose({0, 0}));
    CHECK(navigator.pin("a.one"));
    CHECK(navigator.unpin("a.one"));
    navigator.setCustom({NavigatorCustomEntry{.id = "custom:aa", .title = "Mine"}});
    backend.click(*backend.find("Button", "testId", "nav_custom:aa"));
    CHECK(navigator.custom().size() == 1);
}

TEST_CASE("cov h navigator: a custom screen pending, current or replaced when the list changes",
          "[covh][ui][navigator]") {
    Fixture h;
    h.navigator->setCurrent("custom:zz");
    h.navigator->setCurrent("custom:zz");  // still waiting for its list
    h.navigator->setCustom({NavigatorCustomEntry{.id = "custom:aa", .title = "Mine"}});
    h.navigator->setCustom({NavigatorCustomEntry{.id = "custom:aa", .title = "Mine"},
                            NavigatorCustomEntry{.id = "custom:zz", .title = "Late"}});
    CHECK(h.navigator->current() == "custom:zz");
    h.navigator->setCurrent("custom:aa");
    // A list that is rebuilt marks the open custom screen again, and one that leaves a catalog screen open keeps it.
    h.navigator->setCustom({NavigatorCustomEntry{.id = "custom:aa", .title = "Renamed"}});
    CHECK(h.backend.prop(h.widget("Button", "nav_custom:aa"), "selected") == "true");
    h.navigator->setCurrent("a.one");
    h.navigator->setCustom({NavigatorCustomEntry{.id = "custom:bb", .title = "Other"}});
    CHECK(h.navigator->current() == "a.one");
}

TEST_CASE("cov h navigator: a module and group with no name list their entries without a heading",
          "[covh][ui][navigator]") {
    Fixture h{{}, 12, {entry("x.one", "", ""), entry("x.two", "", "")}};
    auto const menu = h.navigator->menuEntries();
    REQUIRE(menu.size() == 2);
    CHECK(menu[0].label == "x.one");
    CHECK(h.navigator->choose({1}));
    CHECK(h.opened == std::vector<std::string>{"x.two"});
}
