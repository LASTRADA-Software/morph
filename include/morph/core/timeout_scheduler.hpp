// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <core/net/EventLoop.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/platform/Clock.hpp>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define MORPH_TIMEOUT_SCHEDULER_HOST_DRIVEN 1
#else
#include <thread>
#include <vector>
#endif

#include "logger.hpp"

/// @file
/// `TimeoutScheduler` — "run this callback once, in N milliseconds, unless
/// cancelled first" — over core-cpp's event-loop timers.
///
/// @par One class, two ways of driving its loop
/// The deadlines live in a `core::net::PlatformLoop`, and the one thing that
/// differs between builds is who turns it:
/// - **Native:** the scheduler owns a `std::thread` that runs the loop.
///   `schedule()` and `cancel()` may be called from any thread; they record
///   the request under a mutex and hand the loop a batch to arm or disarm.
///   Callbacks run on that thread, so a callback must be prepared to run
///   concurrently with the caller.
/// - **Single-threaded WebAssembly** (`__EMSCRIPTEN__` without
///   `__EMSCRIPTEN_PTHREADS__`): there is no thread to start — Qt for
///   WebAssembly is `wasm_singlethread` here and no `-pthread` is passed, so
///   `std::thread`'s constructor would throw. The loop is host-driven: the
///   browser's own timer pumps it, and `schedule()` and `cancel()` arm and
///   retire the timer directly. Callbacks run on the main thread, the one the
///   Qt event loop and every `QtExecutor`-posted completion already run on.
///
/// @par What the two builds share
/// - `cancel()` releases the callback, and everything it captured, before it
///   returns — the entry lives in this class's own `pending` map, not in the
///   loop — and retires the loop's timer as well.
/// - The destructor drops every pending callback without firing it.
/// - A callback that throws is logged through `morph::log` and swallowed.
///
/// @par What differs
/// - **Cancelling a callback that has already started.** Native: `cancel()`
///   cannot stop it and does not wait for it — the entry is taken out of
///   `pending` before the callback is invoked, so a racing `cancel()` finds
///   nothing and returns while the callback is still running. WebAssembly:
///   the case cannot arise, because the callback and `cancel()` run on the
///   same thread. A caller that must work in both builds cannot rely on the
///   second.
/// - **Destruction.** Native: the destructor stops the loop and joins its
///   thread, so no callback is in flight afterwards. WebAssembly: nothing to
///   join; the timers are retired and the loop is destroyed. A browser timer
///   already scheduled to pump it finds it gone and runs nothing: from
///   core-cpp 0.3.0 each pump carries a weak reference to the loop, where
///   0.2.1's wrote into the freed loop.

namespace morph::async::detail {

/// @brief Invokes a callback once after a delay, unless cancelled first.
///
/// Neither `Bridge` nor `RemoteServer` is bound to a specific `IExecutor`
/// with a delayed-post primitive, so each owns one of these. Used by
/// `RemoteServer` to enforce `LimitPolicy::executeTimeout` (server-side —
/// see `docs/spec/core/backend.md`) and by `Bridge::setExecuteDeadline`
/// (client-side — see `docs/spec/core/completion.md`). See this file's
/// `@file` comment for how the native and single-threaded WebAssembly builds
/// drive it.
class TimeoutScheduler {
public:
    /// @brief Opaque identifier for one scheduled callback.
    using Handle = std::uint64_t;

#ifndef MORPH_TIMEOUT_SCHEDULER_HOST_DRIVEN
    /// @brief Starts the thread that runs the scheduler's event loop.
    TimeoutScheduler() : _thread{[this] { _loop.run(); }} {}

    /// @brief Drops every pending callback without firing it, stops the loop
    ///        and joins its thread.
    ///
    /// The timers are retired on the loop's own thread, before it stops, so
    /// the loop is destroyed with nothing armed.
    ~TimeoutScheduler() {
        auto dropped = takePending();
        _loop.post([this] {
            disarmAll();
            _loop.stop();
        });
        _thread.join();
        // `dropped` is destroyed here, with the loop's thread gone and every
        // member still alive: a capture whose destructor calls back into this
        // scheduler finds it whole.
    }
#else
    /// @brief Creates the scheduler. Starts no thread: the browser's timer
    ///        pumps the loop.
    TimeoutScheduler() = default;

