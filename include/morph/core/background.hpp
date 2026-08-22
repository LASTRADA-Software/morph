// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/session/session.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace morph::exec {

/// @brief What a background task inherits from the dispatch that posted it.
enum class SessionPropagation : std::uint8_t {
    /// @brief The task runs with **no** session context (the default).
    ///
    /// A background task is not the caller's request; it is work the model
    /// decided to do. Running it with the caller's authority by default would
    /// mean a long-running job keeps acting as a principal well after that
    /// principal's request finished — and, because the task outlives the
    /// dispatch, potentially after their session should have ended. A task
    /// that needs authority should mint its own service-principal token, the
    /// way `bookmarks`' metadata fetcher already does.
    None,

    /// @brief The task runs under a **copy** of the posting dispatch's session.
    ///
    /// A copy, not a reference: `session::ScopedContext` holds a reference to
    /// the `Context` it installs, and the caller's own context is long gone by
    /// the time the task runs. Choose this only when the work genuinely is the
    /// caller's request continued on another thread.
    Inherit,
};

namespace detail {

/// @brief Mutex-guarded slot holding the process-wide background executor.
/// @return Reference to the singleton state.
[[nodiscard]] inline auto& backgroundExecutorState() {
    struct State {
        std::mutex mutex;
        std::shared_ptr<IExecutor> executor;
    };
    static State state;
    return state;
}

}  // namespace detail

/// @brief Installs the process-wide executor that `postBackground` posts to.
///
/// Mirrors `morph::journal::setActionLog`'s install pattern deliberately: a
/// model needs to reach an executor without the App/Bridge/RemoteServer layer
/// handing it one, exactly as it needs to reach an action log the same way.
///
/// @param executor Executor to install, or `nullptr` to uninstall.
inline void setBackgroundExecutor(std::shared_ptr<IExecutor> executor) {
    auto& state = detail::backgroundExecutorState();
    std::scoped_lock const lock{state.mutex};
    state.executor = std::move(executor);
}

/// @brief The currently installed background executor, or `nullptr`.
/// @return The installed executor.
[[nodiscard]] inline std::shared_ptr<IExecutor> backgroundExecutor() {
    auto& state = detail::backgroundExecutorState();
    std::scoped_lock const lock{state.mutex};
    return state.executor;
}

/// @brief Runs @p task on the installed background executor, off the caller's strand.
///
/// The seam a model's own `execute()` can reach for when it needs
/// submit-now/compute-later work — report generation, batch export, long
/// aggregation — without inventing a private executor member, and without
/// requiring an App, a `Bridge`, or a `RemoteServer` to exist around it.
///
/// Returns `false` and logs when no executor is installed, rather than
/// throwing or silently dropping: a model that posts background work in a
/// deployment that never installed one has a configuration error, and it
/// should surface as a visible failure the caller can react to, not as work
/// that appears to have been scheduled.
///
/// **The task must not capture the model.** It runs off the model's strand, so
/// anything it touches must be owned by the task itself — copy the values it
/// needs in. Persisting a result is the task's own business, through its own
/// data mapper.
///
/// @param task        Work to run on the background executor.
/// @param propagation What the task inherits from the posting session.
/// @return `true` if the task was posted; `false` if no executor is installed.
inline bool postBackground(std::function<void()> task, SessionPropagation propagation = SessionPropagation::None) {
    auto executor = backgroundExecutor();
    if (executor == nullptr) {
        ::morph::log::logError("[background] no background executor installed; task not posted");
        return false;
    }

    if (propagation == SessionPropagation::Inherit) {
        const auto* current = ::morph::session::current();
        auto inherited = current != nullptr ? *current : ::morph::session::Context{};
        executor->post([task = std::move(task), inherited = std::move(inherited)]() mutable {
            // `ScopedContext` holds a reference, so the copy has to outlive the
            // scope -- hence a named local rather than a temporary.
            const ::morph::session::detail::ScopedContext scope{inherited};
            task();
        });
        return true;
    }

    executor->post(std::move(task));
    return true;
}

/// @brief Installs a background executor for the current scope and restores the
///        previous one on destruction. Intended for tests and for an App that
///        owns its pool.
class ScopedBackgroundExecutor {
  public:
    /// @param executor Executor to install for this scope.
    explicit ScopedBackgroundExecutor(std::shared_ptr<IExecutor> executor) : _previous{backgroundExecutor()} {
        setBackgroundExecutor(std::move(executor));
    }

    ScopedBackgroundExecutor(const ScopedBackgroundExecutor&) = delete;
    ScopedBackgroundExecutor& operator=(const ScopedBackgroundExecutor&) = delete;
    ScopedBackgroundExecutor(ScopedBackgroundExecutor&&) = delete;
    ScopedBackgroundExecutor& operator=(ScopedBackgroundExecutor&&) = delete;

    /// @brief Restores the previously installed executor.
    ~ScopedBackgroundExecutor() { setBackgroundExecutor(std::move(_previous)); }

  private:
    std::shared_ptr<IExecutor> _previous;
};

/// @brief An `IExecutor` that queues tasks and runs them only when told to.
///
/// The worker-side test double the ladder has never had. `DeterministicExecutor`
/// and `StepExecutor` control the *delivery order of continuations*; nothing
/// substituted for the *worker* side of an async job, so every async-job test
/// spins a real thread pool and polls with a wall-clock deadline. With this, a
/// submit-then-poll job is testable as a sequence of exact states: submit,
/// assert "pending", `runOne()`, assert "complete" — no sleeps, no retry loop,
/// no flakiness budget.
///
/// Not thread-safe by design: a test that needs to reason about exact task
/// ordering is driving it from one thread.
class ManualExecutor : public IExecutor {
  public:
    /// @brief Queues @p fn instead of running it.
    /// @param fn Task to queue.
    void post(std::function<void()> fn) override { _queued.push_back(std::move(fn)); }

    /// @brief Runs the oldest queued task, if any.
    /// @return `true` if a task ran.
    bool runOne() {
        if (_queued.empty()) {
            return false;
        }
        auto task = std::move(_queued.front());
        _queued.erase(_queued.begin());
        task();
        return true;
    }

    /// @brief Runs queued tasks until none remain.
    ///
    /// Tasks posted *by* a task are picked up too, so a chained job runs to
    /// completion rather than leaving its own continuation stranded.
    /// @return How many tasks ran.
    std::size_t runAll() {
        std::size_t ran = 0;
        while (runOne()) {
            ++ran;
        }
        return ran;
    }

    /// @brief How many tasks are queued and not yet run.
    /// @return The queue depth.
    [[nodiscard]] std::size_t pending() const noexcept { return _queued.size(); }

  private:
    std::vector<std::function<void()>> _queued;
};

}  // namespace morph::exec
