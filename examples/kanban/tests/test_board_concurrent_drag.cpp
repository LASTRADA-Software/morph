// SPDX-License-Identifier: Apache-2.0
//
// Task 3's own concurrent-drag stress test — mirrors test_kanban_stress.cpp's
// invariant exactly, but drives it through N `BoardBridge` instances (each
// wrapping its own `BoardPresenter`/`BridgeHandler<BoardModel, AllowShared>`)
// rather than raw `BoardModel::execute(MoveTaskPosition)` calls, proving the
// GUI's own code path — `BoardBridge::moveTask()`'s per-call `opId` minting,
// `BoardPresenter::moveTask()`'s per-call `track()` continuation, the Qt
// signal plumbing in between — doesn't break the exactly-once/dense-position
// guarantee the backend already proves at the model level
// (test_kanban_stress.cpp), or reintroduce the cross-contamination class of
// bug Task 2's ProjectAdminBridge::createProject fix round found and fixed
// (a per-call value stashed in a single shared field instead of captured in
// that call's own continuation).
//
// Read in full before writing this file, per this task's brief:
//  - test_kanban_stress.cpp itself (client-setup/interleave shape, and its
//    own header comment documenting two real API gotchas: no
//    "StrandInterleaver" class exists anywhere in the tree, and
//    BackendRig{Mode::Local, ...} builds its own ThreadPoolExecutor with no
//    seam for a DeterministicExecutor underneath — so this test, like that
//    one, drives real Mode::Local dispatch across a real
//    ThreadPoolExecutor{4}, not simulated/stepped execution).
//  - test_board_qml_bridge.cpp (this task's own bridge suite) for the
//    BackendRig/session-setup idiom this file reuses.
//
// Unlike test_kanban_stress.cpp's SeededScript-driven random action
// generator, this test drives a small fixed round-robin schedule per bridge
// (each bridge repeatedly moves its own "home" task between two columns at
// varying positions) — BoardBridge::moveTask() takes QString-typed ids, not
// the raw MoveTaskPosition DTO SeededScript<T>'s WeightedGenerator was built
// to produce, so reusing SeededScript verbatim here would need a new
// specialization for no real benefit: the property under test (every
// concurrently-dispatched moveTask() call resolves without corrupting the
// board) does not depend on the *particular* action-generation mechanism,
// only on genuinely concurrent, unawaited dispatch — which firing every
// bridge's whole schedule before awaiting any of it already provides, the
// same "fire all before awaiting" shape test_kanban_stress.cpp itself uses.

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <string>
#include <vector>

#include "board_qml_bridge.hpp"
#include "kanban/auth/kanban_authorizer.hpp"
#include "kanban/models/project_admin_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

