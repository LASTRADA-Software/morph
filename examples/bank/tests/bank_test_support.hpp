// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "bank/db/database.hpp"
#include "bank/db/entities.hpp"
#include "bank/db/user_ops.hpp"
#include "unique_test_database.hpp"

/// @file
/// Shared helpers for the bank example tests.

namespace bank::testing {

/// @brief The ODBC connection string every test in this process shares.
///
/// A path of this process's own, so two ctest cases running concurrently never
/// name the same SQLite file.
///
/// @return The connection string.
[[nodiscard]] inline const std::string& connectionString() { return uniqueDatabaseConnection(); }

/// @brief Sets up this process's test database exactly once.
///
/// All tests in one process run against one on-disk SQLite file (a single
/// `:memory:` connection cannot be shared across the per-model DataMappers).
/// Migrations are applied once; individual tests isolate themselves by using
/// unique owner principals rather than by wiping tables.
///
/// The file is private to the process rather than a fixed path shared by every
/// bank test binary -- see unique_test_database.hpp for why, and for what a
/// fixed path cost under `ctest -j`.
inline void ensureDatabase() {
    static const bool once = [] {
        bank::db::setup(connectionString());
        return true;
    }();
    (void)once;
}

/// @brief Ensures the database exists and a `users` row for @p principal does
///        too, so models can resolve it to a `user_id`.
///
/// `App::login` provisions the principal automatically; tests that drive a model
/// without `App` (e.g. the remote-backend test, which sets the session principal
/// directly) call this to get the same effect.
inline void ensurePrincipal(const std::string& principal) {
    ensureDatabase();
    Lightweight::DataMapper mapper;
    bank::db::ensureUser(mapper, principal);
}

/// @brief Runs a morph action to completion synchronously by pumping @p gui.
///
/// Posts the completion's callbacks onto @p gui (which must be the same
/// executor the handler was constructed with), drains it on the calling thread
/// until the result or error arrives, and either returns the value or rethrows
/// the error — so tests can write straight-line `REQUIRE(await(...) == ...)`.
///
/// @tparam T         The action's result type.
/// @param completion The completion returned by `handler.execute(action)`.
/// @param gui        The pumpable GUI executor to drain.
/// @return The resolved value.
template <typename T>
T await(morph::async::Completion<T> completion, morph::exec::MainThreadExecutor& gui) {
    std::atomic<bool> done{false};
    std::optional<T> value;
    std::exception_ptr error;
    completion
        .then([&](T resolved) {
            value = std::move(resolved);
            done.store(true);
        })
        .onError([&](const std::exception_ptr& err) {
            error = err;
            done.store(true);
        });
    while (!done.load()) {
        gui.runFor(std::chrono::milliseconds{20});
    }
    if (error) {
        std::rethrow_exception(error);
    }
    return std::move(*value);
}

// -- Why the two durations below are two types -------------------
//
// This `waitUntil` used to take `(Pred, milliseconds budget, milliseconds step,
// MainThreadExecutor&)`: two adjacent, same-type parameters that every caller
// must pass, whose values differ by 400x at both live call sites. Transposing
// them compiled silently and produced a 2000 ms pump quantum inside a 5 ms
// budget -- one predicate check, then failure. Measured on `d03c66f3` before
// this change, by transposing `test_payee.cpp:70` and compiling that
// translation unit with its own real command from
// `build/linux-everything/compile_commands.json` (clang 22, `-std=c++23
// -Weverything -Werror` plus the project's `-Wno-` list, `-fsyntax-only`): no
// diagnostic at all, exit 0.
//
// `WaitBudget` and `WaitStep` make the transposition a compile error instead.
// Both constructors are `explicit`, so neither a raw
// `std::chrono::milliseconds` nor the other wrapper converts, and the
// `static_assert` block below pins every route back to the hazard.
//
// The same two types, with the same names and the same explicit constructors,
// are what `tests/test_support.hpp`'s framework `waitUntil` takes,
// and what `examples/kanban/tests/test_kanban_stress.cpp` carries -- the same
// shape in three places rather than three shapes. They are redeclared here
// because bank deliberately links neither `morph_ladder_testkit` nor the
// framework's private test headers (see `examples/bank/CMakeLists.txt`'s own
// note on why bank is not a ladder rung).
//
// A `NOLINT` is not an option: it would remove the *warning* and leave the
// hazard. Nor is widening one parameter's type to silence
// `bugprone-easily-swappable-parameters`, which leaves the transposition
// compiling.

/// @brief `waitUntil`'s overall polling budget: the longest it may wait before
///        giving up and returning `false`.
///
///        A distinct type from `WaitStep` so passing the two in the wrong
///        order is a compile error rather than a 400x-wrong program.
///        Construction is deliberately explicit.
struct WaitBudget {
    /// @brief The budget itself.
    std::chrono::milliseconds value;

