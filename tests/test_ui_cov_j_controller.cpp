// SPDX-License-Identifier: Apache-2.0

// The headless custom screens controller's refusals of a save it cannot make, and of a save the server refuses after
// the draft it was asked for is gone.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <expected>
#include <latch>
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
using morph::ui::CustomScreenOutcome;
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

/// Occupies every thread of the rig's server pool until released, so a call sent meanwhile is answered only after.
class ServerHold {
public:
    explicit ServerHold(ControllerRig& rig) {
        std::latch started{kThreads};
        for (std::ptrdiff_t thread = 0; thread < kThreads; ++thread) {
            rig.server.pool.post([this, &started] {
                started.count_down();
                _released.wait(false);
            });
        }
        started.wait();
    }
    ~ServerHold() { release(); }
    ServerHold(ServerHold const&) = delete;
    ServerHold& operator=(ServerHold const&) = delete;
    ServerHold(ServerHold&&) = delete;
    ServerHold& operator=(ServerHold&&) = delete;

    void release() {
        _released.store(true);
        _released.notify_all();
    }

private:
    static constexpr std::ptrdiff_t kThreads = 2;
    std::atomic<bool> _released{false};
};

using Answer = std::optional<std::expected<void, std::string>>;

}  // namespace

TEST_CASE("covj custom controller: Save as on a tab that closed is refused as a closed screen",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    WorkspaceTabId const tab = rig.open("notes.edit", note(1));
    rig.guardChoice = morph::ui::CloseChoice::Discard;
    std::optional<bool> closed;
    rig.workspace->close(0, [&closed](bool outcome) { closed = outcome; });
    REQUIRE(rig.pump([&closed] { return closed.has_value(); }));
    Answer answered;
    rig.controller->saveAs(tab, "Gone", std::nullopt,
                           [&answered](std::expected<void, std::string> answer) { answered = std::move(answer); });
    REQUIRE(answered.has_value());
    REQUIRE_FALSE(answered->has_value());
    CHECK(answered->error() == "the screen is closed");
    CHECK(rig.sync->listed().screens.empty());
}

TEST_CASE("covj custom controller: Save as is refused when the server does not store custom screens",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    WorkspaceTabId const tab = rig.open("notes.edit", note(1));
    REQUIRE(rig.preview(tab, ControllerRig::headed("Nowhere")).status == CustomScreenCheckStatus::Done);
    rig.controller->setOffered(false);
    Answer answered;
    rig.controller->saveAs(tab, "Nowhere", std::nullopt,
                           [&answered](std::expected<void, std::string> answer) { answered = std::move(answer); });
    REQUIRE(answered.has_value());
    REQUIRE_FALSE(answered->has_value());
    CHECK(answered->error() == "this server does not store custom screens");
    CHECK(rig.controller->message(tab) == "this server does not store custom screens");
    CHECK_FALSE(rig.controller->saving(tab));
    CHECK(rig.controller->drafted(tab));
}

TEST_CASE("covj custom controller: a second Save as while one runs is refused and the first still lands",
          "[covj][ui][custom-screens][controller]") {
    morph::exec::ThreadPoolExecutor storePool{1};
    auto const store = std::make_shared<custom_client::GatedStore>(storePool);
    ControllerRig rig{store};
    custom_client::GateGuard const guard{*store};
    WorkspaceTabId const tab = rig.open("notes.edit", note(1));
    REQUIRE(rig.preview(tab, ControllerRig::headed("Twice")).status == CustomScreenCheckStatus::Done);
    store->hold();
    Answer first;
    rig.controller->saveAs(tab, "Twice", std::nullopt,
                           [&first](std::expected<void, std::string> answer) { first = std::move(answer); });
    REQUIRE(rig.controller->saving(tab));
    Answer second;
    rig.controller->saveAs(tab, "Twice", std::nullopt,
                           [&second](std::expected<void, std::string> answer) { second = std::move(answer); });
    REQUIRE(second.has_value());
    REQUIRE_FALSE(second->has_value());
    CHECK(second->error() == "another save is still running");
    CHECK(rig.controller->message(tab) == "another save is still running");
    store->release();
    REQUIRE(rig.pump([&rig] { return !rig.outcomes.empty(); }));
    REQUIRE(first.has_value());
    CHECK(first->has_value());
    CHECK(store->puts == 1);
}

