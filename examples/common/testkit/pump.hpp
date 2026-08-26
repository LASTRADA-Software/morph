// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QCoreApplication>
#include <QEventLoop>
#include <chrono>
#include <concepts>
#include <morph/core/completion.hpp>
#include <optional>
#include <stdexcept>

#include "testkit/deadline.hpp"

/// @file
/// The ladder testkit's only sanctioned wait surface (examples/TESTING.md,
/// "Pumping discipline"). A `sleep_for` anywhere else in ladder test code is a
/// review-rejectable defect.

namespace morph::ladder::testkit {

// `detail::computeDeadlineScale` / `detail::deadlineScale` used to be defined
// here. They moved to `testkit/deadline.hpp`, included above, so a wait loop
// that cannot take a Qt dependency can share the one `MORPH_LADDER_DEADLINE_MS`
// knob -- see that header for why. Same namespace, same names: nothing that
// called them through this header had to change.

/// @brief Bounded `processEvents` slices until @p pred is true or @p deadline elapses.
///
/// @param pred     Polled after every slice.
/// @param deadline Wall-clock budget, scaled by `MORPH_LADDER_DEADLINE_MS`.
/// @return `true` if @p pred became true before the deadline, `false` on timeout.
template <std::predicate<> Pred>
[[nodiscard]] bool pumpUntil(Pred pred, std::chrono::milliseconds deadline = std::chrono::milliseconds{5000}) {
    const auto scaledDeadline = std::chrono::milliseconds{detail::scaledDeadlineMs(deadline.count())};
    const auto start = std::chrono::steady_clock::now();
    while (!pred()) {
        if (std::chrono::steady_clock::now() - start >= scaledDeadline) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

/// @brief Resolves one `Completion<T>` by pumping the Qt loop; rethrows errors.
///
/// @tparam T Result type of @p completion.
/// @param completion   The completion to await.
/// @param deadline     Wall-clock budget passed through to `pumpUntil`.
/// @return The resolved value.
/// @throws std::runtime_error if the deadline elapses before resolution.
template <typename T>
T awaitQt(::morph::async::Completion<T> completion,
          std::chrono::milliseconds deadline = std::chrono::milliseconds{5000}) {
    // `value`/`error` live in a heap-allocated block kept alive by `shared_ptr`s
    // captured (by value) in the `then`/`onError` handlers below. Those handlers
    // are held by the completion's backing state, which can outlive this stack
    // frame: if `pumpUntil` times out, `awaitQt` throws and unwinds while the
    // underlying async operation is still pending. Were `value`/`error` plain
    // locals captured by reference, a callback firing after that unwind would
    // write through a dangling reference into destroyed stack memory. Routing
    // them through `state` means a late callback instead writes into orphaned
    // (but valid) heap memory — harmless, since nothing reads it anymore.
    struct State {
        std::optional<T> value;
        std::exception_ptr error;
    };
    auto state = std::make_shared<State>();

    completion.then([state](T resolved) { state->value = std::move(resolved); })
        .onError([state](const std::exception_ptr& err) { state->error = err; });

    const bool settled = pumpUntil([state] { return state->value.has_value() || state->error != nullptr; }, deadline);
    if (!settled) {
        throw std::runtime_error("awaitQt: deadline elapsed before the completion resolved");
    }
    if (state->error) {
        std::rethrow_exception(state->error);
    }
    return std::move(*state->value);
}

/// @brief `pumpUntil(!presenter.busy())` — waits for a presenter's tracked
///        completions to drain. See `examples/common/gui/presenter.hpp`
///        (Task 6) for `busy()`'s contract; this template has no header
///        dependency on that type, so Task 6 requires no change here.
/// @tparam PresenterLike Anything exposing `bool busy() const`.
/// @param presenter Presenter whose in-flight completions to drain.
/// @param deadline  Wall-clock budget passed through to `pumpUntil`.
/// @return `true` if the presenter went idle before the deadline, `false` on
///         timeout — `[[nodiscard]]` because a silently ignored timeout turns
///         "the action never completed" into "the assertion below reads stale
///         state", which is exactly the flake this primitive exists to avoid.
template <typename PresenterLike>
[[nodiscard]] bool settle(const PresenterLike& presenter,
                          std::chrono::milliseconds deadline = std::chrono::milliseconds{5000}) {
    return pumpUntil([&] { return !presenter.busy(); }, deadline);
}

}  // namespace morph::ladder::testkit
