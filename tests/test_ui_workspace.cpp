// SPDX-License-Identifier: Apache-2.0
//
// The workspace shell (spec 6 §7, §15): tabs over screens mounted through DocumentScreenHost, focused by identity,
// one per singleton, a reuseEmpty placeholder replaced, captions that follow title and dirty, closing tab by tab
// until a veto, and the most-recently-used walk.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <morph/reactive/signal.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/workspace.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui_interpreter_support.hpp"

using morph::ui::Document;
using morph::ui::DocumentScreenHost;
using morph::ui::DocumentScreenHostOptions;
using morph::ui::ParamMap;
using morph::ui::ScreenHandle;
using morph::ui::Value;
using morph::ui::Workspace;
using morph::ui::WorkspaceOptions;
using morph::ui::WorkspaceScreen;
using morph::ui::testing::Harness;
using morph::ui::testing::loadOrFail;

namespace {

/// A note: identity is its param, title and dirty follow its state.
constexpr std::string_view kNote = R"({
  "vocab": "ui/1", "id": "note", "params": { "noteId": "int" },
  "identity": { "ref": "noteId" }, "title": { "ref": "name" }, "dirty": { "ref": "changed" },
  "state": { "name": { "init": "Note" }, "changed": { "init": false } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "edit", "label": "edit", "onClick": [ { "set": "changed", "value": true } ] },
    { "kind": "button", "testId": "rename", "label": "rename", "onClick": [ { "set": "name", "value": "Renamed" } ] }
  ] } })";

/// The overview: no identity, a singleton by its catalog facts.
constexpr std::string_view kHome = R"({
  "vocab": "ui/1", "id": "home", "title": "Home",
  "view": { "kind": "text", "testId": "home", "text": "home" } })";

/// A draft: its identity is its optional param, so a new draft has none; replaced rather than stacked.
constexpr std::string_view kDraft = R"({
  "vocab": "ui/1", "id": "draft", "params": { "noteId": "int?" },
  "identity": { "ref": "noteId" }, "title": "Draft",
  "view": { "kind": "text", "testId": "draft", "text": "draft" } })";

/// A screen with a head: a subtitle that follows its state, two badges (one hidden) and two actions (one disabled).
constexpr std::string_view kHeaded = R"({
  "vocab": "ui/1", "id": "headed", "title": "Sample",
  "subtitle": { "ref": "stage" },
  "status": [ { "text": "Draft", "tone": "warn" }, { "text": "Hidden", "visible": false } ],
  "actions": [
    { "id": "save", "label": "Save", "icon": "save", "emphasis": "primary",
      "onClick": [ { "set": "stage", "value": { "fn": "concat", "args": [ { "ref": "stage" }, "!" ] } } ] },
    { "id": "publish", "label": "Publish", "enabled": false, "onClick": [ { "set": "stage", "value": "Published" } ] }
  ],
  "state": { "stage": { "init": "Editing" } },
  "view": { "kind": "text", "testId": "body", "text": { "ref": "stage" } } })";

/// A screen whose head gives its subtitle as runs beside the plain line an older client draws.
constexpr std::string_view kRunHead = R"({
  "vocab": "ui/1", "id": "runhead", "title": "Instrument",
  "subtitle": "Instruments > EQ-0142",
  "subtitleSpans": { "list": [ "Instruments > ", { "obj": { "text": "EQ-0142", "mono": true } } ] },
  "view": { "kind": "text", "testId": "body", "text": "x" } })";

/// A screen whose title is runs that follow its state, beside the plain line an older client draws.
constexpr std::string_view kTitled = R"({
  "vocab": "ui/1", "id": "titled", "title": "Plate PL-0193",
  "titleSpans": { "list": [ "Plate ", { "obj": { "text": { "ref": "plate" }, "mono": true } } ] },
  "state": { "plate": { "init": "PL-0193" } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "body", "text": { "ref": "plate" } },
    { "kind": "button", "testId": "rename", "label": "rename", "onClick": [ { "set": "plate", "value": "PL-0200" } ] }
  ] } })";

/// A screen whose title runs include one the head cannot read.
constexpr std::string_view kBadTitle = R"({
  "vocab": "ui/1", "id": "badtitle", "title": "Plate",
  "titleSpans": { "list": [ "Plate ", { "list": [ "nested" ] } ] },
  "view": { "kind": "text", "testId": "body", "text": "x" } })";

/// A screen whose eyebrow is runs beside the plain line an older client draws.
constexpr std::string_view kEyebrowRuns = R"({
  "vocab": "ui/1", "id": "eyebrowruns", "title": "Instrument",
  "eyebrow": "Worksheet WS-1",
  "eyebrowSpans": { "list": [ "Worksheet ", { "obj": { "text": "WS-1", "mono": true } } ] },
  "view": { "kind": "text", "testId": "body", "text": "x" } })";

/// A screen whose head is an eyebrow alone, shown while `lineShown`.
constexpr std::string_view kEyebrowOnly = R"({
  "vocab": "ui/1", "id": "eyebrowonly", "title": "Results",
  "eyebrow": { "if": [ { "ref": "lineShown" }, "Step 2 of 4", "" ] },
  "state": { "lineShown": { "init": true } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "clear", "label": "clear", "onClick": [ { "set": "lineShown", "value": false } ] }
  ] } })";

/// A screen whose root lays itself at the page's edges.
constexpr std::string_view kEdgeRoot = R"({
  "vocab": "ui/1", "id": "edgeroot", "title": "Pinned",
  "view": { "kind": "column", "pageEdge": true, "children": [
    { "kind": "text", "testId": "body", "text": "x" } ] } })";

/// A screen whose head places its lead by `place`, asks the shell for the surface `surface`, and has a tool and an
/// action that shows its icon alone while `compact`.
constexpr std::string_view kLeadPlaced = R"({
  "vocab": "ui/1", "id": "leadplaced", "title": "Plate",
  "subtitle": "ELISA", "leadPlacement": { "ref": "place" }, "headSurface": { "ref": "surface" },
  "pageSurface": { "ref": "pageName" },
  "status": [ { "text": "Draft", "tone": "warn" } ],
  "tools": [ { "id": "q", "kind": "search", "label": "Search", "value": "" } ],
  "actions": [ { "id": "scan", "label": "Scan", "icon": "scan", "iconOnly": { "ref": "compact" }, "onClick": [] } ],
  "state": { "place": { "init": "end" }, "surface": { "init": "" }, "pageName": { "init": "" }, "compact": { "init": true } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "below", "label": "below", "onClick": [ { "set": "place", "value": "below" } ] },
    { "kind": "button", "testId": "inline", "label": "inline", "onClick": [ { "set": "place", "value": "inline" } ] },
    { "kind": "button", "testId": "dark", "label": "dark", "onClick": [ { "set": "surface", "value": "darkHead" } ] },
    { "kind": "button", "testId": "plain", "label": "plain", "onClick": [ { "set": "pageName", "value": "plainPage" } ] },
    { "kind": "button", "testId": "wide", "label": "wide", "onClick": [ { "set": "compact", "value": false } ] }
  ] } })";

/// A screen whose head has only a menu: an entry, a separator, an entry shown while `retireShown`, and a submenu.
constexpr std::string_view kMenued = R"({
  "vocab": "ui/1", "id": "menued", "title": "Instrument", "menuLabel": "More instrument actions",
  "menu": [
    { "id": "calibrate", "label": "Calibrate", "onClick": [ { "set": "log", "value": "calibrated" } ] },
    { "separator": true },
    { "id": "retire", "label": "Take out of service", "description": "needs reason",
      "visible": { "ref": "retireShown" }, "onClick": [ { "set": "log", "value": "retired" } ] },
    { "label": "Export", "items": [
      { "id": "csv", "label": "CSV", "onClick": [ { "set": "log", "value": "csv" } ] } ] }
  ],
  "state": { "log": { "init": "" }, "retireShown": { "init": true } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "log", "text": { "ref": "log" } },
    { "kind": "button", "testId": "hide", "label": "hide", "onClick": [ { "set": "retireShown", "value": false } ] }
  ] } })";

