// SPDX-License-Identifier: Apache-2.0
//
// Fixtures of the custom screens client's tests: a server without the model, a store that refuses every save, a
// client of a server's custom screens over a simulated connection, and helpers over the model's replies.

#pragma once
#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/owner_strand.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/custom_screen_chrome.hpp>
#include <morph/ui/custom_screen_sync.hpp>
#include <morph/ui/custom_screens.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/json_source.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/workspace.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_support.hpp"
#include "ui_custom_screens_support.hpp"
#include "ui_interpreter_support.hpp"

namespace custom_client {

/// A RemoteServer that never registered `morph.customScreens`.
struct BareServer {
    // The dispatcher and the registry come before the pool: the pool joins first, so a call still running when the
    // fixture goes holds a dispatcher that is still alive.
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    morph::exec::ThreadPoolExecutor pool{2};
    std::shared_ptr<uiservice::RuleAuthorizer> authorizer = std::make_shared<uiservice::RuleAuthorizer>();
    std::shared_ptr<morph::backend::RemoteServer> server =
        std::make_shared<morph::backend::RemoteServer>(pool, authorizer, dispatcher, registry);
};

/// A store that keeps what a memory store keeps but refuses every save, with a reason for the person.
struct RefusingStore final : morph::ui::ICustomScreenStore {
    explicit RefusingStore(morph::exec::IExecutor& base) : inner{base}, strand{base} {}
    morph::async::Completion<std::vector<morph::ui::CustomScreen>> list(std::string person, std::string app) override {
        return inner.list(std::move(person), std::move(app));
    }
    morph::async::Completion<std::expected<morph::ui::CustomScreen, morph::ui::CustomScreenRefusal>> get(
        morph::ui::CustomScreenKey key) override {
        return inner.get(std::move(key));
    }
    morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>> put(
        morph::ui::CustomScreenKey /*key*/, morph::ui::CustomScreen /*screen*/) override {
        auto [completion, promise] =
            morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>>::makeSettleable(&strand);
        promise.resolve(
            std::unexpected(morph::ui::CustomScreenRefusal{.reason = "over your limit of custom screens"}));
        return std::move(completion);
    }
    morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>> remove(
        morph::ui::CustomScreenKey key) override {
        return inner.remove(std::move(key));
    }
    morph::ui::MemoryCustomScreenStore inner;
    /// The serial owner the refusal is delivered on, declared last so it is destroyed first.
    morph::exec::OwnerStrand strand;
};

/// A store that keeps what a memory store keeps, and holds every save at the door while it is closed, so a test can
/// act while a save is in flight.
struct GatedStore final : morph::ui::ICustomScreenStore {
    explicit GatedStore(morph::exec::IExecutor& base) : inner{base} {}
    ~GatedStore() override { release(); }
    GatedStore(GatedStore const&) = delete;
    GatedStore& operator=(GatedStore const&) = delete;
    GatedStore(GatedStore&&) = delete;
    GatedStore& operator=(GatedStore&&) = delete;

    morph::async::Completion<std::vector<morph::ui::CustomScreen>> list(std::string person, std::string app) override {
        return inner.list(std::move(person), std::move(app));
    }
    morph::async::Completion<std::expected<morph::ui::CustomScreen, morph::ui::CustomScreenRefusal>> get(
        morph::ui::CustomScreenKey key) override {
        return inner.get(std::move(key));
    }
    morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>> put(
        morph::ui::CustomScreenKey key, morph::ui::CustomScreen screen) override {
        ++puts;
        {
            std::unique_lock lock{mutex};
            opened.wait(lock, [this] { return open; });
        }
        return inner.put(std::move(key), std::move(screen));
    }
    morph::async::Completion<std::expected<void, morph::ui::CustomScreenRefusal>> remove(
        morph::ui::CustomScreenKey key) override {
        return inner.remove(std::move(key));
    }

