// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QCoreApplication>
#include <QMetaObject>
#include <functional>
#include <memory>
#include <morph/core/executor.hpp>
#include <utility>

namespace morph::qt {

/// @brief `IExecutor` implementation that posts tasks to a Qt event loop.
///
/// Uses `QMetaObject::invokeMethod` with `Qt::QueuedConnection` so the callable
/// always runs on the thread that owns the configured context object — by
/// default `QCoreApplication::instance()`, i.e. the GUI thread.
///
/// Thread-safe: `post()` may be called from any thread. No `QObject` subclass is
/// required by the caller.
///
/// **A task still queued when this executor is destroyed is dropped, not run.**
/// `post()` enqueues and returns; it does not run the callable, and the queued
/// Qt event outlives the `post()` call that created it. Joining the worker pool
/// that made the call proves the `post()` *happened* — never that the event was
/// *delivered*. Without a guard, an event still on the Qt queue at teardown is
/// delivered against a freed executor and reads `_context` off freed memory.
///
/// That is not hypothetical. `Bridge::executeVia` chains three `Completion`
/// objects per dispatched action, each settled from inside the previous one's
/// delivered callback, so a caller waiting only on its own top-level completion
/// can observe "done" while an intermediate post is still queued. When that
/// stale event is finally pumped, its body calls `post()` for the next link in
/// the chain — against an executor that no longer exists. It segfaults on an
/// ordinary uninstrumented build, not only under a sanitizer.
///
/// Each queued task therefore carries a weak observer of this executor's
/// lifetime and does nothing if the executor is already gone. Dropping is the
/// correct outcome rather than a lesser evil: a chain being torn down has
/// nobody left to observe its result, and `Completion`'s orphan logging already
/// covers a completion that never resolves.
///
/// **Boundary of the guarantee.** The check assumes this executor is destroyed
/// on the same thread that runs its context's event loop, which holds for every
/// owner in this repository. Destroying one from another thread while its loop
/// is mid-delivery still needs external synchronisation: this closes the "torn
/// down with events still queued" hole, not a genuine cross-thread race.
class QtExecutor : public ::morph::exec::IExecutor {
public:
    /// @brief Constructs an executor that posts tasks to @p context's thread.
    ///
    /// @param context `QObject` whose owning thread's event loop runs posted
    /// tasks; `QMetaObject::invokeMethod` dispatches to whichever thread
    /// `context->thread()` reports at the time each task is posted, so tasks
    /// posted after `context` is moved to a different thread run there.
    /// Defaults to `QCoreApplication::instance()`, preserving the previous
    /// GUI-thread-only behaviour. Passing `nullptr` (e.g. when constructed
    /// before `QCoreApplication` exists) makes `post()` a no-op, matching
    /// `QMetaObject::invokeMethod`'s own handling of a null target.
    explicit QtExecutor(QObject* context = QCoreApplication::instance())
        : _context{context}, _alive{std::make_shared<char>()} {}

    QtExecutor(const QtExecutor&) = delete;
    QtExecutor& operator=(const QtExecutor&) = delete;
    QtExecutor(QtExecutor&&) = delete;
    QtExecutor& operator=(QtExecutor&&) = delete;

    /// @brief Destroys the lifetime token, so any still-queued task becomes a no-op.
    ~QtExecutor() override = default;

    /// @brief Posts @p fn to the Qt event loop for execution on the context's thread.
    ///
    /// Returns immediately. @p fn is invoked asynchronously once the event loop
    /// processes the queued event.
    ///
    /// Runs only if this executor is still alive when the event is delivered;
    /// see the class docstring.
    ///
    /// @param fn Callable to execute on the context object's thread.
    void post(std::function<void()> fn) override {
        QMetaObject::invokeMethod(
            _context,
            [alive = std::weak_ptr<const char>{_alive}, fn = std::move(fn)]() mutable {
                if (alive.expired()) {
                    return;
                }
                fn();
            },
            Qt::QueuedConnection);
    }

private:
    QObject* _context;

    // The pointee is never read -- only the control block matters, so that a
    // queued task can ask whether this executor still exists. `make_shared`
    // keeps it to a single allocation.
    std::shared_ptr<const char> _alive;
};

}  // namespace morph::qt
