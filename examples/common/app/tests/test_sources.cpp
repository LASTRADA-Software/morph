// SPDX-License-Identifier: Apache-2.0

// First, ahead of every header that brings view.hpp: GCC's -Wshadow reports value.hpp's enumerator `Type::Key` when
// view.hpp's `ui::Key` was declared before it.
// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <algorithm>
#include <app/app_source.hpp>
#include <app/client_shell.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/service.hpp>
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

#include "probe_app.hpp"

using morph::examples::ClientShellOptions;
using morph::examples::LocalAppSource;
using morph::examples::LocalSetup;
using morph::examples::testing::FakeAppContext;
using morph::examples::testing::pumpUntil;
using Opened = std::expected<morph::ui::Bundle, morph::ui::ConnectError>;

namespace {

/// What a shell reported.
struct ShellLog {
    std::vector<std::string> problems;
    std::vector<std::string> mounted;

    ClientShellOptions options(std::string start = {}) {
        return ClientShellOptions{.startScreen = std::move(start),
                                  .onProblem = [this](std::string const& problem) { problems.push_back(problem); },
                                  .onMounted = [this](std::string const& screenId) { mounted.push_back(screenId); }};
    }
};

/// The text a widget with @p testId shows, or nothing when there is none.
std::optional<std::string> textOf(morph::ui::testing::RecordingBackend const& backend, std::string const& testId) {
    auto const found = backend.find("Text", "testId", testId);
    if (!found) {
        return std::nullopt;
    }
    return backend.prop(*found, "text");
}

/// Opens @p source on @p context and waits for its answer.
Opened openAndWait(FakeAppContext& context, morph::ui::AppSource& source) {
    std::optional<Opened> opened;
    source.open(context, [&opened](Opened result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    return std::move(opened).value();
}

}  // namespace

TEST_CASE("LocalAppSource: opens the linked application's bundle, verified, through the UI service in process",
          "[examples-common][source]") {
    FakeAppContext context;
    LocalAppSource source{probe::app(), LocalSetup{}};
    Opened const opened = openAndWait(context, source);
    REQUIRE(opened.has_value());
    CHECK(opened->applicationId == "probe");
    CHECK(opened->documents.contains("probe.home"));
    CHECK(opened->documents.contains("probe.other"));
    CHECK(source.screens() == std::vector<std::string>{"probe.home", "probe.other"});
    source.close();
}

// Mutation: mount the bundle's last screen instead of the first — the answer text is missing.
TEST_CASE("Client shell: mounts the start screen, and the query's answer crosses the bridge",
          "[examples-common][shell]") {
    FakeAppContext context;
    morph::ui::testing::RecordingBackend backend;
    LocalAppSource source{probe::app(), LocalSetup{}};
    ShellLog log;
    {
        auto shell =
            morph::examples::mountClientShell(context, backend, openAndWait(context, source), source, log.options());
        CHECK(log.mounted == std::vector<std::string>{"probe.home"});
        CHECK(pumpUntil(context.owner(), [&] { return textOf(backend, "answer") == "answer 42"; }));
        CHECK(log.problems.empty());
    }
    source.close();
}

// Mutation: mount the navigated screen from inside the click handler — the old screen is destroyed under its own
// handler (ASan) or the mount order changes.
TEST_CASE("Client shell: a document's navigate opens the screen as a tab and shows it, after the handler returns",
          "[examples-common][shell]") {
    FakeAppContext context;
    morph::ui::testing::RecordingBackend backend;
    LocalAppSource source{probe::app(), LocalSetup{}};
    ShellLog log;
    {
        auto shell =
            morph::examples::mountClientShell(context, backend, openAndWait(context, source), source, log.options());
        auto const next = backend.find("Button", "testId", "next");
        REQUIRE(next.has_value());
        backend.click(next.value());
        CHECK(log.mounted == std::vector<std::string>{"probe.home"});  // posted, not yet run
        CHECK(pumpUntil(context.owner(), [&] { return textOf(backend, "other").has_value(); }));
        CHECK(log.mounted == std::vector<std::string>{"probe.home", "probe.other"});
        // Two tabs, the new one selected; the first keeps its screen, hidden.
        auto const tabs = backend.find("Tabs", "selected", "1");
        REQUIRE(tabs.has_value());
        CHECK(textOf(backend, "answer").has_value());
    }
    source.close();
}

TEST_CASE("Client shell: --screen picks the start screen; an unknown one is shown as a problem",
          "[examples-common][shell]") {
    FakeAppContext context;
    morph::ui::testing::RecordingBackend backend;
    LocalAppSource source{probe::app(), LocalSetup{}};
    ShellLog log;
    {
        auto shell = morph::examples::mountClientShell(context, backend, openAndWait(context, source), source,
                                                       log.options("probe.other"));
        CHECK(log.mounted == std::vector<std::string>{"probe.other"});
    }
    ShellLog missing;
    {
        auto shell = morph::examples::mountClientShell(context, backend, openAndWait(context, source), source,
                                                       missing.options("probe.none"));
        CHECK(missing.mounted.empty());
        REQUIRE(missing.problems.size() == 1);
        CHECK(missing.problems.front() == "Cannot open probe.none: unknown screen");
        CHECK(backend.find("Text", "text", "Cannot open probe.none: unknown screen").has_value());
    }
    source.close();
}

// Mutation: mount a moved-to entry from a posted task (the history pushes it, and forward is lost).
TEST_CASE("Client shell: the shown screen's route is kept in the history, and back and forward reopen it",
          "[examples-common][shell][route-history]") {
    using Entries = std::vector<std::string>;
    FakeAppContext context;
    morph::ui::testing::RecordingBackend backend;
    LocalAppSource source{probe::app(), LocalSetup{}};
    ShellLog log;
    morph::ui::testing::MemoryHistory history;
    {
        ClientShellOptions options = log.options();
        options.history = &history;
        auto shell = morph::examples::mountClientShell(context, backend, openAndWait(context, source), source,
                                                       std::move(options));
        CHECK(history.entries() == Entries{"probe.home"});
        auto const next = backend.find("Button", "testId", "next");
        REQUIRE(next.has_value());
        backend.click(next.value());
        REQUIRE(pumpUntil(context.owner(), [&] { return textOf(backend, "other").has_value(); }));
        CHECK(history.entries() == Entries{"probe.home", "probe.other"});

        REQUIRE(history.back());
        CHECK(log.mounted == Entries{"probe.home", "probe.other", "probe.home"});
        CHECK(pumpUntil(context.owner(), [&] { return textOf(backend, "answer") == "answer 42"; }));
        REQUIRE(history.forward());
        CHECK(log.mounted.back() == "probe.other");
        CHECK(history.entries() == Entries{"probe.home", "probe.other"});
        CHECK(history.index() == 1);

        history.follow("probe.none");  // an address the user typed, for a screen there is not
        CHECK(log.mounted.back() == "probe.home");
        CHECK(history.entries() == Entries{"probe.home", "probe.other", "probe.home"});
        CHECK(log.problems.empty());
    }
    source.close();
}

// Mutation: ignore the history's route at start (the start screen mounts instead of probe.other).
TEST_CASE("Client shell: a reloaded page opens the route it holds, and a route it cannot open shows the start screen",
          "[examples-common][shell][route-history]") {
    using Entries = std::vector<std::string>;
    for (auto const& [initial, expected] : {std::pair<std::string, std::string>{"probe.other", "probe.other"},
                                            std::pair<std::string, std::string>{"probe.none", "probe.home"},
                                            std::pair<std::string, std::string>{"probe.home?x=1", "probe.home"}}) {
        CAPTURE(initial);
        FakeAppContext context;
        morph::ui::testing::RecordingBackend backend;
        LocalAppSource source{probe::app(), LocalSetup{}};
        ShellLog log;
        morph::ui::testing::MemoryHistory history{initial};
        {
            ClientShellOptions options = log.options();
            options.history = &history;
            auto shell = morph::examples::mountClientShell(context, backend, openAndWait(context, source), source,
                                                           std::move(options));
            CHECK(log.mounted == Entries{expected});
            CHECK(history.entries() == Entries{expected});
        }
        source.close();
    }
}

// Mutation: open the history's route even when a start screen is named (probe.other mounts).
TEST_CASE("Client shell: a start screen asked for by name wins over the history's route",
          "[examples-common][shell][route-history]") {
    FakeAppContext context;
    morph::ui::testing::RecordingBackend backend;
    LocalAppSource source{probe::app(), LocalSetup{}};
    ShellLog log;
    morph::ui::testing::MemoryHistory history{"probe.other"};
    {
        ClientShellOptions options = log.options("probe.home");
        options.history = &history;
        auto shell = morph::examples::mountClientShell(context, backend, openAndWait(context, source), source,
                                                       std::move(options));
        CHECK(log.mounted == std::vector<std::string>{"probe.home"});
        CHECK(history.entries() == std::vector<std::string>{"probe.home"});
    }
    source.close();
}

TEST_CASE("Client shell: a source that did not open is shown, not mounted", "[examples-common][shell]") {
    FakeAppContext context;
    morph::ui::testing::RecordingBackend backend;
    LocalAppSource source{probe::app(), LocalSetup{}};
    ShellLog log;
    auto shell =
        morph::examples::mountClientShell(context, backend,
                                          std::unexpected(morph::ui::ConnectError{
                                              .kind = morph::ui::ConnectError::Kind::Unreachable, .message = "down"}),
                                          source, log.options());
    CHECK(log.mounted.empty());
    CHECK(log.problems == std::vector<std::string>{"Cannot open the application: down"});
    CHECK(backend.find("Text", "text", "Cannot open the application: down").has_value());
}

namespace {

/// A source whose exchange the test answers by hand.
class ManualSource final : public morph::examples::ClientSource {
public:
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): the test answers and inspects these.
    std::vector<std::function<void(morph::ui::ExchangeReply)>> waiting;
    int sent = 0;
    std::chrono::milliseconds timeout{0};
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

protected:
    [[nodiscard]] std::expected<Transport, morph::ui::ConnectError> connect(
        morph::ui::AppContext& /*context*/) override {
        _workers = std::make_unique<morph::exec::ThreadPoolExecutor>(1);
        return Transport{.backend = std::make_unique<morph::backend::LocalBackend>(*_workers),
                         .exchange = [this](morph::wire::Envelope const& /*request*/,
                                            std::function<void(morph::ui::ExchangeReply)> done) {
                             ++sent;
                             waiting.push_back(std::move(done));
                         }};
    }
    [[nodiscard]] std::chrono::milliseconds openTimeout() const override { return timeout; }
    void disconnect() override { _workers.reset(); }

private:
    std::unique_ptr<morph::exec::ThreadPoolExecutor> _workers;

public:
    ~ManualSource() override { close(); }
    ManualSource() = default;
    ManualSource(ManualSource const&) = delete;
    ManualSource& operator=(ManualSource const&) = delete;
    ManualSource(ManualSource&&) = delete;
    ManualSource& operator=(ManualSource&&) = delete;
};

}  // namespace

// Mutation: drop the liveness check in the guarded exchange — the closed source sends a second request.
TEST_CASE("ClientSource: once closed, it delivers nothing and sends nothing more", "[examples-common][source]") {
    FakeAppContext context;
    ManualSource source;
    int readies = 0;
    source.open(context, [&readies](Opened /*result*/) { ++readies; });
    REQUIRE(source.sent == 1);  // the ui-hello
    source.close();
    // The hello's reply arrives after the close: the client would fetch the manifest next.
    morph::wire::Envelope reply;
    reply.kind = "ok";
    reply.body = R"({"application":"probe","version":"1","vocabularies":["ui/1","expr/1"],"digest":"sha256:)" +
                 std::string(64, '0') + R"("})";
    source.waiting.front()(reply);
    CHECK(source.sent == 1);
    CHECK(readies == 0);
}

