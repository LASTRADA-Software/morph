// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace morph::offline {

/// @brief Configuration for `NetworkMonitor`.
///
/// Declared outside `NetworkMonitor` so that its default member initialisers
/// are fully parsed before any constructor default argument that names
/// `Config{}` is evaluated. Compilers (clang, GCC) reject `Config cfg = Config{}`
/// in a constructor default argument when `Config` is an incomplete nested type.
struct NetworkMonitorConfig {
    /// @brief Time between probe calls.
    std::chrono::milliseconds probeInterval = std::chrono::seconds{5};

    /// @brief Consecutive failures required before going offline.
    int failureThreshold = 3;

    /// @brief Consecutive successes required before going online.
    int onlineThreshold = 1;
};

/// @brief Background connectivity monitor that fires callbacks on state changes.
///
/// A dedicated thread calls the user-supplied probe function at regular intervals.
/// The monitor starts in the *online* state and transitions to *offline* only after
/// `failureThreshold` consecutive failures. It returns to *online* after
/// `onlineThreshold` consecutive successes.
///
/// The monitor is non-copyable and non-movable. Destroy it to stop monitoring.
class NetworkMonitor {
public:
    /// @brief Returns `true` when the network is reachable.
    using ProbeFunction = std::function<bool()>;

    /// @brief Called on connectivity state change.
    using Callback = std::function<void()>;

    /// @brief Alias for the configuration struct.
    using Config = NetworkMonitorConfig;

    /// @brief Starts the monitor and launches the probe thread.
    ///
    /// The monitor starts in the online state. The first `failureThreshold`
    /// consecutive probe failures must occur before `onOffline` is invoked.
    /// Callers that know the initial state is offline should use a probe that
    /// starts returning `false` immediately.
    ///
    /// @param probe     Callable that tests connectivity. Must not throw (exceptions are swallowed).
    /// @param onOffline Called on the probe thread when the monitor goes offline.
    /// @param onOnline  Called on the probe thread when the monitor comes back online.
    /// @param cfg       Tuning parameters (interval, thresholds).
    NetworkMonitor(ProbeFunction probe, Callback onOffline, Callback onOnline, Config cfg = Config{})
        : _probe{std::move(probe)},
          _onOffline{std::move(onOffline)},
          _onOnline{std::move(onOnline)},
          _cfg{cfg},
          _online{true},
          _stopped{false},
          _thread{[this] { run(); }} {}

    /// @brief Stops the monitor and waits for the probe thread to exit.
    ///
    /// Calls `stop()` internally. The destructor spin-waits on `_runExited` to
    /// handle the case where `stop()` was called from within a probe callback
    /// (which would deadlock a `join()`).
    ///
    /// The destructor itself must not run on the probe thread: this spin-wait
    /// blocks until `run()` sets `_runExited`, but `run()` cannot reach that
    /// store while it is paused lower on the same thread's call stack waiting
    /// for a callback (and this destructor, called from within it) to return.
    /// A callback that drops the owning object's last reference and thereby
    /// triggers this destructor synchronously, on the probe thread, deadlocks
    /// here by construction — not a supported lifetime. The supported,
    /// documented seam for a self-thread teardown request is calling `stop()`
    /// itself from the callback (see its doc comment and `_thread.get_id() ==
    /// std::this_thread::get_id()` below): that only detaches and returns,
    /// letting `run()` continue and exit on its own; the actual destructor
    /// runs later, from a different thread, once the owner is done reacting
    /// to the callback.
    ~NetworkMonitor() {
        stop();
        while (!_runExited.load()) {
            std::this_thread::yield();
        }
    }

    NetworkMonitor(const NetworkMonitor&) = delete;
    NetworkMonitor& operator=(const NetworkMonitor&) = delete;
    NetworkMonitor(NetworkMonitor&&) = delete;
    NetworkMonitor& operator=(NetworkMonitor&&) = delete;

    /// @brief Returns `true` if the monitor currently considers the network reachable.
    ///
    /// Reads an atomic flag — safe to call from any thread at any time.
    ///
    /// @return Current online state.
    [[nodiscard]] bool isOnline() const noexcept { return _online.load(); }

    /// @brief Signals the probe thread to stop and, if possible, joins it.
    ///
    /// Idempotent — subsequent calls are no-ops. If called from inside the probe
    /// thread (e.g. from a probe callback), the thread is detached instead of
    /// joined to avoid a deadlock; the destructor then spin-waits for it to exit.
    ///
    /// Thread-safe.
    void stop() {
        {
            std::scoped_lock const lock{_mtx};
            if (_stopped) {
                return;
            }
            _stopped = true;
        }
        _cv.notify_all();
        if (_thread.joinable()) {
            if (_thread.get_id() == std::this_thread::get_id()) {
                _thread.detach();
            } else {
                _thread.join();
            }
        }
    }

private:
    static bool safeProbe(const ProbeFunction& probe) noexcept {
        try {
            return probe ? probe() : false;
        } catch (...) {
            return false;
        }
    }

    void handleProbeResult(bool probeOk, int& consecutiveFailures, int& consecutiveSuccesses) {
        if (!probeOk) {
            consecutiveSuccesses = 0;
            ++consecutiveFailures;
            if (_online && consecutiveFailures >= _cfg.failureThreshold) {
                _online.store(false);
                if (_onOffline) {
                    _onOffline();
                }
            }
        } else {
            consecutiveFailures = 0;
            ++consecutiveSuccesses;
            if (!_online && consecutiveSuccesses >= _cfg.onlineThreshold) {
                _online.store(true);
                if (_onOnline) {
                    _onOnline();
                }
            }
        }
    }

    void run() {
        int consecutiveFailures = 0;
        int consecutiveSuccesses = 0;

        while (true) {
            {
                std::unique_lock lock{_mtx};
                _cv.wait_for(lock, _cfg.probeInterval, [this] { return _stopped.load(); });
                if (_stopped) {
                    break;
                }
            }
            handleProbeResult(safeProbe(_probe), consecutiveFailures, consecutiveSuccesses);
        }
        _runExited.store(true);
    }

    ProbeFunction _probe;
    Callback _onOffline;
    Callback _onOnline;
    Config _cfg;
    std::atomic<bool> _online;
    std::atomic<bool> _stopped;
    std::atomic<bool> _runExited{false};
    std::mutex _mtx;
    std::condition_variable _cv;
    std::thread _thread;
};

}  // namespace morph::offline