    /// Holds the saves that arrive from now on.
    void hold() {
        std::lock_guard const lock{mutex};
        open = false;
    }
    /// Lets the held saves, and the later ones, through.
    void release() {
        {
            std::lock_guard const lock{mutex};
            open = true;
        }
        opened.notify_all();
    }

    /// The saves that reached the store, held or not.
    std::atomic<int> puts{0};
    std::mutex mutex;
    std::condition_variable opened;
    bool open = true;
    morph::ui::MemoryCustomScreenStore inner;
};

/// Opens a gate when the test's scope ends, so a save held at it does not keep the server from stopping.
struct GateGuard {
    explicit GateGuard(GatedStore& held) : gate{held} {}
    ~GateGuard() { gate.release(); }
    GateGuard(GateGuard const&) = delete;
    GateGuard& operator=(GateGuard const&) = delete;
    GateGuard(GateGuard&&) = delete;
    GateGuard& operator=(GateGuard&&) = delete;
    GatedStore& gate;
};

/// The id a `save` reply carries.
inline std::string savedIdOf(morph::wire::Envelope const& reply) {
    REQUIRE(reply.kind == "ok");
    morph::ui::CustomScreenSaved out;
    REQUIRE_FALSE(glz::read_json(out, reply.body));
    return out.id;
}

/// A `save` body: a screen of @p app based on @p basedOn, overwriting @p id when it is given.
inline std::string saveBody(std::string const& app, std::string const& title, std::string const& basedOn,
                            std::string const& document, std::string const& id = {}) {
    return R"({"app":)" + screens_test::quoted(app) +
           (id.empty() ? std::string{} : R"(,"id":)" + screens_test::quoted(id)) + R"(,"title":)" +
           screens_test::quoted(title) + R"(,"basedOn":)" + screens_test::quoted(basedOn) + R"(,"document":)" +
           screens_test::quoted(document) + "}";
}

/// One client of @p server, signed in as @p person, for @p app: a bridge over a simulated connection, the dispatcher
/// over it, and a sync. With @p recordWatches, the session's watches are recorded and answered with nothing refused.
struct SyncClient {
    SyncClient(morph::backend::RemoteServer& server, std::string person, std::string app, bool recordWatches = false)
        : session{runtime, morph::ui::SessionOptions{.watcher = recordWatches ? watcher() : morph::ui::Watcher{}}},
          bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(server), owner},
          sync{dispatcher, std::move(app), [] { return uiservice::facts(); }, &session} {
        morph::session::Context signedIn;
        signedIn.token = std::move(person);
        bridge.setDefaultSession(std::move(signedIn));
    }

    /// Runs the owner until @p done holds, for at most @p budget.
    template <typename Pred>
    bool pump(Pred done, std::chrono::milliseconds budget = morph::testing::kDefaultWaitBudget) {
        return morph::testing::waitUntil(
            [&] {
                owner.runAll();
                return done();
            },
            morph::testing::WaitBudget{budget});
    }

    /// Probes, and answers whether the server offers custom screens.
    bool probe() {
        std::optional<bool> offered;
        sync.probe([&offered](bool answer) { offered = answer; });
        REQUIRE(pump([&offered] { return offered.has_value(); }));
        return *offered;
    }

    morph::ui::Watcher watcher() {
        return [this](std::string const& kind, std::string const& body) {
            watches.push_back(kind + " " + body);
            auto [completion, promise] = morph::async::Completion<std::string>::makeSettleable(&owner);
            promise.resolve(refuseWatches
                                ? std::string{R"({"refusedRecords":["morph.customScreens"],"refusedInstances":[]})"}
                                : std::string{R"({"refusedRecords":[],"refusedInstances":[]})"});
            return std::move(completion);
        };
    }

    /// Whether the recorded watches are answered as refused for the record `morph.customScreens`.
    bool refuseWatches = false;
    std::vector<std::string> watches;
    morph::testing::StepExecutor owner;
    morph::reactive::Runtime runtime{owner};
    morph::ui::Session session;
    morph::bridge::Bridge bridge;
    morph::ui::BridgeDispatcher dispatcher{bridge, &owner};
    morph::ui::CustomScreenSync sync;
};

