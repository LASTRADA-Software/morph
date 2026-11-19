// SPDX-License-Identifier: Apache-2.0
//
// Drafts (docs/spec/ui/host.md, "Drafts"): a document mounted in place of a screen through remount, keeping the
// screen's id, params and state by name; dirty while it is up, so a close asks the guard; discarded back to the
// screen's own document; left alone by a development reload.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/ui/bundle.hpp>
#include <morph/ui/dev_reload.hpp>
#include <morph/ui/host.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_interpreter_support.hpp"

using morph::ui::CloseChoice;
using morph::ui::Document;
using morph::ui::DocumentMap;
using morph::ui::DocumentScreenHost;
using morph::ui::DocumentScreenHostOptions;
using morph::ui::ParamMap;
using morph::ui::ScreenHandle;
using morph::ui::Value;
using morph::ui::testing::Harness;
using morph::ui::testing::loadOrFail;

namespace {

/// A note: its body is state a button changes.
constexpr std::string_view kNote = R"({
  "vocab": "ui/1", "id": "note", "params": { "noteId": "int" }, "identity": { "ref": "noteId" }, "title": "Note",
  "state": { "content": { "type": "string", "init": "first" } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "heading", "text": "Note" },
    { "kind": "text", "testId": "body", "text": { "ref": "content" } },
    { "kind": "button", "testId": "change", "label": "change",
      "onClick": [ { "set": "content", "value": "changed" } ] } ] } })";

/// A draft of the note: another heading, the same state.
constexpr std::string_view kNoteDraft = R"({
  "vocab": "ui/1", "id": "note", "params": { "noteId": "int" }, "identity": { "ref": "noteId" }, "title": "Note",
  "state": { "content": { "type": "string", "init": "draft" } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "heading", "text": "Drafted" },
    { "kind": "text", "testId": "body", "text": { "ref": "content" } } ] } })";

/// A draft that needs a param the screen was not given: it loads, and the interpreter refuses the mount.
constexpr std::string_view kNoteNeedsOwner = R"({
  "vocab": "ui/1", "id": "note", "params": { "noteId": "int", "owner": "string" },
  "view": { "kind": "text", "testId": "heading", "text": "needs an owner" } })";

/// A document of another screen.
constexpr std::string_view kOther = R"({
  "vocab": "ui/1", "id": "other", "params": { "noteId": "int" },
  "view": { "kind": "text", "testId": "heading", "text": "Other" } })";

/// The note as a development reload changes it.
constexpr std::string_view kNoteReloaded = R"({
  "vocab": "ui/1", "id": "note", "params": { "noteId": "int" }, "identity": { "ref": "noteId" }, "title": "Note",
  "state": { "content": { "type": "string", "init": "first" } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "heading", "text": "Note v2" },
    { "kind": "text", "testId": "body", "text": { "ref": "content" } } ] } })";

std::shared_ptr<Document const> load(std::string_view text) {
    return loadOrFail(text, morph::ui::testing::corpusCatalog());
}

/// A host over a document map, whose close guard records each screen it is asked about and discards it.
class Shell final : public DocumentScreenHost {
public:
    Shell(Harness& harness, DocumentMap& documents, std::vector<std::string>& asked)
        : DocumentScreenHost{harness.session, harness.dispatcher, options(harness, documents, asked)} {}

    void onNavigate(std::string_view /*screenId*/, ParamMap const& /*params*/) override {}

    using DocumentScreenHost::onNavigate;

private:
    static DocumentScreenHostOptions options(Harness& harness, DocumentMap& documents,
                                             std::vector<std::string>& asked) {
        DocumentScreenHostOptions made;
        made.documents = [&documents](std::string_view screenId) -> std::shared_ptr<Document const> {
            auto const found = documents.find(screenId);
            return found == documents.end() ? nullptr : found->second;
        };
        made.backend = &harness.backend;
        made.application = &harness.host;
        made.guard = [&asked](ScreenHandle& screen, std::function<void(CloseChoice)> const& choose) {
            asked.push_back(screen.title().peek());
            choose(CloseChoice::Discard);
        };
        return made;
    }
};

/// The note mounted with id 7, its body changed by the user.
struct Opened {
    explicit Opened(Harness& harnessIn) : harness{&harnessIn}, shell{harnessIn, documents, asked} {
        documents.emplace("note", load(kNote));
        auto mounted = shell.mount("note", ParamMap{{"noteId", Value{std::int64_t{7}}}});
        REQUIRE(mounted.has_value());
        handle = std::move(*mounted);
        harness->backend.click(harness->byTestId("Button", "change"));
        harness->settle();
        REQUIRE(harness->text("body") == "changed");
    }

    Harness* harness;
    DocumentMap documents;
    std::vector<std::string> asked;
    Shell shell;
    std::unique_ptr<ScreenHandle> handle;
};

}  // namespace

