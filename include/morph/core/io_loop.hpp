// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cassert>
#include <core/async/ExecutorContext.hpp>
#include <core/net/EventLoop.hpp>
#include <core/net/PlatformLoop.hpp>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

#include "executor.hpp"
#include "logger.hpp"
#include "profiler.hpp"

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define MORPH_IO_LOOP_HOST_DRIVEN 1
#else
#include <thread>
#endif

/// @file
/// `IoLoop` — the one I/O loop an application runs, and the owner of every
/// socket, timer and probe built on it.
///
/// `morph::net::SocketBackend`, `morph::net::SocketServer`,
/// `TimeoutScheduler` and `offline::NetworkMonitor` own no thread. Each keeps
/// its state on a `core::net::PlatformLoop` and touches it only in tasks that
/// loop runs; its public verbs post to the loop and return. An application
/// constructs one `IoLoop` and hands it to every one of them, so one thread
/// carries every connection, every deadline and the connectivity probe.
///
/// @par Who turns the loop
/// - **`IoLoopDriver::OwnThread`** (native default): the `IoLoop` owns one
///   thread that runs the loop until the `IoLoop` is destroyed.
/// - **`IoLoopDriver::Caller`**: no thread is started. The constructing thread
///   is the loop's and turns it — `loop().runOnce()`, `loop().runUntilIdle()`
///   or a `blockOn()` — so a terminal UI and its sockets, timers and bridge
///   callbacks share that one thread. No other thread may turn it.
/// - **Single-threaded WebAssembly** (`__EMSCRIPTEN__` without
///   `__EMSCRIPTEN_PTHREADS__`): there is no thread to start whichever driver is
///   asked for. The loop is host-driven — the browser's timer pumps it — and
///   the one thread there is is always the loop's, so a verb runs its body
///   inline.

namespace morph::exec {

/// @brief Who turns an `IoLoop`.
enum class IoLoopDriver : std::uint8_t {
    /// The `IoLoop` starts one thread that runs the loop until it is destroyed.
    OwnThread,
    /// No thread is started: the constructing thread turns the loop, and is the
    /// loop's between turns as well as inside them. No other thread may turn
    /// it; a debug build asserts when a task posted to it runs on another.
    Caller,
};

/// @brief Owns a `core::net::PlatformLoop` and, for `IoLoopDriver::OwnThread`
///        natively, the one thread that runs it.
///
/// Cross-thread surface: `post()`, `runAndWait()`, `runningHere()`, `driver()`
/// and `weak()`. Everything else about the loop — timers, sockets, flows — is
/// touched only in tasks it runs, or, for a `Caller` loop, on its driving
/// thread.
///
/// @par Lifetime
/// Must outlive every component built on it: a component's destructor runs
/// its close on the loop and waits for it, which a stopped loop would never
/// run. Destroy the components first, then the `IoLoop`. A `Caller` loop and
/// its components are destroyed on the driving thread, outside a turn.
class IoLoop {
    /// The loop, shared with the thread that runs it, so that an `IoLoop`
    /// destroyed on its own thread can let that thread finish the turn it is in.
    struct Impl {
        ::core::net::PlatformLoop loop;
#ifndef MORPH_IO_LOOP_HOST_DRIVEN
        /// The thread that turns a `Caller` loop; no thread for `OwnThread`.
        std::thread::id caller;
#endif
    };

public:
    /// @brief A non-owning handle that posts to the loop only while it exists.
    ///
    /// For a callback that another executor runs and that may outlive the
    /// component that armed it — a `RemoteServer` reply arriving on a pool
    /// thread after its transport has gone.
    class Weak {
    public:
        /// @brief Posts @p task if the loop still exists.
        /// @tparam F A copyable callable taking no arguments.
        /// @param task What to run on the loop thread.
        /// @return `false` if the loop is gone and @p task was dropped.
        template <class F>
        bool post(F&& task) const {
            std::shared_ptr<Impl> const impl = _impl.lock();
            if (!impl) {
                return false;
            }
            impl->loop.post(guarded(*impl, std::forward<F>(task)));
            return true;
        }

