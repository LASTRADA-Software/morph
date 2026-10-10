// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <expected>
#include <functional>
#include <memory>
#include <morph/reactive/control.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "reactive_fake_server.hpp"
#include "test_support.hpp"

namespace ui = morph::ui;

namespace {

using namespace std::chrono_literals;
using ui::testing::RecordingBackend;
using Events = std::vector<std::string>;
using Args = std::vector<std::string>;

/// A context over a step executor and a manual scheduler, as a frontend's would be over its loop.
class TestContext : public ui::AppContext {
public:
    morph::reactive::Runtime& runtime() override { return _runtime; }
    morph::exec::IExecutor& executor() override { return _owner; }
    ui::Scheduler& scheduler() override { return _scheduler; }
    morph::exec::IoLoop* ioLoop() override { return nullptr; }
    void quit(int exitCode) override { _exitCode = exitCode; }

    [[nodiscard]] morph::testing::StepExecutor& owner() noexcept { return _owner; }
    [[nodiscard]] morph::reactive::testing::ManualScheduler& manualScheduler() noexcept { return _scheduler; }
    [[nodiscard]] std::optional<int> exitCode() const noexcept { return _exitCode; }

private:
    morph::testing::StepExecutor _owner;
    morph::reactive::testing::ManualScheduler _scheduler;
    morph::reactive::Runtime _runtime{_owner};
    std::optional<int> _exitCode;
};

/// A context that records when it starts going, which is when its runtime goes: its members are destroyed right
/// after this destructor's body.
class LoggedContext final : public TestContext {
public:
    explicit LoggedContext(Events& events) : _events{&events} {}
    ~LoggedContext() override {
        try {
            _events->emplace_back("runtime");
        } catch (...) {  // NOLINT(bugprone-empty-catch): a lost entry fails the test that reads the events
        }
    }
    LoggedContext(LoggedContext const&) = delete;
    LoggedContext& operator=(LoggedContext const&) = delete;
    LoggedContext(LoggedContext&&) = delete;
    LoggedContext& operator=(LoggedContext&&) = delete;

private:
    Events* _events;
};

/// A source that records what is done to it. It delivers when told to, or during `open` when `immediate` is set,
/// and drops its `ready` on `close` unless `keepReadyAfterClose` is set, as a misbehaving source would.
class FakeSource final : public ui::AppSource {
public:
    using Opened = std::expected<ui::Bundle, ui::ConnectError>;

    explicit FakeSource(Events& events) : _events{&events} {}

    void open(ui::AppContext&, std::function<void(Opened)> ready) override {
        _events->emplace_back("open");
        _ready = std::move(ready);
        for (Opened const& opened : immediate) {
            deliver(opened);
        }
    }
    void switchBackend(ui::Backend backend, std::function<void(std::expected<void, std::string>)> done) override {
        _events->push_back("switch to " + backend.name);
        done({});
    }
    void close() override {
        _events->emplace_back("close");
        if (!keepReadyAfterClose) {
            _ready = nullptr;
        }
    }

    /// Calls the `ready` it holds, if any, as the network would.
    void deliver(Opened const& opened) const {
        if (_ready) {
            auto const ready = _ready;
            ready(opened);
        }
    }

