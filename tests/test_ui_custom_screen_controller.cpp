// SPDX-License-Identifier: Apache-2.0
//
// The headless custom screens controller (docs/spec/ui/custom_screens.md, "The client"), driven with no view: drafts
// mounted from a checked edit, saved, discarded and removed, Open as and the navigator as data, and an answer that
// finds its draft gone told as an outcome rather than acted on.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/ui/custom_screen_controller.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/json_source.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ui_custom_client_support.hpp"

using custom_client::kEdit;
using custom_client::kHome;
using custom_client::note;
using morph::ui::CustomScreenCheckStatus;
using morph::ui::CustomScreenEvent;
using morph::ui::CustomScreenOutcome;
using morph::ui::CustomScreenSaveKind;
using morph::ui::DocumentScreenHost;
using morph::ui::ParamMap;
using morph::ui::WorkspaceTabId;

namespace {

/// One window of the notes application signed in as `ann`, with a controller over its workspace and no view: the
/// workspace's chrome hook only has the controller follow each tab, and every outcome is kept as data.
struct ControllerRig {
    explicit ControllerRig(std::shared_ptr<morph::ui::ICustomScreenStore> store = nullptr)
        : server{std::move(store), morph::ui::CustomScreensOptions{}, custom_client::notesBundle()} {
        bridge = std::make_unique<morph::bridge::Bridge>(
            std::make_unique<morph::backend::SimulatedRemoteBackend>(*server.server), harness.owner);
        morph::session::Context signedIn;
        signedIn.token = "ann";
        bridge->setDefaultSession(std::move(signedIn));
        models = std::make_unique<morph::ui::BridgeDispatcher>(*bridge, &harness.owner);
        sync = std::make_unique<morph::ui::CustomScreenSync>(*models, "notes", [] { return uiservice::facts(); });
        host = std::make_unique<custom_client::NotesHost>(harness, hostOptions());
        morph::ui::WorkspaceOptions made;
        made.chrome = [this](WorkspaceTabId tab, std::string_view /*screenId*/) -> morph::ui::Node {
            controller->follow(tab);
            return nullptr;
        };
        workspace = std::make_unique<morph::ui::Workspace>(harness.runtime, harness.backend, *host, std::move(made));
        controller = std::make_unique<morph::ui::CustomScreenController>(controllerOptions());
        controller->attach(&workspace->model());
        sync->setOnListed([this](morph::ui::CustomScreensListed const& listed,
                                 std::vector<std::string> const& /*dropped*/) { controller->listed(listed); });
        std::optional<bool> offered;
        sync->probe([this, &offered](bool answer) {
            controller->setOffered(answer);
            offered = answer;
        });
        REQUIRE(pump([&offered] { return offered.has_value(); }));
        REQUIRE(*offered);
    }

    template <typename Pred>
    bool pump(Pred done, std::chrono::milliseconds budget = morph::testing::kDefaultWaitBudget) {
        return morph::testing::waitUntil(
            [&] {
                harness.owner.runAll();
                return done();
            },
            morph::testing::WaitBudget{budget});
    }

    WorkspaceTabId open(std::string_view screenId, ParamMap const& params) {
        auto const opened = workspace->open(screenId, params);
        REQUIRE(opened.has_value());
        harness.settle();
        return workspace->model().order().at(*opened);
    }

    [[nodiscard]] std::string screenOf(std::size_t index) const { return workspace->tabs().at(index).screenId; }

    /// Saves a custom screen of `ann`'s on the server, fetches it, and waits for the list to show it.
    std::string saveOnServer(std::string const& title, std::string const& basedOn, std::string const& document) {
        std::string const id = custom_client::savedIdOf(server.call(
            morph::ui::kCustomScreensSave, custom_client::saveBody("notes", title, basedOn, document), "ann"));
        sync->relist();
        REQUIRE(pump([this, &id] { return controller->summaryOf(id).has_value(); }));
        std::optional<bool> fetched;
        sync->open(id, [&fetched](auto loaded) { fetched = loaded.has_value(); });
        REQUIRE(pump([&fetched] { return fetched.has_value(); }));
        REQUIRE(*fetched);
        return id;
    }

