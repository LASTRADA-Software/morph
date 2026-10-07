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
#include <functional>
#include <memory>
#include <morph/reactive/detail/graph.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/tui/frontend.hpp>
#include <morph/ui/frontend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../owner_probe_recorder.hpp"
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

/// A frontend over scripted input and a frame-counting mock terminal.
struct Rig {
    explicit Rig(core::tui::Size size) : pipe{openPipe()}, source{pipe.get()}, terminal{makeOutput(size)} {}

    [[nodiscard]] morph::tui::Frontend frontend(core::tui::MouseTracking mouseMode = core::tui::MouseTracking::Drag) {
        return morph::tui::Frontend{
            morph::tui::FrontendConfig{.terminal = &terminal, .input = &source, .mouse = mouseMode}};
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

/// An application whose view a lambda builds; it keeps a timer and a counter for the test.
struct TestApp final : ui::Application {
    explicit TestApp(morph::reactive::Runtime& runtime) : count{runtime, 0} {}
    [[nodiscard]] ui::Node view() override { return build(); }

    std::function<ui::Node()> build;
    morph::reactive::Signal<int> count;
    morph::reactive::TimerHandle timer;
};

/// An application whose view is @p build, with nothing else.
ui::ApplicationFactory showing(std::function<ui::Node()> build) {
    return [build = std::move(build)](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
        auto app = std::make_unique<TestApp>(ctx.runtime());
        app->build = build;
        return app;
    };
}

}  // namespace

TEST_CASE("tui::Frontend: each input event is dispatched once: typing h, i reports \"hi\"", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    std::string seen;
    rig.source.pushEvents({key('h'), key('i')});
    rig.source.closeInput();
    auto const exitCode = rig.frontend().run([&](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
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
    auto const exitCode = rig.frontend().run([](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
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
    auto const exitCode = rig.frontend().run([](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
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
    static_cast<void>(rig.frontend().run([&](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
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
    static_cast<void>(rig.frontend().run([&](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
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
    static_cast<void>(rig.frontend().run([&](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
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
    static_cast<void>(rig.frontend().run([](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
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
    static_cast<void>(rig.frontend().run([](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
        auto app = std::make_unique<TestApp>(ctx.runtime());  // owns a Signal on the runtime
        app->build = [] { return ui::text({.text = "idle"}); };
        return app;
    }));
    CHECK(recorder.count(morph::reactive::detail::site::kRuntimeOutlived) == 0);
}

TEST_CASE("tui::frontendOption names the frontend tui, asks whether stdin is a terminal, and makes one",
          "[tui][frontend]") {
    auto const option = morph::tui::frontendOption();
    CHECK(option.name == "tui");
    CHECK(option.usable() == core::platform::isTerminal(core::platform::standardInput()));
    auto const frontend = option.make();
    REQUIRE(frontend != nullptr);
    CHECK(frontend->name() == "tui");
}

TEST_CASE("tui::Frontend: quit from the factory ends run with its code once the view is mounted", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    bool viewBuilt = false;
    auto const exitCode = rig.frontend().run([&viewBuilt](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
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
    CHECK_THROWS_AS(rig.frontend().run([](ui::AppContext&) -> std::unique_ptr<ui::Application> { return nullptr; }),
                    std::invalid_argument);
}

TEST_CASE("tui::Frontend: quit in a handler ends run before the next event is dispatched", "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    int clicks = 0;
    rig.source.pushEvents({special(KeyCode::Enter), special(KeyCode::Enter)});  // one read: both buffered at once
    rig.source.closeInput();  // ends the run with 0 if the click never quits
    auto const exitCode = rig.frontend().run([&clicks](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
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
    static_cast<void>(rig.frontend().run(showing([&rig, &framesAtClick] {
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
    static_cast<void>(
        rig.frontend().run([&rig, &clicks, &press](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
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
    static_cast<void>(rig.frontend().run(showing([&dropped] {
        auto const tenWide = ui::LayoutHints{.width = ui::Sizing::fixed(10), .height = {}};
        return ui::row(
            {.children = {ui::column({.children = {ui::text(
                                          {.text = "task",
                                           .common = {.dragKey = std::optional<ui::Key>{ui::Key{std::int64_t{7}}}}})},
                                      .common = {.layout = tenWide}}),
                          ui::column({.children = {},
                                      .common = {.layout = tenWide, .onDrop = [&dropped](ui::Key) { ++dropped; }}})},
             .gap = 2});
    })));
    CHECK(dropped == (escape ? 0 : 1));
}

TEST_CASE("tui::Frontend: asks the terminal for drag reports by default, or for the mode it is given",
          "[tui][frontend]") {
    Rig rig{{.width = 30, .height = 5}};
    rig.source.closeInput();
    SECTION("the default") {
        auto frontend =
            morph::tui::Frontend{morph::tui::FrontendConfig{.terminal = &rig.terminal, .input = &rig.source}};
        static_cast<void>(frontend.run(showing([] { return ui::text({.text = "idle"}); })));
        CHECK(rig.terminal.input().mouseTracking() == core::tui::MouseTracking::Drag);
    }
    SECTION("a given mode") {
        static_cast<void>(
            rig.frontend(core::tui::MouseTracking::Buttons).run(showing([] { return ui::text({.text = "idle"}); })));
        CHECK(rig.terminal.input().mouseTracking() == core::tui::MouseTracking::Buttons);
    }
}


TEST_CASE("tui::Frontend: a resize re-fits the view before the next event is handled", "[tui][frontend]") {
    using Type = core::tui::MouseEvent::Type;
    Rig rig{{.width = 20, .height = 3}};
    int clicks = 0;
    morph::reactive::TimerHandle grow;
    static_cast<void>(
        rig.frontend().run([&rig, &clicks, &grow](ui::AppContext& ctx) -> std::unique_ptr<ui::Application> {
            auto app = std::make_unique<TestApp>(ctx.runtime());
            app->build = [&clicks] {
                return ui::column({.children = {ui::spacer(), ui::button({.label = "Go", .onClick = [&clicks] {
                                                                               ++clicks;
                                                                           }})}});
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