    std::vector<Opened> immediate;
    bool keepReadyAfterClose = false;

private:
    Events* _events;
    std::function<void(Opened)> _ready;
};

/// A shell that mounts one Text on a recording backend and records its own unmounting.
class TracedShell final : public ui::MountedShell {
public:
    TracedShell(ui::AppContext& context, RecordingBackend& backend, Events& events, std::string text)
        : _events{&events}, _mounted{context.runtime(), backend, ui::text({.text = std::move(text)})} {}
    ~TracedShell() override {
        try {
            _events->emplace_back("shell");
        } catch (...) {  // NOLINT(bugprone-empty-catch): a lost entry fails the test that reads the events
        }
    }
    TracedShell(TracedShell const&) = delete;
    TracedShell& operator=(TracedShell const&) = delete;
    TracedShell(TracedShell&&) = delete;
    TracedShell& operator=(TracedShell&&) = delete;

private:
    Events* _events;
    ui::Mounted _mounted;
};

ui::Bundle bundle() {
    return ui::Bundle{.applicationId = "demo", .manifestDigest = "00", .documents = {{"home", "{}"}}};
}

/// A shell factory that mounts a `TracedShell` showing the application's id, or nothing for a connect error.
ui::ShellFactory tracedShells(RecordingBackend& backend, Events& events) {
    return
        [&backend, &events](ui::AppContext& context,
                            std::expected<ui::Bundle, ui::ConnectError> opened) -> std::unique_ptr<ui::MountedShell> {
            if (!opened.has_value()) {
                events.push_back("connect error: " + opened.error().message);
                return nullptr;
            }
            events.push_back("mount " + opened->applicationId);
            return std::make_unique<TracedShell>(context, backend, events, opened->applicationId);
        };
}

/// A frontend built as a real one is: its context, and with it the runtime, is a local of `run`, made first and
/// destroyed last; `runApp` does the rest.
class TestFrontend final : public ui::Frontend {
public:
    TestFrontend(Events& events, ui::ShellFactory mountShell, std::function<int(TestContext&)> loop)
        : _events{&events}, _mountShell{std::move(mountShell)}, _loop{std::move(loop)} {}

    [[nodiscard]] std::string_view name() const override { return "test"; }

    int run(ui::AppSource& source) override {
        LoggedContext context{*_events};
        return ui::runApp(context, source, _mountShell, [this, &context] { return _loop(context); });
    }

private:
    Events* _events;
    ui::ShellFactory _mountShell;
    std::function<int(TestContext&)> _loop;
};

/// Sets a process environment variable for one test, and unsets it afterwards.
class ScopedVariable {
public:
    ScopedVariable(char const* name, char const* value) : _name{name} {
#ifdef _WIN32
        static_cast<void>(_putenv_s(name, value));
#else
        static_cast<void>(setenv(name, value, 1));  // NOLINT(concurrency-mt-unsafe): the test runs on one thread
#endif
    }

    /// Unsets the variable.
    ~ScopedVariable() {
#ifdef _WIN32
        static_cast<void>(_putenv_s(_name, ""));
#else
        static_cast<void>(unsetenv(_name));  // NOLINT(concurrency-mt-unsafe): the test runs on one thread
#endif
    }

    ScopedVariable(ScopedVariable const&) = delete;
    ScopedVariable& operator=(ScopedVariable const&) = delete;
    ScopedVariable(ScopedVariable&&) = delete;
    ScopedVariable& operator=(ScopedVariable&&) = delete;

private:
    char const* _name;
};

class NamedFrontend final : public ui::Frontend {
public:
    explicit NamedFrontend(std::string name) : _name{std::move(name)} {}
    [[nodiscard]] std::string_view name() const override { return _name; }
    int run(ui::AppSource&) override { return 0; }

private:
    std::string _name;
};

ui::FrontendOption option(std::string const& name, bool usable) {
    return ui::FrontendOption{
        .name = name,
        .usable = [usable] { return usable; },
        .make = [name = name] { return std::unique_ptr<ui::Frontend>{std::make_unique<NamedFrontend>(name)}; },
    };
}

ui::EnvironmentReader morphUi(std::optional<std::string> value) {
    return [value = std::move(value)](std::string_view name) { return name == "MORPH_UI" ? value : std::nullopt; };
}

std::string chosen(std::vector<ui::FrontendOption> const& built, Args args, ui::EnvironmentReader const& env) {
    auto const selected = ui::selectFrontend(built, args, env);
    return selected.has_value() ? std::string{(*selected)->name()} : "error: " + selected.error().message();
}

std::string selectionError(std::vector<ui::FrontendOption> const& built, Args args, ui::EnvironmentReader const& env) {
    auto const selected = ui::selectFrontend(built, args, env);
    return selected.has_value() ? "no error" : selected.error().message();
}

std::vector<ui::FrontendOption> qtThenTui(bool qtUsable = true, bool tuiUsable = true) {
    return {option("qt", qtUsable), option("tui", tuiUsable)};
}

struct Lookup {
    int id = 0;
    bool operator==(Lookup const&) const = default;
};

}  // namespace

