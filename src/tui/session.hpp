// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <core/async/AsyncQueue.hpp>
#include <core/async/Task.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/runtime/TuiRuntime.hpp>
#include <memory>
#include <morph/core/io_loop.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/tui/backend.hpp>
#include <morph/tui/loop_executor.hpp>
#include <morph/ui/frontend.hpp>
#include <string_view>

#include "tui/scheduler.hpp"

namespace morph::tui::detail {

/// One run of the TUI frontend: the AppContext the application is built with, the input pump, one draw per loop
/// turn, and the quit signal the pump races. Lives on the loop's (the calling) thread.
///
/// Member order is teardown order reversed: the animation timer goes first, then the backend (every widget is
/// already gone), the scheduler (before the loop its timers are armed on), and the runtime last. The application
/// and its mounted view never outlive `run`.
class Session final : public ui::AppContext {
public:
    Session(exec::IoLoop& loop, LoopExecutor& executor, ::core::tui::Screen& screen,
            ::core::tui::runtime::TuiRuntime& input);
    ~Session() override;
    Session(Session const&) = delete;
    Session& operator=(Session const&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    /// Builds the application, mounts its view and pumps input until quit() or the end of input, through
    /// `ui::runApplication`: the view and the application are destroyed before this returns, so before the runtime.
    [[nodiscard]] int run(ui::ApplicationFactory const& factory);

    reactive::Runtime& runtime() override { return _runtime; }
    exec::IExecutor& executor() override { return *_executor; }
    ui::Scheduler& scheduler() override { return _scheduler; }
    exec::IoLoop* ioLoop() override { return _io; }
    void quit(int exitCode) override;
    [[nodiscard]] std::string_view frontendName() const override { return "tui"; }

private:
    /// The loop `ui::runApplication` runs with the view mounted.
    [[nodiscard]] int loop();
    [[nodiscard]] ::core::async::Task<int> serve();
    [[nodiscard]] ::core::async::Task<void> pump();
    [[nodiscard]] ::core::async::Task<void> awaitQuit();
    void handle(::core::tui::InputEvent const& event);
    void handleKey(::core::tui::KeyEvent const& key, ::core::tui::InputEvent const& event);
    void requestDraw();
    void draw();

    exec::IoLoop* _io;
    LoopExecutor* _executor;
    ::core::tui::Screen* _screen;
    ::core::tui::runtime::TuiRuntime* _input;
    /// Expires with the session; read by every task it posts, which the loop may reach after the session is gone.
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    reactive::Runtime _runtime;
    LoopScheduler _scheduler;
    Backend _backend;
    ::core::async::AsyncQueue<int> _quitSignal;
    std::atomic<int> _exitCode{0};
    std::atomic<bool> _quitting{false};
    bool _drawPosted = false;
    reactive::TimerHandle _animation;
};

}  // namespace morph::tui::detail