/// A screen whose head has only tools: a search bound to `q`, a select bound to `lab`, and a hidden search.
constexpr std::string_view kTooled = R"({
  "vocab": "ui/1", "id": "tooled", "title": "Samples",
  "tools": [
    { "id": "q", "kind": "search", "label": "Search samples", "placeholder": "Sample ID",
      "value": { "ref": "q" }, "onChange": [ { "set": "q", "value": { "ref": "event" } } ],
      "onSubmit": [ { "set": "submitted", "value": { "ref": "event" } } ] },
    { "id": "lab", "kind": "select", "label": "Lab", "options": { "lit": ["North", "South"] },
      "selected": { "ref": "lab" }, "onChoose": [ { "set": "lab", "value": { "ref": "event" } } ] },
    { "id": "later", "kind": "search", "label": "Later", "visible": false }
  ],
  "state": { "q": { "init": "" }, "lab": { "init": "North" }, "submitted": { "init": "" } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "q", "text": { "ref": "q" } },
    { "kind": "text", "testId": "lab", "text": { "ref": "lab" } },
    { "kind": "text", "testId": "submitted", "text": { "ref": "submitted" } },
    { "kind": "button", "testId": "upper", "label": "upper", "onClick": [ { "set": "q", "value": "HPLC-7" } ] }
  ] } })";

/// A screen that asks the picker for a value, and shows the answer it got.
constexpr std::string_view kAsker = R"({
  "vocab": "ui/1", "id": "asker", "title": "Asker",
  "state": { "got": { "init": "none" } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "got", "text": { "ref": "got" } },
    { "kind": "button", "testId": "ask", "label": "ask",
      "onClick": { "navigate": "picker",
                   "onResult": [ { "set": "got", "value": { "ref": "event" } } ],
                   "onCancel": [ { "set": "got", "value": "cancelled" } ] } },
    { "kind": "button", "testId": "show", "label": "show the picker", "onClick": { "navigate": "picker" } },
    { "kind": "button", "testId": "askDialog", "label": "ask in a dialog",
      "onClick": { "navigate": "picker", "as": "dialog",
                   "onResult": [ { "set": "got", "value": { "ref": "event" } } ],
                   "onCancel": [ { "set": "got", "value": "cancelled" }, { "notify": "cancelled" } ] } },
    { "kind": "button", "testId": "askDrawer", "label": "ask in a drawer",
      "onClick": { "navigate": "picker", "as": "drawer",
                   "onResult": [ { "set": "got", "value": { "ref": "event" } } ],
                   "onCancel": [ { "set": "got", "value": "cancelled" } ] } },
    { "kind": "button", "testId": "askNote", "label": "edit a note in a dialog",
      "onClick": { "navigate": "note", "params": { "obj": { "noteId": 3 } }, "as": "dialog",
                   "onCancel": [ { "set": "got", "value": "cancelled" } ] } },
    { "kind": "button", "testId": "askTooled", "label": "samples in a dialog",
      "onClick": { "navigate": "tooled", "as": "dialog",
                   "onCancel": [ { "set": "got", "value": "cancelled" } ] } } ] } })";

/// The picker, a singleton by its catalog facts: it answers 42.
constexpr std::string_view kPicker = R"({
  "vocab": "ui/1", "id": "picker", "title": "Picker",
  "view": { "kind": "button", "testId": "answer", "label": "answer", "onClick": [ { "return": 42 } ] } })";

/// The application's side of the host: it counts the `return`s no call answered.
class Application final : public morph::ui::InterpreterHost {
public:
    bool closeSelf(std::string const& screen, std::optional<Value> result) override {
        static_cast<void>(result);
        returned.push_back(screen);
        return true;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): InterpreterHost's signature.
    bool notify(std::string const& text, std::string const& role) override {
        static_cast<void>(role);
        notified.push_back(text);
        return true;
    }

    std::vector<std::string> returned;
    std::vector<std::string> notified;
};

/// What the document host reported, across the tests that read it.
std::vector<std::string>& reported() {
    static std::vector<std::string> reasons;
    return reasons;
}

/// A host over the harness that routes navigation to the shell and decides a dirty screen's close by a script.
class Host final : public DocumentScreenHost {
public:
    Host(Harness& harness, std::optional<bool> guardAnswer, morph::ui::InterpreterHost* application = nullptr)
        : DocumentScreenHost{harness.session, harness.dispatcher, options(harness, guardAnswer, application)} {}

    void onNavigate(std::string_view screenId, ParamMap const& params) override {
        if (shell != nullptr) {
            auto const opened = shell->open(screenId, params);  // a refused mount opens nothing
            static_cast<void>(opened);
        }
    }

    // A dialog or drawer over the tabs, the rest as a tab; a call whose navigation mounted no screen is settled.
    void onNavigate(morph::ui::NavigateRequest const& request) override {
        morph::ui::ValueRecord const* const record = request.params.asRecord();
        ParamMap const params = record == nullptr ? ParamMap{} : *record;
        if (shell == nullptr) {
            return;
        }
        if (request.as == "dialog" || request.as == "drawer") {
            if (!shell->openOverlay(request) && request.call) {
                settleCall(request.screen, params);
            }
            return;
        }
        auto const opened = shell->openTab(request.screen, params);
        if (request.call && (!opened || !opened->mounted)) {
            settleCall(request.screen, params);
        }
    }

    void onScreenClosed(ScreenHandle& screen) override {
        if (shell != nullptr) {
            shell->screenClosed(screen);
        }
    }

    Workspace* shell = nullptr;

private:
    static DocumentScreenHostOptions options(Harness& harness, std::optional<bool> guardAnswer,
                                             morph::ui::InterpreterHost* application) {
        static std::map<std::string, std::shared_ptr<Document const>, std::less<>> const documents{
            {"note", loadOrFail(kNote, morph::ui::testing::corpusCatalog())},
            {"home", loadOrFail(kHome, morph::ui::testing::corpusCatalog())},
            {"draft", loadOrFail(kDraft, morph::ui::testing::corpusCatalog())},
            {"headed", loadOrFail(kHeaded, morph::ui::testing::corpusCatalog())},
            {"runhead", loadOrFail(kRunHead, morph::ui::testing::corpusCatalog())},
            {"titled", loadOrFail(kTitled, morph::ui::testing::corpusCatalog())},
            {"badtitle", loadOrFail(kBadTitle, morph::ui::testing::corpusCatalog())},
            {"eyebrowruns", loadOrFail(kEyebrowRuns, morph::ui::testing::corpusCatalog())},
            {"eyebrowonly", loadOrFail(kEyebrowOnly, morph::ui::testing::corpusCatalog())},
            {"edgeroot", loadOrFail(kEdgeRoot, morph::ui::testing::corpusCatalog())},
            {"leadplaced", loadOrFail(kLeadPlaced, morph::ui::testing::corpusCatalog())},
            {"menued", loadOrFail(kMenued, morph::ui::testing::corpusCatalog())},
            {"tooled", loadOrFail(kTooled, morph::ui::testing::corpusCatalog())},
            {"asker", loadOrFail(kAsker, morph::ui::testing::corpusCatalog())},
            {"picker", loadOrFail(kPicker, morph::ui::testing::corpusCatalog())}};
        DocumentScreenHostOptions made;
        made.documents = [](std::string_view id) -> std::shared_ptr<Document const> {
            auto const found = documents.find(id);
            return found == documents.end() ? nullptr : found->second;
        };
        made.backend = &harness.backend;
        made.application = application;
        made.diagnostics = [](morph::ui::Refusal const& refusal) { reported().push_back(refusal.reason); };
        if (guardAnswer) {
            made.guard = [answer = *guardAnswer](ScreenHandle&,
                                                 std::function<void(morph::ui::CloseChoice)> const& choose) {
                choose(answer ? morph::ui::CloseChoice::Discard : morph::ui::CloseChoice::Cancel);
            };
        }
        return made;
    }
};

WorkspaceOptions catalog() {
    return WorkspaceOptions{.screens = [](std::string_view id) {
        return WorkspaceScreen{.singleton = id == "home" || id == "picker", .reuseEmpty = id == "draft"};
    }};
}

ParamMap note(std::int64_t id) { return ParamMap{{"noteId", Value{id}}}; }

std::vector<std::string> captions(Workspace const& shell) {
    std::vector<std::string> out;
    for (auto const& tab : shell.tabs()) {
        out.push_back(tab.caption);
    }
    return out;
}

}  // namespace

// Mutation: open a new tab whatever the identity (note 1 is opened twice).
TEST_CASE("workspace: opening an identity that is open focuses its tab; a singleton has one tab", "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    REQUIRE(shell.open("note", note(1)) == 0U);
    REQUIRE(shell.open("note", note(2)) == 1U);
    REQUIRE(shell.open("home", {}) == 2U);
    harness.settle();
    CHECK(shell.active() == 2U);

    CHECK(shell.open("note", note(1)) == 0U);
    CHECK(shell.active() == 0U);
    CHECK(shell.open("home", {}) == 2U);
    CHECK(shell.tabs().size() == 3);
    CHECK(shell.tabs()[1].identity == Value{std::int64_t{2}});
}