// ── selectFrontend ─────────────────────────────────────────────────────────

// Mutation: read MORPH_UI before the command line in selectFrontend.
TEST_CASE("ui::selectFrontend: --ui= wins over MORPH_UI and over the order", "[ui]") {
    CHECK(chosen(qtThenTui(), {"app", "--ui=tui"}, morphUi("qt")) == "tui");
}

TEST_CASE("ui::selectFrontend: --ui takes its name from the next argument", "[ui]") {
    CHECK(chosen(qtThenTui(), {"app", "--verbose", "--ui", "tui"}, morphUi(std::nullopt)) == "tui");
}

TEST_CASE("ui::selectFrontend: the last --ui wins", "[ui]") {
    CHECK(chosen(qtThenTui(), {"app", "--ui=qt", "--ui=tui"}, morphUi(std::nullopt)) == "tui");
}

TEST_CASE("ui::selectFrontend: MORPH_UI wins over the order, and an empty one is ignored", "[ui]") {
    CHECK(chosen(qtThenTui(), {"app"}, morphUi("tui")) == "tui");
    CHECK(chosen(qtThenTui(), {"app"}, morphUi("")) == "qt");
}

TEST_CASE("ui::selectFrontend: otherwise the first usable option, in the order given", "[ui]") {
    CHECK(chosen(qtThenTui(), {"app"}, morphUi(std::nullopt)) == "qt");
    CHECK(chosen(qtThenTui(false, true), {"app"}, morphUi(std::nullopt)) == "tui");
    std::vector<ui::FrontendOption> const unconditional{{.name = "web", .usable = {}, .make = [] {
                                                             return std::unique_ptr<ui::Frontend>{
                                                                 std::make_unique<NamedFrontend>("web")};
                                                         }}};
    CHECK(chosen(unconditional, {"app"}, morphUi(std::nullopt)) == "web");
}

TEST_CASE("ui::selectFrontend: a named frontend is used even when it is not usable", "[ui]") {
    CHECK(chosen(qtThenTui(false, true), {"app", "--ui=qt"}, morphUi(std::nullopt)) == "qt");
}

TEST_CASE("ui::selectFrontend: an unknown or unbuilt name is an error that lists the built frontends", "[ui]") {
    Args args{"app", "--ui=web"};
    auto const selected = ui::selectFrontend(qtThenTui(), args, morphUi(std::nullopt));
    REQUIRE_FALSE(selected.has_value());
    CHECK(selected.error().kind == ui::FrontendError::Kind::UnknownName);
    CHECK(selected.error().requested == "web");
    CHECK(selected.error().built == Args{"qt", "tui"});
    CHECK(selected.error().message() == "morph::ui: no frontend named 'web' is built (built: qt, tui)");
    CHECK(selectionError(qtThenTui(), {"app"}, morphUi("web")) ==
          "morph::ui: no frontend named 'web' is built (built: qt, tui)");
}

TEST_CASE("ui::selectFrontend: nothing usable is an error", "[ui]") {
    CHECK(selectionError(qtThenTui(false, false), {"app"}, morphUi(std::nullopt)) ==
          "morph::ui: no built frontend is usable here (built: qt, tui)");
    CHECK(selectionError({}, {"app"}, morphUi(std::nullopt)) ==
          "morph::ui: no built frontend is usable here (built: none)");
}