/// A screen with a param, so Open as applies to it; its body is state a button changes.
inline constexpr std::string_view kEdit =
    R"({"vocab":"ui/1","id":"notes.edit","params":{"noteId":"int"},"identity":{"ref":"noteId"},"title":"Note",)"
    R"("state":{"content":{"type":"string","init":"first"}},)"
    R"("view":{"kind":"column","children":[{"kind":"text","testId":"heading","text":"Edit note"},)"
    R"({"kind":"text","testId":"body","text":{"ref":"content"}},)"
    R"({"kind":"button","testId":"change","label":"Change","onClick":[{"set":"content","value":"changed"}]}]}})";

/// A screen without params, so its custom screens are navigator entries.
inline constexpr std::string_view kHome =
    R"({"vocab":"ui/1","id":"notes.home","title":"Home","view":{"kind":"text","testId":"home","text":"Home"}})";

/// The notes application's bundle: the two screens, against the service fixture's facts.
inline std::shared_ptr<morph::ui::ServedBundle const> notesBundle() {
    morph::ui::UiRegistry registry{morph::ui::ApplicationInfo{.id = "notes", .version = "1.0.0"}, uiservice::facts()};
    for (auto const& [id, text] :
         {std::pair{std::string_view{"notes.edit"}, kEdit}, std::pair{std::string_view{"notes.home"}, kHome}}) {
        auto const added = registry.addScreen(uiservice::entry(std::string{id}), std::string{text});
        if (!added) {
            throw std::runtime_error{added.error().message()};
        }
    }
    auto built = registry.build();
    if (!built) {
        throw std::runtime_error{built.error().message()};
    }
    return *built;
}

/// @p text loaded as the client loads the notes documents.
inline std::shared_ptr<morph::ui::Document const> loadedNotes(std::string_view text) {
    auto loaded = morph::ui::loadDocumentText(text, morph::ui::LoadOptions{.catalog = uiservice::facts()});
    if (!loaded) {
        FAIL("fixture does not load: " << loaded.error().message());
    }
    return std::make_shared<morph::ui::Document const>(std::move(*loaded));
}

/// A text input's text as RecordingBackend records it, with its escapes undone.
inline std::string unformatted(std::string_view recorded) {
    std::string out;
    for (std::size_t at = 0; at < recorded.size(); ++at) {
        if (recorded[at] == '\\' && at + 1 < recorded.size()) {
            char const next = recorded[++at];
            out += next == 'n' ? '\n' : next == 'r' ? '\r' : next;
            continue;
        }
        out += recorded[at];
    }
    return out;
}

inline morph::ui::ParamMap note(std::int64_t id) { return morph::ui::ParamMap{{"noteId", morph::ui::Value{id}}}; }

/// A host whose navigation goes nowhere: the chrome opens tabs through its own `openTab`.
class NotesHost final : public morph::ui::DocumentScreenHost {
public:
    NotesHost(morph::ui::testing::Harness& harness, morph::ui::DocumentScreenHostOptions options)
        : DocumentScreenHost{harness.session, harness.dispatcher, std::move(options)} {}

    void onNavigate(std::string_view /*screenId*/, morph::ui::ParamMap const& /*params*/) override {}

    using DocumentScreenHost::onNavigate;
};

/// Whether the rig's server offers custom screens.
enum class Offer : std::uint8_t { Store, NoStore };

