// SPDX-License-Identifier: Apache-2.0

// Development reload (spec 5 §12): a screen remounted on a changed document keeps each state of its own scope that
// the new document still declares under the same name and type, keeps its identity, and a new document that fails
// validation keeps the old one and is reported.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/ui/bundle.hpp>
#include <morph/ui/dev_reload.hpp>
#include <morph/ui/host.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ui_interpreter_support.hpp"

using morph::ui::Bundle;
using morph::ui::DocumentMap;
using morph::ui::DocumentScreenHost;
using morph::ui::DocumentScreenHostOptions;
using morph::ui::Manifest;
using morph::ui::ManifestFile;
using morph::ui::MountedScreen;
using morph::ui::ParamMap;
using morph::ui::ScreenHandle;
using morph::ui::Value;
using morph::ui::testing::Harness;
using morph::ui::testing::loadOrFail;

namespace {

/// The first version: a name, a count and a level, edited by buttons, shown as text.
constexpr std::string_view kCounter = R"({
  "vocab": "ui/1", "id": "counter", "params": { "counterId": "int" },
  "identity": { "ref": "counterId" }, "title": { "ref": "name" },
  "state": { "name": { "type": "string", "init": "Draft" }, "count": { "type": "int", "init": 0 },
             "level": { "type": "int", "init": 0 } },
  "onUnmount": [ { "notify": "unmounted" } ],
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "name", "text": { "ref": "name" } },
    { "kind": "text", "testId": "count", "text": { "fn": "format", "args": ["{0}", { "ref": "count" }] } },
    { "kind": "button", "testId": "rename", "label": "rename", "onClick": [ { "set": "name", "value": "Renamed" } ] },
    { "kind": "button", "testId": "bump", "label": "bump",
      "onClick": [ { "set": "count", "value": 3 }, { "set": "level", "value": 4 } ] } ] } })";

/// The second version: `name` is still a string; `count` is now a string; `level` is now untyped, which admits the
/// old value but is not the same type; `added` is new; the view changed.
constexpr std::string_view kCounterChanged = R"({
  "vocab": "ui/1", "id": "counter", "params": { "counterId": "int" },
  "identity": { "ref": "counterId" }, "title": { "fn": "concat", "args": ["v2 ", { "ref": "name" }] },
  "state": { "name": { "type": "string", "init": "Fresh" }, "count": { "type": "string", "init": "zero" },
             "level": { "init": "reset" }, "added": { "type": "int", "init": 1 } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "name", "text": { "ref": "name" } },
    { "kind": "text", "testId": "count", "text": { "ref": "count" } },
    { "kind": "text", "testId": "level", "text": { "fn": "format", "args": ["{0}", { "ref": "level" }] } },
    { "kind": "text", "testId": "added", "text": { "fn": "format", "args": ["{0}", { "ref": "added" }] } } ] } })";

/// A version that needs a param the mounted screen was not given: it loads, and the interpreter refuses the mount.
constexpr std::string_view kCounterNewParam = R"({
  "vocab": "ui/1", "id": "counter", "params": { "counterId": "int", "owner": "string" },
  "view": { "kind": "text", "testId": "name", "text": "needs an owner" } })";

/// A version that does not validate: a node kind `ui/1` does not have.
constexpr std::string_view kCounterInvalid = R"({
  "vocab": "ui/1", "id": "counter", "params": { "counterId": "int" },
  "view": { "kind": "hologram", "testId": "name" } })";

/// A host over a document map the test replaces entries of.
class Shell final : public DocumentScreenHost {
public:
    Shell(Harness& harness, DocumentMap& documents)
        : DocumentScreenHost{harness.session, harness.dispatcher, options(harness, documents)} {}

    void onNavigate(std::string_view screenId, ParamMap const& params) override {
        static_cast<void>(screenId);
        static_cast<void>(params);
    }

    using DocumentScreenHost::onNavigate;

private:
    static DocumentScreenHostOptions options(Harness& harness, DocumentMap& documents) {
        DocumentScreenHostOptions made;
        made.documents = [&documents](std::string_view screenId) -> std::shared_ptr<morph::ui::Document const> {
            auto const found = documents.find(screenId);
            return found == documents.end() ? nullptr : found->second;
        };
        made.backend = &harness.backend;
        made.application = &harness.host;
        return made;
    }
};

/// A bundle holding one document, the counter, as @p text.
Bundle bundleWith(std::string_view text) {
    Bundle bundle;
    bundle.applicationId = "dev";
    bundle.documents.emplace("counter", std::string{text});
    return bundle;
}

int count(std::vector<std::string> const& log, std::string_view entry) {
    return static_cast<int>(std::ranges::count(log, std::string{entry}));
}

/// A mounted counter whose name and count the user has changed.
struct Edited {
    explicit Edited(Harness& harnessIn) : harness{&harnessIn}, shell{harnessIn, documents} {
        documents.emplace("counter", loadOrFail(kCounter, morph::ui::testing::corpusCatalog()));
        auto mounted = shell.mount("counter", ParamMap{{"counterId", Value{std::int64_t{7}}}});
        REQUIRE(mounted.has_value());
        handle = std::move(*mounted);
        harness->backend.click(harness->byTestId("Button", "rename"));
        harness->backend.click(harness->byTestId("Button", "bump"));
        harness->settle();
        REQUIRE(harness->text("name") == "Renamed");
        REQUIRE(harness->text("count") == "3");
    }