    private:
        friend class IoLoop;
        explicit Weak(std::weak_ptr<Impl> impl) : _impl{std::move(impl)} {}
        std::weak_ptr<Impl> _impl;
    };

#ifndef MORPH_IO_LOOP_HOST_DRIVEN
    /// @brief Creates the loop and, with `IoLoopDriver::OwnThread`, starts the
    ///        thread that runs it.
    /// @param driver Who turns the loop. `Caller` starts no thread and makes the
    ///        constructing thread the loop's.
    explicit IoLoop(IoLoopDriver driver = IoLoopDriver::OwnThread) : _impl{std::make_shared<Impl>()}, _driver{driver} {
        if (driver == IoLoopDriver::Caller) {
            _impl->caller = std::this_thread::get_id();
        } else {
            _thread = std::thread{[impl = _impl] {
                // One name for every IoLoop thread: an application runs one, and
                // its sockets, deadlines and probe all turn on it.
                MORPH_THREAD_NAME("morph.io");
                impl->loop.run();
            }};
        }
    }

    /// @brief Stops the loop and joins its thread; a `Caller` loop is
    ///        destroyed in place.
    ///
    /// Work still queued is dropped by the loop's own teardown, not run. On the
    /// loop's own thread — a task that dropped the last owner — an `OwnThread`
    /// loop cannot join: it asks the loop to stop and lets the thread end with
    /// the turn it is in, the loop kept alive by the thread's own share of it.
    /// A `Caller` loop has no thread to stop: destroyed outside a turn nothing
    /// is running it, so its teardown is serialised with dispatch. Destroyed
    /// inside one of its own turns, that turn would return into a destroyed
    /// loop; an assertion refuses it.
    ~IoLoop() {
        if (_driver == IoLoopDriver::Caller) {
            assert(!::morph::exec::runningOn(static_cast<::core::async::IExecutor const&>(_impl->loop)) &&
                   "a caller-driven IoLoop must be destroyed outside its own turns");
            return;
        }
        _impl->loop.stop();
        if (_thread.get_id() == std::this_thread::get_id()) {
            _thread.detach();
        } else if (_thread.joinable()) {
            _thread.join();
        }
    }
#else
    /// @brief Creates the loop. Starts no thread whichever driver is asked for:
    ///        the browser's timer pumps it.
    /// @param driver Recorded and reported by `driver()`; both drivers behave
    ///        alike here.
    explicit IoLoop(IoLoopDriver driver = IoLoopDriver::OwnThread)
        : _impl{std::make_shared<Impl>()}, _driver{driver} {}

    /// @brief Destroys the loop, dropping whatever it still holds.
    ~IoLoop() = default;
#endif

    IoLoop(const IoLoop&) = delete;
    IoLoop& operator=(const IoLoop&) = delete;
    IoLoop(IoLoop&&) = delete;
    IoLoop& operator=(IoLoop&&) = delete;

    /// @brief The loop itself, for a component arming timers or opening
    ///        sockets from inside one of its tasks, and for the driving thread
    ///        of a `Caller` loop to turn it.
    /// @return The platform loop; valid for this object's lifetime.
    [[nodiscard]] ::core::net::EventLoop& loop() noexcept { return _impl->loop; }

    /// @brief Who turns this loop.
    /// @return The driver passed at construction.
    [[nodiscard]] IoLoopDriver driver() const noexcept { return _driver; }

    /// @brief Whether the calling thread may touch the loop's state directly.
    ///
    /// Natively that is the loop's own `ExecutorScope`, stated for every turn,
    /// and — for a `Caller` loop — also the driving thread between turns, where
    /// nothing else can be running the loop. Under single-threaded WebAssembly
    /// there is one thread, so the answer is always yes.
    /// @return True inside one of the loop's tasks, or on a `Caller` loop's
    ///         driving thread.
    [[nodiscard]] bool runningHere() const noexcept {
#ifndef MORPH_IO_LOOP_HOST_DRIVEN
        // An OwnThread loop's `caller` is the default id, which names no thread.
        return ::morph::exec::runningOn(static_cast<::core::async::IExecutor const&>(_impl->loop)) ||
               std::this_thread::get_id() == _impl->caller;
#else
        return true;
#endif
    }