TEST_CASE("ui::selectFrontend: --ui without a name is an error", "[ui]") {
    CHECK(selectionError(qtThenTui(), {"app", "--ui"}, morphUi("tui")) ==
          "morph::ui: --ui needs a frontend name (built: qt, tui)");
    CHECK(selectionError(qtThenTui(), {"app", "--ui="}, morphUi("tui")) ==
          "morph::ui: --ui needs a frontend name (built: qt, tui)");
}

TEST_CASE("ui::selectFrontend: an option that makes nothing, or has no make, is an error", "[ui]") {
    std::vector<ui::FrontendOption> const broken{
        {.name = "tui", .usable = [] { return true; }, .make = [] { return std::unique_ptr<ui::Frontend>{}; }}};
    CHECK(selectionError(broken, {"app"}, morphUi(std::nullopt)) == "morph::ui: frontend 'tui' could not be made");
    std::vector<ui::FrontendOption> const unmakeable{{.name = "tui", .usable = {}, .make = {}}};
    CHECK(selectionError(unmakeable, {"app"}, morphUi(std::nullopt)) == "morph::ui: frontend 'tui' could not be made");
}

TEST_CASE("ui::selectFrontend: an option after --ui is not its name", "[ui]") {
    CHECK(selectionError(qtThenTui(), {"app", "--ui", "--verbose"}, morphUi("tui")) ==
          "morph::ui: --ui needs a frontend name (built: qt, tui)");
    CHECK(selectionError(qtThenTui(), {"app", "--ui", "--", "tui"}, morphUi("tui")) ==
          "morph::ui: --ui needs a frontend name (built: qt, tui)");
}

TEST_CASE("ui::selectFrontend: -- ends option parsing", "[ui]") {
    CHECK(chosen(qtThenTui(), {"app", "--", "--ui=tui"}, morphUi(std::nullopt)) == "qt");
    CHECK(chosen(qtThenTui(), {"app", "--ui=tui", "--", "--ui=qt"}, morphUi(std::nullopt)) == "tui");
    CHECK(chosen(qtThenTui(), {"app", "--", "--ui"}, morphUi(std::nullopt)) == "qt");
}

TEST_CASE("ui::selectFrontend: an empty --ui= followed by a named one takes the named one", "[ui]") {
    CHECK(chosen(qtThenTui(), {"app", "--ui=", "--ui=tui"}, morphUi(std::nullopt)) == "tui");
}

TEST_CASE("ui::selectFrontend: an empty environment reader is never read", "[ui]") {
    CHECK(chosen(qtThenTui(), {"app"}, ui::EnvironmentReader{}) == "qt");
    CHECK(chosen(qtThenTui(), {"app", "--ui=tui"}, ui::EnvironmentReader{}) == "tui");
}

// Mutations: in selectFrontend, erase only the last used argument; erase them front to back (the positions shift and
// the wrong arguments go).
TEST_CASE("ui::selectFrontend: the --ui arguments it used are removed, and the rest keep their order", "[ui]") {
    Args args{"app", "--verbose", "--ui", "tui", "file", "--ui=qt", "--", "--ui=web"};
    auto const selected = ui::selectFrontend(qtThenTui(), args, morphUi(std::nullopt));
    REQUIRE(selected.has_value());
    CHECK((*selected)->name() == "qt");
    CHECK(args == Args{"app", "--verbose", "file", "--", "--ui=web"});
}

// Mutation: in selectFrontend, erase the used arguments before the frontend is found and made.
TEST_CASE("ui::selectFrontend: an error leaves the arguments as they were", "[ui]") {
    Args unknown{"app", "--ui=web", "file"};
    CHECK_FALSE(ui::selectFrontend(qtThenTui(), unknown, morphUi(std::nullopt)).has_value());
    CHECK(unknown == Args{"app", "--ui=web", "file"});
    Args unusable{"app", "--ui", "tui"};
    std::vector<ui::FrontendOption> const broken{{.name = "tui", .usable = {}, .make = {}}};
    CHECK_FALSE(ui::selectFrontend(broken, unusable, morphUi(std::nullopt)).has_value());
    CHECK(unusable == Args{"app", "--ui", "tui"});
}

