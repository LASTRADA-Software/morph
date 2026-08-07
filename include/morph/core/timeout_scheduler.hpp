// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "logger.hpp"

namespace morph::async::detail {

/// @brief Background scheduler that invokes a callback once after a delay, unless cancelled first.
///
/// Neither `Bridge` nor `RemoteServer` is bound to a specific `IExecutor`
/// with a delayed-post primitive, so a single dedicated thread per instance
/// tracks pending deadlines and fires callbacks when they elapse. Used by
/// `RemoteServer` to enforce `LimitPolicy::executeTimeout` (server-side —
/// see `docs/spec/core/backend.md`) and by `Bridge::setExecuteDeadline`
/// (client-side — see `docs/spec/core/completion.md`).
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

}  // namespace morph::async::detail
