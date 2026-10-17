// SPDX-License-Identifier: Apache-2.0
//
// The workspace shell (spec 6 §7, §15): tabs over screens mounted through DocumentScreenHost, focused by identity,
// one per singleton, a reuseEmpty placeholder replaced, captions that follow title and dirty, closing tab by tab
// until a veto, and the most-recently-used walk.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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

/// A host over the harness that routes navigation to the shell and decides a dirty screen's close by a script.
class Host final : public DocumentScreenHost {
public:
    Host(Harness& harness, std::optional<bool> guardAnswer)
        : DocumentScreenHost{harness.session, harness.dispatcher, options(harness, guardAnswer)} {}

    using DocumentScreenHost::onNavigate;
    void onNavigate(std::string_view screenId, ParamMap const& params) override {
        if (shell != nullptr) {
            auto const opened = shell->open(screenId, params);  // a refused mount opens nothing
            static_cast<void>(opened);
        }
    }

    Workspace* shell = nullptr;

private:
    static DocumentScreenHostOptions options(Harness& harness, std::optional<bool> guardAnswer) {
        static std::map<std::string, std::shared_ptr<Document const>, std::less<>> const documents{
            {"note", loadOrFail(kNote, morph::ui::testing::corpusCatalog())},
            {"home", loadOrFail(kHome, morph::ui::testing::corpusCatalog())},
            {"draft", loadOrFail(kDraft, morph::ui::testing::corpusCatalog())}};
        DocumentScreenHostOptions made;
        made.documents = [](std::string_view id) -> std::shared_ptr<Document const> {
            auto const found = documents.find(id);
            return found == documents.end() ? nullptr : found->second;
        };
        made.backend = &harness.backend;
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
        return WorkspaceScreen{.singleton = id == "home", .reuseEmpty = id == "draft"};
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