TEST_CASE("covj custom controller: a refused Save keeps the draft and says why in the tab's message",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    std::string const id = rig.saveOnServer("Mine", "notes.edit", std::string{kEdit});
    WorkspaceTabId const tab = rig.open(id, note(1));
    std::string const mine =
        screens_test::replaced(ControllerRig::headed("Saved"), "\"notes.edit\"", "\"" + id + "\"");
    REQUIRE(rig.preview(tab, mine).status == CustomScreenCheckStatus::Done);
    REQUIRE(
        rig.server
            .call(morph::ui::kCustomScreensRemove, R"({"app":"notes","id":)" + screens_test::quoted(id) + "}", "ann")
            .kind == "ok");
    rig.controller->save(tab);
    REQUIRE(rig.pump([&rig, tab] { return !rig.controller->saving(tab); }));
    CHECK(rig.controller->message(tab).contains("no such custom screen"));
    CHECK(rig.controller->drafted(tab));
    CHECK(rig.outcomes.empty());
}

TEST_CASE("covj custom controller: a Save refused after its draft was discarded says nothing to the new screen",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    std::string const id = rig.saveOnServer("Mine", "notes.edit", std::string{kEdit});
    WorkspaceTabId const tab = rig.open(id, note(1));
    std::string const mine =
        screens_test::replaced(ControllerRig::headed("Saved"), "\"notes.edit\"", "\"" + id + "\"");
    REQUIRE(rig.preview(tab, mine).status == CustomScreenCheckStatus::Done);
    REQUIRE(
        rig.server
            .call(morph::ui::kCustomScreensRemove, R"({"app":"notes","id":)" + screens_test::quoted(id) + "}", "ann")
            .kind == "ok");
    ServerHold hold{rig};
    rig.controller->save(tab);
    REQUIRE(rig.controller->saving(tab));
    rig.controller->discard(tab);
    hold.release();
    REQUIRE(rig.pump([&rig, tab] { return !rig.controller->saving(tab); }));
    CHECK(rig.controller->message(tab).empty());
    CHECK_FALSE(rig.controller->drafted(tab));
    CHECK(rig.outcomes.empty());
}

namespace {

using Checked = std::optional<morph::ui::CustomScreenCheckResult>;

/// A stored screen of `ann`'s, its text renamed, and a function that asks to check and save it.
struct StoredScreen {
    explicit StoredScreen(ControllerRig& owner)
        : rig{&owner},
          id{owner.saveOnServer("Mine", "notes.edit", std::string{kEdit})},
          text{screens_test::replaced(ControllerRig::headed("Fixed"), "\"notes.edit\"", "\"" + id + "\"")} {}

    void removeOnServer() const {
        REQUIRE(rig->server
                    .call(morph::ui::kCustomScreensRemove, R"({"app":"notes","id":)" + screens_test::quoted(id) + "}",
                          "ann")
                    .kind == "ok");
    }

    ControllerRig* rig;
    std::string id;
    std::string text;
};

}  // namespace

TEST_CASE("covj custom controller: checkAndSave of a screen that is not listed is refused",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    Checked result;
    rig.controller->checkAndSave("custom:nobody", "{}", {},
                                 [&result](morph::ui::CustomScreenCheckResult answer) { result = std::move(answer); });
    REQUIRE(result.has_value());
    CHECK(result->status == CustomScreenCheckStatus::Refused);
    CHECK(result->refusal.reason == "the custom screen is not listed");
}

TEST_CASE("covj custom controller: checkAndSave whose save the server refuses answers the refusal",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    StoredScreen const stored{rig};
    stored.removeOnServer();
    Checked result;
    rig.controller->checkAndSave(stored.id, stored.text, {},
                                 [&result](morph::ui::CustomScreenCheckResult answer) { result = std::move(answer); });
    REQUIRE(rig.pump([&result] { return result.has_value(); }));
    CHECK(result->status == CustomScreenCheckStatus::Refused);
    CHECK(result->refusal.reason.contains("no such custom screen"));
    CHECK(rig.outcomes.empty());
}

