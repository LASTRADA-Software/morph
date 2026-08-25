// SPDX-License-Identifier: Apache-2.0
//
// Task 19: concurrent-move stress test, run under ThreadSanitizer in CI
// (ThreadPoolExecutor{4} only -- CI keeps Qt stacks out of the sanitizer
// matrix, examples/TESTING.md's kanban-specific note).
//
// This file's client-setup/interleave body was written only after reading
// examples/common/testkit/strand_interleaver.hpp's real API, not against the
// brief's guess. Two load-bearing findings from that read (see
// docs/superpowers/sdd/2026-08-16-kanban-backend/task-19-report.md for the
// full account):
//
//   1. The class the brief calls "StrandInterleaver" does not exist anywhere
//      in the tree; strand_interleaver.hpp defines `DeterministicExecutor`,
//      which sits *underneath* a `morph::exec::detail::StrandExecutor` as its
//      `base` `IExecutor` and only runs posted tasks when explicitly
//      `step()`/`runSchedule()`-d. It is exercised directly against
//      `StrandExecutor` in test_strand_interleaver.cpp, naming the production
//      `detail::` types by hand.
//   2. A `BackendRig{Mode::Local, ...}` builds its own `ThreadPoolExecutor`
//      internally (backend_rig.hpp's Mode::Local branch) and hands it
//      straight to `LocalBackend`, which wraps it in its own internal strand
//      executor -- there is no seam for a test to substitute a
//      `DeterministicExecutor` underneath that strand. `DeterministicExecutor`
//      is therefore not wireable into a `BackendRig`-driven test at all: it is
//      a lower-level harness for testing `StrandExecutor` in isolation, not a
//      knob `BackendRig`/`BoardModel` tests can reach.
//
// Given that, this test exercises the *real* concurrency guarantee design
// spec §8 actually asks for: `BoardModel` is keyed/shared per `projectId`
// (`ModelKeyTraits<BoardModel>`, board_model.hpp), so every client attached to
// the same project drives the *same* server-side instance, serialized behind
// one strand backed by a real `ThreadPoolExecutor{4}`. Determinism here comes
// from `SeededScript`'s seeded RNG (reproducible action sequence --
// MORPH_STRESS_SEED to re-run a failure) and from the invariant check
// happening only after every fired action has genuinely settled, not from
// single-stepping the executor. Real concurrent dispatch across the pool's 4
// worker threads, racing on the shared strand, is exactly what a
// ThreadSanitizer run over this test is meant to catch -- a `Completion`
// resolving into a `.then/.onError` pair while another worker thread is still
// inside `BoardModel::execute(MoveTaskPosition)` would be a real data race
// TSan should flag, and the two invariants below (dense/unique positions, no
// task lost or duplicated) are the correctness half of that same guarantee.
//
// `SeededScript`'s schedule is generated lazily per `next()` call (not
// computed up front -- Task 16's own follow-up note), which does not matter
// here: this test never needs the full shape of a client's schedule before or
// during the run, only "generate one action, fire it, repeat" -- exactly
// `next()`'s designed usage. Nothing here needs the eagerly-materialized
// schedule TESTING.md's description would imply.
//
// **No Qt anywhere in this file (fixing morph#128)**: the original version of
// this test drove everything through `BackendRig{Mode::Local, ...}` and
// `awaitQt`/`pumpUntil` (examples/common/testkit/pump.hpp), which the CI
// job's own comment claimed involved "no Qt/GUI" -- a claim morph#128 proved
// false: `Mode::Local` unconditionally constructs a real `morph::qt::
// QtExecutor` for client-facing callback delivery (backend_rig.hpp's own
// doc comment explains why: pool-thread callback delivery would race
// pumpUntil/awaitQt's unsynchronized reads otherwise), and every one of the
// 165 ThreadSanitizer warnings morph#128 catalogued bottoms out in genuine
// Qt-internal frames (QMetaObject::invokeMethod, QCallableObject,
// QObject::event) reached through that QtExecutor. Since a prebuilt,
// non-TSan-instrumented Qt package can't be seen through by ThreadSanitizer,
// those warnings are unusable evidence either way -- real bugs or false
// positives, TSan cannot tell from outside an instrumented Qt build.
//
// This version drives `BoardModel` through a bare `morph::bridge::Bridge`
// wrapping a `morph::backend::LocalBackend` directly (the exact pattern
// `tests/test_concurrency_invariants.cpp`'s own concurrent-dispatch test
// already uses), with `morph::testing::InlineExecutor`-equivalent semantics
// for client-facing callback delivery (defined locally below -- `tests/
// test_support.hpp` itself is private to the `tests/` target, not on
// `examples/`'s include path, and the class is two lines) instead of
// `QtExecutor`. `BoardModel`'s own `requireRole`/session checks are backend-
// agnostic (they read `morph::session::current()` directly, never routed
// through an `IAuthorizer` in `Mode::Local`'s old shape either -- confirmed
// against board_model.hpp and backend_rig.hpp: `Mode::Local` never
// constructs or uses an authorizer at all), so `Bridge::setDefaultSession`
// still gates access exactly as before. `ModelKeyTraits<BoardModel>`'s
// shared-per-project instance semantics are a `Bridge`-level mechanism
// (`registerModelShared`), unaffected by how `Bridge`/`LocalBackend` were
// constructed. The result: every code path this test exercises is the real
// morph core (Bridge, LocalBackend, StrandExecutor, ThreadPoolExecutor,
// Completion) with zero Qt frames anywhere in the call graph, making this
// CI job's own "no Qt/GUI involvement" premise genuinely true rather than
// merely claimed.
#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <string>
#include <thread>
#include <vector>

