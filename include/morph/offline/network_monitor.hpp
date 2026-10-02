// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <chrono>
#include <core/net/EventLoop.hpp>
#include <functional>
#include <memory>
#include <utility>

#include "../attributes.hpp"
#include "../core/detail/owner_probe.hpp"
#include "../core/io_loop.hpp"

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

/// @brief Connectivity monitor that calls a probe on a timer and fires
///        callbacks on state changes.
///
/// The probe runs every `probeInterval` on an `exec::IoLoop`'s thread, as a
/// loop timer. The monitor starts in the *online* state and transitions to
/// *offline* only after `failureThreshold` consecutive failures. It returns to
/// *online* after `onlineThreshold` consecutive successes.
///
/// The probe and both callbacks run on the loop's thread and **must not block
/// it**: every socket and timer on the same loop waits while they run. A probe
/// that needs a slow check should start it elsewhere and report the last
/// result it has.
///
/// Cross-thread surface: `isOnline()`, an atomic read, and `stop()` and the
/// destructor, which run their close on the loop and wait for it. Every other
/// field is touched only on the loop.
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

    /// @brief Starts the monitor on @p loop.
    ///
    /// The monitor starts in the online state. The first `failureThreshold`
    /// consecutive probe failures must occur before `onOffline` is invoked.
    /// Callers that know the initial state is offline should use a probe that
    /// starts returning `false` immediately.
    ///
    /// @param loop      The application's loop, whose thread runs the probe and
    ///                  the callbacks. Borrowed: it must outlive this monitor.
    /// @param probe     Callable that tests connectivity. Must not throw
    ///                  (exceptions are swallowed) and must not block. Stored and
    ///                  invoked on the loop's thread for this monitor's whole
    ///                  lifetime, so anything the callable refers to must outlive
    ///                  the monitor.
    /// @param onOffline Called on the loop's thread when the monitor goes
    ///                  offline. Retained on the same terms as @p probe.
    /// @param onOnline  Called on the loop's thread when the monitor comes back
    ///                  online. Retained on the same terms as @p probe.
    /// @param cfg       Tuning parameters (interval, thresholds).
    NetworkMonitor(::morph::exec::IoLoop& loop MORPH_LIFETIMEBOUND, ProbeFunction probe MORPH_LIFETIMEBOUND,
                   Callback onOffline MORPH_LIFETIMEBOUND, Callback onOnline MORPH_LIFETIMEBOUND,
                   Config cfg = Config{})
        : _loop{&loop},
          _state{std::make_shared<State>(loop, std::move(probe), std::move(onOffline), std::move(onOnline), cfg)} {
        start();
    }

    /// @brief Starts the monitor on a loop of its own, for a caller with no
    ///        `IoLoop` to share. Natively that loop owns one thread.
    ///
    /// @param probe     As for the other constructor.
    /// @param onOffline As for the other constructor.
    /// @param onOnline  As for the other constructor.
    /// @param cfg       Tuning parameters (interval, thresholds).
    NetworkMonitor(ProbeFunction probe MORPH_LIFETIMEBOUND, Callback onOffline MORPH_LIFETIMEBOUND,
                   Callback onOnline MORPH_LIFETIMEBOUND, Config cfg = Config{})
        : _ownedLoop{std::make_unique<::morph::exec::IoLoop>()},
          _loop{_ownedLoop.get()},
          _state{
              std::make_shared<State>(*_ownedLoop, std::move(probe), std::move(onOffline), std::move(onOnline), cfg)} {
        start();
    }

    /// @brief Stops the monitor. Once this returns, no probe or callback of
    ///        this monitor will run again, and from any thread but the loop's
    ///        none is running.
    ///
    /// May run anywhere, including inside this monitor's own probe or
    /// callback: the timer that called it keeps the loop-side state alive
    /// until it has returned.
    ~NetworkMonitor() { stop(); }

    NetworkMonitor(const NetworkMonitor&) = delete;
    NetworkMonitor& operator=(const NetworkMonitor&) = delete;
    NetworkMonitor(NetworkMonitor&&) = delete;
    NetworkMonitor& operator=(NetworkMonitor&&) = delete;

    /// @brief Returns `true` if the monitor currently considers the network reachable.
    ///
    /// Reads an atomic flag — safe to call from any thread at any time.
    ///
    /// @return Current online state.
    [[nodiscard]] bool isOnline() const noexcept { return _state->online.load(); }

    /// @brief Stops probing. Idempotent.
    ///
    /// Runs on the loop: inline when called there (from the probe or a
    /// callback, which then finishes normally, and no further probe runs),
    /// posted and waited for from any other thread, so that once it returns
    /// no probe or callback is running.
    void stop() {
        _loop->runAndWait([state = _state] { state->stop(); });
    }