    /// @brief Queues @p task to run on the loop thread, in a later turn.
    ///
    /// A throw out of @p task is logged and swallowed: one escaping a turn
    /// would end the loop's thread.
    /// @tparam F A copyable callable taking no arguments.
    /// @param task What to run.
    template <class F>
    void post(F&& task) {
        _impl->loop.post(guarded(*_impl, std::forward<F>(task)));
    }

    /// @brief Runs @p task on the loop and returns once it has run.
    ///
    /// Inline where `runningHere()` holds, so a task of the loop — or a
    /// `Caller` loop's driving thread — never waits on itself; inline on a
    /// `Caller` loop's driving thread between turns, it runs inside the loop's
    /// `ExecutorScope`, so `runningOn()` agrees with `runningHere()` there. For
    /// teardown and for the few verbs that must answer with the loop's own
    /// result.
    ///
    /// A throw out of @p task reaches the caller, inline or not. A task the
    /// loop drops unrun — it was destroyed first — ends the wait rather than
    /// hanging it, and the call returns without @p task having run.
    /// @tparam F A copyable callable taking no arguments.
    /// @param task What to run.
    template <class F>
    void runAndWait(F&& task) {
        if (runningHere()) {
            if (::morph::exec::runningOn(static_cast<::core::async::IExecutor const&>(_impl->loop))) {
                std::forward<F>(task)();
            } else {
                ::core::async::ExecutorScope const scope{_impl->loop};
                std::forward<F>(task)();
            }
            return;
        }
        // The posted task holds the only share of the promise, so a task the
        // loop drops unrun breaks it, and that is what ends the wait.
        auto promise = std::make_shared<std::promise<std::exception_ptr>>();
        std::future<std::exception_ptr> finished = promise->get_future();
        post([task = std::forward<F>(task), done = std::move(promise)]() mutable {
            std::exception_ptr failure;
            try {
                task();
            } catch (...) {
                failure = std::current_exception();
            }
            done->set_value(std::move(failure));
        });
        std::exception_ptr failure;
        try {
            failure = finished.get();
        } catch (const std::future_error&) {  // NOLINT(bugprone-empty-catch)
            // Broken: dropped unrun, so there is nothing left to wait for.
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    /// @brief A handle that posts only while this loop exists.
    /// @return The weak handle.
    [[nodiscard]] Weak weak() const { return Weak{_impl}; }

private:
    /// Wraps @p task to log a throw rather than let it end the turn, and, in a
    /// debug build, to refuse a `Caller` loop turned by a thread other than the
    /// one that constructed it: two threads taking turns would race every
    /// component's state, which `runningHere()` tells each of them it owns.
    template <class F>
    static auto guarded([[maybe_unused]] Impl const& impl, F&& task) {
#ifndef MORPH_IO_LOOP_HOST_DRIVEN
        return [caller = impl.caller, task = std::forward<F>(task)]() mutable noexcept {
            assert((caller == std::thread::id{} || caller == std::this_thread::get_id()) &&
                   "a caller-driven IoLoop is turned only by the thread that constructed it");
            static_cast<void>(caller);
            runLogged(task);
        };
#else
        return [task = std::forward<F>(task)]() mutable noexcept { runLogged(task); };
#endif
    }

    template <class F>
    static void runLogged(F& task) noexcept {
        try {
            task();
        } catch (const std::exception& exc) {
            ::morph::log::logError("[io-loop] task threw: {}", exc.what());
        } catch (...) {
            ::morph::log::logError("[io-loop] task threw an unknown exception");
        }
    }

    std::shared_ptr<Impl> _impl;
    IoLoopDriver _driver;
#ifndef MORPH_IO_LOOP_HOST_DRIVEN
    std::thread _thread;
#endif
};

}  // namespace morph::exec
