// SPDX-License-Identifier: Apache-2.0
//
// The workspace shell's start and its neighbours (spec 6 §7, §15, §20; spec 5 §5): a prefetch warms the entries a
// screen will read before it mounts; the record lists the open tabs' routes and resume reopens them; a host screen
// mounts from the registry into a tab; and the navigator lists, searches, pins and prefetches the catalog.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/reactive/scope.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/navigator.hpp>
#include <morph/ui/workspace.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui_interpreter_support.hpp"

using morph::ui::CatalogEntry;
using morph::ui::ContainerWidget;
using morph::ui::Document;
using morph::ui::DocumentScreenHost;
using morph::ui::DocumentScreenHostOptions;
using morph::ui::MountError;
using morph::ui::Navigator;
using morph::ui::NavigatorOptions;
using morph::ui::ParamMap;
using morph::ui::ScreenHandle;
using morph::ui::ScreenHostRegistry;
using morph::ui::Value;
using morph::ui::Workspace;
using morph::ui::WorkspaceOptions;
using morph::ui::WorkspaceScreen;
using morph::ui::WorkspaceStart;
using morph::ui::testing::Harness;
using morph::ui::testing::loadOrFail;

namespace {

/// A sample: a keyed model, a stateless one and a private one, each with a query it prefetches.
constexpr std::string_view kSample = R"({
  "vocab": "ui/1", "id": "sample", "params": { "sampleId": "int" },
  "identity": { "ref": "sampleId" }, "title": "Sample",
  "models": { "sample": { "type": "Lims_SampleModel", "instance": { "ref": "sampleId" } },
              "clientModel": { "type": "Lims_ClientModel" },
              "limitModel": { "type": "Lims_CatalogModel" } },
  "queries": { "results": { "model": "sample", "action": "Lims_ListResults",
                            "body": { "obj": { "sampleId": { "ref": "sampleId" } } } },
               "clients": { "model": "clientModel", "action": "Lims_ClientList" },
               "limits": { "model": "limitModel", "action": "Lims_CatLimits" } },
  "prefetch": [ "results", "clients", "limits" ],
  "view": { "kind": "text", "testId": "sample", "text": "sample" } })";

constexpr std::string_view kHome = R"({
  "vocab": "ui/1", "id": "home", "title": "Home",
  "view": { "kind": "text", "testId": "home", "text": "home" } })";

/// A host over the harness's documents.
class Host final : public DocumentScreenHost {
public:
    explicit Host(Harness& harness) : DocumentScreenHost{harness.session, harness.dispatcher, options(harness)} {}

    using DocumentScreenHost::onNavigate;
    void onNavigate(std::string_view screenId, ParamMap const& params) override {
        static_cast<void>(screenId);
        static_cast<void>(params);
    }

private:
    static DocumentScreenHostOptions options(Harness& harness) {
        static std::map<std::string, std::shared_ptr<Document const>, std::less<>> const documents{
            {"sample", loadOrFail(kSample, morph::ui::testing::corpusCatalog())},
            {"home", loadOrFail(kHome, morph::ui::testing::corpusCatalog())}};
        DocumentScreenHostOptions made;
        made.documents = [](std::string_view id) -> std::shared_ptr<Document const> {
            auto const found = documents.find(id);
            return found == documents.end() ? nullptr : found->second;
        };
        made.backend = &harness.backend;
        return made;
    }
};

ParamMap sample(std::int64_t id) { return ParamMap{{"sampleId", Value{id}}}; }

/// A native screen: its own widget, a fixed title, never dirty, closes when asked.
class NativeScreen final : public ScreenHandle {
public:
    NativeScreen(morph::reactive::Runtime& runtime, std::string title, std::unique_ptr<morph::ui::Widget> content)
        : _content{std::move(content)},
          _scope{runtime},
          _title{_scope.computed([title = std::move(title)] { return title; })},
          _dirty{_scope.computed([] { return false; })},
          _identity{_scope.computed([] { return Value{}; })} {}