    /// @brief Wraps a duration as a budget.
    /// @param millis How long `waitUntil` may keep polling.
    explicit constexpr WaitBudget(std::chrono::milliseconds millis) noexcept : value{millis} {}
};

/// @brief `waitUntil`'s polling step: how long it pumps @p gui between
///        predicate checks.
///
///        A distinct type from `WaitBudget` -- see that type, and the note
///        above it, for why. Construction is deliberately explicit.
struct WaitStep {
    /// @brief The step itself.
    std::chrono::milliseconds value;

    /// @brief Wraps a duration as a polling step.
    /// @param millis How long to pump between predicate checks.
    explicit constexpr WaitStep(std::chrono::milliseconds millis) noexcept : value{millis} {}
};

/// @brief Polls @p pred until true or @p budget elapses, **pumping @p gui** each
///        step so that callbacks posted to the GUI executor actually run.
/// @tparam Pred  Nullary predicate returning something contextually
///               convertible to `bool`.
/// @param pred   Polled until it returns `true`.
/// @param budget Longest time to keep polling before returning `false`.
/// @param step   How long to pump @p gui between two polls.
/// @param gui    The main-thread executor to pump.
/// @return `true` if @p pred became `true` within @p budget.
template <typename Pred>
bool waitUntil(Pred pred, WaitBudget budget, WaitStep step, morph::exec::MainThreadExecutor& gui) {
    const auto deadline = std::chrono::steady_clock::now() + budget.value;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        gui.runFor(step.value);
    }
    return true;
}

namespace detail {

// Satisfied when `waitUntil` is callable with `Args` -- the predicate type
// first, then whatever follows it. Plain comments rather than Doxygen because
// clang's `-Wdocumentation` does not accept `@tparam` on a concept.
template <typename... Args>
concept WaitUntilCallableWith = requires(Args... args) { waitUntil(args...); };

/// @brief A stand-in predicate type for the assertions below.
using ExampleWaitPred = bool (*)();

/// @brief A stand-in executor reference type for the assertions below.
using ExampleGui = morph::exec::MainThreadExecutor&;

// The acceptance test for that compile error, in the header that owns the
// hazard, so it runs in every bank test translation unit that includes it.
//
// What must keep working -- both live call sites pass all four arguments:
static_assert(WaitUntilCallableWith<ExampleWaitPred, WaitBudget, WaitStep, ExampleGui>);

// What must not compile. The first is the transposition itself; the rest are
// the routes back to it, each of which would restore a silent 400x error.
static_assert(!WaitUntilCallableWith<ExampleWaitPred, WaitStep, WaitBudget, ExampleGui>);
static_assert(
    !WaitUntilCallableWith<ExampleWaitPred, std::chrono::milliseconds, std::chrono::milliseconds, ExampleGui>);
static_assert(!WaitUntilCallableWith<ExampleWaitPred, WaitBudget, WaitBudget, ExampleGui>);
static_assert(!WaitUntilCallableWith<ExampleWaitPred, WaitStep, WaitStep, ExampleGui>);

}  // namespace detail

}  // namespace bank::testing
