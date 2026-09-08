// SPDX-License-Identifier: Apache-2.0

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <morph/core/observability.hpp>
#include <morph/offline/sqlite_offline_queue.hpp>
#include <morph/offline/sync_worker.hpp>
#include <optional>
#include <string>
#include <vector>

#include "../offline_queue_conformance.hpp"

namespace {

std::filesystem::path tempDbPath() {
    static std::atomic<int> counter{0};
    auto const now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("morph_sqlite_offline_queue_test_" + std::to_string(now) + "_" + std::to_string(++counter) + ".db");
}

void removeDbFiles(const std::filesystem::path& path) {
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + "-wal");
    std::filesystem::remove(path.string() + "-shm");
}

}  // namespace

TEST_CASE("morph::offline::SqliteOfflineQueue: items survive destroying and reopening over the same file",
          "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);

    uint64_t id1 = 0;
    uint64_t id2 = 0;
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        id1 = queue.enqueue("payload-1", "key-1");
        id2 = queue.enqueue("payload-2");
        queue.setAttempts(id2, 2);
    }
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        auto items = queue.drain();
        REQUIRE(items.size() == 2);
        REQUIRE(items[0].id == id1);
        REQUIRE(items[0].payload == "payload-1");
        REQUIRE(items[0].idempotencyKey == "key-1");
        REQUIRE(items[0].attempts == 0);
        REQUIRE(items[1].id == id2);
        REQUIRE(items[1].payload == "payload-2");
        REQUIRE(items[1].attempts == 2);
    }
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: item survives a crash between drain() and markDone()", "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);

    uint64_t enqueuedId = 0;
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        enqueuedId = queue.enqueue("payload");
        auto items = queue.drain();
        REQUIRE(items.size() == 1);
        // Simulate a crash: the replay side effect notionally ran, but the
        // process dies before markDone() -- the queue is destroyed without it.
    }
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        auto items = queue.drain();
        REQUIRE(items.size() == 1);
        REQUIRE(items[0].id == enqueuedId);
        queue.markDone(enqueuedId);
        REQUIRE(queue.drain().empty());
    }
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: re-enqueue with the same idempotencyKey is deduplicated", "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    morph::offline::SqliteOfflineQueue queue{dbPath};

    auto id1 = queue.enqueue("first-payload", "op-123");
    auto id2 = queue.enqueue("second-payload", "op-123");

    REQUIRE(id1 == id2);
    auto items = queue.drain();
    REQUIRE(items.size() == 1);
    REQUIRE(items[0].payload == "first-payload");  // first write wins

    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: empty idempotencyKey items are never deduplicated", "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    morph::offline::SqliteOfflineQueue queue{dbPath};

    (void)queue.enqueue("a");
    (void)queue.enqueue("b");

    REQUIRE(queue.drain().size() == 2);
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue + SyncWorker: poison item dead-letters across a simulated restart",
          "[sqlite][sync]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);

    uint64_t enqueuedId = 0;
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        enqueuedId = queue.enqueue("poison-payload");
    }

    auto alwaysFail = [](const std::string&) { return false; };

    // 3 pre-restart run() calls persist attempts == 3 in the database.
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        morph::offline::SyncWorker worker{queue, alwaysFail};
        worker.run();
        worker.run();
        worker.run();
        auto items = queue.drain();
        REQUIRE(items.size() == 1);
        REQUIRE(items[0].attempts == 3);
    }

    // "Restart": brand-new SqliteOfflineQueue + brand-new SyncWorker over the
    // same file -- the in-memory _attempts map is gone; only the persisted
    // `attempts` column survives.
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        std::vector<morph::offline::QueueItem> deadLettered;
        morph::offline::SyncWorker worker{
            queue, alwaysFail, [&](const morph::offline::QueueItem& item) { deadLettered.push_back(item); }};

        auto result1 = worker.run();  // 4th cumulative attempt
        REQUIRE(result1.failed == 1);
        auto result2 = worker.run();  // 5th cumulative attempt -> dead-letters
        REQUIRE(result2.deadLettered == 1);
        REQUIRE(deadLettered.size() == 1);
        REQUIRE(deadLettered[0].id == enqueuedId);
        REQUIRE(deadLettered[0].attempts == 5);
        REQUIRE(queue.drain().empty());
    }
    removeDbFiles(dbPath);
}

// ── Coverage: maxDepth / overflow policy (morph#112) ───────────────────────

