// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "logger.hpp"

namespace morph::offline {

/// @brief Outcome of a single `onOnline()` attempt sequence.
enum class ReconnectOutcome : std::uint8_t {
    Reconnected,  ///< Backend reopened, made active, context bound, queue replay invoked.
    GaveUp,       ///< Exhausted maxAttempts without a successful reconnect; stayed offline.
    Aborted,      ///< shouldContinue() returned false before any reconnect (e.g. went offline again).
};

/// @brief Tuning parameters for `ReconnectCoordinator`.
///
/// Declared outside the class so its default member initialisers are fully
/// parsed before any constructor default argument names `Config{}`. (A nested
/// incomplete type breaks constructor default arguments on clang/GCC — see the
/// same note on `NetworkMonitorConfig`.)
struct ReconnectCoordinatorConfig {
    /// @brief Max reconnect attempts per `onOnline()` call before giving up.
    int maxAttempts = 10;

    /// @brief Delay between failed reconnect attempts.
    std::chrono::milliseconds retryDelay = std::chrono::seconds{2};
};

/// @brief Sequences reconnect → activate → bind → replay when connectivity returns.
///
/// All side effects are injected via `Deps`. The coordinator contains only the
/// retry loop, the ordering guarantees, and the abort checks. It performs no I/O
/// and owns no thread; `onOnline()` / `onOffline()` run synchronously on the
/// calling thread.
///
/// @par Ordering guarantee (the reason this class exists)
/// Within a successful `onOnline()`, the steps run in this strict order:
///   1. `tryReconnect()` returns true        (backend usable)
///   2. `activatePrimary()`                   (make primary the active backend)
///   3. `bindContext()`                       (rebind per-connection / per-session state)
///   4. `replay()`                            (drain + replay the offline queue)
/// Step 4 MUST NOT run before step 3 completes, and step 3 MUST NOT run before
/// step 2. Implementations and tests should treat this as an invariant.
///
/// @par Thread safety
/// `onOnline()` and `onOffline()` are mutually serialised by an internal mutex,
/// mirroring `SyncWorker::run()`. Calling them concurrently is safe; the second
/// caller blocks. They are intended to be posted onto a worker executor by the
/// host (see the wiring note in the design doc), not called on the probe thread.
class ReconnectCoordinator {
public:
    /// @brief Alias for the configuration struct.
    using Config = ReconnectCoordinatorConfig;

    /// @brief Injected, host-supplied side effects. None may be null.
    struct Deps {
        /// @brief Attempt to (re)open the primary backend.
        /// @return true once the backend is genuinely usable (not merely TCP-open).
        /// Must not throw; throwing is treated as a failed attempt.
        std::function<bool()> tryReconnect;

        /// @brief Make the freshly-reconnected primary the active backend.
        /// Called exactly once per successful `onOnline()`, after `tryReconnect()`
        /// succeeds and before `bindContext()`.
        std::function<void()> activatePrimary;

        /// @brief Switch the active backend to the local/offline one.
        /// Called by `onOffline()`. Followed by `bindContext()` so context tracks
        /// the now-active backend.
        std::function<void()> activateLocal;

        /// @brief Rebind per-connection / per-session context to the active backend.
        /// Called after every activate* step. Must not throw.
        std::function<void()> bindContext;

        /// @brief Replay the offline queue against the now-active primary.
        /// Typically wraps `SyncWorker::run()`. Called last in `onOnline()`.
        std::function<void()> replay;

        /// @brief Returns false to abort the current `onOnline()` sequence early
        /// (e.g. the monitor reports the backend went offline again mid-retry).
        /// Polled before each reconnect attempt and once more before replay.
        std::function<bool()> shouldContinue;

        /// @brief Sleep for the given duration between failed attempts.
        /// Injected so tests can substitute a no-op / counter. Hosts wire this to
        /// `std::this_thread::sleep_for`.
        std::function<void(std::chrono::milliseconds)> sleep;
    };

