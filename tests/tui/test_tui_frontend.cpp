// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/platform/SystemPipe.hpp>
#include <core/platform/Types.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/MouseTracking.hpp>
#include <core/tui/Rect.hpp>
#include <core/tui/Terminal.hpp>
#include <core/tui/TestHelpers.hpp>
#include <core/tui/runtime/testing/ScriptedInputSource.hpp>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <morph/reactive/detail/graph.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/testing/owner_probe_recorder.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/tui/frontend.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../test_support.hpp"
#include "tui_harness.hpp"

namespace ui = morph::ui;
using core::tui::KeyCode;
using core::tui::runtime::testing::ScriptedInputSource;
using morph::tui::testing::ResizableOutput;
using namespace std::chrono_literals;

namespace {

core::tui::InputEvent key(char character, core::tui::Modifier modifiers = core::tui::Modifier::None) {
    return core::tui::test::charKey(character, modifiers);
}

core::tui::InputEvent special(KeyCode code, core::tui::Modifier modifiers = core::tui::Modifier::None) {
    return core::tui::test::specialKey(code, modifiers);
}

/// A left-button mouse report at 0-based viewport @p cell.
core::tui::InputEvent mouse(core::tui::MouseEvent::Type type, core::tui::Point cell) {
    return core::tui::MouseEvent{.type = type, .button = 0, .x = cell.x + 1, .y = cell.y + 1};
}

std::unique_ptr<core::platform::SystemPipe> openPipe() {
    auto pipe = core::platform::createSystemPipe();
    REQUIRE(pipe.has_value());
    return std::move(*pipe);
}

/// An application whose view a lambda builds; it keeps a timer and a counter for the test.
struct TestApp {
    explicit TestApp(morph::reactive::Runtime& runtime) : count{runtime, 0} {}

    std::function<ui::Node()> build;
    morph::reactive::Signal<int> count;
    morph::reactive::TimerHandle timer;
};

/// Makes the application, given the running frontend's context.
using AppFactory = std::function<std::unique_ptr<TestApp>(ui::AppContext&)>;

/// An application whose view is @p build, with nothing else.
AppFactory showing(std::function<ui::Node()> build) {
    return [build = std::move(build)](ui::AppContext& ctx) -> std::unique_ptr<TestApp> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        app->build = build;
        return app;
    };
}

/// A source with nothing to connect to: ready during `open`.
class LocalSource final : public ui::AppSource {
public:
    void open(ui::AppContext& /*context*/,
              std::function<void(std::expected<ui::Bundle, ui::ConnectError>)> ready) override {
        ready(ui::Bundle{});
    }
    void switchBackend(ui::Backend /*backend*/, std::function<void(std::expected<void, std::string>)> done) override {
        done(std::unexpected{std::string{"no other backend"}});
    }
    void close() override {}
};

/// The application and its mounted view; the view goes first.
struct AppShell final : ui::MountedShell {
    std::unique_ptr<TestApp> app;
    std::optional<ui::Mounted> mounted;
    ~AppShell() override { mounted.reset(); }
    AppShell() = default;
    AppShell(AppShell const&) = delete;
    AppShell& operator=(AppShell const&) = delete;
    AppShell(AppShell&&) = delete;
    AppShell& operator=(AppShell&&) = delete;
};

/// The shell factory that makes @p factory's application and mounts its view; an application that is not made is
/// refused with std::invalid_argument.
morph::tui::ShellFactory mounting(AppFactory factory) {
    return [factory = std::move(factory)](
               ui::AppContext& context, ui::IViewBackend& backend,
               std::expected<ui::Bundle, ui::ConnectError> /*opened*/) -> std::unique_ptr<ui::MountedShell> {
        auto shell = std::make_unique<AppShell>();
        shell->app = factory(context);
        if (shell->app == nullptr) {
            throw std::invalid_argument{"the factory made no application"};
        }
        shell->mounted.emplace(context.runtime(), backend, shell->app->build());
        return shell;
    };
}

/// A frontend over scripted input and a frame-counting mock terminal.
struct Rig {
    explicit Rig(core::tui::Size size) : pipe{openPipe()}, source{pipe.get()}, terminal{makeOutput(size)} {}