TEST_CASE("covj custom controller: a check answered after the asker went is dropped, and saves nothing",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    StoredScreen const stored{rig};
    bool checked = false;
    Checked result;
    {
        ServerHold const hold{rig};
        rig.controller->checkAndSave(
            stored.id, stored.text, [&checked] { checked = true; },
            [&result](morph::ui::CustomScreenCheckResult answer) { result = std::move(answer); });
        rig.controller->dropSourceAnswers();
    }
    REQUIRE(rig.pump([&result] { return result.has_value(); }));
    CHECK(result->status == CustomScreenCheckStatus::Dropped);
    CHECK_FALSE(checked);
    CHECK(rig.outcomes.empty());
}

TEST_CASE("covj custom controller: a refused save whose asker went is told as an outcome",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    StoredScreen const stored{rig};
    stored.removeOnServer();
    bool answered = false;
    rig.controller->checkAndSave(
        stored.id, stored.text, [&rig] { rig.controller->dropSourceAnswers(); },
        [&answered](const morph::ui::CustomScreenCheckResult& /*answer*/) { answered = true; });
    REQUIRE(rig.pump([&rig] { return !rig.outcomes.empty(); }));
    CHECK(rig.outcomes.back().event == morph::ui::CustomScreenEvent::SaveRefused);
    CHECK(rig.outcomes.back().title == "Mine");
    CHECK(rig.outcomes.back().detail.contains("no such custom screen"));
    CHECK_FALSE(answered);
}

TEST_CASE("covj custom controller: a save whose asker went is dropped and still told as Saved",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    StoredScreen const stored{rig};
    Checked result;
    rig.controller->checkAndSave(
        stored.id, stored.text, [&rig] { rig.controller->dropSourceAnswers(); },
        [&result](morph::ui::CustomScreenCheckResult answer) { result = std::move(answer); });
    REQUIRE(rig.pump([&rig, &result] { return result.has_value() && !rig.outcomes.empty(); }));
    CHECK(result->status == CustomScreenCheckStatus::Dropped);
    CHECK(rig.outcomes.back().event == morph::ui::CustomScreenEvent::Saved);
}

TEST_CASE("covj custom controller: opening a custom screen the server lacks answers the refusal, or is told",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    SECTION("asked with an answer") {
        Answer answered;
        rig.controller->openCustom("custom:nobody", {},
                                   [&answered](std::expected<void, std::string> answer) { answered = answer; });
        REQUIRE(rig.pump([&answered] { return answered.has_value(); }));
        REQUIRE_FALSE(answered->has_value());
        CHECK_FALSE(answered->error().empty());
        CHECK(rig.workspace->tabs().empty());
    }
    SECTION("asked with none") {
        rig.controller->openCustom("custom:nobody", {}, {});
        REQUIRE(rig.pump([&rig] { return !rig.outcomes.empty(); }));
        CHECK(rig.outcomes.back().event == morph::ui::CustomScreenEvent::OpenFailed);
        CHECK(rig.outcomes.back().title == "custom:nobody");
        CHECK_FALSE(rig.outcomes.back().detail.empty());
    }
}

TEST_CASE("covj custom controller: Open as a custom screen the server lacks tells it failed and keeps the tab",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    WorkspaceTabId const tab = rig.open("notes.edit", note(1));
    rig.controller->openAs(tab, "custom:nobody");
    REQUIRE(rig.pump([&rig] { return !rig.outcomes.empty(); }));
    CHECK(rig.outcomes.back().event == morph::ui::CustomScreenEvent::OpenFailed);
    CHECK(rig.screenOf(0) == "notes.edit");
}

TEST_CASE("covj custom controller: Discard of a screen whose document went and cannot be fetched says why",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    StoredScreen const stored{rig};
    WorkspaceTabId const tab = rig.open(stored.id, note(1));
    REQUIRE(rig.preview(tab, stored.text).status == CustomScreenCheckStatus::Done);
    stored.removeOnServer();
    rig.sync->documents().forget(stored.id);
    rig.controller->discard(tab);
    REQUIRE(rig.pump([&rig, tab] { return !rig.controller->message(tab).empty(); }));
    CHECK(rig.controller->drafted(tab));
}

namespace {

/// The tab id the workspace issued to a tab that has since closed.
WorkspaceTabId closedTab(ControllerRig& rig) {
    WorkspaceTabId const tab = rig.open("notes.edit", note(1));
    rig.guardChoice = morph::ui::CloseChoice::Discard;
    std::optional<bool> closed;
    rig.workspace->close(0, [&closed](bool outcome) { closed = outcome; });
    REQUIRE(rig.pump([&closed] { return closed.has_value(); }));
    return tab;
}

}  // namespace