// Mutation: leave the timeout unarmed — the source never answers.
TEST_CASE("ClientSource: a source that does not answer in time is reported unreachable, once",
          "[examples-common][source]") {
    FakeAppContext context;
    ManualSource source;
    source.timeout = std::chrono::milliseconds{100};
    std::vector<Opened> answers;
    source.open(context, [&answers](Opened result) { answers.push_back(std::move(result)); });
    context.clock().advance(std::chrono::milliseconds{99});
    static_cast<void>(pumpUntil(context.owner(), [] { return false; }, std::chrono::milliseconds{20}));
    CHECK(answers.empty());
    context.clock().advance(std::chrono::milliseconds{1});
    REQUIRE(pumpUntil(context.owner(), [&answers] { return !answers.empty(); }));
    REQUIRE_FALSE(answers.front().has_value());
    CHECK(answers.front().error().kind == morph::ui::ConnectError::Kind::Unreachable);
    // A reply after the timeout delivers nothing more.
    source.waiting.front()(std::unexpected(std::string{"late"}));
    CHECK(answers.size() == 1);
}

TEST_CASE("ClientSource: switching backends is refused", "[examples-common][source]") {
    ManualSource source;
    std::optional<std::string> refusal;
    source.switchBackend(morph::ui::Backend{.name = "local"}, [&refusal](std::expected<void, std::string> done) {
        if (!done) {
            refusal = done.error();
        }
    });
    REQUIRE(refusal.has_value());
    CHECK(refusal.value() == "this client does not switch backends; the switch to `local` is the hosting layer's");
}

