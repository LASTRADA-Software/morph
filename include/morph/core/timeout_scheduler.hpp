// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <unordered_map>

/// @file
/// `TimeoutScheduler` — "run this callback once, in N milliseconds, unless
/// cancelled first" — in two builds of the same public API.
///
/// @par Why two builds
/// The ordinary build owns a dedicated `std::thread`. A **single-threaded
/// Emscripten** build cannot: Qt for WebAssembly is installed here as
/// `wasm_singlethread` (`.github/workflows/wasm-ladder.yml`) and
/// `cmake/morph_add_rung.cmake` passes no `-pthread`, so Emscripten's
/// non-pthread `pthread_create` stub fails and `std::thread`'s constructor
/// throws `std::system_error` ("thread constructor failed") — from inside
/// whatever completion callback happened to enable the deadline. Every WASM
/// client in this repository is Qt-event-loop driven and would hit this the
/// moment it called `Bridge::setExecuteDeadline` (which
/// `examples/common/gui/event_poller.hpp`'s constructor does
/// unconditionally, on every poll open).
///
/// So under `__EMSCRIPTEN__` without `__EMSCRIPTEN_PTHREADS__` this class is
/// built on `emscripten_async_call` — the browser's own `setTimeout` — and
/// fires its callbacks on the single main thread, i.e. on the same thread the
/// Qt event loop and every `QtExecutor`-posted completion callback already
/// run on. Deadlines still fire; nothing is silently disabled.
///
/// @par What differs between the two builds
/// - **Callback thread.** Threaded build: a private background thread, so a
///   callback must be prepared to run concurrently with the caller (the one
///   real callback in this codebase, `executeVia`'s, only touches a
///   `CompletionState`, which is itself mutex-guarded). Browser build: the
///   main thread, never concurrently with anything.
/// - **Cancellation.** Threaded build: the entry, its callback and everything
///   the callback captured are erased immediately. Browser build: identical
///   for the callback and its captures (the map entry is erased at once), but
///   the underlying browser timer is not itself cleared — it still fires at
///   its original deadline and finds nothing to do. Only a small ticket
///   allocation outlives `cancel()`, until that point.
/// - **Destruction.** Threaded build: the destructor joins its thread, so no
///   callback can be in flight afterwards. Browser build: nothing to join;
///   pending browser timers observe an expired `std::weak_ptr` to the
///   scheduler's state and return without invoking anything.
///
/// @warning The browser build has never been compiled or run in this
/// repository — no Emscripten toolchain is available where it was written.
/// Its only verification is the `ladder-wasm` CI compile gate. Stated plainly
/// here rather than smoothed over, exactly like `examples/TESTING.md`'s note
/// on the WASM clients themselves.

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define MORPH_TIMEOUT_SCHEDULER_BROWSER_TIMERS 1
#include <emscripten/emscripten.h>

#include <limits>
#include <memory>
#include <utility>
#else
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#endif

#include "logger.hpp"

namespace morph::async::detail {

#ifndef MORPH_TIMEOUT_SCHEDULER_BROWSER_TIMERS

/// @brief Background scheduler that invokes a callback once after a delay, unless cancelled first.
///
/// Neither `Bridge` nor `RemoteServer` is bound to a specific `IExecutor`
/// with a delayed-post primitive, so a single dedicated thread per instance
/// tracks pending deadlines and fires callbacks when they elapse. Used by
/// `RemoteServer` to enforce `LimitPolicy::executeTimeout` (server-side —
/// see `docs/spec/core/backend.md`) and by `Bridge::setExecuteDeadline`
/// (client-side — see `docs/spec/core/completion.md`). See this file's `@file`
/// comment for the single-threaded-WASM build of the same API.
class TimeoutScheduler {
public:
    /// @brief Opaque identifier for one scheduled callback.
    using Handle = std::uint64_t;

    /// @brief Starts the background thread.
    TimeoutScheduler() : _thread{[this] { run(); }} {}

    /// @brief Stops the background thread and joins it.
    ~TimeoutScheduler() {
        {
            std::scoped_lock const lock{_mtx};
            _stop = true;
        }
        _cv.notify_all();
        _thread.join();
    }