// Mutations: leave the handle undrafted after a draft mounts (`dirty` stays false); remount the screen's own document
// on discard without clearing the draft (`dirty` stays true).
TEST_CASE("A draft keeps the screen's state, params and identity, is dirty while up, and Discard restores the screen",
          "[ui][host][drafts]") {
    Harness harness;
    Opened opened{harness};
    ScreenHandle& handle = *opened.handle;
    CHECK_FALSE(handle.dirty().peek());

    REQUIRE(opened.shell.mountDraft(handle, load(kNoteDraft)).has_value());
    harness.settle();
    CHECK(harness.text("heading") == "Drafted");
    CHECK(harness.text("body") == "changed");
    CHECK(DocumentScreenHost::drafted(handle));
    CHECK(handle.dirty().peek());
    CHECK(handle.identity().peek() == Value{std::int64_t{7}});
    CHECK(DocumentScreenHost::screenIdOf(handle) == std::optional<std::string>{"note"});
    CHECK(DocumentScreenHost::paramsOf(handle) ==
          std::optional<ParamMap>{ParamMap{{"noteId", Value{std::int64_t{7}}}}});

    REQUIRE(opened.shell.discardDraft(handle).has_value());
    harness.settle();
    CHECK(harness.text("heading") == "Note");
    CHECK(harness.text("body") == "changed");
    CHECK_FALSE(DocumentScreenHost::drafted(handle));
    CHECK_FALSE(handle.dirty().peek());
    CHECK_FALSE(opened.shell.discardDraft(handle).has_value());
}

// Mutations: mark the handle drafted before the remount (a refused draft leaves it dirty); drop the id check (the
// other screen's document mounts as the draft).
TEST_CASE("A draft the interpreter refuses, or one that names another screen, leaves the screen mounted",
          "[ui][host][drafts]") {
    Harness harness;
    Opened opened{harness};
    REQUIRE_FALSE(opened.shell.mountDraft(*opened.handle, load(kNoteNeedsOwner)).has_value());
    harness.settle();
    CHECK(harness.text("heading") == "Note");
    CHECK_FALSE(opened.handle->dirty().peek());

    auto const other = opened.shell.mountDraft(*opened.handle, load(kOther));
    REQUIRE_FALSE(other.has_value());
    CHECK(other.error().reason.contains("keeps the screen's id"));
    harness.settle();
    CHECK(harness.text("heading") == "Note");
    CHECK_FALSE(DocumentScreenHost::drafted(*opened.handle));
}

// Mutation: read only the document's `dirty` in the handle's `dirty` (the close goes through without the guard).
TEST_CASE("Closing a screen with a draft up asks the close guard first", "[ui][host][drafts]") {
    Harness harness;
    Opened opened{harness};
    REQUIRE(opened.shell.mountDraft(*opened.handle, load(kNoteDraft)).has_value());
    std::optional<bool> closed;
    opened.handle->requestClose([&closed](bool outcome) { closed = outcome; });
    harness.settle();
    CHECK(opened.asked == std::vector<std::string>{"Note"});
    CHECK(closed == std::optional<bool>{true});
}

// Mutation: make endDraft remount the screen's own document (the heading goes back to "Note").
TEST_CASE("Ending a draft keeps what is mounted and is no longer dirty", "[ui][host][drafts]") {
    Harness harness;
    Opened opened{harness};
    REQUIRE(opened.shell.mountDraft(*opened.handle, load(kNoteDraft)).has_value());
    opened.shell.endDraft(*opened.handle);
    harness.settle();
    CHECK(harness.text("heading") == "Drafted");
    CHECK_FALSE(DocumentScreenHost::drafted(*opened.handle));
    CHECK_FALSE(opened.handle->dirty().peek());
}

// Mutation: drop the skip of a drafted screen in reloadDocuments (the draft is replaced by "Note v2").
TEST_CASE("A development reload leaves a draft mounted, and Discard then mounts the reloaded document",
          "[ui][host][drafts][dev-reload]") {
    Harness harness;
    Opened opened{harness};
    REQUIRE(opened.shell.mountDraft(*opened.handle, load(kNoteDraft)).has_value());
    morph::ui::Bundle bundle;
    bundle.applicationId = "dev";
    bundle.documents.emplace("note", std::string{kNoteReloaded});
    std::vector<std::string> const changed{"note"};
    std::vector<morph::ui::MountedScreen> const mounted{
        morph::ui::MountedScreen{.screenId = "note", .handle = opened.handle.get()}};
    auto const outcome = morph::ui::reloadDocuments(
        changed, bundle, morph::ui::LoadOptions{.catalog = morph::ui::testing::corpusCatalog()}, opened.documents,
        opened.shell, mounted);
    harness.settle();
    CHECK(outcome.remounted.empty());
    CHECK(outcome.refused.empty());
    CHECK(harness.text("heading") == "Drafted");

    REQUIRE(opened.shell.discardDraft(*opened.handle).has_value());
    harness.settle();
    CHECK(harness.text("heading") == "Note v2");
}
