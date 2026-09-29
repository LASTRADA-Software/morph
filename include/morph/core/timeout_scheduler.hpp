// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <chrono>
#include <core/net/EventLoop.hpp>
#include <core/platform/Clock.hpp>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

#include "../attributes.hpp"
#include "detail/owner_probe.hpp"
#include "io_loop.hpp"
#include "logger.hpp"

/// @file
/// `TimeoutScheduler` — "run this callback once, in N milliseconds, unless
/// cancelled first" — as timers on an `exec::IoLoop`.
///
/// @par One owner
/// Every pending callback, and the loop timer that fires it, lives on the
/// loop and is touched only in the loop's own tasks. `schedule()` and
/// `cancel()` post to the loop and return; callbacks run on the loop's thread
/// (the main thread under single-threaded WebAssembly, where the browser's
/// timer pumps the loop). A callback therefore runs concurrently with any
/// caller that is not on the loop, and must not block the loop: every socket
/// and every other timer built on the same `IoLoop` waits while it runs.
///
/// @par What a caller can rely on
/// - `cancel()` stops a callback that has not started. The callback, and
///   everything it captured, is released on the loop, shortly after `cancel()`
///   returns — not before.
/// - `cancel()` does not wait for a callback that has already started.
/// - The destructor drops every pending callback without firing it, and
///   returns only once no callback of this scheduler is running.
/// - A callback that throws is logged through `morph::log` and swallowed.

namespace morph::async::detail {

/// @brief Invokes a callback once after a delay, unless cancelled first.
///
/// Neither `Bridge` nor `RemoteServer` is bound to a specific `IExecutor`
/// with a delayed-post primitive, so each owns one of these. Used by
/// `RemoteServer` to enforce `LimitPolicy::executeTimeout` (server-side —
/// see `docs/spec/core/backend.md`) and by `Bridge::setExecuteDeadline`
/// (client-side — see `docs/spec/core/completion.md`).
///
/// Cross-thread surface: `schedule()` and `cancel()`, which post to the loop,
/// and the destructor, which runs its close on the loop and waits for it. The
/// handle `schedule()` returns is drawn from an atomic counter on the calling
/// thread, so it is known before the loop has armed anything.
class TimeoutScheduler {
public:
    /// @brief Opaque identifier for one scheduled callback.
    using Handle = std::uint64_t;

    /// @brief Builds the scheduler on @p loop, whose thread runs every callback.
    /// @param loop The application's loop. Borrowed: it must outlive this
    ///        scheduler.
    explicit TimeoutScheduler(::morph::exec::IoLoop& loop MORPH_LIFETIMEBOUND)
        : _loop{&loop}, _state{std::make_shared<State>(loop)} {}

    /// @brief Builds the scheduler on a loop of its own, for a caller with no
    ///        `IoLoop` to share. Natively that loop owns one thread.
    TimeoutScheduler()
        : _ownedLoop{std::make_unique<::morph::exec::IoLoop>()},
          _loop{_ownedLoop.get()},
          _state{std::make_shared<State>(*_ownedLoop)} {}

    /// @brief Drops every pending callback without firing it.
    ///
    /// The close runs on the loop — inline when this runs on the loop's own
    /// thread, posted and waited for otherwise — so no callback is running
    /// once it returns. The dropped callbacks are destroyed here, afterwards,
    /// with the scheduler still whole: a capture whose destructor calls
    /// `cancel()` on it finds it usable.
    ~TimeoutScheduler() {
        Pending dropped;
        _loop->runAndWait([state = _state, &dropped] { dropped = state->close(); });
    }

    TimeoutScheduler(const TimeoutScheduler&) = delete;
    TimeoutScheduler& operator=(const TimeoutScheduler&) = delete;
    TimeoutScheduler(TimeoutScheduler&&) = delete;
    TimeoutScheduler& operator=(TimeoutScheduler&&) = delete;

    /// @brief Schedules @p callback to run after @p delay, unless cancelled
    ///        first via `cancel()`.
    /// @param delay    Time to wait before firing, measured from this call.
    /// @param callback Invoked on the loop's thread if not cancelled in time.
    ///                 Exceptions it throws are logged and swallowed.
    /// @return Handle usable with `cancel()`.
    Handle schedule(std::chrono::milliseconds delay, std::function<void()> callback) {
        auto const deadline = _loop->loop().clock().now() + delay;
        Handle const handle = _nextHandle.fetch_add(1, std::memory_order_relaxed) + 1;
        _loop->post([state = _state, handle, deadline, callback = std::move(callback)]() mutable {
            // Exchanged, not moved: a moved-from `std::function` may keep a
            // copy of its target, and with it the captures `cancel()` is
            // meant to release.
            state->arm(handle, deadline, std::exchange(callback, nullptr));
        });
        return handle;
    }

