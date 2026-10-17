// SPDX-License-Identifier: Apache-2.0

// The host contract (spec 6 §4) and the close protocol (§3), through DocumentScreenHost: a fake shell mounts, reads
// title, dirty and identity, closes with and without a veto, and receives navigate (spec 6 §12, "Lifecycle" and
// "Host contract").

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <morph/ui/host.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_interpreter_support.hpp"

using morph::ui::CloseChoice;
using morph::ui::CloseGuard;
using morph::ui::Document;
using morph::ui::DocumentScreenHost;
using morph::ui::DocumentScreenHostOptions;
using morph::ui::ParamMap;
using morph::ui::ScreenHandle;
using morph::ui::Value;
using morph::ui::testing::Harness;
using morph::ui::testing::loadOrFail;

namespace {

/// A note: its title and dirty follow its state, its identity is its param.
constexpr std::string_view kNote = R"({
  "vocab": "ui/1", "id": "note", "params": { "noteId": "int" },
  "identity": { "ref": "noteId" }, "title": { "ref": "name" }, "dirty": { "ref": "changed" },
  "state": { "name": { "init": "Draft" }, "changed": { "init": false } },
  "onUnmount": [ { "notify": "unmounted" } ],
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "rename", "label": "rename", "onClick": [ { "set": "name", "value": "Renamed" } ] },
    { "kind": "button", "testId": "edit", "label": "edit", "onClick": [ { "set": "changed", "value": true } ] },
    { "kind": "button", "testId": "go", "label": "go",
      "onClick": { "navigate": "note", "params": { "obj": { "noteId": 8 } } } } ] } })";

/// A screen that saves on close, and keeps itself open when `keep` is set, after the save has answered.
constexpr std::string_view kSaving = R"({
  "vocab": "ui/1", "id": "saving", "identity": { "ref": "name" },
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "keep": { "init": false }, "name": { "init": "s" } },
  "mutations": { "save": { "model": "sample", "action": "Lims_Attach", "body": { "obj": {} } } },
  "onClose": [ { "notify": "closing" },
               { "seq": [ { "run": "save" }, { "if": [ { "ref": "keep" }, [ { "cancelClose": true } ] ] } ] } ],
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "keep", "label": "keep", "onClick": [ { "set": "keep", "value": true } ] },
    { "kind": "button", "testId": "upload", "label": "upload",
      "onClick": [ { "upload": "/tmp/a.csv", "to": "save", "as": "ref" } ] } ] } })";

/// A draft that saves through `onSave`: the save's success clears `dirty`, its failure leaves it.
constexpr std::string_view kDraft = R"({
  "vocab": "ui/1", "id": "draft", "dirty": { "ref": "changed" },
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "changed": { "init": false } },
  "mutations": { "save": { "model": "sample", "action": "Lims_Attach", "body": { "obj": {} },
                           "onSuccess": [ { "set": "changed", "value": false } ] } },
  "onSave": [ { "notify": "saving" }, { "run": "save" } ],
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "edit", "label": "edit", "onClick": [ { "set": "changed", "value": true } ] } ] } })";

/// A screen whose onClose uploads a file to its save, or downloads one: the close waits for the transfer.
constexpr std::string_view kTransferring = R"({
  "vocab": "ui/1", "id": "transferring", "params": { "download": "bool" },
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "mutations": { "save": { "model": "sample", "action": "Lims_Attach", "body": { "obj": {} } } },
  "onClose": [ { "if": [ { "ref": "download" },
                         [ { "download": "ref-1", "save": "dialog", "name": "a.csv" } ],
                         [ { "upload": "/tmp/a.csv", "to": "save", "as": "ref" } ] ] } ],
  "view": { "kind": "text", "text": "x" } })";

/// A draft whose onSave uploads the file its save attaches; the save's success clears `dirty`.
constexpr std::string_view kUploadDraft = R"({
  "vocab": "ui/1", "id": "uploadDraft", "dirty": { "ref": "changed" },
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "changed": { "init": false } },
  "mutations": { "save": { "model": "sample", "action": "Lims_Attach", "body": { "obj": {} },
                           "onSuccess": [ { "set": "changed", "value": false } ] } },
  "onSave": [ { "upload": "/tmp/a.csv", "to": "save", "as": "ref" } ],
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "edit", "label": "edit", "onClick": [ { "set": "changed", "value": true } ] } ] } })";

/// A screen that opens a chooser as a dialog and takes its answer.
constexpr std::string_view kOpener = R"({
  "vocab": "ui/1", "id": "opener", "title": { "ref": "picked" },
  "state": { "picked": { "init": "none" } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "pick", "label": "pick",
      "onClick": { "navigate": "chooser", "params": { "obj": { "from": "opener" } }, "as": "dialog",
                   "onResult": [ { "set": "picked", "value": { "ref": "event" } } ],
                   "onCancel": [ { "set": "picked", "value": "cancelled" } ] } } ] } })";

/// The chooser: it answers with a value, or closes itself without one.
constexpr std::string_view kChooser = R"({
  "vocab": "ui/1", "id": "chooser", "params": { "from": "string" },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "choose", "label": "choose", "onClick": [ { "return": "apple" } ] },
    { "kind": "button", "testId": "dismiss", "label": "dismiss", "onClick": [ { "close": "self" } ] } ] } })";

/// A shell over the harness: it records what it is asked to navigate to, and decides the close guard by a script.
class Shell final : public DocumentScreenHost {
public:
    Shell(Harness& harness, std::optional<bool> guardAnswer)
        : DocumentScreenHost{harness.session, harness.dispatcher, options(harness, scripted(guardAnswer, this), {})} {}