    /// Previews @p text on @p tab and answers how it ended.
    morph::ui::CustomScreenCheckResult preview(WorkspaceTabId tab, std::string const& text) {
        std::optional<morph::ui::CustomScreenCheckResult> result;
        controller->preview(tab, text,
                            [&result](morph::ui::CustomScreenCheckResult answer) { result = std::move(answer); });
        REQUIRE(pump([&result] { return result.has_value(); }));
        return *result;
    }

    /// The edit document with its heading renamed to @p heading.
    static std::string headed(std::string const& heading) {
        return screens_test::replaced(std::string{kEdit}, "\"Edit note\"", "\"" + heading + "\"");
    }

    morph::ui::DocumentScreenHostOptions hostOptions() {
        morph::ui::DocumentScreenHostOptions made;
        made.documents = [this](std::string_view id) -> std::shared_ptr<morph::ui::Document const> {
            if (morph::ui::isCustomScreenId(id)) {
                return sync->documents().find(id);
            }
            return applicationDocument(id);
        };
        made.backend = &harness.backend;
        made.application = &harness.host;
        made.guard = [this](morph::ui::ScreenHandle& /*screen*/,
                            std::function<void(morph::ui::CloseChoice)> const& choose) { choose(guardChoice); };
        return made;
    }

    [[nodiscard]] std::shared_ptr<morph::ui::Document const> applicationDocument(std::string_view id) const {
        if (id == "notes.edit") {
            return edit;
        }
        return id == "notes.home" ? home : nullptr;
    }

    morph::ui::CustomScreenControllerOptions controllerOptions() {
        return morph::ui::CustomScreenControllerOptions{
            .runtime = &harness.runtime,
            .host = host.get(),
            .sync = sync.get(),
            .documents = [this](std::string_view id) { return applicationDocument(id); },
            .sourceOf =
                [](std::string_view id) {
                    if (id == "notes.edit") {
                        return std::string{kEdit};
                    }
                    return id == "notes.home" ? std::string{kHome} : std::string{};
                },
            .catalog = [] { return uiservice::facts(); },
            .titleOf = [](std::string_view id) { return id == "notes.edit" ? std::string{"Note"} : std::string{id}; },
            .openTab = [this](std::string const& id, ParamMap const& params) -> std::expected<void, std::string> {
                auto const opened = workspace->openTab(id, params);
                if (!opened) {
                    return std::unexpected(opened.error().reason);
                }
                return {};
            },
            .onOutcome = [this](CustomScreenOutcome const& outcome) { outcomes.push_back(outcome); },
            .onListChanged = {},
        };
    }

    morph::ui::testing::Harness harness;
    screens_test::Server server;
    std::unique_ptr<morph::bridge::Bridge> bridge;
    std::unique_ptr<morph::ui::BridgeDispatcher> models;
    std::unique_ptr<morph::ui::CustomScreenSync> sync;
    std::shared_ptr<morph::ui::Document const> edit = custom_client::loadedNotes(kEdit);
    std::shared_ptr<morph::ui::Document const> home = custom_client::loadedNotes(kHome);
    std::unique_ptr<custom_client::NotesHost> host;
    std::unique_ptr<morph::ui::Workspace> workspace;
    std::unique_ptr<morph::ui::CustomScreenController> controller;
    std::vector<CustomScreenOutcome> outcomes;
    morph::ui::CloseChoice guardChoice = morph::ui::CloseChoice::Cancel;
};

}  // namespace

// Mutation: skip mountDraft after the server's check (the tab is not drafted and draftOf answers nothing).
TEST_CASE("custom screen controller: preview mounts a checked edit as the tab's draft, with no view",
          "[ui][custom-screens][controller]") {
    ControllerRig rig;
    WorkspaceTabId const tab = rig.open("notes.edit", note(1));
    CHECK_FALSE(rig.controller->drafted(tab));
    std::string const edited = ControllerRig::headed("Edited");
    CHECK(rig.preview(tab, edited).status == CustomScreenCheckStatus::Done);
    CHECK(rig.controller->drafted(tab));
    CHECK(rig.controller->draftOf(tab) == std::optional<std::string>{edited});
    CHECK(rig.harness.text("heading") == "Edited");
    CHECK(rig.controller->screenOf(tab) == std::optional<std::string>{"notes.edit"});

    std::optional<morph::ui::CustomScreenSource> source;
    rig.controller->source(tab, [&source](morph::ui::CustomScreenSource shown) { source = std::move(shown); });
    REQUIRE(source.has_value());
    CHECK(source->text == morph::ui::prettyJson(edited));
    CHECK_FALSE(source->readOnly);
    CHECK_FALSE(source->stored);
}

