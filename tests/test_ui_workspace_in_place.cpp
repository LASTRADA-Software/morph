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

/// A host whose close guard keeps a dirty screen open, or discards it.
class Host final : public DocumentScreenHost {
public:
    Host(Harness& harness, CloseChoice guardChoice)
        : DocumentScreenHost{harness.session, harness.dispatcher, options(harness, guardChoice)} {}

    void onNavigate(std::string_view /*screenId*/, ParamMap const& /*params*/) override {}

    using DocumentScreenHost::onNavigate;

private:
    static DocumentScreenHostOptions options(Harness& harness, CloseChoice guardChoice) {
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
        made.guard = [guardChoice](ScreenHandle& /*screen*/, std::function<void(CloseChoice)> const& choose) {
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

    CHECK_FALSE(inPlace(harness, shell, 0, "variant", note(4)).has_value());
    REQUIRE(shell.tabs().size() == 1);
    CHECK(shell.tabs()[0].screenId == "note");

    CHECK_FALSE(inPlace(harness, shell, 0, "needsOwner", note(4)).has_value());
    REQUIRE(shell.tabs().size() == 1);
    CHECK(shell.tabs()[0].screenId == "note");
}

// Mutation: append the chrome after the screen without moving it (the page's first child is not the chrome).
TEST_CASE("workspace: a page's chrome sits at its top, and a null chrome mounts nothing",
          "[ui][workspace][in-place]") {
    Harness harness;
    Host host{harness, CloseChoice::Discard};
    WorkspaceOptions options;
    options.chrome = [](WorkspaceTabId /*tab*/, std::string_view screenId) -> morph::ui::Node {
        if (screenId == "home") {
            return nullptr;
        }
        morph::ui::Text text;
        text.text = morph::ui::Prop<std::string>{std::string{"chrome"}};
        text.common.testId = "chrome_" + std::string{screenId};
        return morph::ui::text(std::move(text));
    };
    Workspace shell{harness.runtime, harness.backend, host, std::move(options)};
    REQUIRE(shell.open("note", note(1)).has_value());
    REQUIRE(shell.open("home", {}).has_value());
    harness.settle();
    std::vector<int> const pages = harness.backend.children(harness.backend.idOf(shell.root()));
    REQUIRE(pages.size() == 2);
    REQUIRE_FALSE(harness.backend.children(pages[0]).empty());
    CHECK(harness.backend.children(pages[0]).front() == harness.byTestId("Text", "chrome_note"));
    CHECK_FALSE(harness.backend.find("Text", "testId", "chrome_home").has_value());
}

// Mutation: drop the draft check (the draft is closed through the guard and the tab replaced).
TEST_CASE("workspace: openInPlace refuses a tab that shows a draft, and mounts nothing", "[ui][workspace][in-place]") {
    Harness harness;
    Host host{harness, CloseChoice::Discard};
    Workspace shell{harness.runtime, harness.backend, host, WorkspaceOptions{}};
    REQUIRE(shell.open("note", note(4)).has_value());
    harness.settle();
    ScreenHandle* const handle = shell.model().handleOf(shell.model().order().at(0));
    REQUIRE(handle != nullptr);
    REQUIRE(host.mountDraft(*handle, loadOrFail(kNote, morph::ui::testing::corpusCatalog())).has_value());
    harness.settle();
    REQUIRE(DocumentScreenHost::draftedNow(*handle));

    auto const refused = inPlace(harness, shell, 0, "variant", note(4));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().reason.find("draft") != std::string::npos);
    REQUIRE(shell.tabs().size() == 1);
    CHECK(shell.tabs()[0].screenId == "note");
    CHECK(DocumentScreenHost::draftedNow(*handle));
}