    Shell(Harness& harness, CloseGuard guard, std::function<void(ScreenHandle&)> saveFailed)
        : DocumentScreenHost{harness.session, harness.dispatcher,
                             options(harness, std::move(guard), std::move(saveFailed))} {}

    void onNavigate(std::string_view screenId, ParamMap const& params) override {
        navigated.emplace_back(std::string{screenId}, params);
    }

    void onNavigate(morph::ui::NavigateRequest const& request) override {
        as.push_back(request.as);
        DocumentScreenHost::onNavigate(request);
    }

    void onScreenClosed(ScreenHandle& screen) override {
        static_cast<void>(screen);
        ++closedThemselves;
    }

    std::vector<std::pair<std::string, ParamMap>> navigated;
    std::vector<std::string> as;
    int closedThemselves = 0;
    int guarded = 0;

private:
    static CloseGuard scripted(std::optional<bool> guardAnswer, Shell* self) {
        if (!guardAnswer) {
            return {};
        }
        return [self, answer = *guardAnswer](ScreenHandle&, std::function<void(CloseChoice)> const& choose) {
            ++self->guarded;
            choose(answer ? CloseChoice::Discard : CloseChoice::Cancel);
        };
    }

    static DocumentScreenHostOptions options(Harness& harness, CloseGuard guard,
                                             std::function<void(ScreenHandle&)> saveFailed) {
        static std::map<std::string, std::shared_ptr<Document const>, std::less<>> const documents{
            {"note", loadOrFail(kNote, morph::ui::testing::corpusCatalog())},
            {"saving", loadOrFail(kSaving, morph::ui::testing::corpusCatalog())},
            {"draft", loadOrFail(kDraft, morph::ui::testing::corpusCatalog())},
            {"transferring", loadOrFail(kTransferring, morph::ui::testing::corpusCatalog())},
            {"uploadDraft", loadOrFail(kUploadDraft, morph::ui::testing::corpusCatalog())},
            {"opener", loadOrFail(kOpener, morph::ui::testing::corpusCatalog())},
            {"chooser", loadOrFail(kChooser, morph::ui::testing::corpusCatalog())}};
        DocumentScreenHostOptions made;
        made.documents = [](std::string_view id) -> std::shared_ptr<Document const> {
            auto const found = documents.find(id);
            return found == documents.end() ? nullptr : found->second;
        };
        made.backend = &harness.backend;
        made.application = &harness.host;
        made.guard = std::move(guard);
        made.saveFailed = std::move(saveFailed);
        return made;
    }
};

/// The outcome of one `requestClose`, once it has been answered.
struct Outcome {
    std::optional<bool> closed;
    int calls = 0;

    [[nodiscard]] std::function<void(bool)> callback() {
        return [this](bool value) {
            closed = value;
            ++calls;
        };
    }
};

int count(std::vector<std::string> const& log, std::string_view entry) {
    return static_cast<int>(std::ranges::count(log, std::string{entry}));
}

}  // namespace

// Mutation: build title from the param instead of the expression (the rename does not reach it).
TEST_CASE("DocumentScreenHost mounts a screen whose title, dirty and identity follow the document", "[ui][host]") {
    Harness harness;
    Shell shell{harness, std::nullopt};
    auto mounted = shell.mount("note", ParamMap{{"noteId", Value{std::int64_t{7}}}});
    REQUIRE(mounted.has_value());
    ScreenHandle& note = **mounted;
    harness.settle();
    CHECK(note.title().get() == "Draft");
    CHECK_FALSE(note.dirty().get());
    CHECK(note.identity().get() == Value{std::int64_t{7}});
    CHECK(DocumentScreenHost::rootOf(note) != nullptr);

    harness.backend.click(harness.byTestId("Button", "rename"));
    harness.backend.click(harness.byTestId("Button", "edit"));
    harness.settle();
    CHECK(note.title().get() == "Renamed");
    CHECK(note.dirty().get());

    CHECK(shell.identityOf("note", ParamMap{{"noteId", Value{std::int64_t{7}}}}) == "7");
    CHECK_FALSE(shell.identityOf("saving", ParamMap{}).has_value());  // reads state, not params
    CHECK_FALSE(shell.identityOf("nothing", ParamMap{}).has_value());

    auto const unknown = shell.mount("nothing", ParamMap{});
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().reason == "unknown screen");
    auto const refused = shell.mount("note", ParamMap{});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().screenId == "note");
}

// Mutation: call done inline (closed is set before the owner runs).
TEST_CASE("A clean screen closes at once, and done is posted, not called inside the request", "[ui][host]") {
    Harness harness;
    Shell shell{harness, std::nullopt};
    auto note = std::move(*shell.mount("note", ParamMap{{"noteId", Value{std::int64_t{1}}}}));
    harness.settle();
    Outcome outcome;
    note->requestClose(outcome.callback());
    CHECK_FALSE(outcome.closed.has_value());
    harness.settle();
    CHECK(outcome.closed == true);
    CHECK(outcome.calls == 1);
    CHECK(DocumentScreenHost::rootOf(*note) == nullptr);
    CHECK(count(harness.host.log, "notify  unmounted") == 1);
    CHECK(note->title().get() == "Draft");  // the last value outlives the screen
    CHECK_FALSE(note->dirty().get());
}