    morph::ui::ReloadOutcome reload(std::string_view text) {
        std::vector<std::string> const changed{"counter"};
        std::vector<MountedScreen> const mounted{MountedScreen{.screenId = "counter", .handle = handle.get()}};
        auto outcome = morph::ui::reloadDocuments(
            changed, bundleWith(text), morph::ui::LoadOptions{.catalog = morph::ui::testing::corpusCatalog()},
            documents, shell, mounted);
        harness->settle();
        return outcome;
    }

    Harness* harness;
    DocumentMap documents;
    Shell shell;
    std::unique_ptr<ScreenHandle> handle;
};

}  // namespace

// Mutation: drop the kept value in ScreenCore::initState (always evaluate the init) — `name` reads "Fresh".
// Mutation: skip the type comparison in ScreenCore::keptState — `count` keeps 3 under a string type.
TEST_CASE("A remount keeps each state of the same name and type, drops one whose type changed, and keeps the identity",
          "[ui][dev-reload]") {
    Harness harness;
    Edited edited{harness};
    auto const before = edited.documents.at("counter");

    morph::ui::ReloadOutcome const outcome = edited.reload(kCounterChanged);
    CHECK(outcome.refused.empty());
    CHECK(outcome.remounted == std::vector<std::string>{"counter"});
    CHECK(edited.documents.at("counter") != before);

    CHECK(harness.text("name") == "Renamed");  // kept: a string both times
    CHECK(harness.text("count") == "zero");    // dropped: an int before, a string now
    CHECK(harness.text("level") == "reset");   // dropped: `int` before, untyped now, though it would admit 4
    CHECK(harness.text("added") == "1");       // new: from its init
    CHECK(edited.handle->identity().get() == Value{std::int64_t{7}});
    CHECK(edited.handle->title().get() == "v2 Renamed");  // the handle follows the new screen
    CHECK(DocumentScreenHost::rootOf(*edited.handle) != nullptr);
    CHECK(count(harness.host.log, "notify  unmounted") == 1);  // the old screen was unmounted, once
    CHECK_FALSE(harness.backend.find("Button", "testId", "rename").has_value());
}

// Mutation: insert the new document into the map before loading succeeds — the map holds a document that did not
// load (or the old screen is replaced).
TEST_CASE("A new document that fails validation keeps the old one, and the refusal is reported", "[ui][dev-reload]") {
    Harness harness;
    Edited edited{harness};
    auto const before = edited.documents.at("counter");

    morph::ui::ReloadOutcome const invalid = edited.reload(kCounterInvalid);
    REQUIRE(invalid.refused.size() == 1U);
    CHECK(invalid.refused.front().document == "counter");
    CHECK_FALSE(invalid.refused.front().reason.empty());
    CHECK(invalid.remounted.empty());
    CHECK(edited.documents.at("counter") == before);

    morph::ui::ReloadOutcome const notJson = edited.reload("{ not json");
    REQUIRE(notJson.refused.size() == 1U);
    CHECK(notJson.refused.front().document == "counter");
    CHECK(edited.documents.at("counter") == before);

    // A document that loads but that the interpreter refuses for this screen's params keeps the old one too.
    morph::ui::ReloadOutcome const refused = edited.reload(kCounterNewParam);
    REQUIRE(refused.refused.size() == 1U);
    CHECK(refused.refused.front().reason.contains("owner"));
    CHECK(refused.remounted.empty());
    CHECK(edited.documents.at("counter") == before);

    // The old screen is still shown, with what the user entered.
    CHECK(harness.text("name") == "Renamed");
    CHECK(harness.text("count") == "3");
    CHECK(edited.handle->title().get() == "Renamed");
    CHECK(count(harness.host.log, "notify  unmounted") == 0);
}

TEST_CASE("A closed screen is not remounted", "[ui][dev-reload]") {
    Harness harness;
    Edited edited{harness};
    edited.handle->forceClose();
    harness.settle();
    auto const document = loadOrFail(kCounterChanged, morph::ui::testing::corpusCatalog());
    auto const remounted = edited.shell.remount(*edited.handle, document);
    REQUIRE_FALSE(remounted.has_value());
    CHECK(remounted.error().reason == "the screen is closed");
}

TEST_CASE("changedDocuments names each screen and app document whose hash differs, appears or disappears",
          "[ui][dev-reload]") {
    auto const file = [](std::string kind, std::string fileId, std::string hash) {
        return ManifestFile{
            .id = std::move(fileId), .kind = std::move(kind), .hash = std::move(hash), .size = 1, .isPublic = false};
    };
    Manifest const before{.application = "dev",
                          .version = "1",
                          .files = {file("app", "app", "h1"), file("screen", "a", "h2"), file("screen", "b", "h3"),
                                    file("screen", "gone", "h4"), file("theme", "default", "h5")}};
    Manifest const after{
        .application = "dev",
        .version = "2",
        .files = {file("app", "app", "h1"), file("screen", "a", "h2-changed"), file("screen", "b", "h3"),
                  file("screen", "new", "h6"), file("theme", "default", "h5-changed")}};
    CHECK(morph::ui::changedDocuments(before, after) == std::vector<std::string>{"a", "gone", "new"});
    CHECK(morph::ui::changedDocuments(before, before).empty());
}