    /// @brief Drops every pending callback without firing it and retires the
    ///        loop's timers.
    ~TimeoutScheduler() {
        auto dropped = takePending();
        disarmAll();
        // `dropped` is destroyed here, with every member still alive.
    }
#endif

    TimeoutScheduler(const TimeoutScheduler&) = delete;
    TimeoutScheduler& operator=(const TimeoutScheduler&) = delete;
    TimeoutScheduler(TimeoutScheduler&&) = delete;
    TimeoutScheduler& operator=(TimeoutScheduler&&) = delete;

    /// @brief Schedules @p callback to run after @p delay, unless cancelled
    ///        first via `cancel()`.
    /// @param delay    Time to wait before firing.
    /// @param callback Invoked on the loop's thread (the main thread under
    ///                 single-threaded WebAssembly) if not cancelled in time.
    ///                 Exceptions it throws are logged and swallowed.
    /// @return Handle usable with `cancel()`.
    Handle schedule(std::chrono::milliseconds delay, std::function<void()> callback) {
        auto const deadline = _loop.clock().now() + delay;
        Handle handle{};
        {
            std::scoped_lock const lock{_mtx};
            handle = ++_nextHandle;
            _pending.emplace(handle, std::move(callback));
        }
        submit(Request{.handle = handle, .deadline = deadline});
        return handle;
    }

    /// @brief Cancels a previously scheduled callback: releases one that has
    ///        not started, and returns without waiting for one that has.
    ///
    /// Two cases, and telling them apart is the caller's business because the
    /// scheduler cannot:
    ///
    /// - **@p handle has not started.** Its callback — and anything it
    ///   captured — is released before this returns, the callback never runs,
    ///   and the loop's timer is retired.
    /// - **@p handle is already running** (native build only). The callback
    ///   was taken out of `pending` before it was invoked, so this call finds
    ///   nothing and **returns while the callback is still executing** on the
    ///   loop's thread. It is neither interrupted nor waited for.
    ///
    /// So `cancel()` returning does **not** mean "no callback is in flight".
    /// The only thing in this class that means that is `~TimeoutScheduler`,
    /// which joins the loop's thread. A caller must therefore keep every
    /// scheduled callback safe to run *after* its `cancel()`: both callbacks in
    /// this repository (`Bridge::executeVia`'s deadline and `RemoteServer`'s
    /// `LimitPolicy::executeTimeout`) capture a `shared_ptr` to the state they
    /// settle and settle it write-once, so a late run is an ignored duplicate
    /// rather than a use-after-free.
    ///
    /// Blocking here until the callback finished would be the wrong contract
    /// rather than a missing feature: a callback that posts back to the
    /// cancelling thread would deadlock it — the hazard
    /// `docs/spec/concurrency_and_lifetimes.md` names, and the same reason
    /// `CallbackScope` deliberately offers no block-until-drained.
    ///
    /// A no-op if @p handle already fired, is firing, or was already cancelled.
    /// @param handle Handle returned by a prior `schedule()` call.
    void cancel(Handle handle) {
        // Moved out under the lock and destroyed after it, so a capture whose
        // destructor calls back into this scheduler cannot deadlock on _mtx.
        std::function<void()> released;
        {
            std::scoped_lock const lock{_mtx};
            auto found = _pending.find(handle);
            if (found == _pending.end()) {
                return;
            }
            released = std::move(found->second);
            _pending.erase(found);
        }
        submit(Request{.handle = handle, .deadline = std::nullopt});
    }

private:
    /// One change for the loop to apply: arm @p handle's timer at a deadline,
    /// or, with none, retire it.
    struct Request {
        Handle handle;
        std::optional<::core::platform::SteadyTimePoint> deadline;
    };

    /// The state an armed loop timer hands back to `fire()`. Held in
    /// `_timers`, whose nodes do not move, so its address is stable for as
    /// long as the timer is armed.
    struct Timer {
        TimeoutScheduler* owner;
        Handle handle;
        ::core::net::TimerId id;
    };