// Mutation: close a dirty screen without asking the guard (guarded stays 0, and it closes).
TEST_CASE("A dirty screen without onClose is put to the host's guard", "[ui][host]") {
    for (bool const answer : {false, true}) {
        Harness harness;
        Shell shell{harness, answer};
        auto note = std::move(*shell.mount("note", ParamMap{{"noteId", Value{std::int64_t{1}}}}));
        harness.backend.click(harness.byTestId("Button", "edit"));
        harness.settle();
        Outcome outcome;
        note->requestClose(outcome.callback());
        harness.settle();
        CHECK(shell.guarded == 1);
        CHECK(outcome.closed == answer);
        CHECK((DocumentScreenHost::rootOf(*note) == nullptr) == answer);
    }

    Harness harness;
    Shell shell{harness, std::nullopt};  // no guard: a dirty screen stays open
    auto note = std::move(*shell.mount("note", ParamMap{{"noteId", Value{std::int64_t{1}}}}));
    harness.backend.click(harness.byTestId("Button", "edit"));
    harness.settle();
    Outcome outcome;
    note->requestClose(outcome.callback());
    harness.settle();
    CHECK(outcome.closed == false);
    CHECK(DocumentScreenHost::rootOf(*note) != nullptr);
}

// Mutation: decide as soon as onClose's batch ends (the screen closes before the save has answered).
TEST_CASE("onClose's work is waited for, a second request joins the first, and cancelClose keeps it open",
          "[ui][host]") {
    for (bool const keep : {false, true}) {
        Harness harness;
        Shell shell{harness, std::nullopt};
        auto saving = std::move(*shell.mount("saving", ParamMap{}));
        if (keep) {
            harness.backend.click(harness.byTestId("Button", "keep"));
        }
        harness.settle();

        Outcome first;
        Outcome second;
        saving->requestClose(first.callback());
        saving->requestClose(second.callback());
        harness.settle();
        CHECK(count(harness.host.log, "notify  closing") == 1);  // one onClose for both requests
        REQUIRE(harness.dispatcher.calls.size() == 1);
        CHECK_FALSE(first.closed.has_value());  // the save is pending
        CHECK(DocumentScreenHost::rootOf(*saving) != nullptr);

        harness.reply(0, "{}");
        CHECK(first.closed == !keep);
        CHECK(second.closed == !keep);
        CHECK(first.calls == 1);
        CHECK(second.calls == 1);
        CHECK((DocumentScreenHost::rootOf(*saving) == nullptr) == !keep);
    }
}

// Mutation: take no count in runTransfer (the screen closes while the upload is in flight, and its save never runs).
TEST_CASE("onClose waits for an upload in flight and the save it hands over to", "[ui][host]") {
    Harness harness;
    Shell shell{harness, std::nullopt};
    auto screen = std::move(*shell.mount("transferring", ParamMap{{"download", Value{false}}}));
    harness.settle();

    Outcome outcome;
    screen->requestClose(outcome.callback());
    harness.settle();
    REQUIRE(harness.host.uploadDone);
    CHECK_FALSE(outcome.closed.has_value());  // the upload is in flight
    CHECK(DocumentScreenHost::rootOf(*screen) != nullptr);

    harness.host.uploadDone(Value{std::string{"ref-1"}});
    harness.settle();
    REQUIRE(harness.dispatcher.calls.size() == 1);
    CHECK(harness.dispatcher.calls.at(0).body == R"({"ref":"ref-1"})");
    CHECK_FALSE(outcome.closed.has_value());  // its save is in flight

    harness.reply(0, "{}");
    CHECK(outcome.closed == true);
    CHECK(DocumentScreenHost::rootOf(*screen) == nullptr);
}

// Mutation: take no count for a download (the screen closes while the download is in flight).
TEST_CASE("onClose waits for a download in flight", "[ui][host]") {
    Harness harness;
    Shell shell{harness, std::nullopt};
    auto screen = std::move(*shell.mount("transferring", ParamMap{{"download", Value{true}}}));
    harness.settle();

    Outcome outcome;
    screen->requestClose(outcome.callback());
    harness.settle();
    REQUIRE(harness.host.downloadDone);
    CHECK_FALSE(outcome.closed.has_value());

    harness.host.downloadDone(Value{std::string{"/home/a.csv"}});
    harness.settle();
    CHECK(outcome.closed == true);
}

/// The host's side of the three-choice guard: it records each ask and whether save was offered, and answers with a
/// scripted choice.
struct Prompt {
    CloseChoice choice = CloseChoice::Cancel;
    std::vector<bool> offeredSave;
    int failures = 0;

    [[nodiscard]] CloseGuard guard() {
        return [this](ScreenHandle& screen, std::function<void(CloseChoice)> const& choose) {
            offeredSave.push_back(screen.canSave());
            choose(choice);
        };
    }

    [[nodiscard]] std::function<void(ScreenHandle&)> failed() {
        return [this](ScreenHandle&) { ++failures; };
    }
};

// Mutation: canSave answers true for every document screen (note offers save).
TEST_CASE("Save is offered only to a screen that declares onSave", "[ui][host][guard]") {
    Harness harness;
    Prompt prompt;
    Shell shell{harness, prompt.guard(), prompt.failed()};
    auto draft = std::move(*shell.mount("draft", ParamMap{}));
    harness.backend.click(harness.byTestId("Button", "edit"));
    harness.settle();
    auto note = std::move(*shell.mount("note", ParamMap{{"noteId", Value{std::int64_t{1}}}}));
    harness.settle();
    CHECK(draft->canSave());
    CHECK_FALSE(note->canSave());
    REQUIRE(draft->dirty().get());
    Outcome outcome;
    draft->requestClose(outcome.callback());
    harness.settle();
    REQUIRE(prompt.offeredSave.size() == 1);
    CHECK(prompt.offeredSave.front());
}