    /// Runs the frontend over the scripted input until it ends, with @p factory's application mounted.
    int run(AppFactory factory, core::tui::MouseTracking mouseMode = core::tui::MouseTracking::Drag) {
        return runShell(mounting(std::move(factory)), mouseMode);
    }

    /// Runs the frontend over the scripted input until it ends, with the shell @p factory makes.
    int runShell(morph::tui::ShellFactory factory,
                 core::tui::MouseTracking mouseMode = core::tui::MouseTracking::Drag) {
        morph::tui::FrontendConfig config;
        config.mountShell = std::move(factory);
        config.terminal = &terminal;
        config.input = &source;
        config.mouse = mouseMode;
        morph::tui::Frontend frontend{std::move(config)};
        LocalSource local;
        return frontend.run(local);
    }

    std::unique_ptr<core::platform::SystemPipe> pipe;
    ScriptedInputSource source;
    ResizableOutput* output = nullptr;
    core::tui::Terminal terminal;

private:
    std::unique_ptr<core::tui::TerminalOutput> makeOutput(core::tui::Size size) {
        auto made = std::make_unique<ResizableOutput>(size);
        output = made.get();
        return made;
    }
};

}  // namespace

TEST_CASE("tui::Frontend: each input event is dispatched once: typing h, i reports \"hi\"", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    std::string seen;
    rig.source.pushEvents({key('h'), key('i')});
    rig.source.closeInput();
    auto const exitCode = rig.run([&](ui::AppContext& ctx) -> std::unique_ptr<TestApp> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        app->build = [&seen] {
            return ui::column(
                {.children = {ui::textInput({.onChange = [&seen](std::string text) { seen = std::move(text); }})}});
        };
        return app;
    });
    CHECK(exitCode == 0);
    CHECK(seen == "hi");
}

TEST_CASE("tui::Frontend: a timer-sent quit ends run with its exit code", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    auto const exitCode = rig.run([](ui::AppContext& ctx) -> std::unique_ptr<TestApp> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        app->build = [] { return ui::text({.text = "idle"}); };
        app->timer = ctx.scheduler().after(10ms, [context = &ctx] { context->quit(7); });
        return app;
    });
    CHECK(exitCode == 7);
}

TEST_CASE("tui::Frontend: Ctrl+C quits with the interrupt exit code 130", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    rig.source.pushEvents({key('c', core::tui::Modifier::Ctrl)});
    auto const exitCode = rig.run([](ui::AppContext& ctx) -> std::unique_ptr<TestApp> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        app->build = [] { return ui::text({.text = "idle"}); };
        app->timer =
            ctx.scheduler().after(2000ms, [context = &ctx] { context->quit(99); });  // only if Ctrl+C did nothing
        return app;
    });
    CHECK(exitCode == morph::tui::kInterruptExitCode);
    CHECK(morph::tui::kInterruptExitCode == 130);
}

TEST_CASE("tui::Frontend: Tab moves the focus through the tree", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    std::string clicks;
    rig.source.pushEvents(
        {special(KeyCode::Tab), special(KeyCode::Enter), special(KeyCode::Tab), special(KeyCode::Enter)});
    rig.source.closeInput();
    static_cast<void>(rig.run([&](ui::AppContext& ctx) -> std::unique_ptr<TestApp> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        app->build = [&clicks] {
            return ui::column({.children = {ui::button({.label = "A", .onClick = [&clicks] { clicks += 'A'; }}),
                                            ui::button({.label = "B", .onClick = [&clicks] { clicks += 'B'; }})}});
        };
        return app;
    }));
    CHECK(clicks == "BA");
}