    /// @brief Cancels a previously scheduled callback: stops one that has not
    ///        started, and returns without waiting for one that has.
    ///
    /// Posted, like `schedule()`, and applied in the order the two were
    /// called: a `cancel()` of a handle whose arming has not reached the loop
    /// yet still finds it, because the loop runs posts in order.
    ///
    /// - **@p handle has not started.** Its timer is retired and its callback
    ///   never runs. The callback and its captures are released on the loop,
    ///   after this returns.
    /// - **@p handle is already running.** The loop took it out of the pending
    ///   set before invoking it, so this finds nothing; it is neither
    ///   interrupted nor waited for.
    ///
    /// So `cancel()` returning does **not** mean "no callback is in flight".
    /// The only thing in this class that means that is `~TimeoutScheduler`. A
    /// caller must keep every scheduled callback safe to run *after* its
    /// `cancel()`: both callbacks in this repository (`Bridge::executeVia`'s
    /// deadline and `RemoteServer`'s `LimitPolicy::executeTimeout`) capture a
    /// `shared_ptr` to the state they settle and settle it write-once, so a late
    /// run is an ignored duplicate rather than a use-after-free.
    ///
    /// Waiting here for a running callback would be the wrong contract rather
    /// than a missing feature: a callback that posts back to the cancelling
    /// thread would deadlock it — the hazard
    /// `docs/spec/concurrency_and_lifetimes.md` names, and the same reason
    /// `CallbackScope` deliberately offers no block-until-drained.
    ///
    /// A no-op if @p handle already fired, is firing, or was already cancelled.
    /// @param handle Handle returned by a prior `schedule()` call.
    void cancel(Handle handle) {
        _loop->post([state = _state, handle] { state->disarm(handle); });
    }

private:
    struct State;

    /// One armed callback. Held in `State::pending`, whose nodes do not move,
    /// so its address is the loop timer's state for as long as it is armed.
    struct Entry {
        State* owner{nullptr};
        Handle handle{0};
        ::core::net::TimerId id{};
        std::function<void()> callback;
    };

    using Pending = std::unordered_map<Handle, Entry>;

    /// Everything the loop owns. Touched only on the loop's thread.
    struct State {
        explicit State(::morph::exec::IoLoop& ioLoop) : loop{ioLoop} {}

        /// Arms @p handle's timer, unless the scheduler has closed.
        void arm(Handle handle, ::core::platform::SteadyTimePoint deadline, std::function<void()> callback) {
            ::morph::exec::detail::noteOwner("TimeoutScheduler::schedule", loop.loop(), loop.runningHere());
            if (closed) {
                return;
            }
            auto [slot, inserted] = pending.try_emplace(
                handle, Entry{.owner = this, .handle = handle, .id = {}, .callback = std::move(callback)});
            if (inserted) {
                slot->second.id = loop.loop().addTimer(deadline, &State::fire, &slot->second);
            }
        }

        /// Retires @p handle's timer and releases its callback, if still pending.
        void disarm(Handle handle) {
            ::morph::exec::detail::noteOwner("TimeoutScheduler::cancel", loop.loop(), loop.runningHere());
            auto node = pending.extract(handle);
            if (node.empty()) {
                return;
            }
            static_cast<void>(loop.loop().cancelTimer(node.mapped().id));
        }

        /// Retires every timer and hands back what was pending.
        Pending close() {
            closed = true;
            for (auto const& entry : pending) {
                static_cast<void>(loop.loop().cancelTimer(entry.second.id));
            }
            return std::exchange(pending, {});
        }

        /// The loop's timer callback.
        /// @param entryPtr The `Entry` the timer was armed with.
        static void fire(void* entryPtr) {
            auto const& entry = *static_cast<Entry const*>(entryPtr);
            State& self = *entry.owner;
            // Out of the pending set before it runs, so a `cancel()` from
            // inside the callback, or from anywhere once it has started, finds
            // nothing. Nothing here touches `self` after the call: the
            // callback may destroy the scheduler.
            auto node = self.pending.extract(entry.handle);
            if (node.empty()) {
                return;
            }
            std::function<void()> const callback = std::move(node.mapped().callback);
            node = {};
            try {
                callback();
            } catch (const std::exception& exc) {
                ::morph::log::logError("[timeout-scheduler] callback threw: {}", exc.what());
            } catch (...) {
                ::morph::log::logError("[timeout-scheduler] callback threw unknown exception");
            }
        }

        ::morph::exec::IoLoop& loop;
        Pending pending;
        bool closed{false};
    };

    /// Present only for the owning constructor. First, so it is destroyed
    /// last, after the destructor has closed the state on it.
    std::unique_ptr<::morph::exec::IoLoop> _ownedLoop;
    ::morph::exec::IoLoop* _loop;
    std::atomic<Handle> _nextHandle{0};
    std::shared_ptr<State> _state;
};

}  // namespace morph::async::detail