// Mutation: answer the save as soon as onSave's batch ends (the screen closes before the save has answered).
TEST_CASE("Save and close runs onSave, waits for it, and closes the screen it left clean", "[ui][host][guard]") {
    Harness harness;
    Prompt prompt{.choice = CloseChoice::Save};
    Shell shell{harness, prompt.guard(), prompt.failed()};
    auto draft = std::move(*shell.mount("draft", ParamMap{}));
    harness.backend.click(harness.byTestId("Button", "edit"));
    harness.settle();

    Outcome outcome;
    draft->requestClose(outcome.callback());
    harness.settle();
    CHECK(count(harness.host.log, "notify  saving") == 1);
    REQUIRE(harness.dispatcher.calls.size() == 1);
    CHECK_FALSE(outcome.closed.has_value());  // the save is pending
    CHECK(DocumentScreenHost::rootOf(*draft) != nullptr);

    harness.reply(0, "{}");
    CHECK(outcome.closed == true);
    CHECK(outcome.calls == 1);
    CHECK(prompt.failures == 0);
    CHECK(DocumentScreenHost::rootOf(*draft) == nullptr);
}

// Mutation: close after a save without reading dirty (the failed save closes the screen).
TEST_CASE("A save that leaves the screen dirty keeps it open and is reported", "[ui][host][guard]") {
    Harness harness;
    Prompt prompt{.choice = CloseChoice::Save};
    Shell shell{harness, prompt.guard(), prompt.failed()};
    auto draft = std::move(*shell.mount("draft", ParamMap{}));
    harness.backend.click(harness.byTestId("Button", "edit"));
    harness.settle();

    Outcome outcome;
    draft->requestClose(outcome.callback());
    harness.settle();
    REQUIRE(harness.dispatcher.calls.size() == 1);
    harness.fail(0);
    CHECK(outcome.closed == false);
    CHECK(prompt.failures == 1);
    CHECK(draft->dirty().get());
    CHECK(DocumentScreenHost::rootOf(*draft) != nullptr);
}

// Mutation: take no count for an upload (the save is judged while the upload is in flight, and reported as failed).
TEST_CASE("Save and close waits for onSave's upload and its save before judging dirty", "[ui][host][guard]") {
    Harness harness;
    Prompt prompt{.choice = CloseChoice::Save};
    Shell shell{harness, prompt.guard(), prompt.failed()};
    auto draft = std::move(*shell.mount("uploadDraft", ParamMap{}));
    harness.backend.click(harness.byTestId("Button", "edit"));
    harness.settle();

    Outcome outcome;
    draft->requestClose(outcome.callback());
    harness.settle();
    REQUIRE(harness.host.uploadDone);
    CHECK_FALSE(outcome.closed.has_value());
    CHECK(prompt.failures == 0);

    harness.host.uploadDone(Value{std::string{"ref-1"}});
    harness.settle();
    REQUIRE(harness.dispatcher.calls.size() == 1);
    CHECK_FALSE(outcome.closed.has_value());
    CHECK(prompt.failures == 0);

    harness.reply(0, "{}");
    CHECK(outcome.closed == true);
    CHECK(prompt.failures == 0);
    CHECK(DocumentScreenHost::rootOf(*draft) == nullptr);
}

// Mutation: run onSave for every choice (discard logs saving and calls the model).
TEST_CASE("Discard closes without saving, cancel keeps the screen, and save without onSave keeps it",
          "[ui][host][guard]") {
    for (CloseChoice const choice : {CloseChoice::Discard, CloseChoice::Cancel}) {
        Harness harness;
        Prompt prompt{.choice = choice};
        Shell shell{harness, prompt.guard(), prompt.failed()};
        auto draft = std::move(*shell.mount("draft", ParamMap{}));
        harness.backend.click(harness.byTestId("Button", "edit"));
        harness.settle();
        Outcome outcome;
        draft->requestClose(outcome.callback());
        harness.settle();
        bool const discard = choice == CloseChoice::Discard;
        CHECK(outcome.closed == discard);
        CHECK(count(harness.host.log, "notify  saving") == 0);
        CHECK(harness.dispatcher.calls.empty());
        CHECK((DocumentScreenHost::rootOf(*draft) == nullptr) == discard);
    }

    Harness harness;
    Prompt prompt{.choice = CloseChoice::Save};
    Shell shell{harness, prompt.guard(), prompt.failed()};
    auto note = std::move(*shell.mount("note", ParamMap{{"noteId", Value{std::int64_t{1}}}}));
    harness.backend.click(harness.byTestId("Button", "edit"));
    harness.settle();
    Outcome outcome;
    note->requestClose(outcome.callback());
    harness.settle();
    REQUIRE(prompt.offeredSave == std::vector<bool>{false});
    CHECK(outcome.closed == false);
    CHECK(prompt.failures == 0);
    CHECK(DocumentScreenHost::rootOf(*note) != nullptr);
}