    [[nodiscard]] morph::reactive::Computed<std::string> const& title() const override { return _title; }
    [[nodiscard]] morph::reactive::Computed<bool> const& dirty() const override { return _dirty; }
    [[nodiscard]] morph::reactive::Computed<Value> const& identity() const override { return _identity; }
    void requestClose(std::function<void(bool closed)> done) override { done(true); }
    void forceClose() override {}

private:
    std::unique_ptr<morph::ui::Widget> _content;
    morph::reactive::Scope _scope;
    morph::reactive::Computed<std::string>& _title;
    morph::reactive::Computed<bool>& _dirty;
    morph::reactive::Computed<Value>& _identity;
};

}  // namespace

// Mutation: skip keyed aliases (no Lims_ListResults call before the mount).
TEST_CASE("prefetch: a keyed and a stateless alias's queries are sent before the mount; a private one's is not",
          "[ui][workspace]") {
    Harness harness;
    Host host{harness};
    host.prefetch("sample", sample(7));
    harness.settle();
    REQUIRE(harness.dispatcher.count("Lims_ListResults") == 1U);
    CHECK(harness.dispatcher.calls.at(harness.dispatcher.last("Lims_ListResults")).instanceKey == "7");
    CHECK(harness.dispatcher.count("Lims_ClientList") == 1U);
    CHECK(harness.dispatcher.count("Lims_CatLimits") == 0U);

    // The mounted screen reads the entries in flight: only the private alias's query is sent now.
    Workspace shell{harness.runtime, harness.backend, host};
    REQUIRE(shell.open("sample", sample(7)).has_value());
    harness.settle();
    CHECK(harness.dispatcher.count("Lims_ListResults") == 1U);
    CHECK(harness.dispatcher.count("Lims_ClientList") == 1U);
    CHECK(harness.dispatcher.count("Lims_CatLimits") == 1U);
    host.prefetch("nope", {});  // an unknown screen does nothing
}

// Mutation: write no record on close (the record still lists the closed tab).
TEST_CASE("record: the open tabs' routes, written as tabs open and close", "[ui][workspace]") {
    Harness harness;
    Host host{harness};
    std::vector<std::vector<std::string>> written;
    Workspace shell{harness.runtime, harness.backend, host,
                    WorkspaceOptions{.onRecord = [&written](std::vector<std::string> const& routes) {
                        written.push_back(routes);
                    }}};
    REQUIRE(shell.open("home", {}).has_value());
    REQUIRE(shell.open("sample", sample(3)).has_value());
    REQUIRE(written.size() == 2U);
    CHECK(written.back() == std::vector<std::string>{"home", "sample?sampleId=3"});
    shell.close(0, {});
    harness.settle();
    CHECK(written.back() == std::vector<std::string>{"sample?sampleId=3"});
}

// Mutation: write the record on every change even with a scheduler (three writes, not one).
TEST_CASE("record: with a scheduler a burst of changes is written once, and a flush writes at once",
          "[ui][workspace]") {
    Harness harness;
    Host host{harness};
    std::vector<std::vector<std::string>> written;
    Workspace shell{
        harness.runtime, harness.backend, host,
        WorkspaceOptions{.onRecord = [&written](std::vector<std::string> const& routes) { written.push_back(routes); },
                         .scheduler = &harness.scheduler,
                         .recordDelay = std::chrono::milliseconds{500}}};
    REQUIRE(shell.open("home", {}).has_value());
    REQUIRE(shell.open("sample", sample(1)).has_value());
    REQUIRE(shell.open("sample", sample(2)).has_value());
    CHECK(written.empty());
    harness.scheduler.advance(std::chrono::milliseconds{499});
    harness.settle();
    CHECK(written.empty());
    harness.scheduler.advance(std::chrono::milliseconds{1});
    harness.settle();
    REQUIRE(written.size() == 1U);
    CHECK(written.back().size() == 3U);

    shell.close(0, {});
    harness.settle();
    shell.flushRecord();
    REQUIRE(written.size() == 2U);
    CHECK(written.back() == std::vector<std::string>{"sample?sampleId=1", "sample?sampleId=2"});
    harness.scheduler.advance(std::chrono::seconds{1});
    harness.settle();
    CHECK(written.size() == 2U);  // the flush cancelled the pending write
}