    TimeoutScheduler(const TimeoutScheduler&) = delete;
    TimeoutScheduler& operator=(const TimeoutScheduler&) = delete;
    TimeoutScheduler(TimeoutScheduler&&) = delete;
    TimeoutScheduler& operator=(TimeoutScheduler&&) = delete;

    /// @brief Schedules @p callback to run after @p delay on the scheduler's
    ///        background thread, unless cancelled first via `cancel()`.
    /// @param delay    Time to wait before firing.
    /// @param callback Invoked on the scheduler thread if not cancelled in time.
    ///                 Exceptions it throws are logged and swallowed.
    /// @return Handle usable with `cancel()`.
    Handle schedule(std::chrono::milliseconds delay, std::function<void()> callback) {
        auto const deadline = std::chrono::steady_clock::now() + delay;
        std::scoped_lock const lock{_mtx};
        Handle const handle = ++_nextHandle;
        auto iter = _entries.emplace(deadline, Entry{handle, std::move(callback)});
        _index[handle] = iter;
        _cv.notify_all();
        return handle;
    }

    /// @brief Cancels a previously scheduled callback immediately.
    ///
    /// If @p handle has not fired yet, its entry (and anything its callback
    /// captured) is erased right away — the caller does not have to wait for
    /// the original deadline for that memory to be released. A no-op if
    /// @p handle already fired or was already cancelled.
    /// @param handle Handle returned by a prior `schedule()` call.
    void cancel(Handle handle) {
        std::scoped_lock const lock{_mtx};
        auto found = _index.find(handle);
        if (found == _index.end()) {
            return;
        }
        _entries.erase(found->second);
        _index.erase(found);
    }

private:
    struct Entry {
        Handle handle;
        std::function<void()> callback;
    };

    void run() {
        std::unique_lock lock{_mtx};
        while (!_stop) {
            if (_entries.empty()) {
                _cv.wait(lock);
                continue;
            }
            auto const nextDeadline = _entries.begin()->first;
            _cv.wait_until(lock, nextDeadline);
            if (_stop) {
                break;
            }
            auto now = std::chrono::steady_clock::now();
            while (!_entries.empty() && _entries.begin()->first <= now) {
                auto iter = _entries.begin();
                Entry entry = std::move(iter->second);
                _index.erase(entry.handle);
                _entries.erase(iter);
                lock.unlock();
                try {
                    entry.callback();
                } catch (const std::exception& exc) {
                    ::morph::log::logError("[timeout-scheduler] callback threw: " + std::string{exc.what()});
                } catch (...) {
                    ::morph::log::logError("[timeout-scheduler] callback threw unknown exception");
                }
                lock.lock();
                now = std::chrono::steady_clock::now();
            }
        }
    }

    std::mutex _mtx;
    std::condition_variable _cv;
    std::multimap<std::chrono::steady_clock::time_point, Entry> _entries;
    std::unordered_map<Handle, std::multimap<std::chrono::steady_clock::time_point, Entry>::iterator> _index;
    Handle _nextHandle{0};
    bool _stop{false};
    std::thread _thread;
};

#else

/// @brief Single-threaded-Emscripten build of the same API, backed by the
///        browser's `setTimeout` (`emscripten_async_call`) instead of a
///        thread. See this file's `@file` comment for why it exists and
///        exactly how its behaviour differs.
class TimeoutScheduler {
public:
    /// @brief Opaque identifier for one scheduled callback.
    using Handle = std::uint64_t;

    /// @brief Creates the scheduler. Starts no thread — there is none to start.
    TimeoutScheduler() = default;

    /// @brief Drops every still-pending callback without firing it.
    ///
    /// Browser timers already queued outlive this object; each holds only a
    /// `std::weak_ptr` to `_state` and returns immediately once it expires,
    /// which is precisely at this destructor. Matches the threaded build's
    /// "`~TimeoutScheduler` drops pending entries without firing them".
    ~TimeoutScheduler() = default;

    TimeoutScheduler(const TimeoutScheduler&) = delete;
    TimeoutScheduler& operator=(const TimeoutScheduler&) = delete;
    TimeoutScheduler(TimeoutScheduler&&) = delete;
    TimeoutScheduler& operator=(TimeoutScheduler&&) = delete;