    /// Applies @p request on the loop's thread. Natively that means queueing
    /// it and waking the loop once per batch rather than once per request —
    /// `Bridge` schedules and cancels one deadline per call.
    void submit(Request request) {
#ifndef MORPH_TIMEOUT_SCHEDULER_HOST_DRIVEN
        bool wake = false;
        {
            std::scoped_lock const lock{_mtx};
            wake = _requests.empty();
            _requests.push_back(request);
        }
        if (wake) {
            _loop.post([this] { applyRequests(); });
        }
#else
        apply(request);
#endif
    }

#ifndef MORPH_TIMEOUT_SCHEDULER_HOST_DRIVEN
    /// Loop thread: applies every request queued since the last batch.
    void applyRequests() {
        std::vector<Request> batch;
        {
            std::scoped_lock const lock{_mtx};
            batch.swap(_requests);
        }
        for (auto const& request : batch) {
            apply(request);
        }
    }
#endif

    /// Loop thread: arms or retires one timer.
    void apply(Request const& request) {
        if (!request.deadline) {
            disarm(request.handle);
            return;
        }
        {
            // Cancelled before the loop got to it: nothing to arm.
            std::scoped_lock const lock{_mtx};
            if (!_pending.contains(request.handle)) {
                return;
            }
        }
        auto [slot, inserted] =
            _timers.try_emplace(request.handle, Timer{.owner = this, .handle = request.handle, .id = {}});
        if (inserted) {
            slot->second.id = _loop.addTimer(*request.deadline, &TimeoutScheduler::fire, &slot->second);
        }
    }

    /// Loop thread: retires @p handle's timer, if it is still armed.
    void disarm(Handle handle) {
        auto found = _timers.find(handle);
        if (found == _timers.end()) {
            return;
        }
        static_cast<void>(_loop.cancelTimer(found->second.id));
        _timers.erase(found);
    }

    /// Empties `_pending` under the lock, so no timer can fire what it held,
    /// and hands its callbacks to the destructor to release outside it.
    std::unordered_map<Handle, std::function<void()>> takePending() {
        std::unordered_map<Handle, std::function<void()>> taken;
        std::scoped_lock const lock{_mtx};
        taken.swap(_pending);
        return taken;
    }

    /// Loop thread: retires every armed timer.
    void disarmAll() {
        for (auto const& entry : _timers) {
            static_cast<void>(_loop.cancelTimer(entry.second.id));
        }
        _timers.clear();
    }

    /// The loop's timer callback, on the loop's thread.
    /// @param state The `Timer` this timer was armed with.
    static void fire(void* state) {
        auto const& timer = *static_cast<Timer const*>(state);
        TimeoutScheduler& self = *timer.owner;
        Handle const handle = timer.handle;
        self._timers.erase(handle);  // `timer` is gone from here on

        std::function<void()> callback;
        {
            std::scoped_lock const lock{self._mtx};
            auto found = self._pending.find(handle);
            if (found == self._pending.end()) {
                return;  // cancelled after the timer came due
            }
            callback = std::move(found->second);
            self._pending.erase(found);
        }
        try {
            callback();
        } catch (const std::exception& exc) {
            ::morph::log::logError("[timeout-scheduler] callback threw: {}", exc.what());
        } catch (...) {
            ::morph::log::logError("[timeout-scheduler] callback threw unknown exception");
        }
    }

    /// Guards `_pending`, `_nextHandle` and, natively, `_requests`.
    std::mutex _mtx;
    std::unordered_map<Handle, std::function<void()>> _pending;
    Handle _nextHandle{0};
#ifndef MORPH_TIMEOUT_SCHEDULER_HOST_DRIVEN
    std::vector<Request> _requests;
#endif
    /// Loop thread only.
    std::unordered_map<Handle, Timer> _timers;
    /// Declared after everything its timers point into, so it is destroyed
    /// first.
    ::core::net::PlatformLoop _loop;
#ifndef MORPH_TIMEOUT_SCHEDULER_HOST_DRIVEN
    std::thread _thread;
#endif
};

}  // namespace morph::async::detail