private:
    /// Everything the loop owns. `online` is the one field read elsewhere.
    struct State : std::enable_shared_from_this<State> {
        State(::morph::exec::IoLoop& ioLoop, ProbeFunction probeFn, Callback offline, Callback online_, Config config)
            : loop{ioLoop},
              probe{std::move(probeFn)},
              onOffline{std::move(offline)},
              onOnline{std::move(online_)},
              cfg{config} {}

        void armNext() {
            timer = loop.loop().addTimer(loop.loop().clock().now() + cfg.probeInterval, &State::fire, this);
        }

        void stop() {
            ::morph::exec::detail::noteOwner("NetworkMonitor::stop", loop.loop(), loop.runningHere());
            if (stopped) {
                return;
            }
            stopped = true;
            static_cast<void>(loop.loop().cancelTimer(timer));
        }

        static bool safeProbe(const ProbeFunction& probeFn) noexcept {
            try {
                return probeFn ? probeFn() : false;
            } catch (...) {
                return false;
            }
        }

        void handleProbeResult(bool probeOk) {
            if (!probeOk) {
                consecutiveSuccesses = 0;
                ++consecutiveFailures;
                if (online.load() && consecutiveFailures >= cfg.failureThreshold) {
                    online.store(false);
                    if (onOffline) {
                        onOffline();
                    }
                }
            } else {
                consecutiveFailures = 0;
                ++consecutiveSuccesses;
                if (!online.load() && consecutiveSuccesses >= cfg.onlineThreshold) {
                    online.store(true);
                    if (onOnline) {
                        onOnline();
                    }
                }
            }
        }

        /// The probe timer's callback, on the loop's thread. The timer has
        /// already fired, so a `stop()` from the probe or a callback retires
        /// nothing and only prevents the re-arm below.
        static void fire(void* statePtr) {
            // Held for the whole call: the probe or a callback may destroy
            // the monitor, and with it the monitor's own share of this state.
            std::shared_ptr<State> const keep = static_cast<State*>(statePtr)->shared_from_this();
            State& self = *keep;
            ::morph::exec::detail::noteOwner("NetworkMonitor::probe", self.loop.loop(), self.loop.runningHere());
            self.timer = {};
            if (self.stopped) {
                return;
            }
            bool const probeOk = safeProbe(self.probe);
            if (self.stopped) {
                return;
            }
            self.handleProbeResult(probeOk);
            if (!self.stopped) {
                self.armNext();
            }
        }

        ::morph::exec::IoLoop& loop;
        ProbeFunction probe;
        Callback onOffline;
        Callback onOnline;
        Config cfg;
        std::atomic<bool> online{true};
        int consecutiveFailures{0};
        int consecutiveSuccesses{0};
        ::core::net::TimerId timer{};
        bool stopped{false};
    };

    void start() {
        _loop->post([state = _state] {
            if (!state->stopped) {
                state->armNext();
            }
        });
    }

    /// Present only for the owning constructor. First, so it is destroyed
    /// last, after the destructor has stopped the state on it.
    std::unique_ptr<::morph::exec::IoLoop> _ownedLoop;
    ::morph::exec::IoLoop* _loop;
    std::shared_ptr<State> _state;
};

}  // namespace morph::offline