namespace {

/// The start screen of the reloading tests, first as served, then as changed: the same state, a different label.
constexpr std::string_view kCounterServed =
    R"({"vocab":"ui/1","id":"dev.counter","state":{"count":{"type":"int","init":0}},)"
    R"("view":{"kind":"column","children":[)"
    R"({"kind":"text","testId":"count","text":{"fn":"format","args":["count {0}",{"ref":"count"}]}},)"
    R"({"kind":"button","testId":"bump","label":"bump","onClick":[{"set":"count","value":5}]}]}})";
constexpr std::string_view kCounterEdited =
    R"({"vocab":"ui/1","id":"dev.counter","state":{"count":{"type":"int","init":0}},)"
    R"("view":{"kind":"column","children":[)"
    R"({"kind":"text","testId":"count","text":{"fn":"format","args":["edited {0}",{"ref":"count"}]}}]}})";

/// A UI service serving one screen, @p text.
std::shared_ptr<morph::ui::UiService> serving(std::string_view text) {
    morph::ui::UiRegistry served{morph::ui::ApplicationInfo{.id = "dev", .version = "1"}};
    morph::ui::CatalogEntry entry;
    entry.id = "dev.counter";
    auto const added = served.addScreen(std::move(entry), std::string{text});
    REQUIRE(added);
    auto built = served.build();
    REQUIRE(built);
    return std::make_shared<morph::ui::UiService>(std::move(*built));
}

/// A source whose server the test redeploys: its exchange answers through whichever service it holds now, and counts
/// the `ui-hello` requests.
class RedeployedSource final : public morph::examples::ClientSource {
public:
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): the test redeploys and counts.
    std::shared_ptr<morph::ui::UiService> service = serving(kCounterServed);
    int hellos = 0;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    RedeployedSource() = default;
    ~RedeployedSource() override { close(); }
    RedeployedSource(RedeployedSource const&) = delete;
    RedeployedSource& operator=(RedeployedSource const&) = delete;
    RedeployedSource(RedeployedSource&&) = delete;
    RedeployedSource& operator=(RedeployedSource&&) = delete;

protected:
    [[nodiscard]] std::expected<Transport, morph::ui::ConnectError> connect(
        morph::ui::AppContext& /*context*/) override {
        _workers = std::make_unique<morph::exec::ThreadPoolExecutor>(1);
        return Transport{
            .backend = std::make_unique<morph::backend::LocalBackend>(*_workers),
            .exchange = [this](morph::wire::Envelope const& request,
                               std::function<void(morph::ui::ExchangeReply)> const& done) {
                if (request.kind == "ui-hello") {
                    ++hellos;
                }
                done(morph::wire::decode(service->handle(request, *morph::session::allowAllAuthorizer())));
            }};
    }
    void disconnect() override { _workers.reset(); }

private:
    std::unique_ptr<morph::exec::ThreadPoolExecutor> _workers;
};

}  // namespace