// Mutation: carry the choice out through the ScreenHandle the guard was given (a use after free under ASan, and the
// closed request is answered twice).
TEST_CASE("A guard that answers after its screen's handle is gone does nothing", "[ui][host][guard]") {
    Harness harness;
    std::function<void(CloseChoice)> later;
    Shell shell{harness, [&later](ScreenHandle&, std::function<void(CloseChoice)> const& choose) { later = choose; },
                [](ScreenHandle&) { FAIL("a save that never ran cannot fail"); }};
    auto draft = std::move(*shell.mount("draft", ParamMap{}));
    harness.backend.click(harness.byTestId("Button", "edit"));
    harness.settle();
    Outcome outcome;
    draft->requestClose(outcome.callback());
    harness.settle();
    REQUIRE(later);
    draft.reset();  // the shell quits: the handle closes its screen and answers the request
    harness.settle();
    CHECK(outcome.closed == true);
    later(CloseChoice::Save);
    harness.settle();
    CHECK(outcome.calls == 1);
    CHECK(harness.dispatcher.calls.empty());
    CHECK(count(harness.host.log, "notify  saving") == 0);
}

// Mutation: run onSave again for a save that joins one in progress (saving is logged twice).
TEST_CASE("A second save joins the first, and a save its screen outlives is never answered", "[ui][host][guard]") {
    Harness harness;
    Shell shell{harness, std::nullopt};
    auto draft = std::move(*shell.mount("draft", ParamMap{}));
    harness.backend.click(harness.byTestId("Button", "edit"));
    harness.settle();

    Outcome first;
    Outcome second;
    draft->save(first.callback());
    draft->save(second.callback());
    harness.settle();
    CHECK(count(harness.host.log, "notify  saving") == 1);
    REQUIRE(harness.dispatcher.calls.size() == 1);
    harness.reply(0, "{}");
    CHECK(first.closed == true);  // `closed` holds the save's answer: clean
    CHECK(second.closed == true);
    CHECK(first.calls == 1);
    CHECK(DocumentScreenHost::rootOf(*draft) != nullptr);  // a save alone never closes

    harness.backend.click(harness.byTestId("Button", "edit"));
    harness.settle();
    Outcome orphan;
    draft->save(orphan.callback());
    harness.settle();
    draft->forceClose();
    harness.settle();
    CHECK(orphan.calls == 0);

    Outcome refused;
    draft->save(refused.callback());  // closed: it cannot save
    CHECK(refused.closed == false);
}

// Mutation: run onClose from forceClose (closing is logged).
TEST_CASE("forceClose skips onClose and answers a pending request", "[ui][host]") {
    Harness harness;
    Shell shell{harness, std::nullopt};
    auto saving = std::move(*shell.mount("saving", ParamMap{}));
    harness.settle();
    saving->forceClose();
    harness.settle();
    CHECK(count(harness.host.log, "notify  closing") == 0);
    CHECK(DocumentScreenHost::rootOf(*saving) == nullptr);

    auto again = std::move(*shell.mount("saving", ParamMap{}));
    harness.settle();
    Outcome outcome;
    again->requestClose(outcome.callback());
    harness.settle();
    CHECK_FALSE(outcome.closed.has_value());
    again->forceClose();
    harness.settle();
    CHECK(outcome.closed == true);
    CHECK(outcome.calls == 1);

    Outcome late;  // a request on a closed screen is answered `closed`
    again->requestClose(late.callback());
    harness.settle();
    CHECK(late.closed == true);
}

// Mutation: drop the call to onNavigate (navigated stays empty).
TEST_CASE("A document's navigate reaches the host's onNavigate with its params", "[ui][host]") {
    Harness harness;
    Shell shell{harness, std::nullopt};
    auto const note = std::move(*shell.mount("note", ParamMap{{"noteId", Value{std::int64_t{1}}}}));
    harness.backend.click(harness.byTestId("Button", "go"));
    harness.settle();
    REQUIRE(shell.navigated.size() == 1);
    CHECK(shell.navigated[0].first == "note");
    CHECK(shell.navigated[0].second == ParamMap{{"noteId", Value{std::int64_t{8}}}});
}

// Mutation: leave the call unanswered when the callee closes another way (picked stays "none" after the platform
// close).
TEST_CASE("A screen opened as a dialog answers its opener once, by return, close: self or the platform",
          "[ui][host]") {
    enum class Ending : std::uint8_t { Return, CloseSelf, Platform };
    for (Ending const ending : {Ending::Return, Ending::CloseSelf, Ending::Platform}) {
        Harness harness;
        Shell shell{harness, std::nullopt};
        auto opener = std::move(*shell.mount("opener", ParamMap{}));
        harness.backend.click(harness.byTestId("Button", "pick"));
        harness.settle();
        REQUIRE(shell.navigated.size() == 1);
        CHECK(shell.as == std::vector<std::string>{"dialog"});

        // The host shows the chooser however it likes; mounting it with the navigated params ties it to the call.
        auto chooser = std::move(*shell.mount(shell.navigated[0].first, shell.navigated[0].second));
        harness.settle();
        switch (ending) {
            case Ending::Return:
                harness.backend.click(harness.byTestId("Button", "choose"));
                break;
            case Ending::CloseSelf:
                harness.backend.click(harness.byTestId("Button", "dismiss"));
                break;
            case Ending::Platform:
            default:
                chooser->forceClose();
                break;
        }
        harness.settle();
        CHECK(opener->title().get() == (ending == Ending::Return ? "apple" : "cancelled"));
        CHECK(DocumentScreenHost::rootOf(*chooser) == nullptr);
        CHECK(shell.closedThemselves == (ending == Ending::Platform ? 0 : 1));

        chooser.reset();  // answered already: destroying the handle answers nothing twice
        harness.settle();
        CHECK(opener->title().get() == (ending == Ending::Return ? "apple" : "cancelled"));
    }
}