/// One window of the notes application signed in as `ann`: a workspace over the notes screens, the chrome over it,
/// and a sync to a notes server that offers custom screens (over @p store when one is given) or does not.
struct ChromeRig {
    explicit ChromeRig(Offer offer = Offer::Store, std::shared_ptr<morph::ui::ICustomScreenStore> store = nullptr) {
        if (offer == Offer::Store) {
            server.emplace(std::move(store), morph::ui::CustomScreensOptions{}, notesBundle());
            remote = server->server.get();
        } else {
            bare.emplace();
            remote = bare->server.get();
        }
        bridge = std::make_unique<morph::bridge::Bridge>(
            std::make_unique<morph::backend::SimulatedRemoteBackend>(*remote), harness.owner);
        morph::session::Context signedIn;
        signedIn.token = "ann";
        bridge->setDefaultSession(std::move(signedIn));
        models = std::make_unique<morph::ui::BridgeDispatcher>(*bridge, &harness.owner);
        sync = std::make_unique<morph::ui::CustomScreenSync>(*models, "notes", [] { return uiservice::facts(); });
        host = std::make_unique<NotesHost>(harness, hostOptions());
        workspace =
            std::make_unique<morph::ui::Workspace>(harness.runtime, harness.backend, *host, workspaceOptions());
        chrome = std::make_unique<morph::ui::CustomScreenChrome>(chromeOptions());
        chrome->attach(&workspace->model());
        sync->setOnListed([this](morph::ui::CustomScreensListed const& listed,
                                 std::vector<std::string> const& /*dropped*/) { chrome->listed(listed); });
        std::optional<bool> offered;
        sync->probe([this, &offered](bool answer) {
            chrome->setOffered(answer);
            offered = answer;
        });
        REQUIRE(pump([&offered] { return offered.has_value(); }));
    }

    /// Runs the owner until @p done holds, for at most @p budget.
    template <typename Pred>
    bool pump(Pred done, std::chrono::milliseconds budget = morph::testing::kDefaultWaitBudget) {
        return morph::testing::waitUntil(
            [&] {
                harness.owner.runAll();
                return done();
            },
            morph::testing::WaitBudget{budget});
    }

    [[nodiscard]] std::optional<int> find(std::string_view kind, std::string const& testId) const {
        return harness.backend.find(kind, "testId", testId);
    }

    /// Whether the widget is mounted and not hidden itself.
    [[nodiscard]] bool shown(std::string_view kind, std::string const& testId) const {
        std::optional<int> const widget = find(kind, testId);
        return widget && harness.backend.prop(*widget, "visible") != "false";
    }

    void click(std::string const& testId) {
        std::optional<int> const widget = find("Button", testId);
        REQUIRE(widget.has_value());
        harness.backend.click(*widget);
        harness.settle();
    }

    void type(std::string const& testId, std::string text) {
        std::optional<int> const widget = find("TextInput", testId);
        REQUIRE(widget.has_value());
        harness.backend.edit(*widget, std::move(text));
        harness.settle();
    }

    [[nodiscard]] std::string text(std::string const& testId) const { return harness.text(testId); }

    /// A text input's text.
    [[nodiscard]] std::string field(std::string const& testId) const {
        std::optional<int> const widget = find("TextInput", testId);
        REQUIRE(widget.has_value());
        return unformatted(harness.backend.prop(*widget, "text"));
    }

    /// Whether the widget is mounted and enabled.
    [[nodiscard]] bool enabled(std::string_view kind, std::string const& testId) const {
        std::optional<int> const widget = find(kind, testId);
        return widget && harness.backend.prop(*widget, "enabled") != "false";
    }

    /// The id of the tab at @p index.
    [[nodiscard]] morph::ui::WorkspaceTabId tabAt(std::size_t index) const {
        return workspace->model().order().at(index);
    }

    void open(std::string_view screenId, morph::ui::ParamMap const& params) {
        REQUIRE(workspace->open(screenId, params).has_value());
        harness.settle();
    }

    [[nodiscard]] std::string screenOf(std::size_t index) const { return workspace->tabs().at(index).screenId; }

    /// Saves a custom screen of `ann`'s on the server, and waits for the sync's list to show it.
    std::string saveOnServer(std::string const& title, std::string const& basedOn, std::string const& document) {
        REQUIRE(server.has_value());
        std::string const id =
            savedIdOf(server->call(morph::ui::kCustomScreensSave, saveBody("notes", title, basedOn, document), "ann"));
        sync->relist();
        REQUIRE(pump([this, &id] {
            return std::ranges::any_of(sync->listed().screens, [&id](morph::ui::CustomScreenSummary const& screen) {
                return screen.id == id;
            });
        }));
        return id;
    }