// Mutation: never call setBarShown (the bar keeps the backend's default, shown, with one tab).
TEST_CASE("workspace: the bar shows with two tabs or more, and hides with one", "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    REQUIRE(shell.open("home", {}) == 0U);
    harness.settle();
    int const bar = harness.backend.find("Tabs", "layout", "stretch(1)/stretch(1)").value();
    CHECK(harness.backend.prop(bar, "barShown") == "false");
    REQUIRE(shell.open("note", note(1)) == 1U);
    harness.settle();
    CHECK(harness.backend.prop(bar, "barShown") == "true");
    std::optional<bool> closed;
    shell.close(1, [&closed](bool done) { closed = done; });
    harness.settle();
    REQUIRE(closed == true);
    CHECK(harness.backend.prop(bar, "barShown") == "false");
}

// Mutation: leave the pages content-sized (a stretching screen cannot fill the page).
TEST_CASE("workspace: the bar and each page stretch, so a stretching screen fills the page", "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    REQUIRE(shell.open("home", {}) == 0U);
    harness.settle();
    int const bar = harness.backend.find("Tabs", "layout", "stretch(1)/stretch(1)").value();
    std::vector<int> const pages = harness.backend.children(bar);
    REQUIRE(pages.size() == 1U);
    CHECK(harness.backend.prop(pages.front(), "layout") == "stretch(1)/stretch(1)");
}

// Mutation: drop the page slot's setScrollable call (no page records `scrollable`).
TEST_CASE("workspace: every page scrolls a screen that keeps its content height", "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    REQUIRE(shell.open("home", {}) == 0U);
    REQUIRE(shell.open("note", note(1)) == 1U);
    harness.settle();
    int const bar = harness.backend.find("Tabs", "layout", "stretch(1)/stretch(1)").value();
    std::vector<int> const pages = harness.backend.children(bar);
    REQUIRE(pages.size() == 2U);
    for (int const page : pages) {
        CHECK(harness.backend.prop(page, "scrollable") == "true");
    }
}

// Mutation: never replace a placeholder (the draft is stacked as a second tab).
TEST_CASE("workspace: a reuseEmpty placeholder without identity is replaced in place", "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    REQUIRE(shell.open("home", {}).has_value());
    REQUIRE(shell.open("draft", {}) == 1U);
    REQUIRE(shell.open("note", note(3)).has_value());
    harness.settle();
    CHECK(shell.tabs()[1].identity.isNull());

    CHECK(shell.open("draft", note(9)) == 1U);
    harness.settle();
    REQUIRE(shell.tabs().size() == 3);
    CHECK(shell.tabs()[1].screenId == "draft");
    CHECK(shell.tabs()[1].identity == Value{std::int64_t{9}});
    CHECK(shell.active() == 1U);
    // A second draft with another identity is a tab of its own: only an empty placeholder is reused.
    CHECK(shell.open("draft", note(10)) == 3U);
}

// Mutations: show every page (two pages visible); leave the caption at the title when the screen is dirty.
TEST_CASE("workspace: only the active page is shown, and a tab's caption follows its title and unsaved changes",
          "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    REQUIRE(shell.open("note", note(1)).has_value());
    REQUIRE(shell.open("home", {}).has_value());
    harness.settle();
    int const bar = harness.backend.idOf(shell.root());
    std::vector<int> const pages = harness.backend.children(bar);
    REQUIRE(pages.size() == 2);
    CHECK(harness.backend.prop(pages[0], "visible") == "false");
    CHECK(harness.backend.prop(pages[1], "visible") == "true");
    CHECK(harness.backend.prop(bar, "tabs") == "[Note,Home]");
    CHECK(harness.backend.prop(bar, "selected") == "1");

    shell.activate(0);
    harness.settle();
    CHECK(harness.backend.prop(pages[0], "visible") == "true");
    CHECK(harness.backend.prop(pages[1], "visible") == "false");
    harness.backend.click(harness.byTestId("Button", "edit"));
    harness.backend.click(harness.byTestId("Button", "rename"));
    harness.settle();
    CHECK(captions(shell) == std::vector<std::string>{"Renamed •", "Home"});
    CHECK(harness.backend.prop(bar, "selected") == "0");

    // A user's pick on the bar activates that tab.
    harness.backend.chooseIndex(bar, 1);
    harness.settle();
    CHECK(shell.active() == 1U);
}

// Mutation: keep walking after a veto (the third tab closes too).
TEST_CASE("workspace: close all walks the tabs one at a time and stops at the first veto", "[ui][workspace]") {
    Harness harness;
    Host host{harness, false};  // the guard keeps every dirty screen open
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    REQUIRE(shell.open("home", {}).has_value());
    REQUIRE(shell.open("note", note(2)).has_value());
    REQUIRE(shell.open("draft", {}).has_value());
    shell.activate(1);
    harness.settle();
    harness.backend.click(harness.byTestId("Button", "edit"));
    harness.settle();

    std::optional<bool> outcome;
    shell.closeAll([&outcome](bool closed) { outcome = closed; });
    harness.settle();
    REQUIRE(outcome.has_value());
    CHECK_FALSE(*outcome);
    REQUIRE(shell.tabs().size() == 2);
    CHECK(shell.tabs()[0].screenId == "note");
    CHECK(shell.tabs()[1].screenId == "draft");

    // Close saved leaves the dirty note and closes the rest.
    outcome.reset();
    shell.closeSaved([&outcome](bool closed) { outcome = closed; });
    harness.settle();
    CHECK(outcome == true);
    REQUIRE(shell.tabs().size() == 1);
    CHECK(shell.tabs()[0].screenId == "note");
    CHECK(shell.active() == 0U);
}

// Mutation: walk the tabs in the order they were opened (Ctrl+Tab from c shows a, not b).
TEST_CASE("workspace: Ctrl+Tab walks the tabs most recently used first, and Ctrl+W closes the active one",
          "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    REQUIRE(shell.open("note", note(1)).has_value());  // a
    REQUIRE(shell.open("note", note(2)).has_value());  // b
    REQUIRE(shell.open("note", note(3)).has_value());  // c, active; recent order c, b, a
    shell.activate(0);                                 // recent order a, c, b
    harness.settle();
    int const bar = harness.backend.idOf(shell.root());
    REQUIRE(harness.backend.press(bar, "Ctrl+Tab"));
    CHECK(shell.active() == 2U);  // c
    REQUIRE(harness.backend.press(bar, "Ctrl+Tab"));
    CHECK(shell.active() == 1U);  // b: the walk keeps the order it started with
    REQUIRE(harness.backend.press(bar, "Ctrl+Shift+Tab"));
    CHECK(shell.active() == 2U);  // back to c

    REQUIRE(harness.backend.press(bar, "Ctrl+W"));
    harness.settle();
    REQUIRE(shell.tabs().size() == 2);
    CHECK(shell.tabs()[1].identity == Value{std::int64_t{2}});
    CHECK(shell.active() == 1U);  // the walk showed b just before c, so b is the most recently used now
}

TEST_CASE("workspace: a refused mount opens nothing; a document's navigate opens through the shell",
          "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    host.shell = &shell;
    auto const refused = shell.open("nothing", {});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().reason == "unknown screen");
    CHECK(shell.tabs().empty());
    CHECK_FALSE(shell.active().has_value());
    CHECK(harness.backend.children(harness.backend.idOf(shell.root())).empty());

    host.onNavigate("home", ParamMap{});
    harness.settle();
    REQUIRE(shell.tabs().size() == 1);
    CHECK(shell.tabs()[0].screenId == "home");
}

// Control: drop the host's settleCall (the asker reads `none`, and the reopened picker answers it).
TEST_CASE("workspace: a call to a screen whose tab is open is cancelled, and no later mount answers it",
          "[ui][workspace]") {
    Harness harness;
    Application application;
    Host host{harness, std::nullopt, &application};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    host.shell = &shell;
    REQUIRE(shell.open("picker", {}) == 0U);
    REQUIRE(shell.open("asker", {}) == 1U);
    harness.settle();
    harness.backend.click(harness.byTestId("Button", "ask"));
    harness.settle();
    // The navigation focused the picker's tab and mounted nothing, so its call was answered with a cancel.
    CHECK(shell.active() == 0U);
    CHECK(shell.tabs().size() == 2);
    CHECK(harness.text("got") == "cancelled");

    // The picker the user opened answers no call: its return is the application's.
    harness.backend.click(harness.byTestId("Button", "answer"));
    harness.settle();
    CHECK(application.returned == std::vector<std::string>{"picker"});
    shell.activate(1);
    harness.settle();
    CHECK(harness.text("got") == "cancelled");

    // Nor does a later mount of the picker: the call is gone, not waiting for it.
    std::optional<bool> closed;
    shell.close(0, [&closed](bool done) { closed = done; });
    harness.settle();
    REQUIRE(closed == true);
    REQUIRE(shell.open("picker", {}) == 1U);
    harness.settle();
    harness.backend.click(harness.byTestId("Button", "answer"));
    harness.settle();
    CHECK(application.returned == std::vector<std::string>{"picker", "picker"});
    shell.activate(0);
    harness.settle();
    CHECK(harness.text("got") == "cancelled");
    CHECK(shell.tabs().size() == 2);
}