TEST_CASE("tui::Frontend: Tab cycles inside an open dialog and never reaches the tree behind it", "[tui][frontend]") {
    Rig rig{{.width = 40, .height = 12}};
    std::string clicks;
    rig.source.pushEvents({special(KeyCode::Tab), special(KeyCode::Enter), special(KeyCode::Tab),
                           special(KeyCode::Enter), special(KeyCode::Tab), special(KeyCode::Enter)});
    rig.source.closeInput();
    static_cast<void>(rig.run([&](ui::AppContext& ctx) -> std::unique_ptr<TestApp> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        app->build = [&clicks] {
            auto const pressed = [&clicks](char label) { return [&clicks, label] { clicks += label; }; };
            return ui::column(
                {.children = {
                     ui::button({.label = "A", .onClick = pressed('A')}),
                     ui::button({.label = "B", .onClick = pressed('B')}),
                     ui::dialog(
                         {.open = true,
                          .title = "Q",
                          .child = ui::column({.children = {ui::button({.label = "C", .onClick = pressed('C')}),
                                                            ui::button({.label = "D", .onClick = pressed('D')})}})}),
                 }});
        };
        return app;
    }));
    CHECK(clicks == "DCD");
}

TEST_CASE("tui::Frontend: a key whose handler writes state draws exactly one frame", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    int framesAtClick = -1;
    int framesAtQuit = -1;
    rig.source.pushEvents({special(KeyCode::Enter)});
    morph::reactive::TimerHandle fallback;
    static_cast<void>(rig.run([&](ui::AppContext& ctx) -> std::unique_ptr<TestApp> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        auto* const self = app.get();
        auto* const context = &ctx;
        fallback = ctx.scheduler().after(2000ms, [context] { context->quit(99); });  // only if the click never came
        app->build = [&rig, &framesAtClick, &framesAtQuit, self, context] {
            return ui::column({.children = {
                                   ui::text({.text = [self] { return std::to_string(self->count.get()); }}),
                                   ui::button({.label = "Bump",
                                               .onClick =
                                                   [&rig, &framesAtClick, &framesAtQuit, self, context] {
                                                       framesAtClick = rig.output->frames();
                                                       self->count.set(self->count.peek() + 1);
                                                       self->timer = context->scheduler().after(
                                                           50ms, [&rig, &framesAtQuit, context] {
                                                               framesAtQuit = rig.output->frames();
                                                               context->quit(0);
                                                           });
                                                   }}),
                               }});
        };
        return app;
    }));
    CHECK(framesAtQuit - framesAtClick == 1);
}

TEST_CASE("tui::Frontend: a spinning Busy keeps frames coming", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    static_cast<void>(rig.run([](ui::AppContext& ctx) -> std::unique_ptr<TestApp> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        app->build = [] { return ui::busy({.active = true, .label = "Working"}); };
        app->timer = ctx.scheduler().after(350ms, [context = &ctx] { context->quit(0); });
        return app;
    }));
    CHECK(rig.output->frames() >= 3);
}

TEST_CASE("tui::Frontend: the application is destroyed before the runtime", "[tui][frontend]") {
    morph::testing::StepExecutor unrelated;
    morph::testing::OwnerProbeRecorder const recorder{unrelated.coreExecutor()};
    Rig rig{{.width = 30, .height = 5}};
    rig.source.closeInput();
    static_cast<void>(rig.run([](ui::AppContext& ctx) -> std::unique_ptr<TestApp> {
        auto app = std::make_unique<TestApp>(ctx.runtime());  // owns a Signal on the runtime
        app->build = [] { return ui::text({.text = "idle"}); };
        return app;
    }));
    CHECK(recorder.count(morph::reactive::detail::site::kRuntimeOutlived) == 0);
}

TEST_CASE("tui::frontendOption names the frontend tui, asks whether stdin is a terminal, and makes one",
          "[tui][frontend]") {
    auto const option = morph::tui::frontendOption({.mountShell = mounting(showing([] { return ui::text({}); }))});
    CHECK(option.name == "tui");
    CHECK(option.usable() == core::platform::isTerminal(core::platform::standardInput()));
    auto const frontend = option.make();
    REQUIRE(frontend != nullptr);
    CHECK(frontend->name() == "tui");
}

TEST_CASE("tui::Frontend: quit from the factory ends run with its code once the view is mounted", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    bool viewBuilt = false;
    auto const exitCode = rig.run([&viewBuilt](ui::AppContext& ctx) -> std::unique_ptr<TestApp> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        app->build = [&viewBuilt] {
            viewBuilt = true;
            return ui::text({.text = "idle"});
        };
        ctx.quit(5);
        return app;
    });
    CHECK(exitCode == 5);
    CHECK(viewBuilt);
}