namespace {

/// @brief Builds a signed session `Context` for @p principal, issued by
///        @p issuer. Same pattern as test_kanban_stress.cpp's own
///        `tokenContextFor` — `KanbanAuthorizer` is `SigningAuthorizer`-
///        derived, so a bare (unsigned) principal is not enough to pass
///        `requireRole`.
/// @param issuer    Mints the session token.
/// @param principal The identity to build a session for.
/// @return The signed session context.
[[nodiscard]] morph::session::Context tokenContextFor(const morph::session::TokenIssuer& issuer,
                                                      std::string principal) {
    morph::session::Context ctx;
    ctx.principal = principal;
    ctx.token = issuer.issue(morph::session::SessionToken{
        .principal = std::move(principal), .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
    return ctx;
}

/// @brief True iff, within every column, the tasks placed there have
///        positions forming a dense `0..n-1` run with no gaps or
///        duplicates. Design spec §8's first invariant — identical check to
///        test_kanban_stress.cpp's own `positionsAreDenseAndUnique`, just
///        reading the board back as a `QVariantMap` (this test's own
///        `BoardBridge::board()` property) instead of a `GetBoardResult`.
/// @param board The board property bag, as `BoardBridge::board()` returns it.
/// @return `true` if every column's task positions are dense and unique.
[[nodiscard]] bool positionsAreDenseAndUnique(const QVariantMap& board) {
    const QVariantList columns = board.value(QStringLiteral("columns")).toList();
    const QVariantList tasks = board.value(QStringLiteral("tasks")).toList();
    for (const QVariant& columnEntry : columns) {
        const QString columnId = columnEntry.toMap().value(QStringLiteral("id")).toString();
        std::vector<qlonglong> positions;
        for (const QVariant& taskEntry : tasks) {
            const QVariantMap task = taskEntry.toMap();
            if (task.value(QStringLiteral("columnId")).toString() == columnId) {
                positions.push_back(task.value(QStringLiteral("position")).toLongLong());
            }
        }
        std::sort(positions.begin(), positions.end());
        for (std::size_t i = 0; i < positions.size(); ++i) {
            if (positions[i] != static_cast<qlonglong>(i)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

TEST_CASE("Concurrent BoardBridge::moveTask calls (N=4) never desync positions", "[kanban][gui][stress]") {
    // Local rig mode on ThreadPoolExecutor only — mirrors
    // test_kanban_stress.cpp's own kanban-specific TSan/CI note
    // (examples/TESTING.md).
    DbFixture fixture;
    constexpr std::string_view kSecret = "test-secret-32-bytes-minimum!!!!";
    const auto authorizer =
        std::make_shared<kanban::auth::KanbanAuthorizer>(std::string{kSecret}, morph::session::hmacSha256);
    constexpr std::size_t kClients = 4;
    BackendRig rig{Mode::Local, kClients, authorizer};

    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    // Mode::Local's every "client" shares one Bridge (backend_rig.hpp's own
    // doc comment) — setDefaultSession only needs to run once, but calling
    // it kClients times is harmless, same rationale as
    // test_kanban_stress.cpp's identical loop.
    for (std::size_t i = 0; i < kClients; ++i) {
        rig.bridge(i).setDefaultSession(tokenContextFor(issuer, "alice"));
    }

    // Seed via one bridge: a project, 2 columns (unlimited WIP — a WIP-limit
    // Conflict would make MoveTaskPosition's failure path, not its
    // exactly-once/renumbering path, the thing under stress here), 1
    // swimlane, kClients tasks (one "home" task per bridge, so every
    // bridge's schedule below moves a distinct task without needing any
    // cross-bridge coordination to pick targets).
    morph::bridge::BridgeHandler<kanban::ProjectAdminModel> creator{rig.bridge(0), rig.executor()};
    const auto projectId =
        morph::ladder::testkit::awaitQt(creator.execute(kanban::CreateProject{.name = "Stress Board"})).id;

    // N independent BoardBridge instances, all attaching to the same
    // projectId — BoardModel is keyed per-project
    // (ModelKeyTraits<BoardModel>, board_model.hpp), so all four share one
    // server-side instance and therefore one strand backed by Mode::Local's
    // real ThreadPoolExecutor{4}, exactly test_kanban_stress.cpp's own
    // concurrency setup, now exercised through BoardBridge/BoardPresenter
    // instead of a bare BridgeHandler<BoardModel, AllowShared>.
    std::vector<std::unique_ptr<kanban::gui::BoardBridge>> bridges;
    for (std::size_t i = 0; i < kClients; ++i) {
        bridges.push_back(std::make_unique<kanban::gui::BoardBridge>(rig.bridge(i), rig.executor()));
    }

    // Attach every bridge to the board, awaiting each in turn (attach itself
    // is not what this test stresses — only the subsequent moveTask() calls
    // are fired concurrently, below).
    for (auto& bridge : bridges) {
        bool opened = false;
        const auto connection =
            QObject::connect(bridge.get(), &kanban::gui::BoardBridge::boardChanged, [&] { opened = true; });
        bridge->openBoard(QString::number(static_cast<qlonglong>(*projectId)));
        REQUIRE(pumpUntil([&] { return opened; }));
        QObject::disconnect(connection);
    }

    // Seed columns/swimlane/tasks through bridges[0].
    auto& seeder = *bridges.front();
    bool changed = false;
    const auto seedConnection =
        QObject::connect(&seeder, &kanban::gui::BoardBridge::boardChanged, [&] { changed = true; });

    changed = false;
    seeder.createColumn(QStringLiteral("To Do"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString col1 = seeder.board()
                             .value(QStringLiteral("columns"))
                             .toList()
                             .front()
                             .toMap()
                             .value(QStringLiteral("id"))
                             .toString();

    changed = false;
    seeder.createColumn(QStringLiteral("Done"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString col2 =
        seeder.board().value(QStringLiteral("columns")).toList().back().toMap().value(QStringLiteral("id")).toString();

    changed = false;
    seeder.createSwimlane(QStringLiteral("Default"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString swimlaneId = seeder.board()
                                   .value(QStringLiteral("swimlanes"))
                                   .toList()
                                   .front()
                                   .toMap()
                                   .value(QStringLiteral("id"))
                                   .toString();

    std::vector<QString> taskIds;
    for (std::size_t i = 0; i < kClients; ++i) {
        changed = false;
        const QString columnId = (i % 2 == 0) ? col1 : col2;
        seeder.createTask(columnId, swimlaneId, QStringLiteral("Task %1").arg(static_cast<int>(i)));
        REQUIRE(pumpUntil([&] { return changed; }));
        taskIds.push_back(seeder.board()
                              .value(QStringLiteral("tasks"))
                              .toList()
                              .back()
                              .toMap()
                              .value(QStringLiteral("id"))
                              .toString());
    }
    REQUIRE(taskIds.size() == kClients);
    QObject::disconnect(seedConnection);

    // Re-attach every bridge (including the seeder) so each one's `board`
    // property reflects the fully seeded state before the concurrent phase
    // starts — a bridge that only ever saw the empty board from its own
    // openBoard() call still dispatches moveTask() correctly (moveTask()
    // does not read `board` at all), but re-syncing here keeps every
    // bridge's own state honest for its own sake.
    for (auto& bridge : bridges) {
        bool refreshed = false;
        const auto connection =
            QObject::connect(bridge.get(), &kanban::gui::BoardBridge::boardChanged, [&] { refreshed = true; });
        bridge->refresh();
        REQUIRE(pumpUntil([&] { return refreshed; }));
        QObject::disconnect(connection);
    }

    const std::vector<QString> columns{col1, col2};

    // Fire every bridge's ~12 moveTask() calls without awaiting between
    // them: BoardBridge::moveTask() returns immediately (its presenter's
    // track()ed Completion resolves asynchronously), so this loop dispatches
    // every action before any of them necessarily has resolved. In
    // Mode::Local, BoardModel's shared instance runs its actual work on the
    // rig's real ThreadPoolExecutor{4} via LocalBackend's strand — four
    // bridges each racing to post onto that one strand is genuine
    // concurrent pressure on the same server-side instance, exactly
    // test_kanban_stress.cpp's own rationale for why this is a real
    // concurrency test, not simulated interleaving.
    constexpr int kMovesPerBridge = 12;
    std::atomic<int> outstanding{0};
    std::atomic<int> failures{0};
    for (std::size_t i = 0; i < kClients; ++i) {
        auto& bridge = *bridges[i];
        const auto failedConnection =
            QObject::connect(&bridge, &kanban::gui::BoardBridge::failed, [&outstanding, &failures](const QString&) {
                // A move landing on an already-occupied slot
                // mid-shuffle is an expected, benign outcome of
                // firing concurrent moves — see
                // test_kanban_stress.cpp's identical rationale.
                // What must never happen is a crash, a hang, or
                // the invariant below failing once the dust
                // settles.
                --outstanding;
                ++failures;
            });
        const auto movedConnection = QObject::connect(&bridge, &kanban::gui::BoardBridge::taskMoved,
                                                      [&outstanding](const QString&) { --outstanding; });
        (void)failedConnection;
        (void)movedConnection;

        for (int a = 0; a < kMovesPerBridge; ++a) {
            const QString destColumn = columns[static_cast<std::size_t>(a) % columns.size()];
            const auto position = (a * 3 + static_cast<int>(i)) % static_cast<int>(kClients);
            ++outstanding;
            bridge.moveTask(taskIds[i], destColumn, swimlaneId, position);
        }
    }

    REQUIRE(pumpUntil([&outstanding] { return outstanding.load() == 0; }, std::chrono::milliseconds{20000}));
    CAPTURE(failures.load());

    // Fetch one final refresh() and assert design spec §8's invariants,
    // reading state back via one bridge's board property (this task's own
    // brief: "reading state back via one bridge's board property").
    auto& reader = *bridges.front();
    bool finalRefreshed = false;
    const auto finalConnection =
        QObject::connect(&reader, &kanban::gui::BoardBridge::boardChanged, [&] { finalRefreshed = true; });
    reader.refresh();
    // The same 20s budget the outstanding-drag wait above uses, and for the
    // same reason: this refresh queues behind that scenario's whole backlog,
    // so pumpUntil's 5s default (sized for a single cheap action) fails
    // deterministically on any machine slower than CI's rather than flaking.
    REQUIRE(pumpUntil([&] { return finalRefreshed; }, std::chrono::milliseconds{20000}));
    QObject::disconnect(finalConnection);

    const QVariantMap finalBoard = reader.board();

    if (!positionsAreDenseAndUnique(finalBoard)) {
        for (const QVariant& columnEntry : finalBoard.value(QStringLiteral("columns")).toList()) {
            const QVariantMap column = columnEntry.toMap();
            std::string line = "column " + column.value(QStringLiteral("id")).toString().toStdString() + ":";
            for (const QVariant& taskEntry : finalBoard.value(QStringLiteral("tasks")).toList()) {
                const QVariantMap task = taskEntry.toMap();
                if (task.value(QStringLiteral("columnId")).toString() ==
                    column.value(QStringLiteral("id")).toString()) {
                    line += " [task " + task.value(QStringLiteral("id")).toString().toStdString() + " pos " +
                            std::to_string(task.value(QStringLiteral("position")).toLongLong()) + "]";
                }
            }
            WARN(line);
        }
    }
    CHECK(positionsAreDenseAndUnique(finalBoard));

    // Every task created at setup must still appear exactly once across all
    // columns — no task vanished or duplicated under concurrent moves.
    const QVariantList finalTasks = finalBoard.value(QStringLiteral("tasks")).toList();
    REQUIRE(static_cast<std::size_t>(finalTasks.size()) == taskIds.size());
    for (const QString& taskId : taskIds) {
        const auto count = std::count_if(finalTasks.begin(), finalTasks.end(), [&taskId](const QVariant& entry) {
            return entry.toMap().value(QStringLiteral("id")).toString() == taskId;
        });
        CHECK(count == 1);
    }
}