// Control: let a navigation without onResult and onCancel file a call (the picker answers it and closes itself, so
// the application hears no return and its tab goes).
TEST_CASE("workspace: a navigate that asks for no answer files no call, so its screen's return is the application's",
          "[ui][workspace]") {
    Harness harness;
    Application application;
    Host host{harness, std::nullopt, &application};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    host.shell = &shell;
    REQUIRE(shell.open("asker", {}) == 0U);
    harness.settle();
    harness.backend.click(harness.byTestId("Button", "show"));
    harness.settle();
    REQUIRE(shell.tabs().size() == 2);
    harness.backend.click(harness.byTestId("Button", "answer"));
    harness.settle();
    CHECK(application.returned == std::vector<std::string>{"picker"});
    CHECK(shell.tabs().size() == 2);
    CHECK(harness.text("got") == "none");
}

// Control: route `as` to the tab path (no dialog is recorded, and the bar gains a tab); or leave the overlay open
// after its screen returned (the dialog stays).
TEST_CASE("workspace: a navigate as a dialog shows the callee over the tabs, and its return answers the opener",
          "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    host.shell = &shell;
    REQUIRE(shell.open("asker", {}) == 0U);
    harness.settle();
    int const bar = harness.backend.idOf(shell.root());
    harness.backend.click(harness.byTestId("Button", "askDialog"));
    harness.settle();
    CHECK(shell.tabs().size() == 1);
    CHECK(harness.backend.prop(bar, "tabs") == "[Asker]");
    CHECK(shell.overlays() == std::vector<std::string>{"picker"});
    std::vector<int> const dialogs = harness.backend.all("Dialog");
    REQUIRE(dialogs.size() == 1);
    CHECK(harness.backend.prop(dialogs[0], "open") == "true");
    CHECK(harness.backend.prop(dialogs[0], "title") == "Picker");
    std::vector<int> const held = harness.backend.children(dialogs[0]);
    CHECK(std::ranges::contains(held, harness.byTestId("Button", "answer")));

    harness.backend.click(harness.byTestId("Button", "answer"));
    harness.settle();
    CHECK(harness.text("got") == "42");
    CHECK(shell.overlays().empty());
    CHECK(harness.backend.all("Dialog").empty());
    CHECK(shell.tabs().size() == 1);
}

// Control: drop the overlay's reopening in its dismissal (the dirty note's dialog stays closed); or remove it before
// its screen answers (the dirty note's dialog goes).
TEST_CASE("workspace: dismissing an overlay asks its screen: a clean one is cancelled, a dirty one stays open",
          "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};  // no guard: a dirty screen stays open
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    host.shell = &shell;
    REQUIRE(shell.open("asker", {}) == 0U);
    harness.settle();

    harness.backend.click(harness.byTestId("Button", "askDrawer"));
    harness.settle();
    std::vector<int> const drawers = harness.backend.all("Drawer");
    REQUIRE(drawers.size() == 1);
    CHECK(harness.backend.prop(drawers[0], "side") == "End");
    harness.backend.dismiss(drawers[0]);
    harness.settle();
    CHECK(harness.text("got") == "cancelled");
    CHECK(shell.overlays().empty());

    harness.backend.click(harness.byTestId("Button", "askNote"));
    harness.settle();
    REQUIRE(shell.overlays() == std::vector<std::string>{"note"});
    harness.backend.click(harness.byTestId("Button", "edit"));
    harness.settle();
    int const dialog = harness.backend.all("Dialog").at(0);
    harness.backend.dismiss(dialog);
    harness.settle();
    CHECK(shell.overlays() == std::vector<std::string>{"note"});
    CHECK(harness.backend.prop(dialog, "open") == "true");
    CHECK(harness.backend.log().back().contains("open=true"));
}

// Control: drop the sheet on a compact class (no `sheet=true`); or leave setWidthClass out (it stays a sheet).
TEST_CASE("workspace: a dialog overlay is a sheet while the width class is compact, and follows it",
          "[ui][workspace]") {
    Harness harness;
    harness.backend.setWidthClass("compact");
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    host.shell = &shell;
    REQUIRE(shell.open("asker", {}) == 0U);
    harness.settle();
    harness.backend.click(harness.byTestId("Button", "askDialog"));
    harness.settle();
    int const dialog = harness.backend.all("Dialog").at(0);
    CHECK(harness.backend.prop(dialog, "sheet") == "true");
    shell.setWidthClass("expanded");
    CHECK(harness.backend.prop(dialog, "sheet") == "false");
}

// Control: close the tabs before the overlays in forceCloseAll (the asker is gone when the picker's close cancels its
// call, so its onCancel notifies nothing); or drop the report of a head's menu and tools.
TEST_CASE(
    "workspace: forceCloseAll closes the overlays first, so the opener's onCancel runs; a head's menu and tools "
    "are reported once",
    "[ui][workspace]") {
    Harness harness;
    Application application;
    Host host{harness, std::nullopt, &application};
    WorkspaceOptions options = catalog();
    options.diagnostics = [](morph::ui::Refusal const& refusal) { reported().push_back(refusal.reason); };
    Workspace shell{harness.runtime, harness.backend, host, std::move(options)};
    host.shell = &shell;
    REQUIRE(shell.open("asker", {}) == 0U);
    harness.settle();
    harness.backend.click(harness.byTestId("Button", "askDialog"));
    harness.settle();
    int const dialog = harness.backend.all("Dialog").at(0);
    int const answer = harness.byTestId("Button", "answer");
    std::size_t const before = harness.backend.log().size();
    shell.forceCloseAll();
    harness.settle();
    // The opener's onCancel ran while it was still mounted, and the callee went before its dialog.
    CHECK(application.notified == std::vector<std::string>{"cancelled"});
    std::vector<std::string> const& log = harness.backend.log();
    auto const at = [&log, before](std::string const& line) {
        return std::find(log.begin() + static_cast<std::ptrdiff_t>(before), log.end(), line) - log.begin();
    };
    CHECK(at("destroy Button#" + std::to_string(answer)) < at("destroy Dialog#" + std::to_string(dialog)));
    CHECK(at("destroy Dialog#" + std::to_string(dialog)) < static_cast<std::ptrdiff_t>(log.size()));
    CHECK(shell.overlays().empty());
    CHECK(shell.tabs().empty());

    REQUIRE(shell.open("asker", {}) == 0U);
    harness.settle();
    reported().clear();
    harness.backend.click(harness.byTestId("Button", "askTooled"));
    harness.settle();
    REQUIRE(shell.overlays() == std::vector<std::string>{"tooled"});
    // A change of the head's tools reports nothing more.
    harness.backend.click(harness.byTestId("Button", "upper"));
    harness.settle();
    CHECK(std::ranges::count(reported(),
                             std::string{"a dialog or drawer does not show the head menu or tools of tooled"}) == 1);
}

// Mutation: close the tab at a clamped index when the index is past the end, or let closeOthers close the kept one.
TEST_CASE("workspace: close past the last tab answers false, and close others keeps the one named",
          "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    REQUIRE(shell.open("home", {}).has_value());
    REQUIRE(shell.open("note", note(1)).has_value());
    REQUIRE(shell.open("note", note(2)).has_value());
    harness.settle();

    std::optional<bool> outcome;
    shell.close(7, [&outcome](bool closed) { outcome = closed; });
    CHECK(outcome == false);
    CHECK(shell.tabs().size() == 3);
    shell.close(9, {});  // no callback: nothing to answer
    CHECK(shell.tabs().size() == 3);

    outcome.reset();
    shell.closeOthers(1, [&outcome](bool closed) { outcome = closed; });
    harness.settle();
    CHECK(outcome == true);
    REQUIRE(shell.tabs().size() == 1);
    CHECK(shell.tabs()[0].identity == Value{std::int64_t{1}});
}

// Mutation: walk with a single tab (it re-activates and re-records the only tab), or answer a close whose workspace
// is gone (the callback runs on a destroyed shell).
TEST_CASE("workspace: a walk needs two tabs, and a close answered after the workspace is gone does nothing",
          "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    auto shell = std::make_unique<Workspace>(harness.runtime, harness.backend, host, catalog());
    shell->next();  // no tabs
    REQUIRE(shell->open("note", note(1)).has_value());
    harness.settle();
    shell->next();
    shell->previous();
    CHECK(shell->active() == 0U);

    bool answered = false;
    shell->closeAll([&answered](bool) { answered = true; });
    shell.reset();
    harness.settle();
    CHECK_FALSE(answered);
}