TEST_CASE("tui::Frontend: a factory that makes no application is refused", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    rig.source.closeInput();
    CHECK_THROWS_AS(rig.run([](ui::AppContext&) -> std::unique_ptr<TestApp> { return nullptr; }),
                    std::invalid_argument);
}

TEST_CASE("tui::Frontend: quit in a handler ends run before the next event is dispatched", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    int clicks = 0;
    rig.source.pushEvents({special(KeyCode::Enter), special(KeyCode::Enter)});  // one read: both buffered at once
    rig.source.closeInput();  // ends the run with 0 if the click never quits
    auto const exitCode = rig.run([&clicks](ui::AppContext& ctx) -> std::unique_ptr<TestApp> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        app->build = [&clicks, context = &ctx] {
            return ui::button({.label = "Quit", .onClick = [&clicks, context] {
                                   ++clicks;
                                   context->quit(3);
                               }});
        };
        return app;
    });
    CHECK(exitCode == 3);
    CHECK(clicks == 1);
}

TEST_CASE("tui::Frontend: every input event is followed by its frame before the next one is handled",
          "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    std::vector<int> framesAtClick;
    rig.source.pushEvents({special(KeyCode::Enter), special(KeyCode::Enter)});  // one read: both buffered at once
    rig.source.closeInput();
    static_cast<void>(rig.run(showing([&rig, &framesAtClick] {
        return ui::button(
            {.label = "Go", .onClick = [&rig, &framesAtClick] { framesAtClick.push_back(rig.output->frames()); }});
    })));
    REQUIRE(framesAtClick.size() == 2);
    CHECK(framesAtClick.at(1) - framesAtClick.at(0) == 1);
}

TEST_CASE("tui::Frontend: a widget that appears while nothing has the focus takes it in the next frame",
          "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    int clicks = 0;
    morph::reactive::TimerHandle press;
    static_cast<void>(rig.run([&rig, &clicks, &press](ui::AppContext& ctx) -> std::unique_ptr<TestApp> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        auto* const self = app.get();
        app->build = [self, &clicks] {
            return ui::column(
                {.children = {ui::text({.text = "wait"}),
                              ui::button({.label = "Go",
                                          .onClick = [&clicks] { ++clicks; },
                                          .common = {.visible = [self] { return self->count.get() > 0; }}})}});
        };
        app->timer = ctx.scheduler().after(10ms, [self] { self->count.set(1); });
        // The user presses Enter once the button is on screen: its flush and its frame take a few loop turns,
        // never a wait, so they are done long before this.
        press = ctx.scheduler().after(60ms, [&rig] {
            rig.source.pushEvents({special(KeyCode::Enter)});
            rig.source.closeInput();
        });
        return app;
    }));
    CHECK(clicks == 1);
}

TEST_CASE("tui::Frontend: Esc ends a drag while no widget has the keyboard focus", "[tui][frontend]") {
    using Type = core::tui::MouseEvent::Type;
    Rig rig{{.width = 30, .height = 6}};
    int dropped = 0;
    std::vector<core::tui::InputEvent> events{mouse(Type::Press, {.x = 0, .y = 0}),
                                              mouse(Type::Move, {.x = 13, .y = 0})};
    bool escape = false;
    SECTION("Esc, then the release over the target: no drop") { escape = true; }
    SECTION("the release over the target without Esc drops") {}
    if (escape) {
        events.push_back(special(KeyCode::Escape));
    }
    events.push_back(mouse(Type::Release, {.x = 13, .y = 0}));
    rig.source.pushEvents(std::move(events));
    rig.source.closeInput();
    static_cast<void>(rig.run(showing([&dropped] {
        auto const tenWide = ui::LayoutHints{.width = ui::Sizing::fixed(10), .height = {}};
        return ui::row(
            {.children = {ui::column({.children = {ui::text(
                                          {.text = "task",
                                           .common = {.dragKey = std::optional<ui::Key>{ui::Key{std::int64_t{7}}}}})},
                                      .common = {.layout = tenWide}}),
                          ui::column(
                              {.children = {},
                               .common = {.layout = tenWide, .onDrop = [&dropped](ui::Key const&) { ++dropped; }}})},
             .gap = 2});
    })));
    CHECK(dropped == (escape ? 0 : 1));
}

