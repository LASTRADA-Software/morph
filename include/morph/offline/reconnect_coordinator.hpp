// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "../attributes.hpp"
#include "../core/completion.hpp"
#include "../core/detail/owner_probe.hpp"
#include "../core/executor.hpp"
#include "../core/logger.hpp"
#include "../core/observability.hpp"
#include "../core/owner_strand.hpp"

namespace morph::offline {

/// @brief Outcome of a single `onOnline()` attempt sequence.
enum class ReconnectOutcome : std::uint8_t {
    Reconnected,  ///< Backend reopened, made active, context bound. Replay is
                  ///< invoked only if `shouldContinue()` still holds at that
                  ///< point — see `onOnline()`; `Reconnected` can be returned
                  ///< without replaying.
    GaveUp,       ///< Exhausted maxAttempts without a successful reconnect; stayed offline.
    Aborted,      ///< shouldContinue() returned false before any reconnect (e.g. went offline again).
};

namespace detail {
/// @brief Tag value for the `reconnectOutcome` metric's "outcome" dimension.
constexpr std::string_view reconnectOutcomeName(ReconnectOutcome outcome) noexcept {
    switch (outcome) {
        case ReconnectOutcome::Reconnected:
            return "Reconnected";
        case ReconnectOutcome::GaveUp:
            return "GaveUp";
        case ReconnectOutcome::Aborted:
            return "Aborted";
        default:
            // Unreachable through any real code path: ReconnectOutcome is a
            // closed enum and every enumerator is already handled explicitly
            // above. This arm only exists to satisfy the compiler that the
            // function returns on every enum value, including one
            // manufactured by an out-of-range `static_cast` -- mirrors
            // ruleKindName's identical default: arm in forms.hpp.
            return "?";
    }
}
}  // namespace detail

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
/// retry loop, the ordering guarantees, and the abort checks. It performs no
/// I/O and owns no thread: it owns a strand over the executor it is given, the
/// *offline strand*, and runs `onOnline()`'s and `onOffline()`'s bodies there.
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
/// @par One owner
/// `onOnline()` posts its whole sequence, retry sleeps included, as one task
/// on the offline strand; `onOffline()` posts its two steps as another. A
/// strand runs one task at a time, in post order, so the two never overlap and
/// an `onOffline()` can never land between `bindContext()` and `replay()`.
/// Both are callable from any thread and return at once. The executor should
/// be a worker pool, **not** the I/O loop that runs `NetworkMonitor`'s
/// callbacks: the retry sleeps block the thread the task runs on.
///
/// A `SyncWorker` given `strand()` as its owner drains inside the `replay`
/// step when the step calls its `run()`, rather than after the task.
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
class ReconnectCoordinator {
public:
    /// @brief Alias for the configuration struct.
    using Config = ReconnectCoordinatorConfig;

    /// @brief Injected, host-supplied side effects. None may be null. Each is
    ///        called on the offline strand.
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
        /// Typically wraps `SyncWorker::run(replyExec)` on a worker owned by
        /// `strand()`.
        /// Called last in `onOnline()`.
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

    /// @brief Constructs a coordinator with injected dependencies and tuning,
    ///        running its sequences on a strand over @p executor.
    /// @param deps     Side-effect callbacks (all required, none null).
    /// @param executor Where the offline strand's tasks run: a worker pool.
    ///                 Borrowed: it must outlive this coordinator and keep
    ///                 running tasks until the coordinator is destroyed.
    /// @param cfg      Retry tuning.
    ///
    /// A null `Deps` member is logged via `morph::log::logError` in all builds;
    /// construction still succeeds. Invoking the coordinator with any null member
    /// is undefined behaviour.
    ReconnectCoordinator(Deps deps, ::morph::exec::IExecutor& executor MORPH_LIFETIMEBOUND, Config cfg = Config{})
        : _state{std::make_shared<State>(std::move(deps), cfg)}, _strand{executor} {
        assertDepsNonNull(_state->deps);
    }

    /// @brief Closes the offline strand: a sequence not yet started is dropped,
    ///        one running on another thread is waited for.
    ~ReconnectCoordinator() { _strand.close(); }

    ReconnectCoordinator(const ReconnectCoordinator&) = delete;
    ReconnectCoordinator& operator=(const ReconnectCoordinator&) = delete;
    ReconnectCoordinator(ReconnectCoordinator&&) = delete;
    ReconnectCoordinator& operator=(ReconnectCoordinator&&) = delete;