// Mutation: answer a refused check as Done (the refusal's pointer is empty and the draft mounts).
TEST_CASE("custom screen controller: refusals come back as data, with their pointer",
          "[ui][custom-screens][controller]") {
    ControllerRig rig;
    WorkspaceTabId const tab = rig.open("notes.edit", note(1));
    auto const unloadable = rig.preview(tab, screens_test::replaced(std::string{kEdit}, "\"column\"", "\"hologram\""));
    CHECK(unloadable.status == CustomScreenCheckStatus::Refused);
    CHECK_FALSE(unloadable.refusal.reason.empty());

    std::string const printing =
        screens_test::replaced(std::string{kEdit}, R"({"set":"content","value":"changed"})", R"({"host":"print"})");
    auto const refused = rig.preview(tab, printing);
    CHECK(refused.status == CustomScreenCheckStatus::Refused);
    CHECK_FALSE(refused.refusal.pointer.empty());
    CHECK_FALSE(rig.controller->drafted(tab));

    auto const otherId =
        rig.preview(tab, screens_test::replaced(std::string{kEdit}, "\"notes.edit\"", "\"notes.other\""));
    CHECK(otherId.status == CustomScreenCheckStatus::Refused);
    CHECK(otherId.refusal.pointer == "/id");
}

// Mutation: drop the serial check in preview's answer (the dropped check mounts its draft).
TEST_CASE("custom screen controller: a check whose asker has gone answers Dropped and mounts nothing",
          "[ui][custom-screens][controller]") {
    ControllerRig rig;
    WorkspaceTabId const tab = rig.open("notes.edit", note(1));
    std::optional<morph::ui::CustomScreenCheckResult> result;
    rig.controller->preview(tab, ControllerRig::headed("Late"),
                            [&result](morph::ui::CustomScreenCheckResult answer) { result = std::move(answer); });
    rig.controller->dropSourceAnswers();
    REQUIRE(rig.pump([&result] { return result.has_value(); }));
    CHECK(result->status == CustomScreenCheckStatus::Dropped);
    CHECK_FALSE(rig.controller->drafted(tab));
    CHECK(rig.harness.text("heading") == "Edit note");
}

// Mutation: drop the tab-generation check in preview's answer (the check mounts over the assistant's draft).
TEST_CASE("custom screen controller: a proposal made while a check runs is not mounted over by its answer",
          "[ui][custom-screens][controller]") {
    ControllerRig rig;
    WorkspaceTabId const tab = rig.open("notes.edit", note(1));
    std::optional<morph::ui::CustomScreenCheckResult> result;
    rig.controller->preview(tab, ControllerRig::headed("Checked"),
                            [&result](morph::ui::CustomScreenCheckResult answer) { result = std::move(answer); });
    REQUIRE(rig.controller->proposeDraft(tab, "notes.edit", ControllerRig::headed("Proposed")));
    REQUIRE(rig.pump([&result] { return result.has_value(); }));
    CHECK(result->status == CustomScreenCheckStatus::Dropped);
    CHECK(rig.harness.text("heading") == "Proposed");

    CHECK_FALSE(rig.controller->proposeDraft(tab, "notes.home", ControllerRig::headed("Elsewhere")));
    CHECK_FALSE(rig.controller->proposeDraft(tab, "notes.edit", "{not json"));
    REQUIRE_FALSE(rig.outcomes.empty());
    CHECK(rig.outcomes.back().event == CustomScreenEvent::ProposalNotShown);
}