TEST_CASE("covj custom controller: options it cannot work without are refused at construction",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    auto options = rig.controllerOptions();
    SECTION("no runtime") { options.runtime = nullptr; }
    SECTION("no host") { options.host = nullptr; }
    SECTION("no sync") { options.sync = nullptr; }
    SECTION("no documents") { options.documents = nullptr; }
    SECTION("no sourceOf") { options.sourceOf = nullptr; }
    SECTION("no catalog") { options.catalog = nullptr; }
    SECTION("no openTab") { options.openTab = nullptr; }
    CHECK_THROWS_AS(morph::ui::CustomScreenController{std::move(options)}, std::invalid_argument);
}

TEST_CASE("covj custom controller: a list change is announced, and a controller with no announcer is as quiet",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    int announced = 0;
    auto options = rig.controllerOptions();
    options.onListChanged = [&announced] { ++announced; };
    options.titleOf = nullptr;
    options.onOutcome = nullptr;
    morph::ui::CustomScreenController plain{std::move(options)};
    plain.setOffered(true);
    CHECK(announced == 1);
    plain.listed(morph::ui::CustomScreensListed{});
    CHECK(announced == 2);
    CHECK(plain.offered());
    CHECK(plain.titleOf("notes.edit") == "notes.edit");
    // An outcome nobody listens for is dropped without a call.
    std::optional<morph::ui::CustomScreenSource> source;
    plain.storedSource("custom:nobody", [&source](morph::ui::CustomScreenSource shown) { source = std::move(shown); });
    CHECK_FALSE(source.has_value());
}

TEST_CASE("covj custom controller: every question about a tab that closed has the empty answer",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    WorkspaceTabId const tab = closedTab(rig);
    CHECK_FALSE(rig.controller->drafted(tab));
    CHECK_FALSE(rig.controller->saving(tab));
    CHECK(rig.controller->message(tab).empty());
    CHECK_FALSE(rig.controller->draftOf(tab).has_value());
    CHECK_FALSE(rig.controller->screenOf(tab).has_value());

    bool called = false;
    rig.controller->source(tab, [&called](const morph::ui::CustomScreenSource& /*shown*/) { called = true; });
    CHECK_FALSE(called);

    Checked previewed;
    rig.controller->preview(tab, std::string{kEdit}, [&previewed](morph::ui::CustomScreenCheckResult answer) {
        previewed = std::move(answer);
    });
    REQUIRE(previewed.has_value());
    CHECK(previewed->status == CustomScreenCheckStatus::Refused);
    CHECK(previewed->refusal.reason == "the screen is closed");

    CHECK_FALSE(rig.controller->proposeDraft(tab, "notes.edit", std::string{kEdit}));
    rig.controller->save(tab);
    rig.controller->discard(tab);
    rig.controller->openAs(tab, "notes.home");
    CHECK(rig.outcomes.empty());
}

TEST_CASE("covj custom controller: a controller over no workspace holds no tab",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    WorkspaceTabId const tab = rig.open("notes.edit", note(1));
    rig.controller->attach(nullptr);
    CHECK_FALSE(rig.controller->drafted(tab));
    CHECK_FALSE(rig.controller->screenOf(tab).has_value());
    rig.controller->openAs(tab, "notes.home");
    rig.controller->remove("custom:nobody", {});
    rig.harness.owner.runAll();
    CHECK(rig.outcomes.empty());
    CHECK(rig.screenOf(0) == "notes.edit");
}