// Mutation: open an unavailable recorded screen (home is reopened).
TEST_CASE("start: resume reopens the recorded routes it can, and the fallback when none opens", "[ui][workspace]") {
    Harness harness;
    Host host{harness};
    bool homeAvailable = false;
    Workspace shell{harness.runtime, harness.backend, host,
                    WorkspaceOptions{.screens = [&homeAvailable](std::string_view id) {
                        return WorkspaceScreen{.available = id != "home" || homeAvailable};
                    }}};
    std::size_t const opened = shell.start(
        WorkspaceStart{.start = "resume",
                       .fallback = "home",
                       .recorded = {"home", "sample?sampleId=4", "sample?sampleId=x", "gone", "sample?sampleId=5"}});
    CHECK(opened == 2U);
    REQUIRE(shell.tabs().size() == 2U);
    CHECK(shell.tabs().at(0).identity == Value{std::int64_t{4}});
    CHECK(shell.tabs().at(1).identity == Value{std::int64_t{5}});
    CHECK(shell.active() == 0U);

    Workspace fresh{harness.runtime, harness.backend, host,
                    WorkspaceOptions{.screens = [&homeAvailable](std::string_view id) {
                        return WorkspaceScreen{.available = id != "home" || homeAvailable};
                    }}};
    homeAvailable = true;
    CHECK(fresh.start(WorkspaceStart{.start = "resume", .fallback = "home", .recorded = {"gone"}}) == 1U);
    REQUIRE(fresh.tabs().size() == 1U);
    CHECK(fresh.tabs().at(0).screenId == "home");

    Workspace direct{harness.runtime, harness.backend, host};
    CHECK(direct.start(WorkspaceStart{.start = "home", .fallback = {}, .recorded = {"sample?sampleId=9"}}) == 1U);
    CHECK(direct.tabs().at(0).screenId == "home");
}

// Mutation: mount a host screen as a document (no document `sync` exists, so nothing opens).
TEST_CASE("host screens: a catalog entry naming a host screen mounts from the registry into a tab",
          "[ui][workspace]") {
    Harness harness;
    Host host{harness};
    ScreenHostRegistry hosts;
    std::vector<ParamMap> made;
    hosts.add("DatabaseSync",
              [&harness, &made](ParamMap const& params,
                                ContainerWidget& page) -> std::expected<std::unique_ptr<ScreenHandle>, MountError> {
                  made.push_back(params);
                  auto text = harness.backend.createText(&page);
                  text->setTestId("native");
                  return std::make_unique<NativeScreen>(harness.runtime, "Sync", std::move(text));
              });
    std::vector<std::vector<std::string>> written;
    Workspace shell{
        harness.runtime, harness.backend, host,
        WorkspaceOptions{
            .screens = [](std::string_view id) { return WorkspaceScreen{.host = id == "sync" ? "DatabaseSync" : ""}; },
            .hosts = &hosts,
            .onRecord = [&written](std::vector<std::string> const& routes) { written.push_back(routes); }}};
    REQUIRE(shell.open("sync", {}).has_value());
    REQUIRE(made.size() == 1U);
    CHECK(shell.tabs().at(0).caption == "Sync");
    CHECK(harness.backend.find("Text", "testId", "native").has_value());
    CHECK(written.back() == std::vector<std::string>{"sync"});

    Workspace unregistered{harness.runtime, harness.backend, host, WorkspaceOptions{.screens = [](std::string_view) {
                               return WorkspaceScreen{.host = "Missing"};
                           }}};
    auto const refused = unregistered.open("x", {});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().reason == "no host screen `Missing` is registered");

    CHECK(shell.start(WorkspaceStart{.start = "resume", .fallback = {}, .recorded = {"sync"}}) == 1U);
}