// Mutation: drop DocumentScreenHost's uploadFor (the application is asked to upload without the mutation's model and
// action, which the server authorises the upload as).
TEST_CASE("A document's upload reaches the application with the mutation it is for", "[ui][host]") {
    Harness harness;
    Shell shell{harness, std::nullopt};
    auto const saving = std::move(*shell.mount("saving", ParamMap{}));
    harness.backend.click(harness.byTestId("Button", "upload"));
    harness.settle();
    CHECK(count(harness.host.log, "uploadFor Lims_SampleModel Lims_Attach") == 1);
}

// Mutation: mount every screen as a backend root (the root is not the page's child).
TEST_CASE("mountInto puts the screen's root in the shell's container, and rootOf finds it", "[ui][host]") {
    Harness harness;
    Shell shell{harness, std::nullopt};
    std::unique_ptr<morph::ui::SlotWidget> const page = harness.backend.createSlot(nullptr);
    auto mounted = shell.mountInto("note", ParamMap{{"noteId", Value{std::int64_t{3}}}}, *page);
    REQUIRE(mounted.has_value());
    harness.settle();
    morph::ui::Widget const* const root = DocumentScreenHost::rootOf(**mounted);
    REQUIRE(root != nullptr);
    CHECK(harness.backend.children(harness.backend.idOf(*page)) == std::vector<int>{harness.backend.idOf(*root)});
    CHECK((*mounted)->title().get() == "Draft");

    // A refused mount leaves the container as it was.
    auto refused = shell.mountInto("nothing", ParamMap{}, *page);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().reason == "unknown screen");
    CHECK(harness.backend.children(harness.backend.idOf(*page)).size() == 1);
}

namespace {

/// A screen with one button per command DocumentScreenHost hands to the application, and a customization point.
constexpr std::string_view kRelay = R"({
  "vocab": "ui/1", "id": "relay",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "mutations": { "save": { "model": "sample", "action": "Lims_Attach", "body": { "obj": {} } } },
  "customize": { "density": { "kind": "choice", "options": [ "comfortable", "compact" ], "default": "comfortable" } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "host", "label": "host", "onClick": [ { "host": "beep" } ] },
    { "kind": "button", "testId": "switch", "label": "switch", "onClick": [ { "switchBackend": "staging" } ] },
    { "kind": "button", "testId": "reset", "label": "reset", "onClick": [ { "resetPrefs": "all" } ] },
    { "kind": "button", "testId": "locale", "label": "locale", "onClick": [ { "setLocale": "de" } ] },
    { "kind": "button", "testId": "signout", "label": "signout", "onClick": [ { "signOut": true } ] },
    { "kind": "button", "testId": "app", "label": "app", "onClick": [ { "set": "app.banner", "value": "hey" } ] },
    { "kind": "button", "testId": "route", "label": "route", "onClick": [ { "navigate": { "route": "/note/1" } } ] },
    { "kind": "button", "testId": "self", "label": "self", "onClick": [ { "close": "self" } ] },
    { "kind": "button", "testId": "save", "label": "save", "onClick": [ { "run": "save" } ] },
    { "kind": "button", "testId": "download", "label": "download",
      "onClick": [ { "download": "ref-1", "save": "dialog", "name": "a.csv" } ] },
    { "kind": "customize", "testId": "density", "point": "density" } ] } })";

/// Identities that read something other than the params, or read the params in ways `identityOf` must follow.
constexpr std::string_view kIdNested = R"({ "vocab": "ui/1", "id": "idNested", "params": { "filter": "record" },
  "identity": { "ref": "filter.kind" }, "view": { "kind": "text", "text": "x" } })";
constexpr std::string_view kIdFormat = R"({ "vocab": "ui/1", "id": "idFormat", "params": { "n": "int" },
  "identity": { "fn": "format", "args": [ "note {0}", { "ref": "n" } ] }, "view": { "kind": "text", "text": "x" } })";
constexpr std::string_view kIdPref = R"({ "vocab": "ui/1", "id": "idPref",
  "customize": { "density": { "kind": "choice", "options": [ "a", "b" ], "default": "a" } },
  "identity": { "pref": "density" }, "view": { "kind": "text", "text": "x" } })";
constexpr std::string_view kIdCan = R"({ "vocab": "ui/1", "id": "idCan",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "mutations": { "save": { "model": "sample", "action": "Lims_Attach", "body": { "obj": {} } } },
  "identity": { "can": "save" }, "view": { "kind": "text", "text": "x" } })";
constexpr std::string_view kIdEnv = R"({ "vocab": "ui/1", "id": "idEnv",
  "identity": { "env": "locale" }, "view": { "kind": "text", "text": "x" } })";
constexpr std::string_view kIdBadLogic = R"({ "vocab": "ui/1", "id": "idBadLogic", "params": { "v": "any" },
  "identity": { "fn": "and", "args": [ { "ref": "v" }, true ] }, "view": { "kind": "text", "text": "x" } })";

/// A host over these documents with an optional application, collecting diagnostics, and keeping the default
/// `onScreenClosed`.
class RelayHost final : public DocumentScreenHost {
public:
    RelayHost(Harness& harness, bool withApplication)
        : DocumentScreenHost{harness.session, harness.dispatcher, options(harness, withApplication, this)} {}

    using DocumentScreenHost::onNavigate;
    void onNavigate(std::string_view screenId, ParamMap const& params) override {
        navigated.emplace_back(std::string{screenId}, params);
    }