TEST_CASE("covj custom controller: a draft the assistant proposes is shown, or refused with the reason",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    WorkspaceTabId const tab = rig.open("notes.edit", note(1));
    SECTION("another screen's tab") {
        CHECK_FALSE(rig.controller->proposeDraft(tab, "notes.home", std::string{kHome}));
        CHECK_FALSE(rig.controller->drafted(tab));
    }
    SECTION("text that does not load") {
        CHECK_FALSE(rig.controller->proposeDraft(tab, "notes.edit", "{not json"));
        REQUIRE_FALSE(rig.outcomes.empty());
        CHECK(rig.outcomes.back().event == morph::ui::CustomScreenEvent::ProposalNotShown);
        CHECK_FALSE(rig.outcomes.back().detail.empty());
    }
    SECTION("a document under another id") {
        CHECK_FALSE(rig.controller->proposeDraft(
            tab, "notes.edit", screens_test::replaced(std::string{kEdit}, "\"notes.edit\"", "\"notes.other\"")));
        REQUIRE_FALSE(rig.outcomes.empty());
        CHECK(rig.outcomes.back().event == morph::ui::CustomScreenEvent::ProposalNotShown);
        CHECK_FALSE(rig.controller->drafted(tab));
    }
    SECTION("a document of the screen") {
        CHECK(rig.controller->proposeDraft(tab, "notes.edit", ControllerRig::headed("Proposed")));
        CHECK(rig.controller->drafted(tab));
        CHECK(rig.harness.text("heading") == "Proposed");
    }
}

TEST_CASE("covj custom controller: Save does nothing without a draft of a screen of mine",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    WorkspaceTabId const tab = rig.open("notes.edit", note(1));
    REQUIRE(rig.preview(tab, ControllerRig::headed("Draft")).status == CustomScreenCheckStatus::Done);
    // An application screen's draft is saved as a new screen, never over the application's.
    rig.controller->save(tab);
    CHECK_FALSE(rig.controller->saving(tab));
    CHECK(rig.controller->drafted(tab));
    rig.controller->discard(tab);
    REQUIRE(rig.pump([&rig, tab] { return !rig.controller->drafted(tab); }));
    // No draft: nothing to save or discard.
    rig.controller->save(tab);
    rig.controller->discard(tab);
    CHECK_FALSE(rig.controller->saving(tab));
    CHECK(rig.sync->listed().screens.empty());
}

TEST_CASE("covj custom controller: Open as the screen already shown does nothing",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    WorkspaceTabId const tab = rig.open("notes.edit", note(1));
    rig.controller->openAs(tab, "notes.edit");
    rig.harness.owner.runAll();
    CHECK(rig.screenOf(0) == "notes.edit");
    CHECK(rig.outcomes.empty());
}

TEST_CASE("covj custom controller: removing a screen opens the base only in the tabs that showed it",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    StoredScreen const stored{rig};
    rig.open("notes.home", ParamMap{});
    rig.open(stored.id, note(2));
    Answer removed;
    rig.controller->remove(stored.id, [&removed](std::expected<void, std::string> answer) { removed = answer; });
    REQUIRE(rig.pump([&rig] { return rig.screenOf(1) == "notes.edit"; }));
    CHECK(rig.screenOf(0) == "notes.home");
    REQUIRE(removed.has_value());
    CHECK(removed->has_value());
}

TEST_CASE("covj custom controller: removing a screen that is not listed names it by its id",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    Answer removed;
    rig.controller->remove("custom:nobody", [&removed](std::expected<void, std::string> answer) { removed = answer; });
    REQUIRE(rig.pump([&removed] { return removed.has_value(); }));
    CHECK_FALSE(removed->has_value());
    CHECK(rig.outcomes.empty());
}

TEST_CASE("covj custom controller: the stored source of a screen the server lacks is told as unreadable",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    std::optional<morph::ui::CustomScreenSource> source;
    rig.controller->storedSource("custom:nobody",
                                 [&source](morph::ui::CustomScreenSource shown) { source = std::move(shown); });
    REQUIRE(rig.pump([&rig] { return !rig.outcomes.empty(); }));
    CHECK(rig.outcomes.back().event == morph::ui::CustomScreenEvent::StoredSourceUnreadable);
    CHECK(rig.outcomes.back().title == "custom:nobody");
    CHECK_FALSE(source.has_value());
}

