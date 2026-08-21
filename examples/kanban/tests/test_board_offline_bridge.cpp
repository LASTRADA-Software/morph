// SPDX-License-Identifier: Apache-2.0
//
// Task 5: BoardBridge (not raw BoardModel/SyncWorker) driven through:
// online move (goes straight through), forced-offline move (queues instead),
// then simulated reconnect (SyncWorker drains the queue and the move
// actually lands). This is the "queued moves replay on reconnect" DoD
// bullet, proven through the bridge's own code path -- the layer the
// earlier audit found untested (this file's own brief).
//
// Whole-file #ifdef, mirroring test_gui_qml_smoke.cpp's own
// MORPH_LADDER_QML_URI precedent: when MORPH_BUILD_OFFLINE_SQLITE is off,
// BoardBridge::enableOfflineQueue doesn't exist at all, so this file compiles
// to an empty translation unit rather than failing to build.
#ifdef MORPH_BUILD_OFFLINE_SQLITE

#include "board_qml_bridge.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include <kanban/models/board_model.hpp>
#include <kanban/models/project_admin_model.hpp>

#include <morph/core/observability.hpp>
#include <morph/offline/network_monitor.hpp>
#include <morph/session/session.hpp>

#include <catch2/catch_test_macros.hpp>

#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

using namespace std::chrono_literals;

/// @brief Builds a rig whose one bridge already carries a valid session for
///        @p principal. Same recipe as test_board_qml_bridge.cpp's own
///        `makeAuthedRig`.
[[nodiscard]] std::unique_ptr<BackendRig> makeAuthedRig(std::string principal) {
    auto rig = std::make_unique<BackendRig>(Mode::Local, 1);
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    rig->bridge(0).setDefaultSession(ctx);
    return rig;
}

/// @brief Seeds one project (alice is its Manager) directly through
///        `ProjectAdminModel`'s own `BridgeHandler` -- same recipe as
///        test_board_qml_bridge.cpp's own `seedProject`.
[[nodiscard]] qlonglong seedProject(BackendRig& rig) {
    morph::bridge::BridgeHandler<kanban::ProjectAdminModel> creator{rig.bridge(0), rig.executor()};
    const auto id =
        morph::ladder::testkit::awaitQt(creator.execute(kanban::CreateProject{.name = "Offline Board"})).id;
    return id.hasValue() ? static_cast<qlonglong>(*id) : -1;
}

/// @brief A fresh, unique temp-file path for one test's `SqliteOfflineQueue`
///        -- same idiom `tests/offline_sqlite/test_sqlite_offline_queue.cpp`
///        uses for its own temp DB paths, so two TEST_CASEs (or two runs)
///        never collide on the same file.
[[nodiscard]] std::filesystem::path tempQueuePath() {
    static std::atomic<int> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("kanban_offline_bridge_test_" + std::to_string(now) + "_" + std::to_string(++counter) + ".db");
}

/// @brief Removes a `SqliteOfflineQueue` db file and its WAL/SHM siblings.
void removeQueueFiles(const std::filesystem::path& path) {
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-wal");
    std::filesystem::remove(path.string() + "-shm");
}

/// @brief RAII cleanup for one test's queue file -- constructed with the
///        path `enableOfflineQueue()` is given, removes every trace of it on
///        destruction regardless of how the test exits (assertion failure
///        included, since Catch2 unwinds normally on a CHECK failure).
///
/// Must outlive every `BoardBridge` whose `enableOfflineQueue()` was given
/// this path: `BoardBridge` never closes its own `SqliteOfflineQueue` until
/// its own destructor runs, and deleting the underlying file out from under
/// a still-open `sqlite3*` handle (WAL mode: `sqlite_offline_queue.hpp`'s own
/// `PRAGMA journal_mode=WAL`) is undefined behaviour -- reproduced
/// empirically as a reliable, hard-to-diagnose crash (SQLite's own internal
/// state referencing an unlinked-but-still-mapped file) when a `BoardBridge`
/// local was declared *before* its own `ScopedQueueFile`, which reverse
/// destruction order then tore down *first*. Declare this object before any
/// `BoardBridge` that uses its path (reverse destruction then closes the
/// bridge's queue before this destructor ever deletes the file).
class ScopedQueueFile {
  public:
    explicit ScopedQueueFile(std::filesystem::path path) : _path{std::move(path)} { removeQueueFiles(_path); }
    ~ScopedQueueFile() { removeQueueFiles(_path); }

