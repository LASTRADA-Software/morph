// SPDX-License-Identifier: Apache-2.0
//
// Opening another screen in a tab's place (docs/spec/ui/workspace.md, "Opening in place"), what Open as does, and the
// chrome a shell mounts at the top of each tab's page.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/ui/host.hpp>
#include <morph/ui/view.hpp>
#include <morph/ui/workspace.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_interpreter_support.hpp"

using morph::ui::CloseChoice;
using morph::ui::Document;
using morph::ui::DocumentScreenHost;
using morph::ui::DocumentScreenHostOptions;
using morph::ui::MountError;
using morph::ui::ParamMap;
using morph::ui::ScreenHandle;
using morph::ui::Value;
using morph::ui::Workspace;
using morph::ui::WorkspaceOptions;
using morph::ui::WorkspaceScreen;
using morph::ui::WorkspaceTabId;
using morph::ui::testing::Harness;
using morph::ui::testing::loadOrFail;

namespace {

constexpr std::string_view kNote = R"({
  "vocab": "ui/1", "id": "note", "params": { "noteId": "int" }, "identity": { "ref": "noteId" }, "title": "Note",
  "dirty": { "ref": "changed" }, "state": { "changed": { "init": false } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "which", "text": "note" },
    { "kind": "button", "testId": "edit", "label": "edit", "onClick": [ { "set": "changed", "value": true } ] } ] } })";

constexpr std::string_view kVariant = R"({
  "vocab": "ui/1", "id": "variant", "params": { "noteId": "int" }, "identity": { "ref": "noteId" },
  "title": "Variant", "view": { "kind": "text", "testId": "variant", "text": "variant" } })";

constexpr std::string_view kHome = R"({
  "vocab": "ui/1", "id": "home", "title": "Home", "view": { "kind": "text", "testId": "home", "text": "home" } })";

constexpr std::string_view kNeedsOwner = R"({
  "vocab": "ui/1", "id": "needsOwner", "params": { "noteId": "int", "owner": "string" },
  "view": { "kind": "text", "text": "x" } })";

/// What a host's close guard did: how often it was asked, and, when `defer` is set, the answer it is waiting for.
struct Guard {
    int asked = 0;
    bool defer = false;
    std::function<void(CloseChoice)> pending;
};

/// A host whose close guard keeps a dirty screen open, or discards it.
class Host final : public DocumentScreenHost {
public:
    Host(Harness& harness, CloseChoice guardChoice, Guard* probe = nullptr)
        : DocumentScreenHost{harness.session, harness.dispatcher, options(harness, guardChoice, probe)} {}

    void onNavigate(std::string_view /*screenId*/, ParamMap const& /*params*/) override {}

    using DocumentScreenHost::onNavigate;

private:
    static DocumentScreenHostOptions options(Harness& harness, CloseChoice guardChoice, Guard* probe) {
        static std::map<std::string, std::shared_ptr<Document const>, std::less<>> const documents{
            {"note", loadOrFail(kNote, morph::ui::testing::corpusCatalog())},
            {"variant", loadOrFail(kVariant, morph::ui::testing::corpusCatalog())},
            {"home", loadOrFail(kHome, morph::ui::testing::corpusCatalog())},
            {"needsOwner", loadOrFail(kNeedsOwner, morph::ui::testing::corpusCatalog())}};
        DocumentScreenHostOptions made;
        made.documents = [](std::string_view id) -> std::shared_ptr<Document const> {
            auto const found = documents.find(id);
            return found == documents.end() ? nullptr : found->second;
        };
        made.backend = &harness.backend;
        made.application = &harness.host;
        made.guard = [guardChoice, probe](ScreenHandle& /*screen*/, std::function<void(CloseChoice)> const& choose) {
            if (probe != nullptr) {
                ++probe->asked;
                if (probe->defer) {
                    probe->pending = choose;
                    return;
                }
            }
            choose(guardChoice);
        };
        return made;
    }
};

ParamMap note(std::int64_t id) { return ParamMap{{"noteId", Value{id}}}; }

/// Opens @p screenId in place of the tab at @p index and answers what `openInPlace` answered, once it has.
std::expected<WorkspaceTabId, MountError> inPlace(Harness& harness, Workspace& shell, std::size_t index,
                                                  std::string_view screenId, ParamMap const& params) {
    std::optional<std::expected<WorkspaceTabId, MountError>> answer;
    shell.model().openInPlace(
        shell.model().order().at(index), screenId, params,
        [&answer](std::expected<WorkspaceTabId, MountError> opened) { answer = std::move(opened); });
    harness.settle();
    REQUIRE(answer.has_value());
    return std::move(*answer);
}

}  // namespace