// Mutation: leave `dev` out of the shell's options (or never arm the poll) — the edited label never shows.
// Mutation: arm the poll whatever `dev` says — the production case sends a second ui-hello.
TEST_CASE(
    "Client shell: in development mode a changed served document remounts the shown screen, keeping its state; "
    "without it nothing is polled",
    "[examples-common][shell][dev-reload]") {
    std::vector<bool> modes{false};
    modes.push_back(true);
    for (bool const dev : modes) {
        CAPTURE(dev);
        FakeAppContext context;
        morph::ui::testing::RecordingBackend backend;
        RedeployedSource source;
        ShellLog log;
        std::vector<std::vector<std::string>> reloads;
        ClientShellOptions options = log.options();
        options.dev = dev;
        options.onReloaded = [&reloads](std::vector<std::string> const& remounted) { reloads.push_back(remounted); };
        {
            auto const shell = morph::examples::mountClientShell(context, backend, openAndWait(context, source),
                                                                 source, std::move(options));
            REQUIRE(log.mounted == std::vector<std::string>{"dev.counter"});
            auto const bump = backend.find("Button", "testId", "bump");
            REQUIRE(bump.has_value());
            backend.click(bump.value());
            REQUIRE(pumpUntil(context.owner(), [&] { return textOf(backend, "count") == "count 5"; }));
            int const hellosAtMount = source.hellos;

            source.service = serving(kCounterEdited);  // the server's document changes
            for (int second = 0; second < 3; ++second) {
                context.clock().advance(std::chrono::seconds{1});
                static_cast<void>(pumpUntil(
                    context.owner(), [&] { return textOf(backend, "count") == "edited 5"; },
                    std::chrono::milliseconds{50}));
            }
            if (dev) {
                CHECK(textOf(backend, "count") == "edited 5");  // remounted, and the state kept
                CHECK_FALSE(backend.find("Button", "testId", "bump").has_value());
                CHECK(source.hellos > hellosAtMount);
                REQUIRE_FALSE(reloads.empty());
                CHECK(reloads.front() == std::vector<std::string>{"dev.counter"});
            } else {
                CHECK(textOf(backend, "count") == "count 5");
                CHECK(source.hellos == hellosAtMount);  // the digest is read once per connection
                CHECK(reloads.empty());
            }
            CHECK(log.mounted == std::vector<std::string>{"dev.counter"});  // remounted in place, not navigated
            CHECK(log.problems.empty());
        }
        source.close();
    }
}