TEST_CASE("morph::offline::SqliteOfflineQueue: enqueue at maxDepth throws OfflineQueueFullError",
          "[sqlite][overflow]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        morph::offline::SqliteOfflineQueue queue{dbPath, 2};
        (void)queue.enqueue("a");
        (void)queue.enqueue("b");
        REQUIRE_THROWS_AS(queue.enqueue("c"), morph::offline::OfflineQueueFullError);
        REQUIRE(queue.drain().size() == 2);
    }
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: maxDepth survives destroying and reopening over the same file",
          "[sqlite][overflow]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        morph::offline::SqliteOfflineQueue queue{dbPath, 1};
        (void)queue.enqueue("a");
        REQUIRE_THROWS_AS(queue.enqueue("b"), morph::offline::OfflineQueueFullError);
    }
    {
        // Reopened with the same maxDepth argument -- still enforced. maxDepth
        // is a per-construction parameter, not persisted in the database itself.
        morph::offline::SqliteOfflineQueue queue{dbPath, 1};
        REQUIRE(queue.drain().size() == 1);
        REQUIRE_THROWS_AS(queue.enqueue("b"), morph::offline::OfflineQueueFullError);
    }
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: size() matches COUNT(*) against the table", "[sqlite][overflow]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        REQUIRE(queue.size() == 0);
        (void)queue.enqueue("a");
        (void)queue.enqueue("b");
        (void)queue.enqueue("c");
        REQUIRE(queue.size() == 3);

        // Cross-check via a raw query against the same database file.
        sqlite3* raw = nullptr;
        REQUIRE(sqlite3_open(dbPath.string().c_str(), &raw) == SQLITE_OK);
        sqlite3_stmt* stmt = nullptr;
        REQUIRE(sqlite3_prepare_v2(raw, "SELECT COUNT(*) FROM morph_offline_queue;", -1, &stmt, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
        auto const rawCount = static_cast<std::size_t>(sqlite3_column_int64(stmt, 0));
        sqlite3_finalize(stmt);
        sqlite3_close(raw);

        REQUIRE(rawCount == queue.size());
    }
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: a dedup hit at capacity is rejected (documented conservatism)",
          "[sqlite][overflow]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        morph::offline::SqliteOfflineQueue queue{dbPath, 1};
        (void)queue.enqueue("first-payload", "op-1");
        // The keyed path checks capacity BEFORE attempting the insert, so a
        // call that would otherwise resolve to a dedup hit (inserting
        // nothing) is still rejected once the queue is full -- documented
        // conservatism, not a bug.
        REQUIRE_THROWS_AS(queue.enqueue("second-payload", "op-1"), morph::offline::OfflineQueueFullError);
        REQUIRE(queue.drain().size() == 1);
    }
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: enqueue at maxDepth emits queueOverflow metric",
          "[sqlite][overflow][observability]") {
    morph::observe::ScopedObserveOverride guard;
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        morph::offline::SqliteOfflineQueue queue{dbPath, 1};
        (void)queue.enqueue("a");

        std::vector<double> samples;
        morph::observe::setMetricSink([&](const morph::observe::MetricEvent& evt) {
            if (evt.metric == morph::observe::Metric::queueOverflow) {
                samples.push_back(evt.value);
            }
        });

        REQUIRE_THROWS_AS(queue.enqueue("b"), morph::offline::OfflineQueueFullError);
        REQUIRE(samples.size() == 1);
        REQUIRE(samples[0] == 1.0);
    }
    removeDbFiles(dbPath);
}

// ── IOfflineQueue conformance ─────────────────────────────────────────────────
//
// `SqliteOfflineQueue` deduplicates a non-empty idempotency key via the partial
// unique index `ix_queue_idem`. That is a permitted strengthening of the
// `IOfflineQueue` contract, declared here so the shared suite asserts it rather
// than tolerating either behaviour.

TEST_CASE("morph::offline::SqliteOfflineQueue: IOfflineQueue idempotency-key conformance", "[sqlite]") {
    std::vector<std::filesystem::path> created;
    morph::test::checkIdempotencyKeyContract("SqliteOfflineQueue", morph::test::KeyDedup::onPendingItems, [&created] {
        auto dbPath = tempDbPath();
        removeDbFiles(dbPath);
        created.push_back(dbPath);
        return std::make_unique<morph::offline::SqliteOfflineQueue>(dbPath);
    });
    for (auto const& dbPath : created) {
        removeDbFiles(dbPath);
    }
}

TEST_CASE("morph::offline::SqliteOfflineQueue: the idempotency-key contract survives a reopen", "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    morph::test::checkIdempotencyKeyContractAcrossReopen(
        "SqliteOfflineQueue", morph::test::KeyDedup::onPendingItems,
        [&dbPath] { return std::make_unique<morph::offline::SqliteOfflineQueue>(dbPath); });
    removeDbFiles(dbPath);
}