TEST_CASE("tui::Frontend: asks the terminal for drag reports by default, or for the mode it is given",
          "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    rig.source.closeInput();
    SECTION("the default") {
        auto frontend = morph::tui::Frontend{
            morph::tui::FrontendConfig{.mountShell = mounting(showing([] { return ui::text({.text = "idle"}); })),
                                       .terminal = &rig.terminal,
                                       .input = &rig.source}};
        LocalSource local;
        static_cast<void>(frontend.run(local));
        CHECK(rig.terminal.input().mouseTracking() == core::tui::MouseTracking::Drag);
    }
    SECTION("a given mode") {
        static_cast<void>(
            rig.run(showing([] { return ui::text({.text = "idle"}); }), core::tui::MouseTracking::Buttons));
        CHECK(rig.terminal.input().mouseTracking() == core::tui::MouseTracking::Buttons);
    }
}

TEST_CASE("tui::Frontend: a resize re-fits the view before the next event is handled", "[tui][frontend]") {
    using Type = core::tui::MouseEvent::Type;
    Rig rig{{.width = 20, .height = 3}};
    int clicks = 0;
    morph::reactive::TimerHandle grow;
    static_cast<void>(rig.run([&rig, &clicks, &grow](ui::AppContext& ctx) -> std::unique_ptr<TestApp> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        app->build = [&clicks] {
            return ui::column(
                {.children = {ui::spacer(), ui::button({.label = "Go", .onClick = [&clicks] { ++clicks; }})}});
        };
        // The terminal grows, and the click on the button's new, bottom row arrives in the same read.
        grow = ctx.scheduler().after(10ms, [&rig] {
            rig.output->resize({.width = 20, .height = 6});
            rig.source.pushEvents({core::tui::ResizeEvent{.columns = 20, .rows = 6},
                                   mouse(Type::Press, {.x = 1, .y = 5}), mouse(Type::Release, {.x = 1, .y = 5})});
            rig.source.closeInput();
        });
        return app;
    }));
    CHECK(clicks == 1);
}

namespace {

/// A shell that records the backend's width class when it mounts and each change after, and shows nothing else.
struct WidthShell final : ui::MountedShell {
    WidthShell(ui::IViewBackend& backend, std::vector<std::string>& heard) : _backend{&backend} {
        heard.push_back(backend.widthClass());
        backend.setOnWidthClass([&heard](std::string const& widthClass) { heard.push_back(widthClass); });
    }
    ~WidthShell() override { _backend->setOnWidthClass({}); }
    WidthShell(WidthShell const&) = delete;
    WidthShell& operator=(WidthShell const&) = delete;
    WidthShell(WidthShell&&) = delete;
    WidthShell& operator=(WidthShell&&) = delete;

private:
    ui::IViewBackend* _backend;
};

}  // namespace

// Mutation: drop the session's refreshWidthClass after a resize (the listener hears nothing more).
TEST_CASE("tui::Frontend: the width class is the screen's columns, and a resize reports a new class",
          "[tui][frontend]") {
    Rig rig{{.width = 70, .height = 3}};
    std::vector<std::string> heard;
    morph::reactive::TimerHandle grow;
    static_cast<void>(
        rig.runShell([&rig, &heard, &grow](
                         ui::AppContext& context, ui::IViewBackend& backend,
                         std::expected<ui::Bundle, ui::ConnectError> /*opened*/) -> std::unique_ptr<ui::MountedShell> {
            grow = context.scheduler().after(10ms, [&rig] {
                rig.output->resize({.width = 100, .height = 3});
                rig.source.pushEvents({core::tui::ResizeEvent{.columns = 100, .rows = 3},
                                       core::tui::ResizeEvent{.columns = 120, .rows = 3}});
                rig.source.closeInput();
            });
            return std::make_unique<WidthShell>(backend, heard);
        }));
    // 70 columns are compact; 100 and then 120 are both medium, reported once.
    CHECK(heard == std::vector<std::string>{"compact", "medium"});
}