#include "kanban/dto/project_dto.hpp"
#include "kanban/models/board_model.hpp"
#include "kanban/models/project_admin_model.hpp"
#include "testkit/action_driver.hpp"
#include "testkit/db_fixture.hpp"

using morph::bridge::AllowShared;
using morph::bridge::Bridge;
using morph::bridge::BridgeHandler;
using morph::ladder::testkit::SeededScript;

namespace {

/// @brief Client-facing callback executor that runs every posted continuation
///        immediately, on whichever thread resolves the `Completion` --
///        never Qt's event loop. Same two-line shape as `morph::testing::
///        InlineExecutor` (`tests/test_support.hpp`, private to the `tests/`
///        target) and the identical role `test_concurrency_invariants.cpp`'s
///        own `InlineExec` alias plays for `morph::bridge::Bridge`'s
///        concurrent-dispatch test. `.then()`/`.onError()` bodies below only
///        touch `std::atomic`s, so running them concurrently from multiple
///        `ThreadPoolExecutor` worker threads (one per resolving completion)
///        is race-free by construction -- no additional synchronization
///        needed here.
struct InlineExecutor : morph::exec::IExecutor {
    void post(std::function<void()> fn) override { fn(); }
};

/// @brief Polls @p pred until it returns `true` or @p budget elapses,
///        sleeping @p step between polls. Same shape as `morph::testing::
///        waitUntil` (`tests/test_support.hpp`) and this file's own former
///        `pumpUntil` call, minus the Qt event-loop pump -- nothing here
///        needs one, since no callback in this file is ever queued onto a
///        Qt event loop in the first place.
template <typename Pred>
[[nodiscard]] bool waitUntil(Pred pred, std::chrono::milliseconds budget = std::chrono::milliseconds{20000},
                             std::chrono::milliseconds step = std::chrono::milliseconds{5}) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(step);
    }
    return true;
}

