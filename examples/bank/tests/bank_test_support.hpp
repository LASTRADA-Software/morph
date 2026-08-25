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

/// @file
/// Shared helpers for the bank example tests.

namespace bank::testing {

/// @brief Sets up the shared test database exactly once for the whole binary.
///
/// All tests run against one on-disk SQLite file (a single `:memory:`
/// connection cannot be shared across the per-model DataMappers). Migrations
/// are applied once; individual tests isolate themselves by using unique owner
/// principals rather than by wiping tables.
inline void ensureDatabase() {
    static const bool once = [] {
        const auto path = std::filesystem::temp_directory_path() / "morph_bank_tests.db";
        std::error_code err;
        std::filesystem::remove(path, err);
        bank::db::setup("DRIVER=SQLite3;Database=" + path.string());
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

/// @brief Polls @p pred until true or @p budget elapses, **pumping @p gui** each
///        step so that callbacks posted to the GUI executor actually run.
template <typename Pred>
bool waitUntil(Pred pred, std::chrono::milliseconds budget, std::chrono::milliseconds step,
               morph::exec::MainThreadExecutor& gui) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        gui.runFor(step);
    }
    return true;
}

}  // namespace bank::testing