// ── setIdempotencyKey on a conflicting key (morph#249) ───────────────────────
//
// The protected hook is reached only through the *base* default
// `IOfflineQueue::enqueue(payload, key)`, which inserts first and stamps
// second. On a key a pending row already holds, the partial unique index
// rejects the stamp. It used to throw, while this same class's own
// `enqueue(payload, key)` resolved the identical conflict silently by keeping
// the existing row — one conflict, two answers.
//
// The scope-qualified call below is how the base default is reached; it mirrors
// tests/test_file_offline_queue.cpp's existing scope-qualified case for the
// sibling implementation.

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("morph::offline::SqliteOfflineQueue: a conflicting setIdempotencyKey does not throw", "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        auto const first = queue.enqueue("payload-A", "K1");

        // Base-qualified: insert, then stamp a key "K1" already holds.
        std::uint64_t viaBase = 0;
        CHECK_NOTHROW(viaBase = queue.morph::offline::IOfflineQueue::enqueue("payload-B", "K1"));

        // The row the base default inserted exists either way — the stamp is
        // what conflicts, and it is skipped rather than raised. The pre-existing
        // keyed row keeps both its key and its payload.
        auto const items = queue.drain();
        REQUIRE(items.size() == 2);

        auto const firstItem = std::ranges::find_if(items, [first](auto const& i) { return i.id == first; });
        REQUIRE(firstItem != items.end());
        CHECK(firstItem->payload == "payload-A");
        CHECK(firstItem->idempotencyKey == "K1");

        // The newly-inserted row is present and unkeyed: the conflict cost it
        // its key, not its existence. It is *not* a dedup hit — the base path
        // cannot produce one, since it has already inserted by the time it
        // stamps. Callers wanting dedup use the virtual two-arg enqueue.
        auto const secondItem = std::ranges::find_if(items, [viaBase](auto const& i) { return i.id == viaBase; });
        REQUIRE(secondItem != items.end());
        CHECK(secondItem->payload == "payload-B");
        CHECK(secondItem->idempotencyKey.empty());
    }
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: a non-conflicting setIdempotencyKey still stamps", "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        (void)queue.enqueue("payload-A", "K1");
        // Control: without it, the case above would pass against a hook that
        // silently stamped nothing at all.
        auto const fresh = queue.morph::offline::IOfflineQueue::enqueue("payload-B", "K2");
        auto const items = queue.drain();
        auto const item = std::ranges::find_if(items, [fresh](auto const& i) { return i.id == fresh; });
        REQUIRE(item != items.end());
        CHECK(item->idempotencyKey == "K2");
    }
    removeDbFiles(dbPath);
}

// ── Constructor failure paths (Task 12, findings #1/#2) ────────────────────

TEST_CASE("morph::offline::SqliteOfflineQueue: construction throws if sqlite3_open() cannot open the file",
          "[sqlite]") {
    // sqlite3_open()'s default flags (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE)
    // fail with SQLITE_CANTOPEN when the parent directory does not exist -- no
    // fault injection seam needed, exact mirror of FileOfflineQueue's own
    // nonexistent-directory technique for its own throw-on-open-failure test.
    auto const path = std::filesystem::path{"/no/such/directory/at/all/q.db"};
    REQUIRE_THROWS_AS(morph::offline::SqliteOfflineQueue(path), morph::offline::SqliteOfflineQueueError);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: construction throws if the schema-setup PRAGMA fails", "[sqlite]") {
    // sqlite3_open() succeeds lazily without validating the file format --
    // pre-create a plain-text file at the path first. sqlite3_open() itself
    // succeeds, but the very first schema-setup statement
    // (PRAGMA journal_mode=WAL;) fails with SQLite's own "file is not a
    // database" error inside execOrThrow(), which throws and is caught by
    // the constructor's own catch (...) block, closing _db and rethrowing.
    // Closes both line clusters in one test: execOrThrow's own throw and the
    // constructor's wrapping catch block.
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        std::ofstream notADatabase{dbPath};
        notADatabase << "not a database";
    }
    REQUIRE_THROWS_AS(morph::offline::SqliteOfflineQueue(dbPath), morph::offline::SqliteOfflineQueueError);
    removeDbFiles(dbPath);
}

// ── maxDepth() accessor (Task 12, finding #10) ─────────────────────────────