    morph::ui::DocumentScreenHostOptions hostOptions() {
        morph::ui::DocumentScreenHostOptions made;
        made.documents = [this](std::string_view id) -> std::shared_ptr<morph::ui::Document const> {
            if (morph::ui::isCustomScreenId(id)) {
                return sync->documents().find(id);
            }
            return id == "notes.edit" ? edit : id == "notes.home" ? home : nullptr;
        };
        made.backend = &harness.backend;
        made.application = &harness.host;
        made.guard = [this](morph::ui::ScreenHandle& /*screen*/,
                            std::function<void(morph::ui::CloseChoice)> const& choose) {
            ++guarded;
            choose(guardChoice);
        };
        return made;
    }

    morph::ui::WorkspaceOptions workspaceOptions() {
        morph::ui::WorkspaceOptions made;
        made.chrome = [this](morph::ui::WorkspaceTabId tab, std::string_view screenId) {
            return chrome->chromeFor(tab, screenId);
        };
        return made;
    }

    morph::ui::CustomScreenChromeOptions chromeOptions() {
        morph::ui::CustomScreenChromeOptions made;
        made.runtime = &harness.runtime;
        made.backend = &harness.backend;
        made.host = host.get();
        made.sync = sync.get();
        made.documents = [this](std::string_view id) -> std::shared_ptr<morph::ui::Document const> {
            return id == "notes.edit" ? edit : id == "notes.home" ? home : nullptr;
        };
        made.sourceOf = [](std::string_view id) {
            return id == "notes.edit" ? std::string{kEdit} : id == "notes.home" ? std::string{kHome} : std::string{};
        };
        made.catalog = [] { return uiservice::facts(); };
        made.titleOf = [](std::string_view id) { return id == "notes.edit" ? std::string{"Note"} : std::string{id}; };
        made.openTab = [this](std::string const& id, morph::ui::ParamMap const& params) {
            static_cast<void>(workspace->openTab(id, params));
        };
        made.notify = [this](std::string const& text, morph::ui::Tone /*tone*/) { notices.push_back(text); };
        return made;
    }

    morph::ui::testing::Harness harness;
    std::optional<screens_test::Server> server;
    std::optional<BareServer> bare;
    morph::backend::RemoteServer* remote = nullptr;
    std::unique_ptr<morph::bridge::Bridge> bridge;
    std::unique_ptr<morph::ui::BridgeDispatcher> models;
    std::unique_ptr<morph::ui::CustomScreenSync> sync;
    std::shared_ptr<morph::ui::Document const> edit = loadedNotes(kEdit);
    std::shared_ptr<morph::ui::Document const> home = loadedNotes(kHome);
    std::unique_ptr<NotesHost> host;
    std::unique_ptr<morph::ui::Workspace> workspace;
    std::unique_ptr<morph::ui::CustomScreenChrome> chrome;
    std::vector<std::string> notices;
    int guarded = 0;
    morph::ui::CloseChoice guardChoice = morph::ui::CloseChoice::Cancel;
};

/// Opens @p screenId's source, renames the heading @p from to @p heading and previews it, until the draft shows and
/// the source view has closed.
inline void draftHeading(ChromeRig& rig, std::string const& screenId, std::string const& heading,
                         std::string const& from = "Edit note") {
    rig.click((rig.shown("Row", "draft_bar_" + screenId) ? "draft_source_" : "source_button_") + screenId);
    rig.type("source_text",
             screens_test::replaced(rig.field("source_text"), "\"" + from + "\"", "\"" + heading + "\""));
    rig.click("source_preview");
    REQUIRE(rig.pump([&rig, &heading] { return rig.text("heading") == heading; }));
    REQUIRE(rig.pump([&rig] { return !rig.find("Dialog", "source_dialog").has_value(); }));
}

}  // namespace custom_client
