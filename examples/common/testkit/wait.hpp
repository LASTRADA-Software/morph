// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <concepts>
#include <exception>
#include <memory>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include "testkit/deadline.hpp"
#include "testkit/step_executor.hpp"

/// @file
/// The Qt-free wait surface of screen tests: pump the owner executor a test runs on until a condition holds, within
/// a budget scaled by `MORPH_LADDER_DEADLINE_MS` (`deadline.hpp`). A test sleeps nowhere else.

namespace morph::examples::testing {

/// @brief The default budget of a wait, before scaling.
inline constexpr std::chrono::milliseconds kWaitBudget{5000};

namespace detail {

/// @brief The scaled deadline of a wait that starts now.
/// @param budget The unscaled budget.
/// @return When the wait gives up.
[[nodiscard]] inline std::chrono::steady_clock::time_point deadlineFrom(std::chrono::milliseconds budget) {
    return std::chrono::steady_clock::now() +
           std::chrono::milliseconds{::morph::ladder::testkit::detail::scaledDeadlineMs(budget.count())};
}

}  // namespace detail

/// @brief Runs @p owner's tasks until @p pred holds or the budget runs out. Waits for posted work rather than
///        spinning: `runFor` blocks until a task arrives.
/// @param owner  The executor the test's runtime and replies run on.
/// @param pred   Checked before each slice.
/// @param budget The unscaled budget.
/// @return Whether @p pred held in time.
template <std::predicate Pred>
[[nodiscard]] bool pumpUntil(exec::MainThreadExecutor& owner, Pred pred,
                             std::chrono::milliseconds budget = kWaitBudget) {
    auto const deadline = detail::deadlineFrom(budget);
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        owner.runFor(std::chrono::milliseconds{10});
    }
    return true;
}

/// @brief Runs @p owner's tasks one at a time until @p pred holds or the budget runs out.
/// @param owner  The executor the test steps.
/// @param pred   Checked before each step.
/// @param budget The unscaled budget.
/// @return Whether @p pred held in time.
template <std::predicate Pred>
[[nodiscard]] bool pumpUntil(::morph::ladder::testkit::StepExecutor& owner, Pred pred,
                             std::chrono::milliseconds budget = kWaitBudget) {
    auto const deadline = detail::deadlineFrom(budget);
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        if (!owner.runOne()) {
            // Nothing queued: the work comes from another thread.
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
    return true;
}

/// @brief Waits for @p completion, delivered on @p owner, by pumping it.
/// @tparam T The value type.
/// @param owner      The completion's executor.
/// @param completion The completion.
/// @param budget     The unscaled budget.
/// @return The value.
/// @throws What the completion failed with; `std::runtime_error` when the budget runs out first.
template <typename T>
T awaitOn(exec::MainThreadExecutor& owner, async::Completion<T> completion,
          std::chrono::milliseconds budget = kWaitBudget) {
    // Heap state: a completion that settles after a timed-out wait writes here, not into a gone frame.
    struct State {
        std::optional<T> value;
        std::exception_ptr error;
    };
    auto const state = std::make_shared<State>();
    completion.thenDetached([state](T const& value) { state->value = value; })
        .onErrorDetached([state](std::exception_ptr const& error) { state->error = error; });
    if (!pumpUntil(owner, [&state] { return state->value.has_value() || state->error != nullptr; }, budget)) {
        throw std::runtime_error{"awaitOn: the completion did not settle within the budget"};
    }
    if (state->error) {
        std::rethrow_exception(state->error);
    }
    return std::move(*state->value);
}

}  // namespace morph::examples::testing