// Mutation: hide the frame whenever no sign-in happened (the probe has none, so its sidebar would never show).
TEST_CASE("Client shell: the frame's navigator opens a screen as a tab", "[examples-common][shell]") {
    FakeAppContext context;
    morph::ui::testing::RecordingBackend backend;
    LocalAppSource source{probe::app(), LocalSetup{}};
    ShellLog log;
    {
        auto const shell =
            morph::examples::mountClientShell(context, backend, openAndWait(context, source), source, log.options());
        auto const sidebar = backend.find("Row", "testId", "shell_sidebar");
        REQUIRE(sidebar.has_value());
        CHECK(backend.prop(*sidebar, "visible") != "false");
        auto const entry = backend.find("Button", "testId", "nav_probe.other");
        REQUIRE(entry.has_value());
        backend.click(*entry);
        CHECK(pumpUntil(context.owner(), [&] { return textOf(backend, "other").has_value(); }));
        CHECK(log.mounted == std::vector<std::string>{"probe.home", "probe.other"});
        CHECK(log.problems.empty());
    }
    source.close();
}

namespace {

// A local backend whose connect hook the test fires, as a transport does after each connect. `reconnect` replays a
// transport's reconnect: the connect hook, then the bridge's reconnect handler posted to its executor. It logs each
// bind and execute, in order.
class ConnectingBackend final : public morph::backend::LocalBackend {
public:
    using LocalBackend::LocalBackend;
    void setConnectHandler(std::function<void()> const& handler) override { *_handler = handler; }
    void setReconnectHandler(std::function<void()> handler, morph::exec::IExecutor* exec) override {
        _reconnect = std::move(handler);
        _reconnectExec = exec;
    }
    [[nodiscard]] std::shared_ptr<std::function<void()>> handler() const { return _handler; }

