// SPDX-License-Identifier: Apache-2.0
//
// The generic client's custom screens (docs/spec/client/client.md, "Custom screens"), in process: the shell probes
// the model, lists the person's custom screens in the navigator, opens one after fetching it, previews a hand edit as
// a draft and saves it over the screen, and asks before a tab with a draft closes.

// First, ahead of every header that brings view.hpp: GCC's -Wshadow reports value.hpp's enumerator `Type::Key` when
// view.hpp's `ui::Key` was declared before it.
// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <expected>
#include <future>
#include <memory>
#include <morph/client/shell.hpp>
#include <morph/client/source.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/custom_screens.hpp>
#include <morph/ui/json_source.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/service_client.hpp>
#include <morph/ui/testing/memory_history.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/fake_app_context.hpp>
#include <testkit/wait.hpp>
#include <utility>
#include <vector>

namespace client = morph::client;
namespace ui = morph::ui;
using morph::examples::testing::FakeAppContext;
using morph::examples::testing::pumpUntil;

namespace {

constexpr std::string_view kHome =
    R"({"vocab":"ui/1","id":"customs.home","title":"Home","view":{"kind":"text","testId":"home","text":"Home"}})";

std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> customsBundle() {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "customs", .version = "1"}};
    ui::CatalogEntry entry;
    entry.id = "customs.home";
    entry.title = "Home";
    if (auto added = served.addScreen(std::move(entry), std::string{kHome}); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

/// `text` with its first `from` replaced by `to`.
std::string replaced(std::string text, std::string_view from, std::string_view to) {
    auto const at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

/// The custom screens model on the process's dispatcher and registry, where an in-process source's models run, over
/// a memory store holding `ann`'s screen "Mine", based on the home screen. ctest runs each case in a process of its
/// own (the cases are discovered PRE_TEST), so the registration reaches no other case.
struct Offered {
    Offered() {
        auto built = customsBundle();
        REQUIRE(built.has_value());
        bundle = *built;
        screens = ui::registerCustomScreens(
            store, [held = bundle] { return held; }, morph::session::allowAllAuthorizer(),
            morph::model::detail::defaultDispatcher(), morph::model::detail::defaultRegistry());
        std::promise<void> stored;
        store
            ->put(ui::CustomScreenKey{.person = "ann", .app = "customs", .id = mineId},
                  ui::CustomScreen{.id = mineId,
                                   .title = "Mine",
                                   .basedOn = "customs.home",
                                   .document = replaced(std::string{kHome}, "\"customs.home\"", "\"" + mineId + "\""),
                                   .savedAt = {},
                                   .savedBy = "ann"})
            .thenDetached(
                [&stored](std::expected<void, ui::CustomScreenRefusal> const& /*result*/) { stored.set_value(); });
        REQUIRE(stored.get_future().wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    }

    std::string mineId = "custom:" + std::string(31, '0') + "a";
    // Inline: the seeding below settles at once on this thread, where handlers may attach.
    std::shared_ptr<ui::MemoryCustomScreenStore> store =
        std::make_shared<ui::MemoryCustomScreenStore>(morph::exec::detail::inlineExecutor());
    std::shared_ptr<ui::ServedBundle const> bundle;
    std::shared_ptr<ui::CustomScreens> screens;
};

/// The generic client over the customs application, signed in locally as `ann`, until the navigator lists "Mine".
struct Window {
    explicit Window(Offered const& offered, std::string start = "customs.home",
                    ui::testing::MemoryHistory* history = nullptr)
        : mineId{offered.mineId} {
        std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
        source.open(context,
                    [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
        REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
        REQUIRE(opened->has_value());
        shell = client::mountShell(context, backend, std::move(*opened), source,
                                   client::ShellOptions{.startScreen = std::move(start), .history = history});
        REQUIRE(wait([this] { return find("Button", "nav_" + mineId).has_value(); }));
    }
    ~Window() {
        shell.reset();
        source.close();
    }
    Window(Window const&) = delete;
    Window& operator=(Window const&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    [[nodiscard]] std::optional<int> find(std::string_view kind, std::string const& testId) const {
        return backend.find(kind, "testId", testId);
    }
    [[nodiscard]] bool shown(std::string_view kind, std::string const& testId) const {
        std::optional<int> const widget = find(kind, testId);
        return widget && backend.prop(*widget, "visible") != "false";
    }
    template <typename Pred>
    bool wait(Pred pred) {
        return pumpUntil(context.owner(), pred);
    }
    void click(std::string const& testId) {
        REQUIRE(wait([this, &testId] { return find("Button", testId).has_value(); }));
        backend.click(*find("Button", testId));
    }

    /// Opens "Mine" from the navigator, edits its text in the source view, and previews it, until the draft shows.
    void draftMine() {
        click("nav_" + mineId);
        click("source_button_" + mineId);
        REQUIRE(wait([this] { return find("Button", "source_preview").has_value(); }));
        std::optional<int> const field = find("TextInput", "source_text");
        REQUIRE(field.has_value());
        std::string const pretty =
            ui::prettyJson(replaced(std::string{kHome}, "\"customs.home\"", "\"" + mineId + "\""));
        backend.edit(*field, replaced(pretty, R"("text": "Home")", R"("text": "My home")"));
        click("source_preview");
        REQUIRE(wait([this] { return shown("Row", "draft_bar_" + mineId); }));
    }

    std::string mineId;
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    client::LocalSource source{client::LocalApp{.name = "customs", .bundle = &customsBundle},
                               client::LocalSetup{.db = {}, .user = "ann"}};
    std::unique_ptr<ui::MountedShell> shell;
};

}  // namespace

// Mutations: mount a navigator's custom entry without fetching it (the mount finds no document: no source button for
// it shows); drop the custom branch of the host's documents lookup (the same).
TEST_CASE("Client shell: the navigator opens a custom screen, Source previews a draft, and Save overwrites it",
          "[client][custom-screens]") {
    Offered const offered;
    Window window{offered};
    window.draftMine();
    REQUIRE(window.wait([&window] {
        std::optional<int> const text = window.backend.find("Text", "text", "My home");
        return text.has_value();
    }));
    window.click("draft_save_" + window.mineId);
    REQUIRE(window.wait([&window] { return !window.shown("Row", "draft_bar_" + window.mineId); }));
    // What the store holds is the draft's text.
    std::optional<std::expected<ui::CustomScreen, ui::CustomScreenRefusal>> stored;
    offered.store->get(ui::CustomScreenKey{.person = "ann", .app = "customs", .id = offered.mineId})
        .then([&stored](std::expected<ui::CustomScreen, ui::CustomScreenRefusal> const& found) { stored = found; });
    REQUIRE(stored.has_value());
    REQUIRE(stored->has_value());
    CHECK((*stored)->document.contains("My home"));
}

// Mutation: answer every close of a drafted screen with Cancel in the client's guard (no confirmation is asked).
TEST_CASE("Client shell: closing a tab with a draft asks first, and Discard closes it", "[client][custom-screens]") {
    Offered const offered;
    Window window{offered};
    window.draftMine();
    auto const tabs = window.backend.all("Tabs");
    REQUIRE_FALSE(tabs.empty());
    CHECK(window.backend.press(tabs.front(), "Ctrl+W"));
    REQUIRE(window.wait([&window] { return window.find("Dialog", "shell_confirm").has_value(); }));
    window.click("shell_confirm_yes");
    REQUIRE(window.wait([&window] { return !window.find("Button", "draft_save_" + window.mineId).has_value(); }));
}

namespace {

/// Runs the owner a few rounds, so work a click queued has been carried out.
void settleRounds(Window& window) {
    for (int round = 0; round < 3; ++round) {
        bool ran = false;
        window.context.owner().post([&ran] { ran = true; });
        REQUIRE(window.wait([&ran] { return ran; }));
    }
}

}  // namespace

// Mutation: answer Keep with Discard in the client's guard (the draft bar goes with the tab).
TEST_CASE("Client shell: Keep on the close guard leaves the draft in place", "[client][custom-screens]") {
    Offered const offered;
    Window window{offered};
    window.draftMine();
    auto const tabs = window.backend.all("Tabs");
    REQUIRE_FALSE(tabs.empty());
    CHECK(window.backend.press(tabs.front(), "Ctrl+W"));
    REQUIRE(window.wait([&window] { return window.find("Dialog", "shell_confirm").has_value(); }));
    window.click("shell_confirm_no");
    settleRounds(window);
    CHECK(window.shown("Row", "draft_bar_" + window.mineId));
    CHECK(window.backend.find("Text", "text", "My home").has_value());
}

// Mutation: ask the guard for every dirty screen, not only drafted ones (the dialog shows).
TEST_CASE("Client shell: a dirty screen that is not a draft is not put to the draft guard",
          "[client][custom-screens]") {
    Offered const offered;
    std::string const dirtyId = "custom:" + std::string(31, '0') + "d";
    std::promise<void> stored;
    offered.store
        ->put(ui::CustomScreenKey{.person = "ann", .app = "customs", .id = dirtyId},
              ui::CustomScreen{.id = dirtyId,
                               .title = "Dirty",
                               .basedOn = "customs.home",
                               .document = replaced(std::string{kHome}, "\"customs.home\"",
                                                    "\"" + dirtyId + R"(","dirty":{"lit":true})"),
                               .savedAt = {},
                               .savedBy = "ann"})
        .thenDetached(
            [&stored](std::expected<void, ui::CustomScreenRefusal> const& /*result*/) { stored.set_value(); });
    REQUIRE(stored.get_future().wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    Window window{offered};
    REQUIRE(window.wait([&window, &dirtyId] { return window.find("Button", "nav_" + dirtyId).has_value(); }));
    window.click("nav_" + dirtyId);
    REQUIRE(
        window.wait([&window, &dirtyId] { return window.find("Button", "source_button_" + dirtyId).has_value(); }));
    auto const tabs = window.backend.all("Tabs");
    REQUIRE_FALSE(tabs.empty());
    CHECK(window.backend.press(tabs.front(), "Ctrl+W"));
    settleRounds(window);
    CHECK_FALSE(window.find("Dialog", "shell_confirm").has_value());
    CHECK(window.find("Button", "source_button_" + dirtyId).has_value());
}

namespace {

/// Whether a Text of the backend shows something containing @p part.
bool showsText(ui::testing::RecordingBackend const& backend, std::string_view part) {
    for (std::string_view const kind : {"Text", "Toast"}) {
        for (int const widget : backend.all(kind)) {
            if (backend.prop(widget, "text").contains(part)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

// Mutation: leave `custom:` routes out of the history's restore (the start screen opens, and no home text of "Mine"
// shows under its id).
TEST_CASE("Client shell: a recorded route to a custom screen is fetched and opened", "[client][custom-screens]") {
    Offered const offered;
    ui::testing::MemoryHistory history{offered.mineId};
    Window window{offered, {}, &history};
    REQUIRE(window.wait([&window] { return window.find("Text", "home").has_value(); }));
    CHECK(history.entries() == std::vector<std::string>{offered.mineId});
    CHECK(window.shown("Button", "source_button_" + offered.mineId));
}

// Mutation: mount the start screen without telling the person (no toast).
TEST_CASE("Client shell: a recorded route to a custom screen that is gone is not restored, and says so",
          "[client][custom-screens]") {
    Offered const offered;
    std::string const gone = "custom:" + std::string(31, '0') + "f";
    ui::testing::MemoryHistory history{gone};
    Window window{offered, {}, &history};
    REQUIRE(window.wait([&window] { return showsText(window.backend, "was not restored"); }));
    CHECK(window.wait([&window] { return window.find("Text", "home").has_value(); }));
    CHECK_FALSE(window.find("Button", "source_button_" + gone).has_value());
}

namespace {

/// A public login screen that signs in through `Probe_Login`.
constexpr std::string_view kLogin =
    R"({"vocab":"ui/1","id":"customs.login","models":{"probe":{"type":"Probe_Model"}},)"
    R"("mutations":{"login":{"model":"probe","action":"Probe_Login","onSuccess":[{"signIn":{"from":"login"}}]}},)"
    R"("view":{"kind":"column","children":[)"
    R"({"kind":"button","testId":"login","label":"Log in","onClick":[{"run":"login"}]}]}})";

/// The customs home behind a public login.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> customsBehindLogin() {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "customs", .version = "1"}};
    for (auto const& [screenId, text] : {std::pair{"customs.login", kLogin}, std::pair{"customs.home", kHome}}) {
        ui::CatalogEntry entry;
        entry.id = screenId;
        entry.title = screenId;
        entry.isPublic = screenId == std::string_view{"customs.login"};
        if (auto added = served.addScreen(std::move(entry), std::string{text}); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

}  // namespace

// Mutation: drop the route when the fetch is refused while signed out (the sign-in finds nothing wanted and mounts the
// start screen).
TEST_CASE("Client shell: a recorded route to a custom screen opened while signed out is opened after the sign-in",
          "[client][custom-screens]") {
    Offered const offered;
    ui::testing::MemoryHistory history{offered.mineId};
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    client::LocalSource source{client::LocalApp{.name = "customs", .bundle = &customsBehindLogin},
                               client::LocalSetup{.db = {}, .user = {}}};
    std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
    source.open(context,
                [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    client::ShellOptions options{.history = &history};
    options.signIn = [](ui::Value const& /*reply*/) {
        return std::optional<client::SignedIn>{client::SignedIn{.principal = "ann", .token = {}}};
    };
    {
        auto const shell = client::mountShell(context, backend, std::move(*opened), source, std::move(options));
        auto const find = [&backend](std::string_view kind, std::string const& testId) {
            return backend.find(kind, "testId", testId);
        };
        // Signed out: the login shows, and the refused fetch toasts nothing.
        REQUIRE(pumpUntil(context.owner(), [&] { return find("Button", "login").has_value(); }));
        CHECK_FALSE(showsText(backend, "was not restored"));
        backend.click(*find("Button", "login"));
        REQUIRE(
            pumpUntil(context.owner(), [&] { return find("Button", "source_button_" + offered.mineId).has_value(); }));
        CHECK(find("Text", "home").has_value());
        CHECK_FALSE(showsText(backend, "was not restored"));
    }
    source.close();
}

/// Lets every call run, and takes a call's token for the principal it is verified as.
struct TokenAuthorizer final : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(morph::session::Context const& /*ctx*/, std::string_view /*modelType*/,
                                 std::string_view /*actionType*/) const override {
        return true;
    }
    [[nodiscard]] std::optional<std::string> authenticate(morph::session::Context const& ctx) const override {
        if (ctx.token.empty()) {
            return std::nullopt;
        }
        return ctx.token;
    }
};

/// The customs application behind a server's connection: the UI service and the custom screens model answer through a
/// real `RemoteServer`'s `handle` on a connection of its own, so `watch` is answered as a server answers it. With
/// `Refuse`, the exchange answers every `watch` itself with the record refused, the control of the measurement.
class WatchedSource final : public client::Source {
public:
    enum class Watch : std::uint8_t { Server, Refuse };

    explicit WatchedSource(Watch watch) : _watch{watch} { requestSession().token = "ann"; }
    ~WatchedSource() override { close(); }
    WatchedSource(WatchedSource const&) = delete;
    WatchedSource& operator=(WatchedSource const&) = delete;
    WatchedSource(WatchedSource&&) = delete;
    WatchedSource& operator=(WatchedSource&&) = delete;

    /// The bodies of the `watch` requests the exchange saw, and the replies' bodies that reached the client.
    [[nodiscard]] std::vector<std::string> watchBodies() {
        std::scoped_lock const lock{_mutex};
        return _bodies;
    }
    [[nodiscard]] std::vector<std::string> watchReplies() {
        std::scoped_lock const lock{_mutex};
        return _replies;
    }

protected:
    [[nodiscard]] std::expected<Transport, ui::ConnectError> connect(ui::AppContext& context) override {
        auto served = customsBundle();
        REQUIRE(served.has_value());
        morph::backend::ServerConfig config;
        config.services.push_back(std::make_shared<ui::UiService>(std::move(*served)));
        _pool = std::make_unique<morph::exec::ThreadPoolExecutor>(2);
        _server = std::make_shared<morph::backend::RemoteServer>(*_pool, std::make_shared<TokenAuthorizer>(), config);
        morph::backend::ConnectionId const cid = _server->openConnection();
        auto made = std::make_unique<morph::backend::SimulatedRemoteBackend>(*_server, cid);
        ui::Exchange real = ui::exchangeOver(*made, context.executor());
        ui::Exchange exchange = [this, real = std::move(real)](morph::wire::Envelope request,
                                                               std::function<void(ui::ExchangeReply)> done) {
            if (request.kind != "watch") {
                real(std::move(request), std::move(done));
                return;
            }
            {
                std::scoped_lock const lock{_mutex};
                _bodies.push_back(request.body);
            }
            if (_watch == Watch::Refuse) {
                morph::wire::Envelope answer;
                answer.kind = "ok";
                answer.body = R"({"refusedRecords":["morph.customScreens"],"refusedInstances":[]})";
                std::scoped_lock const lock{_mutex};
                _replies.push_back(answer.body);
                done(answer);
                return;
            }
            real(std::move(request), [this, done = std::move(done)](ui::ExchangeReply reply) {
                if (reply) {
                    std::scoped_lock const lock{_mutex};
                    _replies.push_back(reply->body);
                }
                done(std::move(reply));
            });
        };
        return Transport{.backend = std::move(made), .exchange = std::move(exchange), .serverEvents = true};
    }
    void disconnect() override {
        _server.reset();
        _pool.reset();
    }

private:
    Watch _watch;
    std::mutex _mutex;
    std::vector<std::string> _bodies;
    std::vector<std::string> _replies;
    std::unique_ptr<morph::exec::ThreadPoolExecutor> _pool;
    std::shared_ptr<morph::backend::RemoteServer> _server;
};

/// What a run of the shell over a `WatchedSource` showed: the warnings logged, the watch requests the exchange saw,
/// and the reply bodies that reached the client.
struct WatchRun {
    std::vector<std::string> warnings;
    std::vector<std::string> bodies;
    std::vector<std::string> replies;
};

namespace {

/// Opens the customs application over a server, mounts the shell, and waits until it listed "Mine" and a watch was
/// answered.
WatchRun measureWatch(Offered const& offered, WatchedSource::Watch watch) {
    WatchRun run;
    std::mutex logged;
    morph::log::ScopedLoggerOverride const capture{[&](morph::log::LogLevel level, std::string_view message) {
        if (level == morph::log::LogLevel::warn) {
            std::scoped_lock const lock{logged};
            run.warnings.emplace_back(message);
        }
    }};
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    WatchedSource source{watch};
    std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
    source.open(context,
                [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    {
        auto shell = client::mountShell(context, backend, std::move(*opened), source,
                                        client::ShellOptions{.startScreen = "customs.home"});
        REQUIRE(pumpUntil(context.owner(), [&backend, &offered] {
            return backend.find("Button", "testId", "nav_" + offered.mineId).has_value();
        }));
        REQUIRE(pumpUntil(context.owner(), [&source] { return !source.watchReplies().empty(); }));
        // The reply is recorded as the exchange hands it on; the session reads it afterwards, on the owner. Tasks
        // posted now run behind that continuation, so the warning of a refusal has been logged (or not) by the time
        // the last of them has run.
        for (int round = 0; round < 3; ++round) {
            bool ran = false;
            context.owner().post([&ran] { ran = true; });
            REQUIRE(pumpUntil(context.owner(), [&ran] { return ran; }));
        }
    }
    run.bodies = source.watchBodies();
    run.replies = source.watchReplies();
    source.close();
    std::scoped_lock const lock{logged};
    return run;
}

}  // namespace

// Measurement: the production watcher (`Source::watcher()`, `ui::watcherOver`) carries the real server's watch reply
// to the session, so the warning about a refused extra watch does not fire when the server admits the record. The
// control makes the exchange answer with the record refused and must warn, so a silent run here is not a silent
// logger.
TEST_CASE(
    "Client shell: the extra watch's warning stays silent when the server admits the record, and fires when it "
    "does not",
    "[client][custom-screens][watch]") {
    Offered const offered;
    WatchRun const quiet = measureWatch(offered, WatchedSource::Watch::Server);
    WatchRun const loud = measureWatch(offered, WatchedSource::Watch::Refuse);
    auto const refusedWatch = [](std::string const& line) { return line.contains("refused the watch on"); };
    // The record was asked for, and the server's own reply, not a stand-in, reached the session.
    REQUIRE_FALSE(quiet.bodies.empty());
    CHECK(std::ranges::any_of(quiet.bodies,
                              [](std::string const& body) { return body.contains("morph.customScreens"); }));
    REQUIRE_FALSE(quiet.replies.empty());
    CHECK(quiet.replies.front() == R"({"refusedRecords":[],"refusedInstances":[]})");
    CHECK(std::ranges::none_of(quiet.warnings, refusedWatch));
    CHECK(std::ranges::any_of(loud.warnings, refusedWatch));
}
