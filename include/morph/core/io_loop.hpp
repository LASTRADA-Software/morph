// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/net/EventLoop.hpp>
#include <core/net/PlatformLoop.hpp>
#include <exception>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

#include "executor.hpp"
#include "logger.hpp"

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
/// - **Native:** the `IoLoop` owns one thread that runs the loop until the
///   `IoLoop` is destroyed.
/// - **Single-threaded WebAssembly** (`__EMSCRIPTEN__` without
///   `__EMSCRIPTEN_PTHREADS__`): there is no thread to start. The loop is
///   host-driven — the browser's timer pumps it — and the one thread there is
///   is always the loop's, so a verb runs its body inline.

namespace morph::exec {

/// @brief Owns a `core::net::PlatformLoop` and, natively, the one thread that
///        runs it.
///
/// Cross-thread surface: `post()`, `runAndWait()`, `runningHere()` and
/// `weak()`. Everything else about the loop — timers, sockets, flows — is
/// touched only in tasks it runs.
///
/// @par Lifetime
/// Must outlive every component built on it: a component's destructor posts
/// its close to the loop and waits for it, which a stopped loop would never
/// run. Destroy the components first, then the `IoLoop`.
class IoLoop {
    /// The loop, shared with the thread that runs it, so that an `IoLoop`
    /// destroyed on its own thread can let that thread finish the turn it is in.
    struct Impl {
        ::core::net::PlatformLoop loop;
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
            impl->loop.post(guarded(std::forward<F>(task)));
            return true;
        }

    private:
        friend class IoLoop;
        explicit Weak(std::weak_ptr<Impl> impl) : _impl{std::move(impl)} {}
        std::weak_ptr<Impl> _impl;
    };

#ifndef MORPH_IO_LOOP_HOST_DRIVEN
    /// @brief Creates the loop and starts the thread that runs it.
    IoLoop() : _impl{std::make_shared<Impl>()}, _thread{[impl = _impl] { impl->loop.run(); }} {}

    /// @brief Stops the loop and joins its thread.
    ///
    /// Work still queued is dropped by the loop's own teardown, not run. On the
    /// loop's own thread — a task that dropped the last owner — it cannot
    /// join: it asks the loop to stop and lets the thread end with the turn it
    /// is in, the loop kept alive by the thread's own share of it.
    ~IoLoop() {
        _impl->loop.stop();
        if (_thread.get_id() == std::this_thread::get_id()) {
            _thread.detach();
        } else if (_thread.joinable()) {
            _thread.join();
        }
    }
#else
    /// @brief Creates the loop. Starts no thread: the browser's timer pumps it.
    IoLoop() : _impl{std::make_shared<Impl>()} {}

    /// @brief Destroys the loop, dropping whatever it still holds.
    ~IoLoop() = default;
#endif

    IoLoop(const IoLoop&) = delete;
    IoLoop& operator=(const IoLoop&) = delete;
    IoLoop(IoLoop&&) = delete;
    IoLoop& operator=(IoLoop&&) = delete;

    /// @brief The loop itself, for a component arming timers or opening
    ///        sockets from inside one of its tasks.
    /// @return The platform loop; valid for this object's lifetime.
    [[nodiscard]] ::core::net::EventLoop& loop() noexcept { return _impl->loop; }

    /// @brief Whether the calling thread is inside one of the loop's tasks.
    ///
    /// Natively that is the loop's own `ExecutorScope`, stated for every turn.
    /// Under single-threaded WebAssembly there is one thread, so the answer is
    /// always yes.
    /// @return True where the loop's state may be touched directly.
    [[nodiscard]] bool runningHere() const noexcept {
#ifndef MORPH_IO_LOOP_HOST_DRIVEN
        return ::morph::exec::runningOn(static_cast<::core::async::IExecutor const&>(_impl->loop));
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
        _impl->loop.post(guarded(std::forward<F>(task)));
    }

    /// @brief Runs @p task on the loop and returns once it has run.
    ///
    /// Inline when the calling thread is already on the loop, so a task of the
    /// loop never waits on itself. For teardown and for the few verbs that must
    /// answer with the loop's own result. A task the loop drops unrun (it was
    /// destroyed first) ends the wait rather than hanging it.
    /// @tparam F A copyable callable taking no arguments.
    /// @param task What to run.
    template <class F>
    void runAndWait(F&& task) {
        if (runningHere()) {
            std::forward<F>(task)();
            return;
        }
        auto done = std::make_shared<std::promise<void>>();
        std::future<void> finished = done->get_future();
        post([task = std::forward<F>(task), done]() mutable {
            task();
            done->set_value();
        });
        try {
            finished.get();
        } catch (const std::future_error&) {  // NOLINT(bugprone-empty-catch)
            // Dropped unrun, or it threw (logged by `post`): nothing is left to
            // wait for either way.
        }
    }

    /// @brief A handle that posts only while this loop exists.
    /// @return The weak handle.
    [[nodiscard]] Weak weak() const { return Weak{_impl}; }

private:
    template <class F>
    static auto guarded(F&& task) {
        return [task = std::forward<F>(task)]() mutable noexcept {
            try {
                task();
            } catch (const std::exception& exc) {
                ::morph::log::logError("[io-loop] task threw: {}", exc.what());
            } catch (...) {
                ::morph::log::logError("[io-loop] task threw an unknown exception");
            }
        };
    }

    std::shared_ptr<Impl> _impl;
#ifndef MORPH_IO_LOOP_HOST_DRIVEN
    std::thread _thread;
#endif
};

}  // namespace morph::exec
