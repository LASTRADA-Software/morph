// SPDX-License-Identifier: Apache-2.0

#include "tui/session.hpp"

#include <chrono>
#include <coroutine>
#include <core/async/Cancellation.hpp>
#include <core/async/ExecutorContext.hpp>
#include <core/async/WhenAny.hpp>
#include <core/net/EventLoop.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <exception>
#include <memory>
#include <morph/core/logger.hpp>
#include <morph/tui/frontend.hpp>
#include <optional>
#include <utility>
#include <variant>

namespace morph::tui::detail {

using ::core::tui::EventResult;

namespace {

/// How often a spinning Busy advances.
constexpr std::chrono::milliseconds kAnimationPeriod{100};

/// Resumes the awaiting coroutine in a later turn of the loop, after every task posted to the executor before it.
///
/// The frame is borrowed: the coroutine must stay alive until it resumes, which `whenAny` guarantees for the pump,
/// since it waits for every child it started. Only a run that ends by an exception leaves the frame gone with the
/// resumption still queued, and then the session is gone too, so the resumption is dropped.
class NextTurn {
public:
    NextTurn(LoopExecutor& executor, ::core::net::EventLoop& loop, std::weak_ptr<bool> alive) noexcept
        : _executor{&executor}, _loop{&loop}, _alive{std::move(alive)} {}
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> awaiting) const {
        _executor->post([loop = _loop, alive = _alive, awaiting] {
            if (!alive.expired()) {
                loop->submit(awaiting);
            }
        });
    }
    void await_resume() const noexcept {}

private:
    LoopExecutor* _executor;
    ::core::net::EventLoop* _loop;
    std::weak_ptr<bool> _alive;
};

/// Ctrl+C. In raw mode it arrives as a key, not as SIGINT.
[[nodiscard]] bool isInterrupt(::core::tui::KeyEvent const& key) noexcept {
    return ::core::tui::withoutLockKeys(key.modifiers) == ::core::tui::Modifier::Ctrl &&
           (key.codepoint == U'c' || key.key == ::core::tui::keyCodeFromCodepoint(U'c'));
}

[[nodiscard]] bool isEscape(::core::tui::KeyEvent const& key) noexcept {
    return key.codepoint == 0 && key.key == ::core::tui::KeyCode::Escape;
}

/// Routes the input runtime's interrupt (SIGINT) to a session for as long as this lives. The runtime outlives the
/// session, so the route must not.
class InterruptRoute {
public:
    InterruptRoute(::core::tui::runtime::TuiRuntime& input, Session& session) : _input{&input} {
        _input->setInterruptHandler([&session] { session.quit(kInterruptExitCode); });
    }
    ~InterruptRoute() { _input->setInterruptHandler(nullptr); }
    InterruptRoute(InterruptRoute const&) = delete;
    InterruptRoute& operator=(InterruptRoute const&) = delete;
    InterruptRoute(InterruptRoute&&) = delete;
    InterruptRoute& operator=(InterruptRoute&&) = delete;

private:
    ::core::tui::runtime::TuiRuntime* _input;
};

}  // namespace

Session::Session(exec::IoLoop& loop, LoopExecutor& executor, ::core::tui::Screen& screen,
                 ::core::tui::runtime::TuiRuntime& input)
    : _io{&loop},
      _executor{&executor},
      _screen{&screen},
      _input{&input},
      _runtime{executor, reactive::RuntimeOptions{.afterFlush = [this] { requestDraw(); }}},
      _scheduler{loop.loop(), executor},
      _backend{screen},
      _quitSignal{loop.loop(), ::core::async::AsyncQueueOptions{}} {}

Session::~Session() = default;

int Session::run(ui::ApplicationFactory const& factory) {
    InterruptRoute const route{*_input, *this};
    return ui::runApplication(*this, _backend, factory, [this] { return loop(); });
}

int Session::loop() {
    {
        ::core::async::ExecutorScope const scope{_executor->coreExecutor()};
        draw();
    }
    auto const exitCode = _input->blockOn(serve());
    _animation.cancel();
    return exitCode;
}