TEST_CASE("morph::offline::SqliteOfflineQueue: maxDepth() reports the configured cap", "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        morph::offline::SqliteOfflineQueue const queue{dbPath, 5};
        REQUIRE(queue.maxDepth() == 5);
    }
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: maxDepth() reports nullopt when unbounded", "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        morph::offline::SqliteOfflineQueue const queue{dbPath};
        REQUIRE_FALSE(queue.maxDepth().has_value());
    }
    removeDbFiles(dbPath);
}

// ── Second-connection cluster (Task 12, findings #5/#6/#9) ─────────────────
//
// Three findings share one seam: a second raw sqlite3 connection to the same
// database file, opened alongside the SqliteOfflineQueue instance under
// test. Each of stepOrThrow()'s, prepare()'s, and drain()'s own
// non-ROW/DONE-step error branches is unreachable through this class's own
// single-connection, single-mutex API alone -- reaching them needs a
// genuinely separate connection racing (or, for #6/#5, having already
// altered the schema) against the first. Mechanisms below were verified via
// a standalone compiled probe against this exact SQLite build (3.51.0)
// before being written up as Catch2 cases, per the corrected task-11 audit.

TEST_CASE(
    "morph::offline::SqliteOfflineQueue: enqueue surfaces SQLITE_BUSY from a second connection's "
    "BEGIN IMMEDIATE transaction",
    "[sqlite]") {
    // Finding #9. A single-instance mutex serialises this class's own calls,
    // but does nothing for lock contention from a genuinely separate
    // connection -- a second raw sqlite3 connection holding an exclusive
    // write lock via BEGIN IMMEDIATE deterministically makes the first
    // instance's own write-statement step() return SQLITE_BUSY.
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    morph::offline::SqliteOfflineQueue queue{dbPath};
    (void)queue.enqueue("seed");

    sqlite3* second = nullptr;
    REQUIRE(sqlite3_open(dbPath.string().c_str(), &second) == SQLITE_OK);
    char* err = nullptr;
    REQUIRE(sqlite3_exec(second, "BEGIN IMMEDIATE;", nullptr, nullptr, &err) == SQLITE_OK);

    REQUIRE_THROWS_AS(queue.enqueue("blocked-by-second-connection"), morph::offline::SqliteOfflineQueueError);

    REQUIRE(sqlite3_exec(second, "ROLLBACK;", nullptr, nullptr, &err) == SQLITE_OK);
    sqlite3_close(second);
    removeDbFiles(dbPath);
}

TEST_CASE(
    "morph::offline::SqliteOfflineQueue: a second connection's DROP TABLE surfaces drain()'s own "
    "step()-level SQLITE_ERROR, then prepare()'s own failure on a later call",
    "[sqlite]") {
    // Findings #5 and #6, sharing one setup. Verified empirically (compiled
    // probe): after a second connection drops the table, the FIRST
    // statement the first instance prepares against the stale schema still
    // returns SQLITE_OK (the connection's cached schema cookie has not yet
    // been invalidated) -- but that statement's own first step() returns
    // SQLITE_ERROR, not SQLITE_ROW/SQLITE_DONE (finding #5, drain()'s own
    // branch). The schema invalidation surfaces to the *next* prepare()
    // call, which now genuinely fails at the prepare() level itself with
    // "no such table" (finding #6).
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    morph::offline::SqliteOfflineQueue queue{dbPath};
    (void)queue.enqueue("seed");

    {
        sqlite3* second = nullptr;
        REQUIRE(sqlite3_open(dbPath.string().c_str(), &second) == SQLITE_OK);
        char* err = nullptr;
        REQUIRE(sqlite3_exec(second, "DROP TABLE morph_offline_queue;", nullptr, nullptr, &err) == SQLITE_OK);
        sqlite3_close(second);
    }

    // Finding #5: drain()'s prepare() succeeds (stale cached schema), but its
    // own first sqlite3_step() returns SQLITE_ERROR rather than
    // SQLITE_ROW/SQLITE_DONE.
    bool drainThrew = false;
    try {
        (void)queue.drain();
    } catch (const morph::offline::SqliteOfflineQueueError& exc) {
        drainThrew = true;
        CHECK(std::string{exc.what()}.contains("drain failed part-way through"));
    }
    REQUIRE(drainThrew);

    // Finding #6: this second, later prepare() call on the same instance now
    // genuinely fails at the prepare() level -- the schema-cookie mismatch
    // drain() triggered above is now visible to a fresh prepare().
    bool enqueueThrew = false;
    try {
        (void)queue.enqueue("after-drop");
    } catch (const morph::offline::SqliteOfflineQueueError& exc) {
        enqueueThrew = true;
        CHECK(std::string{exc.what()}.contains("prepare failed"));
    }
    REQUIRE(enqueueThrew);

    removeDbFiles(dbPath);
}