    ScopedQueueFile(const ScopedQueueFile&) = delete;
    ScopedQueueFile& operator=(const ScopedQueueFile&) = delete;
    ScopedQueueFile(ScopedQueueFile&&) = delete;
    ScopedQueueFile& operator=(ScopedQueueFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return _path; }

  private:
    std::filesystem::path _path;
};

}  // namespace

TEST_CASE("BoardBridge queues a move made while offline and replays it on reconnect",
          "[kanban][gui][offline]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    const auto projectId = seedProject(*rig);

    // Declared before `bridge` below (see ScopedQueueFile's own doc comment):
    // reverse destruction order then closes `bridge`'s SqliteOfflineQueue
    // before this file gets deleted, not after.
    const ScopedQueueFile queueFile{tempQueuePath()};
    std::atomic<bool> simulatedOnline{true};

    kanban::gui::BoardBridge bridge{rig->bridge(0), rig->executor()};

    bool changed = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::boardChanged, [&] { changed = true; });
    bool moved = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::taskMoved, [&](const QString&) { moved = true; });
    int lastQueueDepth = -1;
    int lastDeadLettered = -1;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::syncStatusChanged, [&](int depth, int deadLettered) {
        lastQueueDepth = depth;
        lastDeadLettered = deadLettered;
    });

    // ── Seed a board with one task and two columns ──────────────────────
    bridge.openBoard(QString::number(projectId));
    REQUIRE(pumpUntil([&] { return changed; }));

    changed = false;
    bridge.createColumn(QStringLiteral("To Do"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString col1 =
        bridge.board().value(QStringLiteral("columns")).toList().front().toMap().value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createColumn(QStringLiteral("Done"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString col2 =
        bridge.board().value(QStringLiteral("columns")).toList().back().toMap().value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createSwimlane(QStringLiteral("Default"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString swimlaneId = bridge.board()
                                    .value(QStringLiteral("swimlanes"))
                                    .toList()
                                    .front()
                                    .toMap()
                                    .value(QStringLiteral("id"))
                                    .toString();

    changed = false;
    bridge.createTask(col1, swimlaneId, QStringLiteral("Fix bug"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString taskId =
        bridge.board().value(QStringLiteral("tasks")).toList().front().toMap().value(QStringLiteral("id")).toString();

    // ── Enable the offline stack: a controllable probe, forced online ───
    // A fresh temp-file SqliteOfflineQueue plus a NetworkMonitor whose probe
    // reads a plain atomic this test flips directly -- the "NetworkMonitor
    // test double forced into the offline state" this task's brief calls
    // for. failureThreshold/onlineThreshold = 1 and a short probeInterval so
    // the background probe thread's transition converges quickly under
    // pumpUntil.
    bridge.enableOfflineQueue(
        QString::fromStdString(queueFile.path().string()), [&simulatedOnline] { return simulatedOnline.load(); },
        ::morph::offline::NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 1, .onlineThreshold = 1});

    // ── Online move: goes straight through BoardPresenter ───────────────
    // Proves enabling the offline stack does not change the already-online
    // path -- the pre-Task-5 behaviour, unchanged. moveTask()'s own success
    // handler (BoardPresenter::moveTask) only emits taskMoved, never
    // boardOpened (board_presenter.cpp), so `board()` itself is not expected
    // to reflect the move until a later refresh() -- same as
    // test_board_qml_bridge.cpp's own moveTask test, which likewise checks
    // only `moved`/opId, never `board()`'s content, after a move. An
    // explicit refresh() here proves the move landed server-side.
    changed = false;
    moved = false;
    bridge.moveTask(taskId, col2, swimlaneId, 0);
    REQUIRE(pumpUntil([&] { return moved; }));
    changed = false;
    bridge.refresh();
    REQUIRE(pumpUntil([&] { return changed; }));
    CHECK(bridge.board().value(QStringLiteral("tasks")).toList().front().toMap().value(QStringLiteral("columnId")) ==
          bridge.board().value(QStringLiteral("columns")).toList().back().toMap().value(QStringLiteral("id")));

    // ── Force offline, then move again: queues instead of dispatching ───
    // Flip the probe to failing and wait for the real NetworkMonitor state
    // to flip -- moveTask() reads `_networkMonitor->isOnline()` directly, so
    // driving it off a stale (not-yet-observed) transition would make this
    // assertion flaky rather than deterministic.
    simulatedOnline.store(false);
    REQUIRE(pumpUntil([&] { return !bridge.isNetworkOnlineForTest(); }, 2000ms));

    changed = false;
    moved = false;
    lastQueueDepth = -1;
    bridge.moveTask(taskId, col1, swimlaneId, 0);
    // moveTask()'s offline branch is synchronous (enqueue, then emit
    // syncStatusChanged) -- no pump needed to observe it, but pumpUntil is
    // used anyway for consistency/safety against a future async change.
    REQUIRE(pumpUntil([&] { return lastQueueDepth == 1; }, 500ms));
    CHECK_FALSE(moved);
    CHECK_FALSE(changed);
    CHECK(lastDeadLettered == 0);

    // ── Reconnect: SyncWorker drains the queue and the move lands ───────
    // Flip the probe back to succeeding; NetworkMonitor's onOnline (posted
    // onto the Qt thread per enableOfflineQueue()'s own doc comment) drives
    // ReconnectCoordinator::onOnline(), whose replay dependency runs
    // SyncWorker::run(), which drains the queue and replays the one queued
    // move through BoardPresenter::moveTaskForReplay() -- a dedicated
    // Completion-returning overload that (like getEventsSinceForPolling)
    // deliberately bypasses the shared taskMoved/failed signals, so `moved`
    // itself never fires for a replay; enableOfflineQueue()'s own `replay`
    // dependency calls refresh() after a successful run instead, which is
    // what re-populates `board()` and fires `boardChanged` here.
    changed = false;
    lastQueueDepth = -1;
    simulatedOnline.store(true);
    REQUIRE(pumpUntil([&] { return changed; }, 2000ms));
    CHECK(lastQueueDepth == 0);
    CHECK(bridge.board().value(QStringLiteral("tasks")).toList().front().toMap().value(QStringLiteral("columnId")) ==
          col1);
}

TEST_CASE("BoardBridge's deadLetterCount property reflects dead-lettered moves", "[kanban][gui][offline]") {
    // Mirrors test_kanban_offline.cpp's own 5-cumulative-attempt dead-letter
    // setup, adapted to drive it through BoardBridge's own offline queue
    // rather than SyncWorker directly (this task's own brief): a WIP-limit-1
    // column already holding one task makes every replay of a second task's
    // move into that column fail identically and deterministically
    // (BoardModel::execute(MoveTaskPosition) throws Conflict -- "target
    // column is at its WIP limit" -- board_model.cpp), so five online/offline
    // flaps accumulate exactly five failed replay attempts on the one queued
    // item and SyncWorker's hard-coded 5-attempt cap (sync_worker.hpp)
    // dead-letters it.
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    const auto projectId = seedProject(*rig);

    const ScopedQueueFile queueFile{tempQueuePath()};
    std::atomic<bool> simulatedOnline{true};

    kanban::gui::BoardBridge bridge{rig->bridge(0), rig->executor()};

    bool changed = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::boardChanged, [&] { changed = true; });
    int lastQueueDepth = -1;
    int lastDeadLettered = -1;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::syncStatusChanged, [&](int depth, int deadLettered) {
        lastQueueDepth = depth;
        lastDeadLettered = deadLettered;
    });

    // ── Seed a board: one WIP-limit-1 "To Do" column already holding
    //    `blocker`, plus a second task `mover` sitting in "Backlog" that this
    //    test will queue a doomed move for ─────────────────────────────────
    bridge.openBoard(QString::number(projectId));
    REQUIRE(pumpUntil([&] { return changed; }));

    changed = false;
    bridge.createColumn(QStringLiteral("Backlog"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString backlogCol =
        bridge.board().value(QStringLiteral("columns")).toList().front().toMap().value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createColumn(QStringLiteral("To Do"), 1);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString toDoCol =
        bridge.board().value(QStringLiteral("columns")).toList().back().toMap().value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createSwimlane(QStringLiteral("Default"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString swimlaneId = bridge.board()
                                    .value(QStringLiteral("swimlanes"))
                                    .toList()
                                    .front()
                                    .toMap()
                                    .value(QStringLiteral("id"))
                                    .toString();

    changed = false;
    bridge.createTask(toDoCol, swimlaneId, QStringLiteral("Blocker"));
    REQUIRE(pumpUntil([&] { return changed; }));

    changed = false;
    bridge.createTask(backlogCol, swimlaneId, QStringLiteral("Mover"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QVariantList tasksAfterSeed = bridge.board().value(QStringLiteral("tasks")).toList();
    REQUIRE(tasksAfterSeed.size() == 2);
    QString moverTaskId;
    for (const auto& row : tasksAfterSeed) {
        const QVariantMap map = row.toMap();
        if (map.value(QStringLiteral("title")).toString() == QStringLiteral("Mover")) {
            moverTaskId = map.value(QStringLiteral("id")).toString();
        }
    }
    REQUIRE_FALSE(moverTaskId.isEmpty());

    // ── Enable the offline stack, same recipe as the sibling test above ──
    bridge.enableOfflineQueue(
        QString::fromStdString(queueFile.path().string()), [&simulatedOnline] { return simulatedOnline.load(); },
        ::morph::offline::NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 1, .onlineThreshold = 1});

    // ── Force offline, then queue the doomed move ("Mover" into the full
    //    "To Do" column) ────────────────────────────────────────────────
    simulatedOnline.store(false);
    REQUIRE(pumpUntil([&] { return !bridge.isNetworkOnlineForTest(); }, 2000ms));

    lastQueueDepth = -1;
    bridge.moveTask(moverTaskId, toDoCol, swimlaneId, 0);
    REQUIRE(pumpUntil([&] { return lastQueueDepth == 1; }, 500ms));
    CHECK(bridge.queueDepth() == 1);
    CHECK(bridge.deadLetterCount() == 0);

    // ── Flap online/offline five times: each online transition drives one
    //    SyncWorker::run() -> one failed replay attempt (Conflict, WIP
    //    limit) on the one queued item. The fifth attempt exhausts
    //    SyncWorker's cumulative cap and dead-letters it. ────────────────
    for (int flap = 0; flap < 5; ++flap) {
        lastDeadLettered = -1;
        simulatedOnline.store(true);
        REQUIRE(pumpUntil([&] { return lastDeadLettered >= 0; }, 2000ms));
        if (lastDeadLettered > 0) {
            break;
        }
        simulatedOnline.store(false);
        REQUIRE(pumpUntil([&] { return !bridge.isNetworkOnlineForTest(); }, 2000ms));
    }

    CHECK(bridge.deadLetterCount() == 1);
    CHECK(bridge.queueDepth() == 0);
}

TEST_CASE("BoardBridge's offline queue/reconnect path emits the framework's own morph::observe metrics",
          "[kanban][gui][offline]") {
    // README's DoD: "The offline tests assert the framework's own
    // morph::observe metrics (queueDepth, reconnect attempt/outcome) -- the
    // observability seam gains its first app-scale coverage here." The two
    // tests above already prove the offline stack's *behavior* (queue then
    // replay; five-flap dead-letter); this test proves the same stack's
    // *instrumentation* -- that SyncWorker::run() and
    // ReconnectCoordinator::onOnline() actually call through to
    // morph::observe::detail::emitMetric with the metric kinds the DoD
    // names, not just that the offline behavior itself is correct.
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    const auto projectId = seedProject(*rig);

    const ScopedQueueFile queueFile{tempQueuePath()};
    std::atomic<bool> simulatedOnline{true};

    kanban::gui::BoardBridge bridge{rig->bridge(0), rig->executor()};

    bool changed = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::boardChanged, [&] { changed = true; });
    bool moved = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::taskMoved, [&](const QString&) { moved = true; });
    int lastQueueDepth = -1;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::syncStatusChanged,
                      [&](int depth, int /*deadLettered*/) { lastQueueDepth = depth; });

    // ── Snapshot/restore the process-global metric sink around this test
    //    only -- ScopedObserveOverride is the framework's own RAII idiom for
    //    exactly this (see include/morph/core/observability.hpp), so a
    //    sibling TEST_CASE in this same binary is never left with this
    //    test's sink still installed. `MetricEvent::tags` is a `std::span`
    //    into the emitting call's own stack-local storage, invalid once
    //    `emitMetric` returns -- only `metric` itself (a plain enum value,
    //    safe to copy) is retained, since that's all this test asserts on.
    ::morph::observe::ScopedObserveOverride observeOverride;
    std::vector<::morph::observe::Metric> observedMetrics;
    std::mutex observedMetricsMtx;
    ::morph::observe::setMetricSink([&](const ::morph::observe::MetricEvent& event) {
        std::scoped_lock const lock{observedMetricsMtx};
        observedMetrics.push_back(event.metric);
    });

    auto hasMetric = [&](::morph::observe::Metric metric) {
        std::scoped_lock const lock{observedMetricsMtx};
        return std::ranges::find(observedMetrics, metric) != observedMetrics.end();
    };

    // ── Seed a board with one task and two columns (same shape as the
    //    reconnect test above) ───────────────────────────────────────────
    bridge.openBoard(QString::number(projectId));
    REQUIRE(pumpUntil([&] { return changed; }));

    changed = false;
    bridge.createColumn(QStringLiteral("To Do"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString col1 =
        bridge.board().value(QStringLiteral("columns")).toList().front().toMap().value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createColumn(QStringLiteral("Done"), 0);
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString col2 =
        bridge.board().value(QStringLiteral("columns")).toList().back().toMap().value(QStringLiteral("id")).toString();

    changed = false;
    bridge.createSwimlane(QStringLiteral("Default"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString swimlaneId = bridge.board()
                                    .value(QStringLiteral("swimlanes"))
                                    .toList()
                                    .front()
                                    .toMap()
                                    .value(QStringLiteral("id"))
                                    .toString();

    changed = false;
    bridge.createTask(col1, swimlaneId, QStringLiteral("Fix bug"));
    REQUIRE(pumpUntil([&] { return changed; }));
    const QString taskId =
        bridge.board().value(QStringLiteral("tasks")).toList().front().toMap().value(QStringLiteral("id")).toString();

    bridge.enableOfflineQueue(
        QString::fromStdString(queueFile.path().string()), [&simulatedOnline] { return simulatedOnline.load(); },
        ::morph::offline::NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 1, .onlineThreshold = 1});

    // ── Force offline, queue one move ────────────────────────────────────
    simulatedOnline.store(false);
    REQUIRE(pumpUntil([&] { return !bridge.isNetworkOnlineForTest(); }, 2000ms));

    changed = false;
    moved = false;
    lastQueueDepth = -1;
    bridge.moveTask(taskId, col2, swimlaneId, 0);
    REQUIRE(pumpUntil([&] { return lastQueueDepth == 1; }, 500ms));

    // ── Reconnect: drives ReconnectCoordinator::onOnline() (reconnectAttempts
    //    + reconnectOutcome) and SyncWorker::run() (queueDepth, emitted once
    //    per drain with the pre-drain item count) ─────────────────────────
    changed = false;
    simulatedOnline.store(true);
    REQUIRE(pumpUntil([&] { return changed; }, 2000ms));

    // Give the metric sink's own lock-protected callback a moment to catch
    // up with the last emission -- emitMetric() is synchronous on the same
    // thread that calls it (Qt's posted executor), so by the time
    // boardChanged has fired (the last step of the replay/refresh chain)
    // every metric this run will ever emit has already been recorded.
    CHECK(hasMetric(::morph::observe::Metric::queueDepth));
    CHECK(hasMetric(::morph::observe::Metric::reconnectAttempts));
    CHECK(hasMetric(::morph::observe::Metric::reconnectOutcome));
}

#endif  // MORPH_BUILD_OFFLINE_SQLITE