namespace {

std::vector<CatalogEntry> lab() {
    std::vector<CatalogEntry> entries{
        CatalogEntry{.id = "lims.samples", .title = std::string{"Samples"}, .module = "Lab", .group = "Work"},
        CatalogEntry{.id = "lims.results", .title = std::string{"Results"}, .module = "Lab", .group = "Work"},
        CatalogEntry{.id = "lims.report", .title = std::string{"Report"}, .module = "Lab", .group = "Office"},
        CatalogEntry{.id = "admin.users", .title = std::string{"Users"}, .module = "Admin", .group = ""}};
    entries.at(2).available = false;
    entries.at(2).reason = "coming in 2.0";
    return entries;
}

/// Whether a navigator entry (`nav_<id>`, shown with its row) or a heading is shown.
bool shown(Harness& harness, std::string_view testId) {
    if (testId.starts_with("nav_")) {
        auto const row = harness.backend.find("Row", "testId", "nav_row_" + std::string{testId.substr(4)});
        return row && harness.backend.prop(*row, "visible") != "false";
    }
    auto const widget = harness.backend.find("Button", "testId", testId);
    return widget && harness.backend.prop(*widget, "visible") != "false";
}

}  // namespace

// Mutation: search the shown title only (the German title finds nothing).
TEST_CASE("navigator: groups, a disabled entry with its reason, and a search over every locale's title",
          "[ui][workspace][navigator]") {
    Harness harness;
    std::vector<std::string> opened;
    Navigator navigator{harness.backend, nullptr,
                        NavigatorOptions{.entries = lab(),
                                         .titles = [](CatalogEntry const& entry) -> std::vector<std::string> {
                                             if (entry.id == "lims.results") {
                                                 return {"Results", "Ergebnisse"};
                                             }
                                             return {};
                                         },
                                         .pins = {},
                                         .maxPins = 12,
                                         .onOpen = [&opened](std::string const& id) { opened.push_back(id); },
                                         .onPrefetch = {},
                                         .onPins = {}}};
    CHECK(harness.backend.find("Text", "testId", "navigator_group_Lab/Work").has_value());
    CHECK(harness.backend.find("Text", "testId", "navigator_group_Admin/").has_value());
    int const report = *harness.backend.find("Button", "testId", "nav_lims.report");
    CHECK(harness.backend.prop(report, "enabled") == "false");
    CHECK(harness.backend.prop(report, "tooltip") == "coming in 2.0");

    harness.backend.click(*harness.backend.find("Button", "testId", "nav_lims.samples"));
    CHECK(opened == std::vector<std::string>{"lims.samples"});

    harness.backend.edit(*harness.backend.find("TextInput", "testId", "navigator_search"), "ERGEB");
    CHECK(shown(harness, "nav_lims.results"));
    CHECK_FALSE(shown(harness, "nav_lims.samples"));
    CHECK_FALSE(shown(harness, "nav_admin.users"));
    int const admin = *harness.backend.find("Column", "testId", "navigator_group_Admin/_section");
    CHECK(harness.backend.prop(admin, "visible") == "false");
    harness.backend.edit(*harness.backend.find("TextInput", "testId", "navigator_search"), "");
    CHECK(shown(harness, "nav_admin.users"));
}

