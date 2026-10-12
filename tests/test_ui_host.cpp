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
    { "kind": "button", "testId": "keep", "label": "keep", "onClick": [ { "set": "keep", "value": true } ] } ] } })";

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
        : DocumentScreenHost{harness.session, harness.dispatcher, options(harness, guardAnswer, this)} {}

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
    static DocumentScreenHostOptions options(Harness& harness, std::optional<bool> guardAnswer, Shell* self) {
        static std::map<std::string, std::shared_ptr<Document const>, std::less<>> const documents{
            {"note", loadOrFail(kNote, morph::ui::testing::corpusCatalog())},
            {"saving", loadOrFail(kSaving, morph::ui::testing::corpusCatalog())},
            {"opener", loadOrFail(kOpener, morph::ui::testing::corpusCatalog())},
            {"chooser", loadOrFail(kChooser, morph::ui::testing::corpusCatalog())}};
        DocumentScreenHostOptions made;
        made.documents = [](std::string_view id) -> std::shared_ptr<Document const> {
            auto const found = documents.find(id);
            return found == documents.end() ? nullptr : found->second;
        };
        made.backend = &harness.backend;
        made.application = &harness.host;
        if (guardAnswer) {
            made.guard = [self, answer = *guardAnswer](ScreenHandle&, std::function<void(bool)> const& decide) {
                ++self->guarded;
                decide(answer);
            };
        }
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