// Mutation: let forceCloseAll ask the guard (the dirty draft keeps its tab).
TEST_CASE("workspace: a tab's handle is reachable, and forceCloseAll closes every tab without asking",
          "[ui][workspace]") {
    Harness harness;
    Host host{harness, false};  // a guard that would keep every dirty screen open
    int changes = 0;
    WorkspaceOptions options = catalog();
    options.onChanged = [&changes] { ++changes; };
    Workspace shell{harness.runtime, harness.backend, host, std::move(options)};
    REQUIRE(shell.open("note", note(1)) == 0U);
    REQUIRE(shell.open("draft", {}) == 1U);
    harness.settle();
    CHECK(changes >= 2);
    REQUIRE(shell.handleOf(0) != nullptr);
    CHECK(shell.handleOf(2) == nullptr);
    CHECK(shell.activeHandle() == shell.handleOf(1));

    shell.forceCloseAll();
    harness.settle();
    CHECK(shell.tabs().empty());
    CHECK(shell.activeHandle() == nullptr);
}

// Mutation: leave the menu out of buildHeadRow (no `head_menu`), or out of headHasContent (a head with a menu only is
// hidden); let a hidden entry be chosen (no "is hidden").
TEST_CASE("workspace: a head's own menu is drawn behind one button, and an entry is chosen by its id",
          "[ui][workspace]") {
    using morph::ui::HeadMenuEntry;
    reported().clear();
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    REQUIRE(shell.open("menued", {}) == 0U);
    harness.settle();
    morph::ui::ScreenHead* const head = shell.handleOf(0)->head();
    REQUIRE(head != nullptr);
    REQUIRE(head->menu() != nullptr);
    CHECK(head->menu()->peek() ==
          std::vector<HeadMenuEntry>{{.id = "calibrate", .label = "Calibrate"},
                                     {.separator = true},
                                     {.id = "retire", .label = "Take out of service", .description = "needs reason"},
                                     {.label = "Export", .items = {{.id = "csv", .label = "CSV"}}}});
    REQUIRE(head->menuLabel() != nullptr);
    CHECK(head->menuLabel()->peek() == "More instrument actions");
    CHECK(shell.activeHeadShown());

    // The row draws the menu as a button-style menu: the icon alone, named by the head's menuLabel.
    CHECK(harness.backend.prop(harness.byTestId("Row", "head"), "visible") != "false");
    int const menu = harness.byTestId("Menu", "head_menu");
    CHECK(harness.backend.prop(menu, "style") == "Button");
    CHECK(harness.backend.prop(menu, "icon") == "more");
    CHECK(harness.backend.prop(menu, "items") ==
          "[Calibrate,;separator,Take out of service;description=needs reason,Export[CSV]]");
    CHECK(harness.backend.dump().contains("More instrument actions"));

    harness.backend.chooseMenuEntry(menu, {2});
    harness.settle();
    CHECK(harness.text("log") == "retired");
    harness.backend.chooseMenuEntry(menu, {3, 0});
    harness.settle();
    CHECK(harness.text("log") == "csv");

    // Hiding an entry draws the menu again without it; the entry past it keeps its own handler.
    harness.backend.click(harness.byTestId("Button", "hide"));
    harness.settle();
    int const redrawn = harness.byTestId("Menu", "head_menu");
    CHECK(harness.backend.prop(redrawn, "items") == "[Calibrate,;separator,Export[CSV]]");
    harness.backend.chooseMenuEntry(redrawn, {0});
    harness.settle();
    CHECK(harness.text("log") == "calibrated");
    harness.backend.chooseMenuEntry(redrawn, {2, 0});
    harness.settle();
    CHECK(harness.text("log") == "csv");

    // Choosing by id refuses a hidden or an unknown entry by name, and runs nothing.
    CHECK_FALSE(head->choose("retire"));
    CHECK_FALSE(head->choose("nowhere"));
    CHECK(head->choose("calibrate"));
    harness.settle();
    CHECK(harness.text("log") == "calibrated");
    CHECK(reported() ==
          std::vector<std::string>{"head menu entry retire is hidden", "head menu entry nowhere is unknown"});
}

// Mutation: leave the tools out of buildHeadRow (no `head_tool_q`), or out of headHasContent (a head with tools only
// is hidden); let a hidden tool be edited (no "is hidden").
TEST_CASE("workspace: a head's search and select are drawn in its row, controlled by the document",
          "[ui][workspace]") {
    using morph::ui::HeadTool;
    using morph::ui::HeadToolKind;
    reported().clear();
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    REQUIRE(shell.open("tooled", {}) == 0U);
    harness.settle();
    morph::ui::ScreenHead* const head = shell.handleOf(0)->head();
    REQUIRE(head != nullptr);
    REQUIRE(head->tools() != nullptr);
    CHECK(head->tools()->peek() ==
          std::vector<HeadTool>{{.id = "q",
                                 .kind = HeadToolKind::Search,
                                 .label = "Search samples",
                                 .value = "",
                                 .placeholder = "Sample ID",
                                 .options = {},
                                 .selected = std::nullopt,
                                 .width = 30,
                                 .enabled = true},
                                {.id = "lab",
                                 .kind = HeadToolKind::Select,
                                 .label = "Lab",
                                 .value = "",
                                 .placeholder = "",
                                 .options = {{.key = morph::ui::Key{"North"}, .label = "North"},
                                             {.key = morph::ui::Key{"South"}, .label = "South"}},
                                 .selected = morph::ui::Key{"North"},
                                 .width = 0,
                                 .enabled = true}});
    CHECK(shell.activeHeadShown());
    CHECK(harness.backend.prop(harness.byTestId("Row", "head"), "visible") != "false");

    // The search: typing runs onChange, the field shows the state's value, and a change of the state shows in it.
    int const search = harness.byTestId("TextInput", "head_tool_q");
    CHECK(harness.backend.prop(search, "placeholder") == "Sample ID");
    CHECK(harness.backend.dump().contains("Search samples"));
    harness.backend.edit(search, "HPLC");
    harness.settle();
    CHECK(harness.text("q") == "HPLC");
    CHECK(harness.backend.prop(search, "text") == "HPLC");
    harness.backend.click(harness.byTestId("Button", "upper"));
    harness.settle();
    CHECK(harness.backend.prop(search, "text") == "HPLC-7");
    harness.backend.submit(search, "HPLC-7");
    harness.settle();
    CHECK(harness.text("submitted") == "HPLC-7");

    // The select: a choice runs onChoose with the key.
    int const select = harness.byTestId("Select", "head_tool_lab");
    harness.backend.choose(select, morph::ui::Key{"South"});
    harness.settle();
    CHECK(harness.text("lab") == "South");

    // A hidden tool is not drawn, and is refused by name; an unknown one too.
    CHECK_FALSE(harness.backend.find("TextInput", "testId", "head_tool_later").has_value());
    CHECK_FALSE(head->edit("later", Value{std::string{"x"}}));
    CHECK_FALSE(head->edit("x", Value{std::string{"x"}}));
    CHECK_FALSE(head->submit("lab"));
    CHECK(reported() == std::vector<std::string>{"head tool later is hidden", "head tool x is unknown",
                                                 "head tool lab is a select, which is not submitted"});
}

// Mutation: leave the runs out of the head's text (it draws the plain subtitle), or leave `subtitleSpans` out of the
// head (a host that knows runs finds none).
TEST_CASE("workspace: a head with subtitle runs mounts its subtitle text with those runs", "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    REQUIRE(shell.open("runhead", {}) == 0U);
    harness.settle();
    morph::ui::ScreenHead const* const head = shell.handleOf(0)->head();
    REQUIRE(head != nullptr);
    REQUIRE(head->subtitleSpans() != nullptr);
    CHECK(head->subtitleSpans()->peek() ==
          std::vector<morph::ui::TextSpan>{{.text = "Instruments > "}, {.text = "EQ-0142", .mono = true}});
    int const subtitle = harness.byTestId("Text", "head_subtitle");
    CHECK(harness.backend.prop(subtitle, "spans") == "[Instruments > ,EQ-0142;mono]");
}