    morph::async::Completion<morph::exec::detail::ModelId> bindModel(morph::backend::detail::BindRequest request,
                                                                     morph::exec::IExecutor& cbExec) override {
        note("bind");
        return LocalBackend::bindModel(std::move(request), cbExec);
    }
    void executeInto(morph::exec::detail::ModelId mid, morph::backend::detail::ActionCall call,
                     morph::exec::IExecutor* cbExec,
                     std::shared_ptr<morph::async::detail::ISettleSink> sink) override {
        note("execute");
        LocalBackend::executeInto(mid, std::move(call), cbExec, std::move(sink));
    }

    void reconnect() {
        (*_handler)();
        if (_reconnect && _reconnectExec != nullptr) {
            _reconnectExec->post(_reconnect);
        }
    }

    std::vector<std::string> calls() {
        std::scoped_lock const lock{_mutex};
        return _calls;
    }
    void clearCalls() {
        std::scoped_lock const lock{_mutex};
        _calls.clear();
    }

private:
    void note(std::string call) {
        std::scoped_lock const lock{_mutex};
        _calls.push_back(std::move(call));
    }

    std::shared_ptr<std::function<void()>> _handler = std::make_shared<std::function<void()>>();
    std::function<void()> _reconnect;
    morph::exec::IExecutor* _reconnectExec = nullptr;
    std::mutex _mutex;
    std::vector<std::string> _calls;
};

// The probe application behind a server-like transport: the UI service answers the `ui-*` kinds in process, the
// exchange answers `watch` itself, as `RemoteServer` does, and every request's kind is logged in order.
class ReconnectingSource final : public morph::examples::ClientSource {
public:
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): the test fires and inspects these.
    std::vector<std::string> kinds;
    std::vector<std::string> watchBodies;
    std::shared_ptr<std::function<void()>> connectHook;
    ConnectingBackend* backend = nullptr;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    ReconnectingSource() = default;
    ~ReconnectingSource() override { close(); }
    ReconnectingSource(ReconnectingSource const&) = delete;
    ReconnectingSource& operator=(ReconnectingSource const&) = delete;
    ReconnectingSource(ReconnectingSource&&) = delete;
    ReconnectingSource& operator=(ReconnectingSource&&) = delete;

protected:
    [[nodiscard]] std::expected<Transport, morph::ui::ConnectError> connect(
        morph::ui::AppContext& /*context*/) override {
        auto served = probe::bundle();
        REQUIRE(served.has_value());
        _service =
            std::make_shared<morph::ui::UiService>(std::move(*served), morph::model::detail::defaultDispatcher());
        _workers = std::make_unique<morph::exec::ThreadPoolExecutor>(1);
        auto made = std::make_unique<ConnectingBackend>(*_workers);
        backend = made.get();
        connectHook = made->handler();
        return Transport{
            .backend = std::move(made),
            .exchange =
                [this](morph::wire::Envelope const& request,
                       std::function<void(morph::ui::ExchangeReply)> const& done) {
                    kinds.push_back(request.kind);
                    if (request.kind == "watch" || request.kind == "unwatch") {
                        watchBodies.push_back(request.body);
                        morph::wire::Envelope answer;
                        answer.kind = "ok";
                        answer.body = "{}";
                        done(answer);
                        return;
                    }
                    done(morph::wire::decode(_service->handle(request, *morph::session::allowAllAuthorizer())));
                },
            .serverEvents = true};
    }
    void disconnect() override {
        _workers.reset();
        _service.reset();
    }

private:
    std::shared_ptr<morph::ui::UiService> _service;
    std::unique_ptr<morph::exec::ThreadPoolExecutor> _workers;
};

}  // namespace