// Mutations: leave the new tab where it was appended (it is the last tab, not the second); keep the old tab beside the
// new one (four tabs remain).
TEST_CASE("workspace: openInPlace puts another screen in a tab's place, with the params given",
          "[ui][workspace][in-place]") {
    Harness harness;
    Host host{harness, CloseChoice::Discard};
    std::vector<std::vector<std::string>> records;
    WorkspaceOptions options;
    options.onRecord = [&records](std::vector<std::string> const& routes) { records.push_back(routes); };
    Workspace shell{harness.runtime, harness.backend, host, std::move(options)};
    REQUIRE(shell.open("home", {}).has_value());
    REQUIRE(shell.open("note", note(4)).has_value());
    REQUIRE(shell.open("note", note(5)).has_value());
    harness.settle();

    auto const opened = inPlace(harness, shell, 1, "variant", note(4));
    REQUIRE(opened.has_value());
    auto const tabs = shell.tabs();
    REQUIRE(tabs.size() == 3);
    CHECK(tabs[1].screenId == "variant");
    CHECK(tabs[1].identity == Value{std::int64_t{4}});
    CHECK(shell.active() == 1U);
    REQUIRE_FALSE(records.empty());
    CHECK(records.back() == std::vector<std::string>{"home", "variant?noteId=4", "note?noteId=5"});
}

// Mutation: keep the new tab when the old screen stays open (two tabs remain).
TEST_CASE("workspace: openInPlace keeps the tab when its screen stays open, and opens nothing for a refused mount",
          "[ui][workspace][in-place]") {
    Harness harness;
    Host host{harness, CloseChoice::Cancel};
    Workspace shell{harness.runtime, harness.backend, host, WorkspaceOptions{}};
    REQUIRE(shell.open("note", note(4)).has_value());
    harness.settle();
    harness.backend.click(harness.byTestId("Button", "edit"));
    harness.settle();
    ScreenHandle* const handle = shell.model().handleOf(shell.model().order().at(0));

    CHECK_FALSE(inPlace(harness, shell, 0, "variant", note(4)).has_value());
    REQUIRE(shell.tabs().size() == 1);
    CHECK(shell.tabs()[0].screenId == "note");
    CHECK(shell.active() == 0U);
    CHECK(shell.model().handleOf(shell.model().order().at(0)) == handle);
    CHECK(handle->dirty().peek());
    CHECK(harness.backend.children(harness.backend.idOf(shell.root())).size() == 1);

    CHECK_FALSE(inPlace(harness, shell, 0, "needsOwner", note(4)).has_value());
    REQUIRE(shell.tabs().size() == 1);
    CHECK(shell.tabs()[0].screenId == "note");
    CHECK(harness.backend.children(harness.backend.idOf(shell.root())).size() == 1);
}

// Mutation: ask for the draft's close with no guard (Cancel still replaces it); keep the new page on Cancel.
TEST_CASE("workspace: openInPlace on a drafted tab asks the close guard, and a Cancel keeps the draft",
          "[ui][workspace][in-place]") {
    for (CloseChoice const choice : {CloseChoice::Cancel, CloseChoice::Discard}) {
        Harness harness;
        Guard guard;
        Host host{harness, choice, &guard};
        Workspace shell{harness.runtime, harness.backend, host, WorkspaceOptions{}};
        REQUIRE(shell.open("note", note(4)).has_value());
        harness.settle();
        ScreenHandle* const handle = shell.model().handleOf(shell.model().order().at(0));
        REQUIRE(host.mountDraft(*handle, loadOrFail(kNote, morph::ui::testing::corpusCatalog())).has_value());
        harness.settle();
        REQUIRE(DocumentScreenHost::draftedNow(*handle));

        auto const answer = inPlace(harness, shell, 0, "variant", note(4));
        CHECK(guard.asked == 1);
        REQUIRE(shell.tabs().size() == 1);
        if (choice == CloseChoice::Cancel) {
            CHECK_FALSE(answer.has_value());
            CHECK(shell.tabs()[0].screenId == "note");
            CHECK(DocumentScreenHost::draftedNow(*handle));
            CHECK(harness.backend.children(harness.backend.idOf(shell.root())).size() == 1);
        } else {
            CHECK(answer.has_value());
            CHECK(shell.tabs()[0].screenId == "variant");
            CHECK(harness.backend.children(harness.backend.idOf(shell.root())).size() == 1);
        }
    }
}