    /// @brief Schedules @p callback to run after @p delay on the main
    ///        (browser) thread, unless cancelled first via `cancel()`.
    /// @param delay    Time to wait before firing.
    /// @param callback Invoked on the main thread if not cancelled in time.
    ///                 Exceptions it throws are logged and swallowed.
    /// @return Handle usable with `cancel()`.
    Handle schedule(std::chrono::milliseconds delay, std::function<void()> callback) {
        Handle const handle = ++_state->nextHandle;
        _state->pending.emplace(handle, std::move(callback));
        // Owned by the browser timer, deleted by `fire` below whether or not
        // the entry is still live by then. A raw `new` rather than a
        // `unique_ptr` because the ownership genuinely crosses a C callback
        // boundary that cannot carry a smart pointer.
        auto* ticket = new Ticket{_state, handle};
        ::emscripten_async_call(&TimeoutScheduler::fire, ticket, clampMillis(delay));
        return handle;
    }

    /// @brief Cancels a previously scheduled callback immediately.
    ///
    /// If @p handle has not fired yet, its callback (and anything that
    /// callback captured) is released right away, exactly like the threaded
    /// build. The browser timer itself is left to elapse and find nothing —
    /// see the `@file` comment. A no-op if @p handle already fired or was
    /// already cancelled.
    /// @param handle Handle returned by a prior `schedule()` call.
    void cancel(Handle handle) { _state->pending.erase(handle); }

private:
    struct State {
        std::unordered_map<Handle, std::function<void()> > pending;
        Handle nextHandle{0};
    };

    struct Ticket {
        std::weak_ptr<State> state;
        Handle handle;
    };

    /// @brief @p delay as the `int` milliseconds `emscripten_async_call`
    ///        takes, saturating rather than wrapping (a `std::chrono`
    ///        duration can hold far more than an `int` can).
    ///
    /// @note This is a real, documented behavioural asymmetry from the
    /// threaded build, which honours the full `std::chrono::milliseconds`
    /// range unconditionally: a delay beyond `INT_MAX` ms (~24.85 days) fires
    /// at ~24.85 days here instead of at its true, much later requested time.
    /// `emscripten_async_call`'s `int` parameter is a hard platform
    /// constraint with no larger-range alternative to fall back to, so this
    /// is accepted rather than worked around. No caller in this codebase
    /// currently requests a deadline anywhere near that range.
    /// @param delay The requested delay.
    /// @return A non-negative millisecond count that fits in an `int`.
    [[nodiscard]] static int clampMillis(std::chrono::milliseconds delay) noexcept {
        auto const count = delay.count();
        if (count <= 0) {
            return 0;
        }
        if (count > static_cast<decltype(count)>(std::numeric_limits<int>::max())) {
            return std::numeric_limits<int>::max();
        }
        return static_cast<int>(count);
    }

    /// @brief The C callback the browser timer invokes.
    /// @param arg The `Ticket*` handed to `emscripten_async_call`; always
    ///        deleted here, whether or not its entry is still live.
    static void fire(void* arg) {
        std::unique_ptr<Ticket> const ticket{static_cast<Ticket*>(arg)};
        auto state = ticket->state.lock();
        if (!state) {
            return;
        }
        auto found = state->pending.find(ticket->handle);
        if (found == state->pending.end()) {
            return;  // cancelled before this timer elapsed
        }
        std::function<void()> callback = std::move(found->second);
        state->pending.erase(found);
        try {
            callback();
        } catch (const std::exception& exc) {
            ::morph::log::logError("[timeout-scheduler] callback threw: " + std::string{exc.what()});
        } catch (...) {
            ::morph::log::logError("[timeout-scheduler] callback threw unknown exception");
        }
    }

    /// @brief Held by `shared_ptr` so a browser timer that outlives this
    ///        object detects that fact instead of writing to freed storage —
    ///        the same weak-token pattern as `morph::bridge::Bridge::_liveness`.
    std::shared_ptr<State> _state{std::make_shared<State>()};
};

#endif

}  // namespace morph::async::detail