// Mutation: call the listeners before the ui-hello, or forward no events — the order or the last watch changes.
TEST_CASE("remote_source_rehellos_then_rewatches_on_reconnect", "[examples-common][source][push]") {
    FakeAppContext context;
    morph::ui::testing::RecordingBackend screens;
    ReconnectingSource source;
    ShellLog log;
    {
        auto shell =
            morph::examples::mountClientShell(context, screens, openAndWait(context, source), source, log.options());
        REQUIRE(log.mounted == std::vector<std::string>{"probe.home"});
        REQUIRE(source.connectHook != nullptr);

        // A connect once the bundle loaded is a new connection: it negotiates events again, then the session
        // re-sends its whole watch set.
        source.kinds.clear();
        (*source.connectHook)();
        REQUIRE(pumpUntil(context.owner(), [&] { return source.kinds.size() >= 2; }));
        CHECK(source.kinds == std::vector<std::string>{"ui-hello", "watch"});
        CHECK(source.watchBodies.back() == R"({"records":[],"instances":[],"replace":true})");

        // A server event reaches the session: a resync answers with the whole watch set.
        source.kinds.clear();
        source.backend->deliverEvent({.topic = "resync", .payload = {"null"}});
        REQUIRE(pumpUntil(context.owner(), [&] { return !source.kinds.empty(); }));
        CHECK(source.kinds == std::vector<std::string>{"watch"});
        CHECK(log.problems.empty());
    }
    source.close();
}

// Mutation: run the connect listeners in the connect hook's own task, ahead of the bridge's reconnect handler — the
// mounted query refetches through the old connection's binding before the rebind.
TEST_CASE("remote_source_refetches_after_the_bridge_rebinds", "[examples-common][source][push]") {
    FakeAppContext context;
    morph::ui::testing::RecordingBackend screens;
    ReconnectingSource source;
    ShellLog log;
    {
        auto const shell =
            morph::examples::mountClientShell(context, screens, openAndWait(context, source), source, log.options());
        REQUIRE(log.mounted == std::vector<std::string>{"probe.home"});
        REQUIRE(source.backend != nullptr);
        // The screen's query has run, through a bound private instance.
        REQUIRE(pumpUntil(context.owner(), [&] {
            auto const calls = source.backend->calls();
            return std::ranges::find(calls, "execute") != calls.end();
        }));

        source.backend->clearCalls();
        source.backend->reconnect();
        REQUIRE(pumpUntil(context.owner(), [&] {
            auto const calls = source.backend->calls();
            return std::ranges::find(calls, "execute") != calls.end();
        }));
        // The refetch waits for the new binding.
        auto const calls = source.backend->calls();
        REQUIRE_FALSE(calls.empty());
        CHECK(calls.front() == "bind");
        CHECK(log.problems.empty());
    }
    source.close();
}
