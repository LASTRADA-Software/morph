// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <stdexcept>
#include <string>

#include "test_support.hpp"

namespace {
using CovSyncExecutor = morph::testing::InlineExecutor;
}  // namespace

// ── executor.hpp: while-loop guard takes its `now() >= deadline` arm ─────────
//
// All existing runFor tests enter the loop body and exit via `wait_until` →
// `return;`. The while condition's False arm at line 128 stays untaken. Run
// with a zero-duration timeout: deadline equals "now" at entry, so the next
// `now()` read is past deadline and the loop is skipped.

TEST_CASE("morph::exec::MainThreadExecutor: runFor with zero timeout skips the loop", "[executor]") {
    morph::exec::MainThreadExecutor exec;
    bool taskRan = false;
    exec.post([&] { taskRan = true; });
    exec.runFor(std::chrono::milliseconds(0));
    REQUIRE_FALSE(taskRan);  // queue not drained — loop never entered
}

// ── bridge.hpp: easy partial branches ────────────────────────────────────────
//
// unsubscribe<A> on a handler that never subscribed → False arm of `iter != end`.
// reset<A>      on a handler that never `set<...>`  → False arm of `iter != end`.

struct CovAction {
    int v = 0;
};
struct CovModel {
    int execute(CovAction act) { return act.v + 1; }
};

BRIDGE_REGISTER_MODEL(CovModel, "Cov_CovModel")
BRIDGE_REGISTER_ACTION(CovModel, CovAction, "Cov_CovAction")

TEST_CASE("BridgeHandler::unsubscribe on type with no entry is a no-op", "[bridge]") {
    morph::exec::ThreadPoolExecutor pool{1};
    CovSyncExecutor cb;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CovModel> handler{bridge, &cb};

    // No prior subscribe<int>, so the removal path finds nothing to erase.
    REQUIRE_NOTHROW(handler.unsubscribe<int>());
}

TEST_CASE("BridgeHandler::unsubscribe is idempotent", "[bridge]") {
    morph::exec::ThreadPoolExecutor pool{1};
    CovSyncExecutor cb;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CovModel> handler{bridge, &cb};

    handler.subscribe<int>([](int) {});
    REQUIRE_NOTHROW(handler.unsubscribe<int>());
    REQUIRE_NOTHROW(handler.unsubscribe<int>());
}
