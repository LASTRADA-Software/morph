// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "logger.hpp"

namespace morph::exec {

/// @brief Abstract executor interface.
///
/// Concrete implementations decide *where* and *when* posted tasks run
/// (thread pool, main thread, Qt event loop, …).
struct IExecutor {
    virtual ~IExecutor() = default;

    /// @brief Schedules @p task for asynchronous execution.
    ///
    /// Thread-safe. The task is invoked at some point after this call returns.
    /// Any exception thrown by the task is silently swallowed unless the
    /// implementation documents otherwise.
    /// @param task Callable to execute.
    virtual void post(std::function<void()> task) = 0;
};

/// @brief Multi-threaded executor backed by a fixed-size thread pool.
///
/// Tasks are placed in a FIFO queue and consumed by worker threads.
/// Exceptions thrown by tasks are silently caught and discarded.
/// The destructor blocks until all worker threads have exited.
class ThreadPoolExecutor : public IExecutor {
public:
    /// @brief Constructs the pool with @p n worker threads.
    /// @param n Number of threads to spawn. Must be > 0.
    explicit ThreadPoolExecutor(std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            _workers.emplace_back([this] { loop(); });
        }
    }

    /// @brief Signals all workers to stop and joins them.
    ///
    /// Remaining queued tasks that have not started are dropped.
    ~ThreadPoolExecutor() override {
        {
            std::scoped_lock lock{_m};
            _stop = true;
        }
        _cv.notify_all();
        for (auto& worker : _workers) {
            worker.join();
        }
    }

    /// @brief Enqueues @p task for execution on one of the pool threads.
    /// @param task Callable to execute. Thread-safe.
    void post(std::function<void()> task) override {
        {
            std::scoped_lock lock{_m};
            _q.push(std::move(task));
        }
        _cv.notify_one();
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock{_m};
                _cv.wait(lock, [this] { return _stop || !_q.empty(); });
                if (_stop && _q.empty()) {
                    return;
                }
                task = std::move(_q.front());
                _q.pop();
            }
            try {
                task();
            } catch (...) {
            }
        }
    }
    std::mutex _m;
    std::condition_variable _cv;
    std::queue<std::function<void()>> _q;
    std::vector<std::thread> _workers;
    bool _stop = false;
};

/// @brief Single-thread executor intended for use on the calling (main) thread.
///
/// Tasks posted from other threads are collected and dispatched only when
/// the caller invokes `runFor()`. Useful in tests or event loops that have no
/// native dispatcher.
class MainThreadExecutor : public IExecutor {
public:
    /// @brief Enqueues @p task to be run on the next `runFor()` call.
    ///
    /// Thread-safe. The task is *not* executed immediately.
    /// @param task Callable to execute.
    void post(std::function<void()> task) override {
        {
            std::scoped_lock lock{_m};
            _q.push(std::move(task));
        }
        _cv.notify_all();
    }

    /// @brief Drains the task queue for up to @p timeout.
    ///
    /// Runs queued tasks one by one until the deadline is reached or the queue
    /// is empty. Exceptions thrown by tasks are logged and execution continues
    /// with the next task.
    ///
    /// Must be called from the thread that "owns" this executor.
    /// @param timeout Maximum wall-clock time to spend draining.
    void runFor(std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            std::function<void()> task;
            {
                std::unique_lock lock{_m};
                if (!_cv.wait_until(lock, deadline, [this] { return !_q.empty(); })) {
                    return;
                }
                task = std::move(_q.front());
                _q.pop();
            }
            try {
                task();
            } catch (const std::exception& exc) {
                ::morph::log::logError("[main-thread] callback threw: " + std::string{exc.what()});
            }
        }
    }

private:
    std::mutex _m;
    std::condition_variable _cv;
    std::queue<std::function<void()>> _q;
};

}  // namespace morph::exec