    std::vector<std::pair<std::string, ParamMap>> navigated;
    std::vector<std::string> diagnostics;

private:
    static DocumentScreenHostOptions options(Harness& harness, bool withApplication, RelayHost* self) {
        static std::map<std::string, std::shared_ptr<Document const>, std::less<>> const documents{
            {"relay", loadOrFail(kRelay, morph::ui::testing::corpusCatalog())},
            {"note", loadOrFail(kNote, morph::ui::testing::corpusCatalog())},
            {"idNested", loadOrFail(kIdNested, morph::ui::testing::corpusCatalog())},
            {"idFormat", loadOrFail(kIdFormat, morph::ui::testing::corpusCatalog())},
            {"idPref", loadOrFail(kIdPref, morph::ui::testing::corpusCatalog())},
            {"idCan", loadOrFail(kIdCan, morph::ui::testing::corpusCatalog())},
            {"idEnv", loadOrFail(kIdEnv, morph::ui::testing::corpusCatalog())},
            {"idBadLogic", loadOrFail(kIdBadLogic, morph::ui::testing::corpusCatalog())},
            {"opener", loadOrFail(kOpener, morph::ui::testing::corpusCatalog())},
            {"chooser", loadOrFail(kChooser, morph::ui::testing::corpusCatalog())}};
        DocumentScreenHostOptions made;
        made.documents = [](std::string_view id) -> std::shared_ptr<Document const> {
            auto const found = documents.find(id);
            return found == documents.end() ? nullptr : found->second;
        };
        made.backend = &harness.backend;
        made.application = withApplication ? &harness.host : nullptr;
        made.diagnostics = [self](morph::ui::Refusal const& refusal) { self->diagnostics.push_back(refusal.reason); };
        return made;
    }
};

/// A host screen that implements nothing but the required parts of the handle.
class BareHandle final : public ScreenHandle {
public:
    [[nodiscard]] morph::reactive::Computed<std::string> const& title() const override { return _title; }
    [[nodiscard]] morph::reactive::Computed<bool> const& dirty() const override { return _dirty; }
    [[nodiscard]] morph::reactive::Computed<Value> const& identity() const override { return _identity; }
    void requestClose(std::function<void(bool)> done) override { done(true); }
    void forceClose() override {}

    explicit BareHandle(morph::reactive::Runtime& runtime)
        : _title{runtime, [] { return std::string{"bare"}; }},
          _dirty{runtime, [] { return false; }},
          _identity{runtime, [] { return Value{}; }} {}

private:
    morph::reactive::Computed<std::string> _title;
    morph::reactive::Computed<bool> _dirty;
    morph::reactive::Computed<Value> _identity;
};

void clickEach(Harness& harness, std::initializer_list<std::string_view> testIds) {
    for (std::string_view const testId : testIds) {
        harness.backend.click(harness.byTestId("Button", testId));
        harness.settle();
    }
}

}  // namespace

// Mutation: drop any one forward to the application (its log line is missing).
TEST_CASE("DocumentScreenHost hands every command but navigate-to-a-screen to the application", "[ui][host]") {
    Harness harness;
    harness.host.commands = {"beep"};
    harness.host.backends = {"staging"};
    RelayHost host{harness, true};
    auto const relay = std::move(*host.mount("relay", ParamMap{}));
    harness.settle();
    clickEach(harness, {"host", "switch", "reset", "locale", "signout", "app", "route", "self", "download"});
    harness.backend.choose(harness.byTestId("Select", "density"), morph::ui::Key{std::string{"compact"}});
    harness.settle();
    clickEach(harness, {"save"});
    harness.fail(harness.dispatcher.last("Lims_Attach"), "denied");

    CHECK(count(harness.host.log, "host beep null") == 1);
    CHECK(count(harness.host.log, "switchBackend staging") == 1);
    CHECK(count(harness.host.log, "resetPrefs relay all") == 1);
    CHECK(count(harness.host.log, "setLocale de") == 1);
    CHECK(count(harness.host.log, "signOut") == 1);
    CHECK(count(harness.host.log, R"(app banner "hey")") == 1);
    CHECK(count(harness.host.log, "close relay") == 1);
    CHECK(count(harness.host.log, R"(pref relay density "compact")") == 1);
    CHECK(std::ranges::count_if(harness.host.log,
                                [](std::string const& line) { return line.starts_with("navigate  as "); }) == 1);
    CHECK(std::ranges::count_if(harness.host.log, [](std::string const& line) {
              return line.starts_with(R"(download "ref-1" dialog)");
          }) == 1);
    CHECK(harness.host.stale == 1);
    // A route is the application's to resolve; the host's own onNavigate hears only screen ids.
    CHECK(host.navigated.empty());
    CHECK(host.diagnostics == std::vector<std::string>{});
}

// Mutation: answer an application's refusal as success (no diagnostic is reported).
TEST_CASE("DocumentScreenHost reports the application's refusal of a command", "[ui][host]") {
    Harness harness;
    harness.host.refuseAll = true;
    harness.host.refuseTransfers = true;
    RelayHost host{harness, true};
    auto const relay = std::move(*host.mount("relay", ParamMap{}));
    harness.settle();
    clickEach(harness, {"host", "switch", "app", "route", "self", "download"});
    CHECK(host.diagnostics.size() == 6);
    CHECK(harness.host.log.size() == 6);
}