// Mutation: answer null from a document screen's `head()` (nothing reaches the head or the row).
TEST_CASE("workspace: a document's head answers its subtitle, visible badges and actions, and runs an enabled one",
          "[ui][workspace]") {
    using morph::ui::HeadAction;
    using morph::ui::HeadBadge;
    reported().clear();
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    REQUIRE(shell.open("headed", {}) == 0U);
    harness.settle();
    morph::ui::ScreenHead* const head = shell.handleOf(0)->head();
    REQUIRE(head != nullptr);
    CHECK(head->subtitle().peek() == "Editing");
    CHECK(head->status().peek() ==
          std::vector<HeadBadge>{{.text = "Draft", .tone = morph::ui::Tone::Warn, .icon = ""}});
    CHECK(head->actions().peek() == std::vector<HeadAction>{{.id = "save",
                                                             .label = "Save",
                                                             .icon = "save",
                                                             .emphasis = morph::ui::ButtonEmphasis::Primary,
                                                             .enabled = true},
                                                            {.id = "publish",
                                                             .label = "Publish",
                                                             .icon = "",
                                                             .emphasis = morph::ui::ButtonEmphasis::Normal,
                                                             .enabled = false}});

    CHECK(head->run("save"));
    harness.settle();
    CHECK(head->subtitle().peek() == "Editing!");
    CHECK_FALSE(head->run("publish"));
    CHECK_FALSE(head->run("nowhere"));
    harness.settle();
    CHECK(head->subtitle().peek() == "Editing!");
    CHECK(reported() == std::vector<std::string>{"head action publish is disabled", "head action nowhere is unknown"});

    // The row above the screen: the subtitle, the visible badge, and the actions as buttons.
    CHECK(harness.backend.prop(harness.byTestId("Row", "head"), "visible") != "false");
    CHECK(harness.text("head_subtitle") == harness.text("body"));
    CHECK(harness.backend.prop(harness.byTestId("Badge", "head_status_0"), "text") == "Draft");
    CHECK(harness.backend.find("Badge", "testId", "head_status_0").has_value());
    CHECK_FALSE(harness.backend.find("Badge", "testId", "head_status_1").has_value());
    CHECK(harness.backend.prop(harness.byTestId("Button", "head_action_save"), "icon") == "save");
    CHECK(harness.backend.prop(harness.byTestId("Button", "head_action_publish"), "enabled") == "false");
    harness.backend.click(harness.byTestId("Button", "head_action_save"));
    harness.settle();
    CHECK(head->subtitle().peek() == "Editing!!");
    CHECK(reported().size() == 2);
}

// Mutation: show the row whatever the head holds (a screen without a head gets an empty row).
TEST_CASE("workspace: a screen without head parts leaves the head row hidden", "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    REQUIRE(shell.open("home", {}) == 0U);
    harness.settle();
    morph::ui::ScreenHead const* const head = shell.handleOf(0)->head();
    REQUIRE(head != nullptr);
    CHECK(head->subtitle().peek().empty());
    CHECK(head->status().peek().empty());
    CHECK(head->actions().peek().empty());
    CHECK(harness.backend.prop(harness.byTestId("Row", "head"), "visible") == "false");
}

// Mutation: answer activeHeadShown() from the first tab rather than the active one (it stays false after the switch
// to the headed tab), or read the active tab untracked (the effect never hears the switch).
TEST_CASE("workspace: activeHeadShown follows the active tab's head, as a tracked read", "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    std::vector<bool> heard;
    morph::reactive::Effect const watch{harness.runtime, [&] { heard.push_back(shell.activeHeadShown()); }};
    CHECK(heard == std::vector<bool>{false});
    REQUIRE(shell.open("home", {}) == 0U);
    harness.settle();
    CHECK_FALSE(shell.activeHeadShown());
    REQUIRE(shell.open("headed", {}) == 1U);
    harness.settle();
    CHECK(shell.activeHeadShown());
    shell.activate(0);
    harness.settle();
    CHECK_FALSE(shell.activeHeadShown());
    CHECK(heard.back() == false);
    CHECK(std::ranges::find(heard, true) != heard.end());
}

// Mutation: build headPartsForActive() from Workspace::active()'s index rather than the model's own tab id (would
// desync across a close that reindexes a later tab). Also covers: no tab open gives defined, invisible rows, never a
// null Node (headPartsForActive() must never throw inside Mounted's own null check).
TEST_CASE("workspace: headPartsForActive mounts the active tab's own head, separately from the in-page one",
          "[ui][workspace]") {
    using morph::ui::Mounted;
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};

    // No tab open: defined, invisible rows, not null Nodes.
    {
        morph::ui::HeadParts parts = shell.headPartsForActive();
        Mounted const lead{harness.runtime, harness.backend, std::move(parts.lead)};
        Mounted const trail{harness.runtime, harness.backend, std::move(parts.trail)};
        CHECK(harness.backend.prop(harness.byTestId("Row", "head_lead"), "visible") == "false");
        CHECK(harness.backend.prop(harness.byTestId("Row", "head_trail"), "visible") == "false");
    }

    REQUIRE(shell.open("headed", {}) == 0U);
    harness.settle();
    morph::ui::HeadParts parts = shell.headPartsForActive();
    Mounted const lead{harness.runtime, harness.backend, std::move(parts.lead)};
    Mounted const trail{harness.runtime, harness.backend, std::move(parts.trail)};
    harness.settle();
    // Two renderings of the same tab's head, both present -- confirming headPartsForActive() reads the live active
    // tab's own data, the same way the in-page headRow() does, not a stale snapshot.
    std::size_t rows = 0;
    for (int const text : harness.backend.all("Text")) {
        rows += harness.backend.prop(text, "testId") == "head_subtitle" ? 1U : 0U;
    }
    CHECK(rows == 2);
}

// Mutation: mount the in-page head even for External (a page holds a head row the host also mounts).
TEST_CASE("workspace: an external head placement leaves the page without a head row", "[ui][workspace]") {
    using morph::ui::HeadPlacement;
    using morph::ui::Mounted;
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog(), nullptr, HeadPlacement::External};
    REQUIRE(shell.open("headed", {}) == 0U);
    harness.settle();
    // The page holds the screen only: no head row of its own.
    CHECK_FALSE(harness.backend.find("Row", "testId", "head").has_value());
    // The parts the host mounts itself show the active tab's subtitle and its action.
    morph::ui::HeadParts parts = shell.headPartsForActive();
    Mounted const lead{harness.runtime, harness.backend, std::move(parts.lead)};
    Mounted const trail{harness.runtime, harness.backend, std::move(parts.trail)};
    harness.settle();
    CHECK(harness.text("head_subtitle") == "Editing");
    CHECK(harness.backend.find("Button", "testId", "head_action_save").has_value());
}

// Mutation: drop `minWidth` from the lead (its layout record loses `min=32`), its badges' `wrap`, or the head row's
// own `wrap`, `justify` or `align`.
TEST_CASE("workspace: the in-page head is a wrapping line of a lead that stretches from 32 units and a trail",
          "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    REQUIRE(shell.open("headed", {}) == 0U);
    harness.settle();
    int const head = harness.byTestId("Row", "head");
    int const lead = harness.byTestId("Row", "head_lead");
    int const trail = harness.byTestId("Row", "head_trail");
    CHECK(harness.backend.prop(head, "wrap") == "true");
    CHECK(harness.backend.prop(head, "justify") == "end");
    CHECK(harness.backend.prop(head, "align") == "start");
    CHECK(harness.backend.children(head) == std::vector<int>{lead, trail});
    CHECK(harness.backend.prop(lead, "layout") == "stretch(1) min=32/content");
    // The subtitle and the badges share one wrapping line, and the badges' holder wraps too.
    CHECK(harness.backend.prop(lead, "wrap") == "true");
    REQUIRE(harness.backend.children(lead).size() == 2);
    CHECK(harness.backend.prop(harness.backend.children(lead).at(1), "wrap") == "true");
    CHECK_FALSE(harness.backend.hasProp(trail, "wrap"));
}

namespace {

using morph::ui::WorkspaceModel;
using morph::ui::WorkspaceModelOptions;
using morph::ui::WorkspaceTabId;

/// A model over the harness whose pages are slots it counts: what an application's own tab strip has.
struct ModelPages {
    explicit ModelPages(Harness& harness, Host& host, WorkspaceOptions options = catalog()) {
        WorkspaceModelOptions made{std::move(options), {}, {}};
        made.mountPage = [this, &harness, &host](WorkspaceTabId tab, std::string_view screenId, ParamMap const& params)
            -> std::expected<std::unique_ptr<ScreenHandle>, morph::ui::MountError> {
            ++mounted;
            auto page = harness.backend.createSlot(nullptr);
            auto handle = host.mountInto(screenId, params, *page);
            if (handle) {
                pages.insert_or_assign(tab.value, std::move(page));
            }
            return handle;
        };
        made.unmountPage = [this](WorkspaceTabId tab) {
            // The handle is gone by now: the model no longer answers it, and the page is still the application's.
            // While the model is destroyed, `model` is already null.
            unmountedAfterHandle.push_back((model == nullptr || model->handleOf(tab) == nullptr) &&
                                           pages.contains(tab.value));
            pages.erase(tab.value);
        };
        model = std::make_unique<WorkspaceModel>(harness.runtime, host, std::move(made));
    }
    ~ModelPages() { model.reset(); }
    ModelPages(ModelPages const&) = delete;
    ModelPages& operator=(ModelPages const&) = delete;
    ModelPages(ModelPages&&) = delete;
    ModelPages& operator=(ModelPages&&) = delete;

