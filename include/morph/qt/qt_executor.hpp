// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QCoreApplication>
#include <QMetaObject>
#include <morph/core/executor.hpp>
#include <functional>

namespace morph::qt {

/// @brief `IExecutor` implementation that posts tasks to a Qt event loop.
///
/// Uses `QMetaObject::invokeMethod` with `Qt::QueuedConnection` so the callable
/// always runs on the thread that owns the configured context object — by
/// default `QCoreApplication::instance()`, i.e. the GUI thread.
///
/// Thread-safe: `post()` may be called from any thread. No `QObject` subclass is
/// required by the caller.
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
    explicit QtExecutor(QObject* context = QCoreApplication::instance()) : _context{context} {}

    /// @brief Posts @p fn to the Qt event loop for execution on the context's thread.
    ///
    /// Returns immediately. @p fn is invoked asynchronously once the event loop
    /// processes the queued event.
    ///
    /// @param fn Callable to execute on the context object's thread.
    void post(std::function<void()> fn) override {
        QMetaObject::invokeMethod(_context, std::move(fn), Qt::QueuedConnection);
    }

private:
    QObject* _context;
};

}  // namespace morph::qt