// Mutation: skip the generation check in the save's answer (the tab is replaced though its draft was discarded).
TEST_CASE("custom screen controller: a save that answers after its draft was discarded is told, not acted on",
          "[ui][custom-screens][controller]") {
    morph::exec::ThreadPoolExecutor storePool{1};
    auto const store = std::make_shared<custom_client::GatedStore>(storePool);
    ControllerRig rig{store};
    custom_client::GateGuard const guard{*store};
    WorkspaceTabId const tab = rig.open("notes.edit", note(1));
    REQUIRE(rig.preview(tab, ControllerRig::headed("First")).status == CustomScreenCheckStatus::Done);
    store->hold();
    std::optional<std::expected<void, std::string>> answered;
    rig.controller->saveAs(tab, "First", std::nullopt,
                           [&answered](std::expected<void, std::string> answer) { answered = std::move(answer); });
    CHECK(rig.controller->saving(tab));
    rig.controller->discard(tab);
    REQUIRE(rig.pump([&rig] { return rig.harness.text("heading") == "Edit note"; }));
    store->release();
    REQUIRE(rig.pump([&rig] { return !rig.outcomes.empty(); }));
    REQUIRE(answered.has_value());
    CHECK(answered->has_value());
    CHECK(rig.outcomes.back().event == CustomScreenEvent::SavedDraftGone);
    CHECK(rig.outcomes.back().save == CustomScreenSaveKind::SaveAs);
    CHECK(rig.outcomes.back().title == "First");
    CHECK(rig.screenOf(0) == "notes.edit");
    CHECK_FALSE(rig.controller->saving(tab));
}

// Mutation: tell no outcome when the tab closed first (the outcomes stay empty).
TEST_CASE("custom screen controller: a save that answers after its tab closed is told as such",
          "[ui][custom-screens][controller]") {
    morph::exec::ThreadPoolExecutor storePool{1};
    auto const store = std::make_shared<custom_client::GatedStore>(storePool);
    ControllerRig rig{store};
    custom_client::GateGuard const guard{*store};
    WorkspaceTabId const tab = rig.open("notes.edit", note(1));
    REQUIRE(rig.preview(tab, ControllerRig::headed("Late")).status == CustomScreenCheckStatus::Done);
    store->hold();
    rig.controller->saveAs(tab, "Late", std::nullopt, {});
    rig.guardChoice = morph::ui::CloseChoice::Discard;
    std::optional<bool> closed;
    rig.workspace->close(0, [&closed](bool outcome) { closed = outcome; });
    REQUIRE(rig.pump([&closed] { return closed.has_value(); }));
    store->release();
    REQUIRE(rig.pump([&rig] { return !rig.outcomes.empty(); }));
    CHECK(rig.outcomes.back().event == CustomScreenEvent::SavedTabClosed);
    CHECK(rig.workspace->tabs().empty());
}

// Mutation: skip openInPlace after Save as (the tab stays on the application screen).
TEST_CASE("custom screen controller: Save as opens the saved screen in the tab's place, attributes and all",
          "[ui][custom-screens][controller]") {
    ControllerRig rig;
    WorkspaceTabId const tab = rig.open("notes.edit", note(3));
    REQUIRE(rig.preview(tab, ControllerRig::headed("Mine")).status == CustomScreenCheckStatus::Done);
    morph::ui::Json::object_t attributes;
    REQUIRE_FALSE(glz::read_json(attributes, std::string{R"({"shared":true})"}));
    std::optional<std::expected<void, std::string>> answered;
    rig.controller->saveAs(tab, "Mine", attributes,
                           [&answered](std::expected<void, std::string> answer) { answered = std::move(answer); });
    REQUIRE(rig.pump([&rig] { return morph::ui::isCustomScreenId(rig.screenOf(0)); }));
    REQUIRE(rig.pump([&rig] { return !rig.outcomes.empty(); }));
    CHECK(rig.outcomes.back().event == CustomScreenEvent::Saved);
    CHECK(rig.outcomes.back().save == CustomScreenSaveKind::SaveAs);
    REQUIRE(answered.has_value());
    CHECK(answered->has_value());
    CHECK_FALSE(rig.controller->drafted(tab));
    REQUIRE(rig.pump([&rig] { return rig.sync->listed().screens.size() == 1; }));
    REQUIRE(rig.sync->listed().screens.front().attributes.has_value());
    CHECK(rig.sync->listed().screens.front().attributes->contains("shared"));
    std::string const id = rig.screenOf(0);
    CHECK(rig.controller->canChange(id));
    CHECK(rig.controller->titleOf(id) == "Mine");
    CHECK(rig.controller->baseOf(id) == "notes.edit");
}