/// @brief Builds a signed session `Context` for @p principal, issued by
///        @p issuer. Same pattern as test_shared_instance_lifecycle.cpp's
///        `tokenContextFor`.
[[nodiscard]] morph::session::Context tokenContextFor(const morph::session::TokenIssuer& issuer,
                                                      std::string principal) {
    morph::session::Context ctx;
    ctx.principal = principal;
    ctx.token = issuer.issue(morph::session::SessionToken{
        .principal = std::move(principal), .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
    return ctx;
}

/// @brief True iff, within every column, the tasks placed there have
///        positions forming a dense `0..n-1` run with no gaps or duplicates.
///        Design spec §8's first invariant.
[[nodiscard]] bool positionsAreDenseAndUnique(const kanban::GetBoardResult& state) {
    for (const auto& column : state.columns) {
        std::vector<std::int64_t> positions;
        for (const auto& task : state.tasks) {
            if (task.columnId == column.id) {
                positions.push_back(task.position);
            }
        }
        std::sort(positions.begin(), positions.end());
        for (std::size_t i = 0; i < positions.size(); ++i) {
            if (positions[i] != static_cast<std::int64_t>(i)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

TEST_CASE("Concurrent MoveTaskPosition calls (N=4) never desync positions -- run under ThreadSanitizer",
          "[kanban][stress][tsan]") {
    morph::ladder::testkit::DbFixture fixture;

    // Real concurrent dispatch on a real pool -- no Qt anywhere in this
    // file's call graph (see this file's own top comment for why that
    // matters). `Bridge` owns the `LocalBackend`, which owns a strand over
    // `workerPool`; every model action genuinely runs on one of these 4
    // threads, serialized per-model-instance by the strand.
    morph::exec::ThreadPoolExecutor workerPool{4};
    Bridge bridge{std::make_unique<morph::backend::LocalBackend>(workerPool)};
    InlineExecutor clientExecutor;

    constexpr std::string_view kSecret = "test-secret-32-bytes-minimum!!!!";
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    // `Mode::Local`'s own real shape shared one Bridge across every "client"
    // (backend_rig.hpp's own doc comment); this test's 4 handlers below share
    // this one `bridge` for the identical reason -- `setDefaultSession` here
    // just needs to run once.
    bridge.setDefaultSession(tokenContextFor(issuer, "alice"));

    // CreateProject via a plain (non-keyed) handler -- alice becomes this
    // project's Manager automatically (ProjectAdminModel::execute(CreateProject)).
    BridgeHandler<kanban::ProjectAdminModel> creator{bridge, &clientExecutor};
    kanban::CreateProjectResult createdProject;
    {
        std::atomic<bool> done{false};
        creator.execute(kanban::CreateProject{.name = "Stress Board"})
            .then([&](const kanban::CreateProjectResult& result) {
                createdProject = result;
                done.store(true);
            })
            .onError([&](const std::exception_ptr&) { done.store(true); });
        REQUIRE(waitUntil([&] { return done.load(); }));
    }
    const auto projectId = createdProject.id;

    // Four independent AllowShared handlers, all attaching to the same
    // projectId -- BoardModel is keyed per-project, so all four share one
    // server-side instance and therefore one strand (board_model.hpp's
    // ModelKeyTraits<BoardModel> specialization) -- a `Bridge`-level
    // mechanism (`registerModelShared`), unaffected by dropping `BackendRig`.
    constexpr std::size_t kClients = 4;
    std::vector<std::unique_ptr<BridgeHandler<kanban::BoardModel, AllowShared>>> handlers;
    for (std::size_t i = 0; i < kClients; ++i) {
        handlers.push_back(std::make_unique<BridgeHandler<kanban::BoardModel, AllowShared>>(bridge, &clientExecutor));
        std::atomic<bool> opened{false};
        handlers.back()
            ->execute(kanban::OpenBoard{.projectId = projectId})
            .then([&](const kanban::GetBoardResult&) { opened.store(true); })
            .onError([&](const std::exception_ptr&) { opened.store(true); });
        REQUIRE(waitUntil([&] { return opened.load(); }));
    }

    // Seed the board: 2 columns (unlimited WIP -- a WIP-limit Conflict would
    // make MoveTaskPosition's failure path, not its exactly-once/renumbering
    // path, the thing under stress here), 1 swimlane, 8 tasks split across
    // the two columns. Seeding happens sequentially through handlers[0], each
    // step awaited before the next -- no concurrency pressure needed yet,
    // that starts once the board is populated.
    auto& seeder = *handlers[0];
    kanban::ColumnId col1;
    kanban::ColumnId col2;
    kanban::SwimlaneId swimlaneId;
    {
        std::atomic<bool> done{false};
        seeder.execute(kanban::CreateColumn{.name = "To Do", .wipLimit = 0})
            .then([&](const kanban::GetBoardResult& state) {
                col1 = state.columns.back().id;
                done.store(true);
            })
            .onError([&](const std::exception_ptr&) { done.store(true); });
        REQUIRE(waitUntil([&] { return done.load(); }));
    }
    {
        std::atomic<bool> done{false};
        seeder.execute(kanban::CreateColumn{.name = "Done", .wipLimit = 0})
            .then([&](const kanban::GetBoardResult& state) {
                col2 = state.columns.back().id;
                done.store(true);
            })
            .onError([&](const std::exception_ptr&) { done.store(true); });
        REQUIRE(waitUntil([&] { return done.load(); }));
    }
    {
        std::atomic<bool> done{false};
        seeder.execute(kanban::CreateSwimlane{.name = "Default"})
            .then([&](const kanban::GetBoardResult& state) {
                swimlaneId = state.swimlanes.back().id;
                done.store(true);
            })
            .onError([&](const std::exception_ptr&) { done.store(true); });
        REQUIRE(waitUntil([&] { return done.load(); }));
    }

    std::vector<kanban::TaskId> taskIds;
    for (int i = 0; i < 8; ++i) {
        const auto columnId = (i % 2 == 0) ? col1 : col2;
        std::atomic<bool> done{false};
        kanban::TaskId newTaskId;
        seeder
            .execute(kanban::CreateTask{
                .columnId = columnId, .swimlaneId = swimlaneId, .title = "Task " + std::to_string(i)})
            .then([&](const kanban::GetBoardResult& state) {
                newTaskId = state.tasks.back().id;
                done.store(true);
            })
            .onError([&](const std::exception_ptr&) { done.store(true); });
        REQUIRE(waitUntil([&] { return done.load(); }));
        taskIds.push_back(newTaskId);
    }
    REQUIRE(taskIds.size() == 8);

    const std::vector<kanban::ColumnId> columns{col1, col2};

    // One SeededScript<MoveTaskPosition> per client, each with its own seed
    // (offset from a shared base so MORPH_STRESS_SEED still reproduces the
    // whole run deterministically by shifting every client's seed together).
    // burstSize/onBurst are unused here -- the real invariant check happens
    // once, after every client's actions have all been fired and settled, not
    // per-burst -- so onBurst is a no-op and burstSize is set larger than the
    // per-client action count to guarantee onBurst never fires mid-run
    // (flushBurst() at the end still runs the no-op once per client, which is
    // harmless).
    constexpr std::uint64_t kBaseSeed = 20260816;
    constexpr int kActionsPerClient = 50;

    std::vector<std::unique_ptr<SeededScript<kanban::MoveTaskPosition>>> scripts;
    for (std::size_t i = 0; i < kClients; ++i) {
        scripts.push_back(std::make_unique<SeededScript<kanban::MoveTaskPosition>>(
            kBaseSeed + i,
            std::vector<typename SeededScript<kanban::MoveTaskPosition>::WeightedGenerator>{
                {1,
                 [&taskIds, &columns, swimlaneId, i] {
                     // Captured by value into a fresh RNG per generator call
                     // would defeat SeededScript's own determinism, so the
                     // pick itself has to come from something reproducible:
                     // reuse the client index and a rotating counter seeded
                     // off i to vary target task/column/position across calls
                     // without a second, uncontrolled random source. A
                     // thread_local-free static counter is fine here -- all
                     // generation happens sequentially on the test's own
                     // thread, before any action is fired.
                     static std::vector<int> counters(4, 0);
                     const int c = counters[i]++;
                     const auto taskId = taskIds[static_cast<std::size_t>(c) % taskIds.size()];
                     const auto columnId = columns[static_cast<std::size_t>(c / 3) % columns.size()];
                     const auto position = static_cast<std::int64_t>((c * 7 + static_cast<int>(i)) % 8);
                     return kanban::MoveTaskPosition{.taskId = taskId,
                                                     .columnId = columnId,
                                                     .swimlaneId = swimlaneId,
                                                     .position = position,
                                                     .opId = ""};
                 }}},
            /*burstSize=*/kActionsPerClient + 1, /*onBurst=*/[](const std::vector<kanban::MoveTaskPosition>&) {}));
    }

    // Fire every client's ~50 MoveTaskPosition calls without awaiting between
    // them: BridgeHandler::execute() returns immediately with a Completion,
    // so this loop dispatches all kClients * kActionsPerClient actions before
    // any of them necessarily has resolved. BoardModel's shared instance runs
    // its actual work on the rig's real ThreadPoolExecutor{4} via
    // LocalBackend's strand -- so with 4 clients each racing to post onto
    // that one strand, this is genuine concurrent pressure on the same
    // server-side instance, not single-threaded simulated interleaving.
    // Completions still resolve one at a time (the strand serializes the
    // *work*), but the *posting*/dispatch machinery around it runs from real,
    // concurrently-scheduled pool threads -- exactly what a ThreadSanitizer
    // run over this test exists to check. `.then`/`.onError` now run inline
    // on whichever pool thread resolves the completion (InlineExecutor,
    // above), not on a Qt thread -- both callbacks only touch atomics, so
    // this is race-free.
    std::atomic<int> outstanding{0};
    std::atomic<int> failures{0};
    for (std::size_t i = 0; i < kClients; ++i) {
        for (int a = 0; a < kActionsPerClient; ++a) {
            const auto action = scripts[i]->next();
            ++outstanding;
            handlers[i]
                ->execute(action)
                .then([&outstanding](const kanban::GetBoardResult&) { --outstanding; })
                .onError([&outstanding, &failures](const std::exception_ptr&) {
                    // A move landing on an already-occupied slot mid-shuffle
                    // (e.g. two clients targeting the same column/position in
                    // the same burst) is an expected, benign outcome of
                    // firing randomly-generated moves concurrently -- not
                    // every generated action is guaranteed conflict-free.
                    // What must never happen is a *crash*, a *hang*, or the
                    // two invariants below failing once the dust settles;
                    // this handler only counts failures for CAPTURE/logging,
                    // it does not fail the test by itself.
                    --outstanding;
                    ++failures;
                });
        }
    }
    for (auto& script : scripts) {
        script->flushBurst();
    }

    // 90s, not 20s: this loop's own real-thread-pool callback delivery
    // (InlineExecutor, above) runs every .then()/.onError() directly on
    // whichever pool worker resolves each completion, unlike the original
    // Qt-based version's client-side callback delivery -- and ThreadSanitizer
    // instrumentation adds a well-documented 5-15x slowdown on top of that.
    // Confirmed empirically: this exact 200-action workload (4 clients x 50
    // actions) finished in ~32s under real TSan instrumentation in CI, comfortably
    // inside 90s but past the original, un-scaled 20s budget the Qt-based
    // version used without ever actually needing more (its own callback
    // delivery path happened to be fast enough not to hit this).
    REQUIRE(waitUntil([&outstanding] { return outstanding.load() == 0; }, std::chrono::milliseconds{90000}));
    CAPTURE(failures.load());

    // Fetch one final GetBoardState and assert both design spec §8 invariants.
    kanban::GetBoardResult finalState;
    {
        std::atomic<bool> done{false};
        handlers[0]
            ->execute(kanban::GetBoardState{})
            .then([&](const kanban::GetBoardResult& state) {
                finalState = state;
                done.store(true);
            })
            .onError([&](const std::exception_ptr&) { done.store(true); });
        REQUIRE(waitUntil([&] { return done.load(); }));
    }

    if (!positionsAreDenseAndUnique(finalState)) {
        for (const auto& column : finalState.columns) {
            std::string line = "column " + std::to_string(*column.id) + ":";
            for (const auto& task : finalState.tasks) {
                if (task.columnId == column.id) {
                    line += " [task " + std::to_string(*task.id) + " pos " + std::to_string(task.position) + "]";
                }
            }
            WARN(line);
        }
    }
    CHECK(positionsAreDenseAndUnique(finalState));

    // Every task created at setup must still appear exactly once across all
    // columns -- no task vanished or duplicated under concurrent moves.
    REQUIRE(finalState.tasks.size() == taskIds.size());
    for (const auto& taskId : taskIds) {
        const auto count = std::count_if(finalState.tasks.begin(), finalState.tasks.end(),
                                         [&taskId](const kanban::TaskView& task) { return task.id == taskId; });
        CHECK(count == 1);
    }
}