    /// @brief Runs the reconnect → activate → bind → replay sequence on the
    ///        offline strand.
    ///
    /// Posted as one task, after any `onOnline()`/`onOffline()` posted before
    /// it. Callable from any thread; returns at once.
    ///
    /// @param replyExec Executor the outcome is delivered on and the caller
    ///        attaches its callbacks on: the caller's own, or `strand()` for a
    ///        caller that does not wait for it. Borrowed: it must outlive the
    ///        returned `Completion`.
    /// @return A `Completion` settled on the offline strand with how the
    ///         sequence ended (see `ReconnectOutcome`), delivered on
    ///         @p replyExec.
    ::morph::async::Completion<ReconnectOutcome> onOnline(::morph::exec::IExecutor& replyExec MORPH_LIFETIMEBOUND) {
        auto settleable = ::morph::async::Completion<ReconnectOutcome>::makeSettleable(&replyExec);
        _strand.postTask([state = _state, strand = &_strand, promise = std::move(settleable.second)]() mutable {
            ::morph::exec::detail::noteOwner("ReconnectCoordinator::onOnline", strand->coreExecutor(),
                                             strand->runningHere());
            promise.resolve(state->runOnline());
        });
        return std::move(settleable.first);
    }

    /// @brief Switches to the local backend and rebinds context, on the
    ///        offline strand.
    ///
    /// Idempotent at the policy level: safe to call when already local. Posted
    /// after any `onOnline()`/`onOffline()` posted before it. Callable from any
    /// thread; returns at once.
    void onOffline() {
        _strand.postTask([state = _state, strand = &_strand] {
            ::morph::exec::detail::noteOwner("ReconnectCoordinator::onOffline", strand->coreExecutor(),
                                             strand->runningHere());
            state->deps.activateLocal();
            state->deps.bindContext();
        });
    }

    /// @brief The offline strand, as an executor: what a `SyncWorker` that
    ///        replays for this coordinator is given as its owner.
    /// @return The strand every `onOnline()`/`onOffline()` body runs on.
    [[nodiscard]] ::morph::exec::IExecutor& strand() noexcept { return _strand; }

private:
    /// @brief Logs each null `Deps` member at error level (in all builds; does
    ///        not throw — construction still succeeds).
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

    /// What the offline strand's tasks run on: the deps and the retry loop.
    /// Shared with each posted task, so a task never reaches through the
    /// coordinator.
    struct State {
        State(Deps injected, Config config) : deps{std::move(injected)}, cfg{config} {}

        Deps deps;
        Config cfg;

        /// @brief The whole `onOnline()` sequence. On the offline strand.
        /// @return How it ended.
        [[nodiscard]] ReconnectOutcome runOnline() const {
            for (int attempt = 1; attempt <= cfg.maxAttempts; ++attempt) {
                if (!callShouldContinue()) {
                    return emitOutcome(ReconnectOutcome::Aborted);
                }
                ::morph::observe::detail::emitMetric(::morph::observe::Metric::reconnectAttempts, 1.0);
                if (callTryReconnect()) {
                    deps.activatePrimary();
                    deps.bindContext();
                    // Re-check before replay so we never replay into a backend
                    // that just went away. We are still Reconnected either way;
                    // this only controls whether replay runs.
                    if (callShouldContinue()) {
                        deps.replay();
                    }
                    return emitOutcome(ReconnectOutcome::Reconnected);
                }
                // No sleep after the final attempt — it would just waste
                // retryDelay before giving up.
                if (attempt < cfg.maxAttempts) {
                    deps.sleep(cfg.retryDelay);
                }
            }
            ::morph::log::logWarn("[reconnect_coordinator] gave up after " + std::to_string(cfg.maxAttempts) +
                                  " attempts, staying offline");
            return emitOutcome(ReconnectOutcome::GaveUp);
        }

        /// @brief Calls `tryReconnect`, treating a thrown exception as a failed attempt.
        [[nodiscard]] bool callTryReconnect() const noexcept {
            try {
                return deps.tryReconnect();
            } catch (...) {
                return false;
            }
        }

        /// @brief Calls `shouldContinue`, treating a throw as "do not continue".
        [[nodiscard]] bool callShouldContinue() const noexcept {
            try {
                return deps.shouldContinue();
            } catch (...) {
                return false;
            }
        }
    };

    /// @brief Emits the `reconnectOutcome` counter tagged by @p outcome, then
    ///        returns it — lets each `onOnline()` return site emit-and-return
    ///        in one expression.
    static ReconnectOutcome emitOutcome(ReconnectOutcome outcome) noexcept {
        std::array<std::pair<std::string_view, std::string_view>, 1> const tags{
            {{"outcome", detail::reconnectOutcomeName(outcome)}}};
        ::morph::observe::detail::emitMetric(::morph::observe::Metric::reconnectOutcome, 1.0, tags);
        return outcome;
    }

    std::shared_ptr<State> _state;
    /// The offline strand. Declared last, so it is closed before the state
    /// the destructor's close waits on a running task to finish with.
    ::morph::exec::OwnerStrand _strand;
};

}  // namespace morph::offline