    int mounted = 0;
    std::vector<bool> unmountedAfterHandle;
    std::map<std::uint64_t, std::unique_ptr<morph::ui::SlotWidget>> pages;
    std::unique_ptr<WorkspaceModel> model;
};

}  // namespace

// Mutation: keep the logic in Workspace and leave the model opening a new tab every time (note 1 twice, home
// twice, the empty draft kept).
TEST_CASE("workspace model: identity, singleton and reuseEmpty decide which tab an open answers", "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    ModelPages pages{harness, host};
    WorkspaceModel& model = *pages.model;
    auto const first = model.open("note", note(1));
    REQUIRE(first.has_value());
    CHECK(model.open("note", note(1)).value() == *first);
    auto const home = model.open("home", {});
    CHECK(model.open("home", {}).value() == home.value());
    auto const empty = model.open("draft", {});
    REQUIRE(empty.has_value());
    auto const kept = model.open("draft", ParamMap{{"noteId", Value{std::int64_t{5}}}});
    REQUIRE(kept.has_value());
    CHECK(*kept != *empty);
    harness.settle();
    CHECK(pages.mounted == 4);
    CHECK(model.order() == std::vector<WorkspaceTabId>{*first, *home, *kept});
    CHECK(model.handleOf(*empty) == nullptr);
    CHECK(model.active().peek() == kept);
    CHECK(pages.unmountedAfterHandle == std::vector<bool>{true});
    CHECK_THROWS_AS(model.activate(*empty), std::out_of_range);
    CHECK_THROWS_AS(model.close(WorkspaceTabId{999}, {}), std::out_of_range);
}

// Mutation: let tabs() follow only opens and closes (a rename never reaches the strip).
TEST_CASE("workspace model: tabs() changes once per open and once per title change", "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    ModelPages pages{harness, host};
    WorkspaceModel& model = *pages.model;
    int runs = 0;
    morph::reactive::Effect const watch{harness.runtime, [&] {
                                            static_cast<void>(model.tabs().get());
                                            ++runs;
                                        }};
    CHECK(runs == 1);
    REQUIRE(model.open("note", note(1)).has_value());
    harness.settle();
    CHECK(runs == 2);
    REQUIRE(model.tabs().peek().size() == 1);
    CHECK(model.tabs().peek().front().title == "Note");
    harness.backend.click(harness.byTestId("Button", "rename"));
    harness.settle();
    CHECK(runs == 3);
    CHECK(model.tabs().peek().front().title == "Renamed");
    harness.settle();
    CHECK(runs == 3);
}

// Mutation: let closeAll go on past a veto (the dirty note closes too).
TEST_CASE("workspace model: a veto stops closeAll, forceCloseAll closes everything, newest first", "[ui][workspace]") {
    Harness harness;
    Host host{harness, false};
    std::vector<std::vector<std::string>> records;
    WorkspaceOptions options = catalog();
    options.onRecord = [&records](std::vector<std::string> const& routes) { records.push_back(routes); };
    ModelPages pages{harness, host, options};
    WorkspaceModel& model = *pages.model;
    REQUIRE(model.open("note", note(1)).has_value());
    REQUIRE(model.open("note", note(2)).has_value());
    harness.backend.click(harness.backend.all("Button").front());  // edit: the first note now has unsaved changes
    harness.settle();
    std::optional<bool> closed;
    model.closeAll([&closed](bool all) { closed = all; });
    harness.settle();
    CHECK(closed == false);
    CHECK(model.order().size() == 2);
    model.forceCloseAll();
    CHECK(model.order().empty());
    CHECK(pages.unmountedAfterHandle == std::vector<bool>{true, true});
    REQUIRE_FALSE(records.empty());
    CHECK(records.back().empty());

    // The record reopens the tabs it lists.
    std::vector<std::string> const recorded{records.at(1)};
    CHECK(model.start({.start = "resume", .fallback = {}, .recorded = recorded}) == 2);
    CHECK(model.order().size() == 2);
}

TEST_CASE("workspace model: an empty mountPage is refused, and one that throws opens nothing", "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    CHECK_THROWS_AS(WorkspaceModel(harness.runtime, host, WorkspaceModelOptions{}), std::invalid_argument);
    WorkspaceModelOptions throwing{catalog(), {}, {}};
    throwing.mountPage = [](WorkspaceTabId, std::string_view,
                            ParamMap const&) -> std::expected<std::unique_ptr<ScreenHandle>, morph::ui::MountError> {
        throw std::runtime_error{"no page"};
    };
    WorkspaceModel model{harness.runtime, host, std::move(throwing)};
    auto const opened = model.open("home", {});
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().reason == "no page");
    CHECK(model.order().empty());
    CHECK(model.handleOf(WorkspaceTabId{1}) == nullptr);
}

// Mutation: drop the `as` check (a tab request then mounts the note in a dialog).
TEST_CASE("workspace: openOverlay refuses an `as` that is no overlay, a host screen and a refused mount",
          "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, WorkspaceOptions{.screens = [](std::string_view id) {
                        return WorkspaceScreen{.host = id == "audit" ? "audit-view" : ""};
                    }}};
    auto const asTab = shell.openOverlay(morph::ui::NavigateRequest{.screen = "note", .as = "tab"});
    REQUIRE_FALSE(asTab.has_value());
    CHECK(asTab.error().reason == "`as: tab` is not a dialog or a drawer");
    auto const hosted = shell.openOverlay(morph::ui::NavigateRequest{.screen = "audit", .as = "drawer"});
    REQUIRE_FALSE(hosted.has_value());
    CHECK(hosted.error().reason == "a host screen opens as a tab, not an overlay");
    auto const unknown = shell.openOverlay(morph::ui::NavigateRequest{.screen = "nowhere", .as = "dialog"});
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().screenId == "nowhere");
    CHECK(harness.backend.all("Dialog").empty());
    CHECK(harness.backend.all("Drawer").empty());
}

// Mutation: restore the lead's minimum in `headPartsForActive` (its layout reads `min=32`), or drop it from the
// in-page head's lead.
TEST_CASE("workspace: the lead built for a shell has no minimum width, and the in-page head's lead keeps it",
          "[ui][workspace]") {
    using morph::ui::Mounted;
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    REQUIRE(shell.open("headed", {}) == 0U);
    harness.settle();
    int const inPage = harness.byTestId("Row", "head_lead");
    CHECK(harness.backend.prop(inPage, "layout") == "stretch(1) min=32/content");
    morph::ui::HeadParts parts = shell.headPartsForActive();
    Mounted const lead{harness.runtime, harness.backend, std::move(parts.lead)};
    harness.settle();
    int shellLead = -1;
    for (int const row : harness.backend.all("Row")) {
        if (row != inPage && harness.backend.prop(row, "testId") == "head_lead") {
            shellLead = row;
        }
    }
    REQUIRE(shellLead != -1);
    CHECK(harness.backend.prop(shellLead, "layout") == "stretch(1)/content");
}

// Mutation: answer no runs for the active tab, or read the tab untracked (the second check fails).
TEST_CASE("workspace: activeTitleSpans follows the active tab and its screen's state", "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    CHECK(shell.activeTitleSpans().empty());
    REQUIRE(shell.open("titled", {}) == 0U);
    harness.settle();
    CHECK(shell.activeTitleSpans() ==
          std::vector<morph::ui::TextSpan>{{.text = "Plate "}, {.text = "PL-0193", .mono = true}});
    harness.backend.click(harness.byTestId("Button", "rename"));
    harness.settle();
    CHECK(shell.activeTitleSpans() ==
          std::vector<morph::ui::TextSpan>{{.text = "Plate "}, {.text = "PL-0200", .mono = true}});
    REQUIRE(shell.open("headed", {}) == 1U);
    harness.settle();
    CHECK(shell.activeTitleSpans().empty());
}

// Mutation: report a dropped run on every read (the reason is listed twice).
TEST_CASE("workspace: a title run the head cannot read is dropped and reported once", "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    reported().clear();
    REQUIRE(shell.open("badtitle", {}) == 0U);
    harness.settle();
    CHECK(shell.activeTitleSpans() == std::vector<morph::ui::TextSpan>{{.text = "Plate "}});
    CHECK(shell.activeTitleSpans() == std::vector<morph::ui::TextSpan>{{.text = "Plate "}});
    CHECK(reported().size() == 1);
}