// Mutations: mount a second tab for a screen another tab shows; lose the focus (the active tab stays the current one).
TEST_CASE("workspace: openInPlace focuses the tab that already shows the screen, and mounts nothing",
          "[ui][workspace][in-place]") {
    Harness harness;
    Host host{harness, CloseChoice::Discard};
    WorkspaceOptions options;
    options.screens = [](std::string_view id) {
        WorkspaceScreen facts;
        facts.singleton = id == "home";
        return facts;
    };
    Workspace shell{harness.runtime, harness.backend, host, std::move(options)};
    REQUIRE(shell.open("home", {}).has_value());
    REQUIRE(shell.open("note", note(4)).has_value());
    REQUIRE(shell.open("note", note(5)).has_value());
    harness.settle();
    REQUIRE(shell.active() == 2U);
    auto const pages = [&] { return harness.backend.children(harness.backend.idOf(shell.root())).size(); };
    REQUIRE(pages() == 3);

    auto const byIdentity = inPlace(harness, shell, 1, "note", note(5));
    REQUIRE(byIdentity.has_value());
    CHECK(*byIdentity == shell.model().order().at(2));
    CHECK(shell.active() == 2U);
    CHECK(shell.tabs().size() == 3);
    CHECK(shell.tabs()[1].identity == Value{std::int64_t{4}});
    CHECK(pages() == 3);

    auto const bySingleton = inPlace(harness, shell, 1, "home", {});
    REQUIRE(bySingleton.has_value());
    CHECK(*bySingleton == shell.model().order().at(0));
    CHECK(shell.active() == 0U);
    CHECK(shell.tabs().size() == 3);
    CHECK(shell.tabs()[1].screenId == "note");
    CHECK(pages() == 3);
}

// Mutation: activate the new tab after the old one is removed (the tab before it is shown meanwhile).
TEST_CASE("workspace: openInPlace on the active tab activates no other tab on the way", "[ui][workspace][in-place]") {
    Harness harness;
    Host host{harness, CloseChoice::Discard};
    std::vector<std::optional<WorkspaceTabId>> seen;
    Workspace* shellPtr = nullptr;
    WorkspaceOptions options;
    options.onChanged = [&] {
        if (shellPtr != nullptr) {
            seen.push_back(shellPtr->model().active().peek());
        }
    };
    Workspace shell{harness.runtime, harness.backend, host, std::move(options)};
    shellPtr = &shell;
    REQUIRE(shell.open("home", {}).has_value());
    REQUIRE(shell.open("note", note(4)).has_value());
    REQUIRE(shell.open("note", note(5)).has_value());
    harness.settle();
    shell.model().activate(shell.model().order().at(1));
    harness.settle();
    WorkspaceTabId const old = shell.model().order().at(1);
    seen.clear();

    auto const opened = inPlace(harness, shell, 1, "variant", note(4));
    REQUIRE(opened.has_value());
    REQUIRE_FALSE(seen.empty());
    for (auto const& tab : seen) {
        CHECK((tab == old || tab == *opened));
    }
    CHECK(shell.active() == 1U);
    CHECK(shell.tabs()[1].screenId == "variant");
}

// Mutations: skip the record write after the move; answer success for a closed new tab.
TEST_CASE(
    "workspace: openInPlace records the tabs in order while the guard is pending, and fails when the new tab closed",
    "[ui][workspace][in-place]") {
    Harness harness;
    Guard guard;
    guard.defer = true;
    Host host{harness, CloseChoice::Discard, &guard};
    std::vector<std::vector<std::string>> records;
    WorkspaceOptions options;
    options.onRecord = [&records](std::vector<std::string> const& routes) { records.push_back(routes); };
    Workspace shell{harness.runtime, harness.backend, host, std::move(options)};
    REQUIRE(shell.open("note", note(4)).has_value());
    harness.settle();
    harness.backend.click(harness.byTestId("Button", "edit"));
    harness.settle();
    REQUIRE(shell.open("home", {}).has_value());
    harness.settle();

    std::optional<std::expected<WorkspaceTabId, MountError>> answer;
    shell.model().openInPlace(
        shell.model().order().at(0), "variant", note(4),
        [&answer](std::expected<WorkspaceTabId, MountError> opened) { answer = std::move(opened); });
    harness.settle();
    REQUIRE(guard.pending);
    CHECK_FALSE(answer.has_value());
    REQUIRE_FALSE(records.empty());
    CHECK(records.back() == std::vector<std::string>{"note?noteId=4", "variant?noteId=4", "home"});

    shell.model().close(shell.model().order().at(1), nullptr);
    harness.settle();
    guard.pending(CloseChoice::Discard);
    harness.settle();
    REQUIRE(answer.has_value());
    CHECK_FALSE(answer->has_value());
    CHECK(shell.tabs().size() == 1);
    CHECK(shell.tabs()[0].screenId == "home");
}