void Session::quit(int exitCode) {
    bool expected = false;
    if (_quitting.compare_exchange_strong(expected, true)) {
        _exitCode.store(exitCode);
        _quitSignal.close();
    }
}

::core::async::Task<int> Session::serve() {
    static_cast<void>(co_await ::core::async::whenAny(pump(), awaitQuit()));
    co_return _exitCode.load();
}

// Input that ends (a closed pipe, EOF on stdin) ends the run with exit code 0; a cancellation from whenAny,
// because quit() won the race, unwinds. A quit() ends the run before the next event, even one already read.
//
// Events read together are buffered, and nextEvent() hands them over without leaving the turn, so the pump waits
// for a later turn after each one: the flush and the draw the event posted run first, and every event is handled
// against a fitted, drawn tree. Otherwise a press whose pointer capture core::tui ended could still take a release
// that arrives in the same read, before any fit noticed. The wait ignores cancellation: it ends in the next turn
// regardless, and the pump stops there.
::core::async::Task<void> Session::pump() {
    while (!_quitting.load()) {
        std::optional<::core::tui::InputEvent> event;
        try {
            event = co_await _input->nextEvent();
        } catch (::core::async::OperationCancelled const&) {
            if (_input->inputClosed()) {
                co_return;
            }
            throw;
        }
        handle(*event);
        co_await NextTurn{*_executor, _io->loop(), _alive};
    }
}

::core::async::Task<void> Session::awaitQuit() { static_cast<void>(co_await _quitSignal.pop()); }

// One dispatch per event, with the loop executor current so reactive writes in a handler are on the runtime's
// owner. A handler that throws is logged; the application keeps running.
void Session::handle(::core::tui::InputEvent const& event) {
    ::core::async::ExecutorScope const scope{_executor->coreExecutor()};
    try {
        if (auto const* key = std::get_if<::core::tui::KeyEvent>(&event)) {
            handleKey(*key, event);
        } else {
            static_cast<void>(_screen->dispatchEvent(event));
        }
    } catch (std::exception const& failure) {
        ::morph::log::logError("[tui] an input handler threw: {}", failure.what());
    } catch (...) {
        ::morph::log::logError("[tui] an input handler threw an unknown exception");
    }
    requestDraw();
}

void Session::handleKey(::core::tui::KeyEvent const& key, ::core::tui::InputEvent const& event) {
    if (isInterrupt(key)) {
        quit(kInterruptExitCode);
        return;
    }
    auto const result = _screen->dispatchEvent(event);
    bool const tab = key.key == ::core::tui::KeyCode::Tab;
    bool const back = ::core::tui::hasModifier(key.modifiers, ::core::tui::Modifier::Shift);
    if (result == EventResult::FocusNext || (result == EventResult::Ignored && tab && !back)) {
        _backend.focusNext();
    } else if (result == EventResult::FocusPrev || (result == EventResult::Ignored && tab && back)) {
        _backend.focusPrev();
    } else if (result == EventResult::Ignored && isEscape(key)) {
        // A focused widget ends a drag on Esc itself; with nothing focused no widget sees the key.
        static_cast<void>(_backend.endDrag());
    }
}

// One draw per loop turn, however many flushes and events asked for one in the turn before.
void Session::requestDraw() {
    if (_drawPosted) {
        return;
    }
    _drawPosted = true;
    _executor->post([this, alive = std::weak_ptr<bool>{_alive}] {
        if (alive.expired()) {
            return;
        }
        _drawPosted = false;
        draw();
    });
}

// Fitted first: a resize reaches the tree, open dialogs are centred, and a press whose pointer capture ended
// without its release is over. Then the focus: content that arrived since the last frame (a dialog that opened,
// a first focusable widget) takes it when nothing else holds it.
void Session::draw() {
    _backend.fit();
    _backend.focusFirst();
    _screen->draw();
    if (_backend.animating()) {
        if (!_animation.active()) {
            _animation = _scheduler.every(kAnimationPeriod, [this] {
                _backend.advanceAnimation();
                requestDraw();
            });
        }
    } else {
        _animation.cancel();
    }
}

}  // namespace morph::tui::detail
