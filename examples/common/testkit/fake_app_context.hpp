// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/executor.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/ui/frontend.hpp>
#include <optional>

/// @file
/// A headless `ui::AppContext` for tests that open a source or mount a shell without a frontend.

namespace morph::examples::testing {

/// @brief A context over a `MainThreadExecutor` the test pumps, a runtime on it and a manual clock; `quit` is
///        recorded.
class FakeAppContext final : public ui::AppContext {
public:
    /// @brief The runtime the screens mount in.
    /// @return It.
    reactive::Runtime& runtime() override { return _runtime; }

    /// @brief The owner executor, which the test pumps.
    /// @return It.
    exec::IExecutor& executor() override { return _owner; }

    /// @brief The manual scheduler.
    /// @return It.
    ui::Scheduler& scheduler() override { return _scheduler; }

    /// @brief None: this context has no I/O loop, as a Qt Quick frontend has none.
    /// @return Null.
    exec::IoLoop* ioLoop() override { return nullptr; }

    /// @brief Records the exit code.
    /// @param exitCode The code.
    void quit(int exitCode = 0) override { _quitCode = exitCode; }

    /// @brief The owner, typed, for `pumpUntil`.
    /// @return It.
    [[nodiscard]] exec::MainThreadExecutor& owner() noexcept { return _owner; }

    /// @brief The manual scheduler, typed, to advance it.
    /// @return It.
    [[nodiscard]] reactive::testing::ManualScheduler& clock() noexcept { return _scheduler; }

    /// @brief The code `quit` was called with, if it was.
    /// @return It.
    [[nodiscard]] std::optional<int> quitCode() const noexcept { return _quitCode; }

private:
    exec::MainThreadExecutor _owner;
    reactive::Runtime _runtime{_owner};
    reactive::testing::ManualScheduler _scheduler;
    std::optional<int> _quitCode;
};

}  // namespace morph::examples::testing