// Mutation: report a refused save as answered (the answer holds no refusal and the message stays empty).
TEST_CASE("custom screen controller: a store's refusal is the tab's message and the save's answer",
          "[ui][custom-screens][controller]") {
    morph::exec::ThreadPoolExecutor storePool{1};
    ControllerRig rig{std::make_shared<custom_client::RefusingStore>(storePool)};
    WorkspaceTabId const tab = rig.open("notes.edit", note(1));
    REQUIRE(rig.preview(tab, ControllerRig::headed("Too many")).status == CustomScreenCheckStatus::Done);
    std::optional<std::expected<void, std::string>> answered;
    rig.controller->saveAs(tab, "Too many", std::nullopt,
                           [&answered](std::expected<void, std::string> answer) { answered = std::move(answer); });
    REQUIRE(rig.pump([&answered] { return answered.has_value(); }));
    REQUIRE_FALSE(answered->has_value());
    CHECK(answered->error().contains("over your limit of custom screens"));
    CHECK(rig.controller->message(tab).contains("over your limit of custom screens"));
    CHECK(rig.controller->drafted(tab));
}

// Mutation: skip the fetch after Save (the draft is never ended and no outcome is told).
TEST_CASE("custom screen controller: Save overwrites my screen and ends the draft",
          "[ui][custom-screens][controller]") {
    ControllerRig rig;
    std::string const id = rig.saveOnServer("Mine", "notes.edit", std::string{kEdit});
    WorkspaceTabId const tab = rig.open(id, note(1));
    std::string const mine =
        screens_test::replaced(ControllerRig::headed("Saved"), "\"notes.edit\"", "\"" + id + "\"");
    REQUIRE(rig.preview(tab, mine).status == CustomScreenCheckStatus::Done);
    rig.controller->save(tab);
    CHECK(rig.controller->saving(tab));
    REQUIRE(rig.pump([&rig] { return !rig.outcomes.empty(); }));
    CHECK(rig.outcomes.back().event == CustomScreenEvent::Saved);
    CHECK(rig.outcomes.back().save == CustomScreenSaveKind::Save);
    CHECK_FALSE(rig.controller->drafted(tab));
    CHECK(rig.harness.text("heading") == "Saved");
}

// Mutation: skip fallBack after a removal (the tab stays on the removed screen).
TEST_CASE("custom screen controller: remove tells the outcome and its tab opens the base",
          "[ui][custom-screens][controller]") {
    ControllerRig rig;
    std::string const id = rig.saveOnServer("Mine", "notes.edit", std::string{kEdit});
    rig.open(id, note(5));
    std::optional<std::expected<void, std::string>> removed;
    rig.controller->remove(id, [&removed](std::expected<void, std::string> answer) { removed = std::move(answer); });
    REQUIRE(rig.pump([&rig] { return rig.screenOf(0) == "notes.edit"; }));
    REQUIRE(removed.has_value());
    CHECK(removed->has_value());
    REQUIRE_FALSE(rig.outcomes.empty());
    CHECK(rig.outcomes.back().event == CustomScreenEvent::Removed);
    CHECK(rig.outcomes.back().title == "Mine");
    CHECK(DocumentScreenHost::paramsOf(*rig.workspace->handleOf(0)) == std::optional<ParamMap>{note(5)});

    std::optional<std::expected<void, std::string>> again;
    rig.controller->remove(id, [&again](std::expected<void, std::string> answer) { again = std::move(answer); });
    REQUIRE(rig.pump([&again] { return again.has_value(); }));
    REQUIRE_FALSE(again->has_value());
    CHECK(again->error().contains("no such custom screen"));
}