    /// @brief Constructs a coordinator with injected dependencies and tuning.
    /// @param deps Side-effect callbacks (all required, none null).
    /// @param cfg  Retry tuning.
    ///
    /// In a debug build, a null `Deps` member is logged via `morph::log::logError`.
    /// Invoking the coordinator with any null member is undefined behaviour.
    explicit ReconnectCoordinator(Deps deps, Config cfg = Config{}) : _deps{std::move(deps)}, _cfg{cfg} {
        assertDepsNonNull(_deps);
    }

    ReconnectCoordinator(const ReconnectCoordinator&) = delete;
    ReconnectCoordinator& operator=(const ReconnectCoordinator&) = delete;
    ReconnectCoordinator(ReconnectCoordinator&&) = delete;
    ReconnectCoordinator& operator=(ReconnectCoordinator&&) = delete;

    /// @brief Run the reconnect → activate → bind → replay sequence.
    ///
    /// Synchronous; runs entirely on the calling thread. Serialised against
    /// `onOffline()` by an internal mutex.
    ///
    /// @return How the sequence ended (see `ReconnectOutcome`).
    ReconnectOutcome onOnline() {
        std::scoped_lock lock{_mtx};

        for (int attempt = 1; attempt <= _cfg.maxAttempts; ++attempt) {
            if (!callShouldContinue()) {
                return ReconnectOutcome::Aborted;
            }
            if (callTryReconnect()) {
                _deps.activatePrimary();
                _deps.bindContext();
                // Re-check before replay so we never replay into a backend that
                // just went away. We are still Reconnected either way; this only
                // controls whether replay runs.
                if (callShouldContinue()) {
                    _deps.replay();
                }
                return ReconnectOutcome::Reconnected;
            }
            // No sleep after the final attempt — it would just waste retryDelay
            // before giving up.
            if (attempt < _cfg.maxAttempts) {
                _deps.sleep(_cfg.retryDelay);
            }
        }

        ::morph::log::logWarn("[reconnect_coordinator] gave up after " + std::to_string(_cfg.maxAttempts) +
                              " attempts, staying offline");
        return ReconnectOutcome::GaveUp;
    }

    /// @brief Switch to the local backend and rebind context.
    ///
    /// Idempotent at the policy level: safe to call when already local. Serialised
    /// against `onOnline()` by an internal mutex.
    void onOffline() {
        std::scoped_lock lock{_mtx};
        _deps.activateLocal();
        _deps.bindContext();
    }

private:
    /// @brief Logs each null `Deps` member at error level (debug-time aid only).
    static void assertDepsNonNull(const Deps& deps) {
        auto check = [](const char* name, bool present) {
            if (!present) {
                ::morph::log::logError(std::string{"[reconnect_coordinator] null Deps member: "} + name);
            }
        };
        check("tryReconnect", static_cast<bool>(deps.tryReconnect));
        check("activatePrimary", static_cast<bool>(deps.activatePrimary));
        check("activateLocal", static_cast<bool>(deps.activateLocal));
        check("bindContext", static_cast<bool>(deps.bindContext));
        check("replay", static_cast<bool>(deps.replay));
        check("shouldContinue", static_cast<bool>(deps.shouldContinue));
        check("sleep", static_cast<bool>(deps.sleep));
    }

    /// @brief Calls `tryReconnect`, treating a thrown exception as a failed attempt.
    bool callTryReconnect() noexcept {
        try {
            return _deps.tryReconnect();
        } catch (...) {
            return false;
        }
    }

    /// @brief Calls `shouldContinue`, treating a throw as "do not continue".
    bool callShouldContinue() noexcept {
        try {
            return _deps.shouldContinue();
        } catch (...) {
            return false;
        }
    }

    Deps _deps;
    Config _cfg;
    std::mutex _mtx;  ///< Serialises onOnline()/onOffline().
};

}  // namespace morph::offline