// Mutation: leave the eyebrow out of `headHasContent` (a head with only an eyebrow is hidden), or hide the eyebrow
// text while it has runs.
TEST_CASE("workspace: the eyebrow is a muted line of the head, and a head with only an eyebrow has content",
          "[ui][workspace]") {
    using morph::ui::Mounted;
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog(), nullptr, morph::ui::HeadPlacement::External};
    REQUIRE(shell.open("eyebrowonly", {}) == 0U);
    harness.settle();
    CHECK(shell.activeHeadShown());
    morph::ui::HeadParts parts = shell.headPartsForActive();
    Mounted const eyebrow{harness.runtime, harness.backend, std::move(parts.eyebrow)};
    harness.settle();
    int const text = harness.byTestId("Text", "head_eyebrow");
    CHECK(harness.backend.prop(text, "spans") == "[Step 2 of 4]");
    CHECK(harness.backend.prop(text, "role") == "Muted");
    CHECK(harness.backend.prop(text, "visible") == "true");
    harness.backend.click(harness.byTestId("Button", "clear"));
    harness.settle();
    CHECK(harness.backend.prop(text, "visible") == "false");
    CHECK_FALSE(shell.activeHeadShown());
}

// Mutation: leave the eyebrow out of the in-page head (no `head_eyebrow`), or put it after the head row.
TEST_CASE("workspace: the in-page head shows its eyebrow above the head row", "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    REQUIRE(shell.open("eyebrowruns", {}) == 0U);
    harness.settle();
    int const eyebrow = harness.byTestId("Text", "head_eyebrow");
    int const head = harness.byTestId("Row", "head");
    int const block = harness.byTestId("Column", "head_block");
    CHECK(harness.backend.children(block) == std::vector<int>{eyebrow, head});
    CHECK(harness.backend.prop(eyebrow, "spans") == "[Worksheet ,WS-1;mono]");
}

// Mutation: ignore the tab strip (the second check fails), or ignore the screen's flag (the first fails).
TEST_CASE("workspace: the page is uninset around a lone screen that insets itself, and inset again with a second tab",
          "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog()};
    CHECK(shell.activePageInset());
    REQUIRE(shell.open("edgeroot", {}) == 0U);
    harness.settle();
    CHECK_FALSE(shell.activePageInset());
    REQUIRE(shell.open("headed", {}) == 1U);
    harness.settle();
    CHECK(shell.activePageInset());
    shell.activate(0);
    harness.settle();
    CHECK(shell.activePageInset());
}

// Mutation: build the lead in one place whatever the placement (the `end` copy or the `inline` copy is never the
// shown one), or leave the `end` copy out of the trail's content.
TEST_CASE("workspace: the lead is built once per place and shown only in the place the head asks for",
          "[ui][workspace]") {
    using morph::ui::LeadPlacement;
    using morph::ui::Mounted;
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog(), nullptr, morph::ui::HeadPlacement::External};
    CHECK(shell.activeLeadPlacement() == LeadPlacement::Below);
    REQUIRE(shell.open("leadplaced", {}) == 0U);
    harness.settle();
    morph::ui::HeadParts parts = shell.headPartsForActive();
    Mounted const lead{harness.runtime, harness.backend, std::move(parts.lead)};
    Mounted const inlineLead{harness.runtime, harness.backend, std::move(parts.inlineLead)};
    Mounted const trail{harness.runtime, harness.backend, std::move(parts.trail)};
    harness.settle();
    auto& backend = harness.backend;
    int const below = harness.byTestId("Row", "head_lead");
    int const inlined = harness.byTestId("Row", "head_lead_inline");
    int const end = harness.byTestId("Row", "head_lead_end");
    int const trailRow = harness.byTestId("Row", "head_trail");

    CHECK(shell.activeLeadPlacement() == LeadPlacement::End);
    CHECK(backend.prop(end, "visible") == "true");
    CHECK(backend.prop(below, "visible") == "false");
    CHECK(backend.prop(inlined, "visible") == "false");
    // Between the tools and the actions, content-sized: no stretch and no minimum.
    std::vector<int> const parts_ = backend.children(trailRow);
    REQUIRE(parts_.size() == 4);
    CHECK(parts_.at(1) == end);
    for (int const copy : {end, inlined}) {
        CHECK_FALSE(backend.prop(copy, "layout").contains("stretch"));
        CHECK_FALSE(backend.prop(copy, "layout").contains("min"));
    }
    CHECK(backend.prop(harness.byTestId("Text", "head_subtitle_end"), "visible") == "true");
    CHECK(backend.prop(harness.byTestId("Badge", "head_status_end_0"), "text") == "Draft");
    CHECK(backend.prop(below, "layout") == "stretch(1)/content");

    // A change of placement swaps the copies, and the shell's read follows.
    backend.click(harness.byTestId("Button", "inline"));
    harness.settle();
    CHECK(shell.activeLeadPlacement() == LeadPlacement::Inline);
    CHECK(backend.prop(inlined, "visible") == "true");
    CHECK(backend.prop(end, "visible") == "false");
    CHECK(backend.prop(below, "visible") == "false");
    backend.click(harness.byTestId("Button", "below"));
    harness.settle();
    CHECK(shell.activeLeadPlacement() == LeadPlacement::Below);
    CHECK(backend.prop(below, "visible") == "true");
    CHECK(backend.prop(inlined, "visible") == "false");
}

// Mutation: ignore the eyebrow (the second check fails), or treat an inline or end lead as one under the title (the
// first fails).
TEST_CASE("workspace: the head is one line while it has no eyebrow and nothing under the title", "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog(), nullptr, morph::ui::HeadPlacement::External};
    REQUIRE(shell.open("leadplaced", {}) == 0U);
    harness.settle();
    CHECK(shell.activeHeadOneLine());
    harness.backend.click(harness.byTestId("Button", "below"));
    harness.settle();
    CHECK_FALSE(shell.activeHeadOneLine());
    REQUIRE(shell.open("eyebrowonly", {}) == 1U);
    harness.settle();
    CHECK_FALSE(shell.activeHeadOneLine());
}

// Mutation: ignore either key (the button keeps its label, or has no name and tooltip).
TEST_CASE("workspace: an icon-only head action draws no label and is named and titled by it", "[ui][workspace]") {
    using morph::ui::Mounted;
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog(), nullptr, morph::ui::HeadPlacement::External};
    REQUIRE(shell.open("leadplaced", {}) == 0U);
    harness.settle();
    morph::ui::HeadParts parts = shell.headPartsForActive();
    Mounted const trail{harness.runtime, harness.backend, std::move(parts.trail)};
    harness.settle();
    int const scan = harness.byTestId("Button", "head_action_scan");
    CHECK(harness.backend.prop(scan, "label").empty());
    CHECK(harness.backend.prop(scan, "icon") == "scan");
    CHECK(harness.backend.prop(scan, "a11yName") == "Scan");
    CHECK(harness.backend.prop(scan, "tooltip") == "Scan");
    harness.backend.click(harness.byTestId("Button", "wide"));
    harness.settle();
    CHECK(harness.backend.prop(scan, "label") == "Scan");
}

// Mutation: read the surface untracked (the second check fails), or answer none.
TEST_CASE("workspace: activeHeadSurface follows the active screen's headSurface", "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog(), nullptr, morph::ui::HeadPlacement::External};
    CHECK(shell.activeHeadSurface().empty());
    REQUIRE(shell.open("leadplaced", {}) == 0U);
    harness.settle();
    CHECK(shell.activeHeadSurface().empty());
    harness.backend.click(harness.byTestId("Button", "dark"));
    harness.settle();
    CHECK(shell.activeHeadSurface() == "darkHead");
    REQUIRE(shell.open("headed", {}) == 1U);
    harness.settle();
    CHECK(shell.activeHeadSurface().empty());
}

// Mutation: read the surface untracked (the second check fails), or answer none.
TEST_CASE("workspace: activePageSurface follows the active screen's pageSurface", "[ui][workspace]") {
    Harness harness;
    Host host{harness, std::nullopt};
    Workspace shell{harness.runtime, harness.backend, host, catalog(), nullptr, morph::ui::HeadPlacement::External};
    CHECK(shell.activePageSurface().empty());
    REQUIRE(shell.open("leadplaced", {}) == 0U);
    harness.settle();
    CHECK(shell.activePageSurface().empty());
    harness.backend.click(harness.byTestId("Button", "plain"));
    harness.settle();
    CHECK(shell.activePageSurface() == "plainPage");
    REQUIRE(shell.open("headed", {}) == 1U);
    harness.settle();
    CHECK(shell.activePageSurface().empty());
}