TEST_CASE("ui::processEnvironment: reads the process environment", "[ui]") {
    constexpr char const* kVariable = "MORPH_UI_TEST_PROCESS_ENVIRONMENT";
    auto const env = ui::processEnvironment();
    CHECK_FALSE(env(kVariable).has_value());
    {
        ScopedVariable const variable{kVariable, "tui"};
        CHECK(env(kVariable) == std::optional<std::string>{"tui"});
    }
    CHECK_FALSE(env(kVariable).has_value());
}

// ── runApp and the destruction order ───────────────────────────────────────

// Mutations: in runApp, close the source before resetting the shell ("close" comes before "shell"); keep the shell
// until runApp returns (it outlives "close").
TEST_CASE("ui::runApp: the shell goes first, then the source's connections, then the runtime", "[ui]") {
    Events events;
    RecordingBackend backend;
    FakeSource source{events};
    source.immediate.emplace_back(bundle());
    TestFrontend frontend{events, tracedShells(backend, events), [&](TestContext&) {
                              events.push_back("loop: " + backend.dump());
                              return 7;
                          }};
    int const exitCode = frontend.run(source);
    CHECK(exitCode == 7);
    CHECK(events == Events{"open", "mount demo", "loop: Text#1 role=Normal text=demo\n", "shell", "close", "runtime"});
    CHECK(backend.all("Text").empty());
}

// Mutation: in runApp, let the loop's exception leave before the teardown.
TEST_CASE("ui::runApp: a loop that throws still tears down in order, then rethrows", "[ui]") {
    Events events;
    RecordingBackend backend;
    FakeSource source{events};
    source.immediate.emplace_back(bundle());
    TestFrontend frontend{events, tracedShells(backend, events), [&](TestContext&) -> int {
                              events.emplace_back("loop");
                              throw std::runtime_error{"loop failed"};
                          }};
    CHECK_THROWS_AS(frontend.run(source), std::runtime_error);
    CHECK(events == Events{"open", "mount demo", "loop", "shell", "close", "runtime"});
}

// A source that connects later delivers from inside the loop, on the owner.
TEST_CASE("ui::runApp: a source that delivers from the loop mounts the shell there", "[ui]") {
    Events events;
    RecordingBackend backend;
    FakeSource source{events};
    TestFrontend frontend{events, tracedShells(backend, events), [&](TestContext&) {
                              events.emplace_back("loop starts");
                              source.deliver(bundle());
                              events.push_back("loop: " + backend.dump());
                              return 0;
                          }};
    CHECK(frontend.run(source) == 0);
    CHECK(events == Events{"open", "loop starts", "mount demo", "loop: Text#1 role=Normal text=demo\n", "shell",
                           "close", "runtime"});
}

// Mutation: in runApp's ready, hold the run's state strongly (the late delivery mounts into a dead runtime).
TEST_CASE("ui::runApp: a ready that comes after the run ended mounts nothing", "[ui]") {
    Events events;
    RecordingBackend backend;
    FakeSource source{events};
    source.keepReadyAfterClose = true;
    TestFrontend frontend{events, tracedShells(backend, events), [](TestContext&) { return 0; }};
    CHECK(frontend.run(source) == 0);
    source.deliver(bundle());
    CHECK(events == Events{"open", "close", "runtime"});
    CHECK(backend.log().empty());
}

// Mutation: in runApp's ready, drop the `opened` check (the second delivery replaces the shell).
TEST_CASE("ui::runApp: a second ready is ignored", "[ui]") {
    Events events;
    RecordingBackend backend;
    FakeSource source{events};
    source.immediate.emplace_back(bundle());
    source.immediate.emplace_back(ui::Bundle{.applicationId = "other"});
    TestFrontend frontend{events, tracedShells(backend, events), [](TestContext&) { return 0; }};
    CHECK(frontend.run(source) == 0);
    CHECK(events == Events{"open", "mount demo", "shell", "close", "runtime"});
}