// Mutation: let the pins grow past the cap (a third pin is accepted).
TEST_CASE("navigator: pins are ordered, capped, reported, and prefetch on hover", "[ui][workspace][navigator]") {
    Harness harness;
    std::vector<std::vector<std::string>> stored;
    std::vector<std::string> prefetched;
    Navigator navigator{
        harness.backend, nullptr,
        NavigatorOptions{.entries = lab(),
                         .titles = {},
                         .pins = {"admin.users", "unknown", "admin.users"},
                         .maxPins = 2,
                         .onOpen = {},
                         .onPrefetch = [&prefetched](std::string const& id) { prefetched.push_back(id); },
                         .onPins = [&stored](std::vector<std::string> const& pins) { stored.push_back(pins); }}};
    CHECK(navigator.pins() == std::vector<std::string>{"admin.users"});
    CHECK(navigator.pin("lims.samples"));
    CHECK_FALSE(navigator.pin("lims.results"));  // full
    CHECK_FALSE(navigator.pin("admin.users"));   // already pinned
    REQUIRE(stored.size() == 1U);
    CHECK(stored.back() == std::vector<std::string>{"admin.users", "lims.samples"});
    CHECK(harness.backend.find("Button", "testId", "pin_lims.samples").has_value());

    harness.backend.hover(*harness.backend.find("Button", "testId", "pin_lims.samples"));
    harness.backend.hover(*harness.backend.find("Button", "testId", "nav_lims.results"));
    harness.backend.hover(*harness.backend.find("Button", "testId", "nav_lims.report"));  // unavailable
    CHECK(prefetched == std::vector<std::string>{"lims.samples", "lims.results"});

    CHECK(navigator.unpin("admin.users"));
    CHECK_FALSE(navigator.unpin("admin.users"));
    CHECK(stored.back() == std::vector<std::string>{"lims.samples"});
    CHECK_FALSE(harness.backend.find("Button", "testId", "pin_admin.users").has_value());
}

// Mutation: show a translatable title's key instead of its fallback (the search for the fallback finds nothing).
TEST_CASE("navigator: a translatable title shows its fallback, and a title of no text shows the id",
          "[ui][workspace][navigator]") {
    Harness harness;
    auto const json = [](std::string_view text) {
        auto parsed = morph::ui::parseJson(text);
        REQUIRE(parsed.has_value());
        return *std::move(parsed);
    };
    std::vector<CatalogEntry> entries{
        CatalogEntry{.id = "lims.samples",
                     .title = json(R"({"t":"lims.samples.title","fallback":"Samples"})"),
                     .module = "Lab",
                     .group = "Work"},
        CatalogEntry{
            .id = "lims.keyless", .title = json(R"({"t":"lims.keyless.title"})"), .module = "Lab", .group = "Work"},
        CatalogEntry{.id = "lims.numbered", .title = json("7"), .module = "Lab", .group = "Work"}};
    Navigator navigator{harness.backend, nullptr,
                        NavigatorOptions{.entries = std::move(entries),
                                         .titles = {},
                                         .pins = {},
                                         .maxPins = 4,
                                         .onOpen = {},
                                         .onPrefetch = {},
                                         .onPins = {}}};
    int const search = *harness.backend.find("TextInput", "testId", "navigator_search");
    navigator.focusSearch();
    CHECK(harness.backend.focused() == search);
    CHECK(harness.backend.idOf(navigator.root()) > 0);

    harness.backend.edit(search, "sampl");
    CHECK(shown(harness, "nav_lims.samples"));
    CHECK_FALSE(shown(harness, "nav_lims.keyless"));
    harness.backend.edit(search, "keyless");
    CHECK(shown(harness, "nav_lims.keyless"));
    harness.backend.edit(search, "numbered");
    CHECK(shown(harness, "nav_lims.numbered"));
}

