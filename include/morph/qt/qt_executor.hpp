// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QCoreApplication>
#include <QMetaObject>
#include <morph/core/executor.hpp>
#include <functional>

namespace morph::qt {

/// @brief `IExecutor` implementation that posts tasks to the Qt event loop.
///
/// Uses `QMetaObject::invokeMethod` with `Qt::QueuedConnection` so the callable
/// always runs on the thread that owns `QCoreApplication` (typically the GUI thread).
///
/// Thread-safe: `post()` may be called from any thread. No `QObject` subclass is
/// required by the caller.
class QtExecutor : public ::morph::exec::IExecutor {
public:
    /// @brief Posts @p fn to the Qt event loop for execution on the GUI thread.
    ///
    /// Returns immediately. @p fn is invoked asynchronously once the event loop
    /// processes the queued event.
    ///
    /// @param fn Callable to execute on the GUI thread.
    void post(std::function<void()> fn) override {
        QMetaObject::invokeMethod(QCoreApplication::instance(), std::move(fn), Qt::QueuedConnection);
    }
};

}  // namespace morph::qt