TEST_CASE("ui::runApp: a source that cannot connect hands the shell factory the error", "[ui]") {
    Events events;
    RecordingBackend backend;
    FakeSource source{events};
    source.immediate.emplace_back(
        std::unexpected{ui::ConnectError{.kind = ui::ConnectError::Kind::NoUiService, .message = "no UI here"}});
    TestFrontend frontend{events, tracedShells(backend, events), [](TestContext&) { return 3; }};
    CHECK(frontend.run(source) == 3);
    CHECK(events == Events{"open", "connect error: no UI here", "close", "runtime"});
}

// Mutation: in runApp's ready, let the factory's exception escape into the source.
TEST_CASE("ui::runApp: a shell factory that throws quits with 1, and its exception leaves after the teardown",
          "[ui]") {
    Events events;
    FakeSource source{events};
    source.immediate.emplace_back(bundle());
    std::optional<int> quitWith;
    TestFrontend frontend{
        events,
        [](ui::AppContext&, std::expected<ui::Bundle, ui::ConnectError>) -> std::unique_ptr<ui::MountedShell> {
            throw std::runtime_error{"no shell"};
        },
        [&](TestContext& context) {
            quitWith = context.exitCode();
            events.emplace_back("loop");
            return 0;
        }};
    std::string caught;
    try {
        static_cast<void>(frontend.run(source));
    } catch (std::runtime_error const& error) {
        caught = error.what();
    }
    CHECK(caught == "no shell");
    CHECK(quitWith == std::optional<int>{1});
    CHECK(events == Events{"open", "loop", "close", "runtime"});
}

TEST_CASE("ui::runApp: an empty shell factory or loop is refused before the source is opened", "[ui]") {
    Events events;
    FakeSource source{events};
    TestContext context;
    CHECK_THROWS_AS(ui::runApp(context, source, ui::ShellFactory{}, [] { return 0; }), std::invalid_argument);
    RecordingBackend backend;
    CHECK_THROWS_AS(ui::runApp(context, source, tracedShells(backend, events), std::function<int()>{}),
                    std::invalid_argument);
    CHECK(events.empty());
}

TEST_CASE("ui::AppSource: switchBackend reports through done", "[ui]") {
    Events events;
    FakeSource source{events};
    std::optional<bool> switched;
    source.switchBackend(ui::Backend{.name = "local"},
                         [&switched](std::expected<void, std::string> done) { switched = done.has_value(); });
    CHECK(switched == std::optional<bool>{true});
    CHECK(events == Events{"switch to local"});
    CHECK(ui::Backend{.name = "local"} == ui::Backend{.name = "local"});
}

// ── The frontend's scheduler drives a query's timed refresh ────────────────

// A frontend's scheduler is what a query's `refreshEvery` runs on. Mutations: build the query without the context's
// scheduler (it is refused); fetch on a tick whatever is in flight (a fourth call while the third is pending).
TEST_CASE("ui::AppContext: refreshEvery runs on the frontend's scheduler and skips a tick while pending", "[ui]") {
    TestContext context;
    morph::testing::FakeServer<std::string> server{context.executor()};
    morph::reactive::Query<Lookup, std::string> const query{
        context.runtime(), server.via<Lookup>(), [] { return std::optional{Lookup{1}}; },
        morph::reactive::QueryOptions{.scheduler = &context.scheduler(), .refreshEvery = 1000ms}};
    REQUIRE(server.calls() == 1);
    server.resolve(0, "one");
    context.owner().runAll();
    context.manualScheduler().advance(1000ms);
    CHECK(server.calls() == 2);  // the tick re-fetched
    context.manualScheduler().advance(1000ms);
    CHECK(server.calls() == 2);  // the second call is still in flight: the tick waits
    server.resolve(1, "two");
    context.owner().runAll();
    CHECK(query.value() == std::optional<std::string>{"two"});
    context.manualScheduler().advance(1000ms);
    CHECK(server.calls() == 3);
}
