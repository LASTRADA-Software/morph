// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <functional>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_support.hpp"
#include "ui_echoing_backend.hpp"

namespace ui = morph::ui;

namespace {

using ui::testing::RecordingBackend;
using Events = std::vector<std::string>;

/// A context over a step executor and a manual scheduler, as a frontend's would be over its loop.
class TestContext final : public ui::AppContext {
public:
    morph::reactive::Runtime& runtime() override { return _runtime; }
    morph::exec::IExecutor& executor() override { return _owner; }
    ui::Scheduler& scheduler() override { return _scheduler; }
    morph::exec::IoLoop* ioLoop() override { return nullptr; }
    void quit(int exitCode) override { _exitCode = exitCode; }
    [[nodiscard]] std::string_view frontendName() const override { return "test"; }

private:
    morph::testing::StepExecutor _owner;
    morph::reactive::testing::ManualScheduler _scheduler;
    morph::reactive::Runtime _runtime{_owner};
    int _exitCode = 0;
};

/// An application whose destructor records what of its view and of the reactive graph is still alive.
class TracedApplication final : public ui::Application {
public:
    TracedApplication(ui::AppContext& context, RecordingBackend const& backend, Events& events)
        : _runtime{&context.runtime()}, _backend{&backend}, _events{&events}, _label{context.runtime(), "hello"} {}

    /// Records how many Text widgets and reactive nodes are alive while the application is destroyed.
    ~TracedApplication() override {
        try {
            _events->push_back("application destroyed: " + std::to_string(_backend->all("Text").size()) +
                               " text widgets, " + std::to_string(_runtime->core()->liveNodes()) + " nodes");
        } catch (...) {
            // A record that cannot be made empties the trace, so the test that reads it fails.
            _events->clear();
        }
    }

    TracedApplication(TracedApplication const&) = delete;
    TracedApplication& operator=(TracedApplication const&) = delete;
    TracedApplication(TracedApplication&&) = delete;
    TracedApplication& operator=(TracedApplication&&) = delete;

    [[nodiscard]] ui::Node view() override {
        return ui::text({.text = [this] { return _label.get(); }});
    }

private:
    morph::reactive::Runtime* _runtime;
    RecordingBackend const* _backend;
    Events* _events;
    morph::reactive::Signal<std::string> _label;
};

ui::ApplicationFactory tracedFactory(RecordingBackend const& backend, Events& events) {
    return [&backend, &events](ui::AppContext& context) -> std::unique_ptr<ui::Application> {
        return std::make_unique<TracedApplication>(context, backend, events);
    };
}

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
    int run(ui::ApplicationFactory const&) override { return 0; }

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

std::string chosen(std::vector<ui::FrontendOption> const& built, std::vector<char const*> args,
                   ui::EnvironmentReader const& env) {
    return std::string{ui::selectFrontend(built, static_cast<int>(args.size()), args.data(), env)->name()};
}

std::string selectionError(std::vector<ui::FrontendOption> const& built, std::vector<char const*> args,
                           ui::EnvironmentReader const& env) {
    try {
        static_cast<void>(ui::selectFrontend(built, static_cast<int>(args.size()), args.data(), env));
    } catch (ui::FrontendSelectionError const& error) {
        return error.what();
    }
    return "no error";
}

std::vector<ui::FrontendOption> qtThenTui(bool qtUsable = true, bool tuiUsable = true) {
    return {option("qt", qtUsable), option("tui", tuiUsable)};
}

}  // namespace

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

TEST_CASE("ui::selectFrontend: an unknown or unbuilt name lists the built frontends", "[ui]") {
    CHECK(selectionError(qtThenTui(), {"app", "--ui=web"}, morphUi(std::nullopt)) ==
          "morph::ui: no frontend named 'web' is built (built: qt, tui)");
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

TEST_CASE("ui::selectFrontend: an option that makes nothing is an error", "[ui]") {
    std::vector<ui::FrontendOption> const broken{
        {.name = "tui", .usable = [] { return true; }, .make = [] { return std::unique_ptr<ui::Frontend>{}; }}};
    CHECK(selectionError(broken, {"app"}, morphUi(std::nullopt)) == "morph::ui: frontend 'tui' could not be made");
}

TEST_CASE("ui::selectFrontend: an option with no make is an error", "[ui]") {
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

TEST_CASE("ui::runApplication: the mount dies before the application, and the application before it returns", "[ui]") {
    RecordingBackend backend;
    TestContext context;
    Events events;
    int const exitCode = ui::runApplication(context, backend, tracedFactory(backend, events), [&] {
        events.push_back("loop: " + backend.dump());
        return 7;
    });
    events.push_back("returned " + std::to_string(exitCode));
    CHECK(events == Events{"loop: Text#1 role=Normal text=hello\n", "application destroyed: 0 text widgets, 1 nodes",
                           "returned 7"});
    CHECK(context.runtime().core()->liveNodes() == 0);
}

TEST_CASE("ui::runApplication: a loop that throws still tears down in order", "[ui]") {
    RecordingBackend backend;
    TestContext context;
    Events events;
    CHECK_THROWS_AS(ui::runApplication(context, backend, tracedFactory(backend, events),
                                       [&]() -> int {
                                           events.emplace_back("loop");
                                           throw std::runtime_error{"loop failed"};
                                       }),
                    std::runtime_error);
    CHECK(events == Events{"loop", "application destroyed: 0 text widgets, 1 nodes"});
    CHECK(backend.all("Text").empty());
    CHECK(context.runtime().core()->liveNodes() == 0);
}

TEST_CASE("ui::runApplication: a mount that throws destroys the application before the exception leaves", "[ui]") {
    morph::testing::EchoingBackend backend;
    backend.setOnCreate([] { throw std::runtime_error{"the backend could not make the widget"}; });
    TestContext context;
    Events events;
    bool looped = false;
    try {
        static_cast<void>(ui::runApplication(context, backend, tracedFactory(backend.recording(), events), [&] {
            looped = true;
            return 0;
        }));
        events.emplace_back("returned");
    } catch (std::runtime_error const& error) {
        events.push_back(std::string{"caught: "} + error.what());
    }
    CHECK(events ==
          Events{"application destroyed: 0 text widgets, 1 nodes", "caught: the backend could not make the widget"});
    CHECK_FALSE(looped);
    CHECK(context.runtime().core()->liveNodes() == 0);
}

TEST_CASE("ui::runApplication: a factory that makes no application is an error, and nothing is mounted", "[ui]") {
    RecordingBackend backend;
    TestContext context;
    bool looped = false;
    auto const none = [](ui::AppContext&) { return std::unique_ptr<ui::Application>{}; };
    auto const loop = [&looped] {
        looped = true;
        return 0;
    };
    std::string message;
    try {
        static_cast<void>(ui::runApplication(context, backend, none, loop));
    } catch (std::invalid_argument const& error) {
        message = error.what();
    }
    CHECK(message == "morph::ui: frontend 'test': the application factory made no application");
    CHECK_FALSE(looped);
    CHECK(backend.log().empty());
    CHECK_THROWS_AS(ui::runApplication(context, backend, ui::ApplicationFactory{}, loop), std::invalid_argument);
    CHECK_FALSE(looped);
}

TEST_CASE("ui::runApplication: an empty loop is refused before anything is made", "[ui]") {
    RecordingBackend backend;
    TestContext context;
    Events events;
    CHECK_THROWS_AS(ui::runApplication(context, backend, tracedFactory(backend, events), std::function<int()>{}),
                    std::invalid_argument);
    CHECK(events.empty());
    CHECK(backend.log().empty());
}