// Mutation: answer `true` for a command when no application is given (no diagnostic is reported).
TEST_CASE("DocumentScreenHost without an application answers every application command not supported", "[ui][host]") {
    Harness harness;
    RelayHost host{harness, false};
    auto const relay = std::move(*host.mount("relay", ParamMap{}));
    harness.settle();
    clickEach(harness, {"host", "switch", "app", "route", "self", "download"});
    CHECK(host.diagnostics.size() == 6);
    // The commands with no answer of their own are dropped without a diagnostic.
    clickEach(harness, {"reset", "locale", "signout"});
    harness.backend.choose(harness.byTestId("Select", "density"), morph::ui::Key{std::string{"compact"}});
    harness.settle();
    clickEach(harness, {"save"});
    harness.fail(harness.dispatcher.last("Lims_Attach"), "denied");
    CHECK(host.diagnostics.size() == 6);
    CHECK(harness.host.log.empty());
    CHECK(harness.host.stale == 0);
    CHECK(relay->title().get().empty());
}

// Mutation: let identityOf evaluate an expression that reads anything but the params (it returns a value instead of
// nothing).
TEST_CASE("identityOf answers only for an identity computed from the params alone", "[ui][host]") {
    Harness harness;
    RelayHost host{harness, true};
    morph::ui::ValueRecord filter;
    filter.insert_or_assign("kind", Value{"open"});
    CHECK(host.identityOf("idNested", ParamMap{{"filter", Value{filter}}}) == R"("open")");
    // A path through a param that has no such member is null, which is no identity.
    CHECK_FALSE(host.identityOf("idNested", ParamMap{{"filter", Value{"flat"}}}).has_value());
    CHECK_FALSE(host.identityOf("idNested", ParamMap{{"filter", Value{morph::ui::ValueRecord{}}}}).has_value());
    CHECK(host.identityOf("idFormat", ParamMap{{"n", Value{std::int64_t{4}}}}).value_or("<none>") == R"("note 4")");
    CHECK_FALSE(host.identityOf("idPref", ParamMap{}).has_value());
    CHECK_FALSE(host.identityOf("idCan", ParamMap{}).has_value());
    CHECK_FALSE(host.identityOf("idEnv", ParamMap{}).has_value());
    CHECK_FALSE(host.identityOf("idBadLogic", ParamMap{{"v", Value{std::int64_t{1}}}}).has_value());
    CHECK(host.identityOf("idBadLogic", ParamMap{{"v", Value{true}}}) == "true");
}

// Mutation: let a DocumentScreenHost be made without a document lookup or a backend (no exception).
TEST_CASE("DocumentScreenHost refuses to be made without documents or a backend", "[ui][host]") {
    Harness harness;
    DocumentScreenHostOptions noDocuments;
    noDocuments.backend = &harness.backend;
    struct Plain final : DocumentScreenHost {
        using DocumentScreenHost::DocumentScreenHost;
        using DocumentScreenHost::onNavigate;
        void onNavigate(std::string_view, ParamMap const&) override {}
    };
    CHECK_THROWS_AS((Plain{harness.session, harness.dispatcher, noDocuments}), std::invalid_argument);
    DocumentScreenHostOptions noBackend;
    noBackend.documents = [](std::string_view) { return std::shared_ptr<Document const>{}; };
    CHECK_THROWS_AS((Plain{harness.session, harness.dispatcher, noBackend}), std::invalid_argument);
}

// Mutation: have the default ScreenHandle claim it can save, or answer a save with `true`.
TEST_CASE("A host screen's handle cannot save unless it says so, and answers a save at once with false",
          "[ui][host]") {
    Harness harness;
    BareHandle handle{harness.runtime};
    CHECK_FALSE(handle.canSave());
    std::optional<bool> clean;
    handle.save([&](bool value) { clean = value; });
    CHECK(clean == false);
    handle.save({});  // no callback is not called
    // A foreign handle has no root, no inspector, and is not remounted.
    RelayHost host{harness, true};
    CHECK(DocumentScreenHost::rootOf(handle) == nullptr);
    CHECK_FALSE(DocumentScreenHost::inspector(handle).has_value());
    auto const remounted = host.remount(handle, loadOrFail(kNote, morph::ui::testing::corpusCatalog()));
    REQUIRE_FALSE(remounted.has_value());
    CHECK(remounted.error().reason == "not a document screen of this host");
    CHECK(remounted.error().screenId == "note");
    auto const noDocument = host.remount(handle, nullptr);
    REQUIRE_FALSE(noDocument.has_value());
    CHECK(noDocument.error().screenId.empty());
}

// Mutation: hand out an inspector over a closed screen (it reads a destroyed screen).
TEST_CASE("The inspector of a mounted screen is there until the screen closes", "[ui][host]") {
    Harness harness;
    RelayHost host{harness, true};
    auto note = std::move(*host.mount("note", ParamMap{{"noteId", Value{std::int64_t{2}}}}));
    harness.settle();
    CHECK(DocumentScreenHost::inspector(*note).has_value());
    note->forceClose();
    harness.settle();
    CHECK_FALSE(DocumentScreenHost::inspector(*note).has_value());
}

// Mutation: skip the opener's answer when the host keeps the default onScreenClosed (the title stays "none").
TEST_CASE("A dialog answers its opener under a host that ignores screens closing themselves", "[ui][host]") {
    Harness harness;
    RelayHost host{harness, true};
    auto const opener = std::move(*host.mount("opener", ParamMap{}));
    harness.settle();
    harness.backend.click(harness.byTestId("Button", "pick"));
    harness.settle();
    REQUIRE(host.navigated.size() == 1);
    auto chooser = std::move(*host.mount(host.navigated.front().first, host.navigated.front().second));
    harness.settle();
    harness.backend.click(harness.byTestId("Button", "choose"));
    harness.settle();
    CHECK(opener->title().get() == "apple");
    CHECK(DocumentScreenHost::rootOf(*chooser) == nullptr);
}
