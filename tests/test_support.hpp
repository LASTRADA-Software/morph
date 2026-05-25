// SPDX-License-Identifier: Apache-2.0

#pragma once

// Common helpers used across the morph test suite. Each helper here was being
// re-declared (often with slightly different names — SyncExec / SyncExecutor /
// InlineExec) inside ~15 individual test translation units; consolidating them
// keeps the test code consistent and lets the production API not have to expose
// test-only utilities.

#include <morph/executor.hpp>
#include <morph/wire.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <utility>

namespace morph::testing {

/// @brief `IExecutor` that runs every posted task synchronously on the caller's thread.
///
/// Replaces ad-hoc `SyncExec` / `SyncExecutor` / `InlineExec` definitions that
/// were sprinkled across the test suite. Catch2 assertions executed inside the
/// posted lambda surface on the same thread as the test body, so failures keep
/// pointing at the right source location.
struct InlineExecutor : ::morph::exec::IExecutor {
    void post(std::function<void()> fn) override { fn(); }
};

/// @brief Default polling budget for `waitUntil`. Picked to cover the slowest
///        TSan/MSan runs without making green tests visibly slow.
inline constexpr std::chrono::milliseconds kDefaultWaitBudget{2000};

/// @brief Default polling step for `waitUntil`.
inline constexpr std::chrono::milliseconds kDefaultWaitStep{5};

/// @brief Polls @p pred until it returns `true` or @p budget elapses.
///
/// Returns `true` if the predicate eventually became `true`, `false` if the
/// budget expired first. Sleeps for @p step between polls so we don't burn the
/// CPU. Sized for asynchronous test fixtures: most callers should just write
/// `REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));`
template <typename Pred>
bool waitUntil(Pred pred, std::chrono::milliseconds budget = kDefaultWaitBudget,
               std::chrono::milliseconds step = kDefaultWaitStep) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(step);
    }
    return true;
}

/// @brief Collects a single `RemoteServer` reply and decodes it.
///
/// Designed to be passed as the reply callback to `RemoteServer::handle()`:
///
/// @code
/// morph::testing::WaitReply waiter;
/// server->handle(envelopeJson, std::ref(waiter));
/// REQUIRE(waiter.await());
/// REQUIRE(waiter.env.kind == "ok");
/// @endcode
///
/// The raw reply string is kept available for tests that need to inspect
/// malformed responses (when `env` may have failed to decode).
struct WaitReply {
    std::atomic<bool> ready{false};
    std::string raw;
    ::morph::wire::Envelope env;

    /// @brief Reply-callback entry point.
    void operator()(const std::string& msg) {
        raw = msg;
        try {
            env = ::morph::wire::decode(msg);
        } catch (...) {
            // Leave `env` default-initialised; tests inspecting `raw` can still assert.
        }
        ready.store(true);
    }

    /// @brief Blocks (polling) until the reply arrives or @p budget elapses.
    /// @return `true` if a reply arrived within the budget.
    bool await(std::chrono::milliseconds budget = kDefaultWaitBudget) {
        return waitUntil([this] { return ready.load(); }, budget);
    }
};

}  // namespace morph::testing