// Mutation: create the entries without setStyle (they record no style); or let setCurrent keep the previous mark.
TEST_CASE("navigator: entries are list items, and setCurrent marks the open screen's alone",
          "[ui][workspace][navigator]") {
    Harness harness;
    Navigator navigator{harness.backend, nullptr,
                        NavigatorOptions{.entries = lab(),
                                         .titles = {},
                                         .pins = {"lims.samples"},
                                         .maxPins = 12,
                                         .onOpen = {},
                                         .onPrefetch = {},
                                         .onPins = {}}};
    auto const prop = [&harness](char const* testId, char const* name) {
        return harness.backend.prop(*harness.backend.find("Button", "testId", testId), name);
    };
    CHECK(prop("nav_lims.samples", "style") == "Item");
    // A group's heading is an overline, a step below the application's name.
    CHECK(harness.backend.prop(*harness.backend.find("Text", "testId", "navigator_group_Lab/Work"), "role") ==
          "Overline");
    CHECK(prop("nav_lims.samples", "layout") == "stretch(1)/content");
    CHECK(prop("pin_lims.samples", "style").empty());     // a pin stays a push button
    CHECK(prop("nav_lims.samples", "selected").empty());  // a new item is not selected, and is not told so
    navigator.setCurrent("lims.samples");
    CHECK(prop("nav_lims.samples", "selected") == "true");
    navigator.setCurrent("lims.results");
    CHECK(prop("nav_lims.samples", "selected") == "false");
    CHECK(prop("nav_lims.results", "selected") == "true");
    CHECK(navigator.current() == "lims.results");
    CHECK(prop("pin_lims.samples", "selected").empty());
    navigator.setCurrent("nowhere");
    CHECK(prop("nav_lims.results", "selected") == "false");
    CHECK(navigator.current().empty());
}

// Control: a no-op setBadge (no badge is drawn, and the accessible name has no count); or leave the count out of
// menuEntries' labels.
TEST_CASE("navigator: a count sits at the end of its entry, named with it, and the menu lists it after the title",
          "[ui][workspace][navigator]") {
    Harness harness;
    std::vector<std::string> opened;
    Navigator navigator{harness.backend, nullptr,
                        NavigatorOptions{.entries = lab(),
                                         .titles = {},
                                         .pins = {"admin.users"},
                                         .maxPins = 12,
                                         .onOpen = [&opened](std::string const& id) { opened.push_back(id); },
                                         .onPrefetch = {},
                                         .onPins = {}}};
    int changes = 0;
    navigator.setOnChanged([&changes] { ++changes; });
    int const row = *harness.backend.find("Row", "testId", "nav_row_lims.results");
    int const button = *harness.backend.find("Button", "testId", "nav_lims.results");
    int const badge = *harness.backend.find("Badge", "testId", "nav_badge_lims.results");
    // The entry's row is the button alone; the title and the badge are the button's content.
    CHECK(harness.backend.children(row) == std::vector<int>{button});
    int const content = *harness.backend.find("Row", "testId", "nav_content_lims.results");
    CHECK(harness.backend.children(content).back() == badge);
    CHECK(harness.backend.prop(badge, "visible") == "false");

    REQUIRE(navigator.setBadge("lims.results", morph::ui::NavigatorBadge{.text = "4", .tone = morph::ui::Tone::Info}));
    CHECK(harness.backend.prop(badge, "visible") == "true");
    CHECK(harness.backend.prop(badge, "text") == "4");
    CHECK(harness.backend.prop(badge, "tone") == "Info");
    CHECK(harness.backend.prop(button, "a11yName") == "Results, 4");
    CHECK(changes == 1);
    // The same count again changes nothing.
    REQUIRE(navigator.setBadge("lims.results", morph::ui::NavigatorBadge{.text = "4", .tone = morph::ui::Tone::Info}));
    CHECK(changes == 1);
    CHECK_FALSE(navigator.setBadge("lims.nowhere", morph::ui::NavigatorBadge{.text = "1", .tone = {}}));

    navigator.setCurrent("lims.samples");
    std::vector<morph::ui::MenuEntry> const menu = navigator.menuEntries();
    // The pin, a separator, then the groups as submenus.
    REQUIRE(menu.size() == 5);
    CHECK(menu.at(0).label == "Users");
    CHECK(menu.at(1).separator);
    CHECK(menu.at(2).label == "Lab / Work");
    REQUIRE(menu.at(2).items.size() == 2);
    CHECK(menu.at(2).items.at(0).label == "Samples");
    CHECK(menu.at(2).items.at(0).checked == true);
    CHECK(menu.at(2).items.at(1).label == "Results · 4");
    CHECK_FALSE(menu.at(2).items.at(1).checked.has_value());
    CHECK(menu.at(3).label == "Lab / Office");
    CHECK_FALSE(menu.at(3).items.at(0).enabled);
    CHECK(menu.at(4).label == "Admin");

    CHECK(navigator.choose({2, 1}));
    CHECK_FALSE(navigator.choose({3, 0}));  // unavailable
    CHECK_FALSE(navigator.choose({1}));     // a separator
    CHECK_FALSE(navigator.choose({2}));     // a submenu
    CHECK(navigator.choose({0}));
    CHECK_FALSE(navigator.choose({9}));     // past the last entry
    CHECK_FALSE(navigator.choose({2, 7}));  // past the submenu's last entry
    CHECK_FALSE(navigator.choose({}));      // no entry
    CHECK(opened == std::vector<std::string>{"lims.results", "admin.users"});

    REQUIRE(navigator.setBadge("lims.results", std::nullopt));
    CHECK(harness.backend.prop(badge, "visible") == "false");
    CHECK(harness.backend.prop(button, "a11yName") == "Results");
    CHECK(navigator.menuEntries().at(2).items.at(1).label == "Results");
}