TEST_CASE("covj custom controller: a stored screen that does not load has no changes to take over, and no update",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    std::string const id = custom_client::customId('2');
    morph::ui::CustomScreenKey key{.person = "ann", .app = "notes", .id = id};
    auto completion = rig.server.store->put(
        std::move(key),
        morph::ui::CustomScreen{.id = id,
                                .title = "Old",
                                .basedOn = "notes.edit",
                                .document = R"({"vocab":"ui/1","id":")" + id + R"(","view":{"kind":"hologram"}})",
                                .savedAt = {},
                                .savedBy = "ann"});
    REQUIRE(rig.pump([&rig, &id] {
        return rig.server
                   .call(morph::ui::kCustomScreensGet, R"({"app":"notes","id":)" + screens_test::quoted(id) + "}",
                         "ann")
                   .kind == "ok";
    }));
    rig.sync->relist();
    REQUIRE(rig.pump([&rig, &id] { return rig.controller->summaryOf(id).has_value(); }));

    std::optional<std::expected<morph::ui::LayoutChanges, std::string>> changes;
    rig.controller->layoutChangesOf(id, [&changes](auto answer) { changes = std::move(answer); });
    REQUIRE(rig.pump([&changes] { return changes.has_value(); }));
    CHECK_FALSE(changes->has_value());

    Checked applied;
    rig.controller->applyLayoutChoices(
        id, morph::ui::LayoutChoices{},
        [&applied](morph::ui::CustomScreenCheckResult answer) { applied = std::move(answer); });
    REQUIRE(rig.pump([&applied] { return applied.has_value(); }));
    CHECK(applied->status == CustomScreenCheckStatus::Refused);
    CHECK(rig.outcomes.empty());
}

TEST_CASE("covj custom controller: an update of a screen the server no longer has is refused with the server's words",
          "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    StoredScreen const stored{rig};
    stored.removeOnServer();
    Checked applied;
    rig.controller->applyLayoutChoices(
        stored.id, morph::ui::LayoutChoices{},
        [&applied](morph::ui::CustomScreenCheckResult answer) { applied = std::move(answer); });
    REQUIRE(rig.pump([&applied] { return applied.has_value(); }));
    CHECK(applied->status == CustomScreenCheckStatus::Refused);
    CHECK(applied->refusal.reason.contains("no such custom screen"));
}

TEST_CASE("covj custom controller: a save that answers after a newer draft replaced its own is told, not acted on",
          "[covj][ui][custom-screens][controller]") {
    morph::exec::ThreadPoolExecutor storePool{1};
    auto const store = std::make_shared<custom_client::GatedStore>(storePool);
    ControllerRig rig{store};
    custom_client::GateGuard const guard{*store};
    WorkspaceTabId const tab = rig.open("notes.edit", note(1));
    REQUIRE(rig.preview(tab, ControllerRig::headed("First")).status == CustomScreenCheckStatus::Done);
    store->hold();
    rig.controller->saveAs(tab, "First", std::nullopt, {});
    REQUIRE(rig.controller->saving(tab));
    REQUIRE(rig.controller->proposeDraft(tab, "notes.edit", ControllerRig::headed("Second")));
    store->release();
    REQUIRE(rig.pump([&rig] { return !rig.outcomes.empty(); }));
    CHECK(rig.outcomes.back().event == morph::ui::CustomScreenEvent::SavedDraftReplaced);
    CHECK(rig.outcomes.back().title == "First");
    CHECK(rig.screenOf(0) == "notes.edit");
    CHECK(rig.controller->drafted(tab));
    CHECK(rig.harness.text("heading") == "Second");
    CHECK_FALSE(rig.controller->saving(tab));
}

TEST_CASE(
    "covj custom controller: Open as and the navigator list nothing for a base they cannot read, or when not offered",
    "[covj][ui][custom-screens][controller]") {
    ControllerRig rig;
    StoredScreen const stored{rig};
    SECTION("a screen whose base is not an application screen") {
        CHECK(rig.controller->openAsChoices("custom:nobody").empty());
        CHECK(rig.controller->openAsChoices("nowhere").empty());
    }
    SECTION("a base that opens with no params is the navigator's, not Open as's") {
        CHECK(rig.controller->openAsChoices("notes.home").empty());
    }
    SECTION("a base with params lists itself and the screens saved on it") {
        std::vector<morph::ui::CustomScreenChoice> const choices = rig.controller->openAsChoices(stored.id);
        REQUIRE(choices.size() == 2);
        CHECK(choices.front().id == "notes.edit");
        CHECK_FALSE(choices.front().current);
        CHECK(choices.back().id == stored.id);
        CHECK(choices.back().current);
    }
    SECTION("nothing is offered when the server stores no custom screens") {
        rig.controller->setOffered(false);
        CHECK(rig.controller->openAsChoices(stored.id).empty());
        CHECK(rig.controller->navigatorEntries().empty());
    }
}