// Mutation: list the base last in Open as (the first choice is a custom screen).
TEST_CASE("custom screen controller: Open as and the navigator are data", "[ui][custom-screens][controller]") {
    ControllerRig rig;
    std::string const alpha = rig.saveOnServer("Alpha", "notes.edit", std::string{kEdit});
    std::string const homely = rig.saveOnServer("Homely", "notes.home", std::string{kHome});
    WorkspaceTabId const tab = rig.open("notes.edit", note(2));
    std::vector<morph::ui::CustomScreenChoice> const choices = rig.controller->openAsChoices("notes.edit");
    REQUIRE(choices.size() == 2);
    CHECK(choices.front().id == "notes.edit");
    CHECK(choices.front().title == "Note");
    CHECK(choices.front().current);
    CHECK(choices.back().id == alpha);
    CHECK(choices.back().mine);
    CHECK(rig.controller->openAsChoices("notes.home").empty());

    std::vector<morph::ui::NavigatorCustomEntry> const entries = rig.controller->navigatorEntries();
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().id == homely);

    rig.controller->openAs(tab, alpha);
    REQUIRE(rig.pump([&rig, &alpha] { return rig.screenOf(0) == alpha; }));
    // Opening in place gives the tab a new id.
    rig.controller->openAs(rig.workspace->model().order().at(0), "notes.edit");
    REQUIRE(rig.pump([&rig] { return rig.screenOf(0) == "notes.edit"; }));

    std::optional<std::expected<void, std::string>> opened;
    rig.controller->openCustom(homely, {},
                               [&opened](std::expected<void, std::string> answer) { opened = std::move(answer); });
    REQUIRE(rig.pump([&opened] { return opened.has_value(); }));
    CHECK(opened->has_value());
    CHECK(rig.workspace->tabs().size() == 2);
}

// Mutation: answer checkAndSave Done without saving (the stored document keeps its old heading).
TEST_CASE("custom screen controller: checkAndSave checks the text and saves it over my stored screen",
          "[ui][custom-screens][controller]") {
    ControllerRig rig;
    std::string const id = rig.saveOnServer("Mine", "notes.edit", std::string{kEdit});
    std::optional<morph::ui::CustomScreenSource> source;
    rig.controller->storedSource(id, [&source](morph::ui::CustomScreenSource shown) { source = std::move(shown); });
    REQUIRE(rig.pump([&source] { return source.has_value(); }));
    CHECK(source->stored);
    CHECK_FALSE(source->readOnly);
    CHECK_FALSE(source->broken.has_value());

    std::string const stored = screens_test::replaced(std::string{kEdit}, "\"notes.edit\"", "\"" + id + "\"");
    std::string const printing =
        screens_test::replaced(stored, R"({"set":"content","value":"changed"})", R"({"host":"print"})");
    std::optional<morph::ui::CustomScreenCheckResult> refused;
    rig.controller->checkAndSave(
        id, printing, {}, [&refused](morph::ui::CustomScreenCheckResult answer) { refused = std::move(answer); });
    REQUIRE(rig.pump([&refused] { return refused.has_value(); }));
    CHECK(refused->status == CustomScreenCheckStatus::Refused);

    bool checked = false;
    std::optional<morph::ui::CustomScreenCheckResult> saved;
    rig.controller->checkAndSave(
        id, screens_test::replaced(stored, "\"Edit note\"", "\"Fixed\""), [&checked] { checked = true; },
        [&saved](morph::ui::CustomScreenCheckResult answer) { saved = std::move(answer); });
    REQUIRE(rig.pump([&saved] { return saved.has_value(); }));
    CHECK(checked);
    CHECK(saved->status == CustomScreenCheckStatus::Done);
    // The save relists, and the list's new hash drops the kept copy.
    REQUIRE(rig.pump([&rig, &id] { return rig.sync->documents().find(id) == nullptr; }));
    std::optional<std::string> text;
    rig.sync->get(id, [&text](std::expected<morph::ui::CustomScreenFound, std::string> found) {
        text = found ? found->document : std::string{};
    });
    REQUIRE(rig.pump([&text] { return text.has_value(); }));
    CHECK(text->contains("Fixed"));
}

// Mutation: drop the null checks of the constructor (it builds over a null sync).
TEST_CASE("custom screen controller: refuses options it cannot work with", "[ui][custom-screens][controller]") {
    ControllerRig rig;
    morph::ui::CustomScreenControllerOptions options = rig.controllerOptions();
    options.sync = nullptr;
    CHECK_THROWS_AS(morph::ui::CustomScreenController{options}, std::invalid_argument);
    options = rig.controllerOptions();
    options.openTab = {};
    CHECK_THROWS_AS(morph::ui::CustomScreenController{options}, std::invalid_argument);
}