// Mutation: answer the head's flag for a screen without a head (a null dereference), or answer false.
TEST_CASE("host screens: a host screen's page stays inset", "[ui][workspace]") {
    Harness harness;
    Host host{harness};
    ScreenHostRegistry hosts;
    hosts.add("DatabaseSync",
              [&harness](ParamMap const&,
                         ContainerWidget& page) -> std::expected<std::unique_ptr<ScreenHandle>, MountError> {
                  auto text = harness.backend.createText(&page);
                  return std::make_unique<NativeScreen>(harness.runtime, "Sync", std::move(text));
              });
    Workspace shell{
        harness.runtime, harness.backend, host,
        WorkspaceOptions{.screens = [](std::string_view) { return WorkspaceScreen{.host = "DatabaseSync"}; },
                         .hosts = &hosts}};
    REQUIRE(shell.open("sync", {}).has_value());
    CHECK(shell.activePageInset());
}

// Mutation: put the groups' headings and entries back in the root, or leave the root without a gap.
TEST_CASE("navigator: each group is a column under a spaced root, and a search that matches nothing in it hides it",
          "[ui][workspace][navigator]") {
    Harness harness;
    Navigator navigator{
        harness.backend, nullptr,
        NavigatorOptions{
            .entries = lab(), .titles = {}, .pins = {}, .maxPins = 12, .onOpen = {}, .onPrefetch = {}, .onPins = {}}};
    int const root = *harness.backend.find("Column", "testId", "navigator");
    CHECK(harness.backend.prop(root, "gap") == "3");
    int const work = *harness.backend.find("Column", "testId", "navigator_group_Lab/Work_section");
    CHECK(harness.backend.prop(work, "gap") == "1");
    int const heading = *harness.backend.find("Text", "testId", "navigator_group_Lab/Work");
    int const samples = *harness.backend.find("Row", "testId", "nav_row_lims.samples");
    CHECK(harness.backend.children(work).front() == heading);
    CHECK(harness.backend.children(work).at(1) == samples);
    std::vector<int> const rootChildren = harness.backend.children(root);
    CHECK(std::ranges::find(rootChildren, work) != rootChildren.end());

    harness.backend.edit(*harness.backend.find("TextInput", "testId", "navigator_search"), "users");
    CHECK(harness.backend.prop(work, "visible") == "false");
    int const admin = *harness.backend.find("Column", "testId", "navigator_group_Admin/_section");
    CHECK(harness.backend.prop(admin, "visible") != "false");
}
