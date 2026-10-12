// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>

#include "../attributes.hpp"
#include "../core/executor.hpp"
#include "../reactive/scheduler.hpp"

class QObject;

/// @file
/// @brief `morph::qt_quick::QtScheduler`: `reactive::Scheduler` on Qt's timers.
///
/// Specified in `docs/spec/qt_quick/frontend.md`, "Scheduler".

namespace morph::qt_quick {

/// @brief `reactive::Scheduler` on `QTimer`s, for a runtime whose owner is the Qt event loop's thread.
///
/// Each callback runs on that thread with @p owner as the current executor, so it may write the reactive state the
/// owner's runtime holds. It keeps every guarantee of the `Scheduler` contract: a cancelled timer never fires; a
/// handle may be cancelled from inside its own callback and may outlive the scheduler; a periodic timer that the
/// loop could not serve for several periods fires once. Neither copyable nor movable; used on its thread only.
class QtScheduler final : public reactive::Scheduler {
public:
    /// @brief A scheduler whose timers belong to @p context.
    /// @param owner   The executor the callbacks run under. Borrowed: it outlives the scheduler.
    /// @param context The object the timers are children of; null makes them children of an object the scheduler
    ///                owns.
    explicit QtScheduler(exec::IExecutor& owner MORPH_LIFETIMEBOUND, QObject* context = nullptr);
    /// @brief Stops every timer; a handle cancelled later does nothing.
    ~QtScheduler() override;
    QtScheduler(QtScheduler const&) = delete;
    QtScheduler& operator=(QtScheduler const&) = delete;
    QtScheduler(QtScheduler&&) = delete;
    QtScheduler& operator=(QtScheduler&&) = delete;

    /// @copydoc reactive::Scheduler::after
    [[nodiscard]] reactive::TimerHandle after(std::chrono::milliseconds delay, std::function<void()> fn) override;
    /// @copydoc reactive::Scheduler::every
    [[nodiscard]] reactive::TimerHandle every(std::chrono::milliseconds period, std::function<void()> fn) override;

    /// @brief How many timers are armed.
    /// @return Their number.
    [[nodiscard]] std::size_t pendingTimers() const noexcept;

private:
    struct State;
    /// Arms @p fn @p delay from now, then every @p period; a zero @p period fires once.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the first delay, then the period, as `every` gives them.
    [[nodiscard]] reactive::TimerHandle add(std::chrono::milliseconds delay, std::chrono::milliseconds period,
                                            std::function<void()> fn);

    std::shared_ptr<State> _state;
};

}  // namespace morph::qt_quick
