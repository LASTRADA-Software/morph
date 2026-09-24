// SPDX-License-Identifier: Apache-2.0

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <morph/core/file_io_ops.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/observability.hpp>
#include <morph/offline/sqlite_offline_queue.hpp>
#include <morph/offline/sync_worker.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../offline_queue_conformance.hpp"
#include "../test_support.hpp"

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

// ── Coverage: maxDepth / overflow policy ───────────────────────

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

TEST_CASE("morph::offline::SqliteOfflineQueue: a NUL-bearing payload and key round-trip intact", "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    auto const open = [&dbPath] { return std::make_unique<morph::offline::SqliteOfflineQueue>(dbPath); };
    morph::test::checkNulPayloadRoundTrip("SqliteOfflineQueue", open, open);
    removeDbFiles(dbPath);
}

// ── setIdempotencyKey on a conflicting key ───────────────────────
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
    // busyTimeout 0 -- this case is about SQLITE_BUSY *surfacing*, and the
    // default 5s timeout would make it surface five seconds later while still
    // passing, turning a sub-millisecond assertion into a stall that says
    // nothing more than this one does.
    morph::offline::SqliteOfflineQueue queue{dbPath,
                                             std::nullopt,
                                             {},
                                             morph::offline::SqliteOfflineQueue::Synchronous::normal,
                                             std::chrono::milliseconds{0}};
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

// ── Durability PRAGMAs ───────────────────────────────────────
//
// journal_mode=WAL, synchronous=FULL, and busy_timeout are all set at
// construction. journal_mode is a persistent property of the database file
// itself (unlike the other two, which are per-connection), so it is the one
// that can be read back from a fresh connection after the original closes;
// it is also the one construction verifies for itself, since a filesystem
// without shared-memory support silently falls back to `delete` mode
// instead of erroring.

TEST_CASE("morph::offline::SqliteOfflineQueue: journal_mode=WAL persists and is verified at construction",
          "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    {
        morph::offline::SqliteOfflineQueue queue{dbPath};
        (void)queue.enqueue("payload");
    }  // closed -- journal_mode lives in the file's own header, not the connection

    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(dbPath.string().c_str(), &raw) == SQLITE_OK);
    sqlite3_stmt* stmt = nullptr;
    REQUIRE(sqlite3_prepare_v2(raw, "PRAGMA journal_mode;", -1, &stmt, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
    // sqlite3_column_text returns `const unsigned char*`; reading it as text
    // has no other spelling.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    std::string const mode{reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))};
    CHECK(mode == "wal");
    sqlite3_finalize(stmt);
    sqlite3_close(raw);
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: a journal_mode that is not WAL warns and keeps working", "[sqlite]") {
    // An in-memory database always reports journal_mode "memory" regardless of
    // what is requested -- SQLite's own documented behavior (WAL needs shared
    // memory a `:memory:` database does not have), not a fault injected here.
    // It stands in for the real cases: NFS, CIFS/SMB, and some overlay/9p
    // mounts, which report "delete".
    //
    // The read-back must still happen -- that is what makes the fallback
    // visible instead of silent -- but it must not refuse the database. None of
    // these modes is less durable than WAL once `PRAGMA synchronous` is set
    // (SQLite's rollback-journal default is already FULL; it is WAL that lowers
    // it), and throwing here meant an app whose data directory sits on NFS
    // could not construct its queue at all.
    std::vector<std::string> warnings;
    morph::log::ScopedLoggerOverride const guard{[&warnings](morph::log::LogLevel level, std::string_view msg) {
        if (level == morph::log::LogLevel::warn) {
            warnings.emplace_back(msg);
        }
    }};

    std::optional<morph::offline::SqliteOfflineQueue> queue;
    REQUIRE_NOTHROW(queue.emplace(std::filesystem::path{":memory:"}));

    CHECK(queue->journalMode() == "memory");
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].contains("journal_mode=WAL did not take"));
    CHECK(warnings[0].contains("memory"));

    // Load-bearing: the queue is not merely constructible, it works.
    auto const id = queue->enqueue("payload");
    CHECK(queue->drain().size() == 1);
    queue->markDone(id);
    CHECK(queue->drain().empty());
}

TEST_CASE("morph::offline::SqliteOfflineQueue: a WAL database reports journalMode() == \"wal\"", "[sqlite]") {
    // The other side of the case above: on an ordinary filesystem the read-back
    // must report `wal`, and must emit no warning. Without this, the warn path
    // above could pass while WAL silently never took anywhere.
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    std::vector<std::string> warnings;
    {
        morph::log::ScopedLoggerOverride const guard{[&warnings](morph::log::LogLevel level, std::string_view msg) {
            if (level == morph::log::LogLevel::warn) {
                warnings.emplace_back(msg);
            }
        }};
        morph::offline::SqliteOfflineQueue const queue{dbPath};
        CHECK(queue.journalMode() == "wal");
    }
    CHECK(warnings.empty());
    removeDbFiles(dbPath);
}

TEST_CASE(
    "morph::offline::SqliteOfflineQueue: PRAGMA busy_timeout lets a write wait out a transient lock instead of "
    "failing immediately",
    "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    morph::offline::SqliteOfflineQueue queue{dbPath};
    (void)queue.enqueue("seed");  // ensure the WAL files exist before a second connection opens

    // A second, independent connection holding the write lock is the only
    // way SQLITE_BUSY becomes reachable from `queue`'s own connection at
    // all -- this class's internal mutex already serialises every call
    // *within* this process, so nothing short of another connection
    // entirely can contend with it.
    //
    // Closed through a guard rather than a bare call at the end: every
    // assertion below is a Catch2 macro that throws on failure, and an early
    // unwind would otherwise leak the connection and leave the database locked
    // for whatever runs next.
    sqlite3* second = nullptr;
    REQUIRE(sqlite3_open(dbPath.string().c_str(), &second) == SQLITE_OK);
    auto const closeSecond = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>{second, &sqlite3_close};
    char* err = nullptr;
    REQUIRE(sqlite3_exec(second, "BEGIN IMMEDIATE;", nullptr, nullptr, &err) == SQLITE_OK);

    std::atomic<bool> enqueueSucceeded{false};
    std::atomic<bool> enqueueThrew{false};
    std::atomic<bool> writerEntered{false};
    // jthread, not thread: a REQUIRE below throws on failure, and unwinding
    // past a still-joinable std::thread calls std::terminate -- which would
    // abort the whole binary and lose every later test case rather than report
    // the assertion that failed. catch(...) for the same reason: anything
    // escaping a thread function terminates, and this one is not limited to
    // throwing SqliteOfflineQueueError.
    std::jthread writer{[&] {
        writerEntered = true;
        try {
            (void)queue.enqueue("blocked-until-lock-released");
            enqueueSucceeded = true;
        } catch (...) {
            enqueueThrew = true;  // what a zero/absent busy_timeout would produce instead
        }
    }};

    // Wait for the writer to be inside enqueue() and blocked on the lock,
    // rather than sleeping a fixed 200ms and hoping. On a loaded runner the
    // fixed sleep could elapse before the writer reached sqlite3_step, so the
    // COMMIT released a lock nobody was waiting on and the test passed without
    // ever proving the busy handler ran.
    REQUIRE(morph::testing::waitUntil([&] { return writerEntered.load(); }));
    // `writerEntered` is published *before* the enqueue() call, so on its own it
    // proves only that the thread started -- not that it reached sqlite3_step
    // and found the lock held. Checking the two result flags right here would
    // therefore pass trivially, and keep passing with the busy_timeout PRAGMA
    // removed, which is the whole thing this test exists to pin.
    //
    // Requiring the call to still be *pending* after a bounded window is what
    // closes that: with no busy_timeout, sqlite3_step returns SQLITE_BUSY as
    // soon as it sees the lock, so `enqueueThrew` would flip well inside this
    // window and fail the assertion. The window is far below the 5s timeout
    // under test, so a correctly configured queue is still blocked when it
    // elapses.
    CHECK_FALSE(morph::testing::waitUntil([&] { return enqueueSucceeded.load() || enqueueThrew.load(); },
                                          morph::testing::WaitBudget{std::chrono::milliseconds{500}}));

    REQUIRE(sqlite3_exec(second, "COMMIT;", nullptr, nullptr, &err) == SQLITE_OK);
    writer.join();

    CHECK(enqueueSucceeded.load());
    CHECK_FALSE(enqueueThrew.load());
    removeDbFiles(dbPath);
}

// ── Directory fsync ──────────────────────────────────────────
//
// `sqlite3_open()` creates `dbPath` (and, once journal_mode=WAL took, its
// "-wal"/"-shm" siblings) on first use -- a fresh directory entry that
// SQLite's own internal fsyncs of the *file's* contents never make durable.
// These confirm `FileIoOps::syncPath` is actually called once construction's
// schema setup succeeds, with the right directory, and that a failure there
// is surfaced rather than swallowed -- mirroring the equivalent
// `FileActionLog`/`FileOfflineQueue` tests in test_action_log_phase2.cpp /
// test_file_offline_queue.cpp.

TEST_CASE("morph::offline::SqliteOfflineQueue: construction syncs the containing directory after creating the file",
          "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    std::vector<std::filesystem::path> syncedPaths;
    morph::core::FileIoOps ioOps;
    ioOps.syncPath = [&syncedPaths](const std::filesystem::path& dir) {
        syncedPaths.push_back(dir);
        return 0;
    };

    // Scoped so the connection (and its -wal/-shm siblings) is closed before
    // removeDbFiles() unlinks them -- the same open-handle-vs-unlink ordering
    // that made the FileOfflineQueue twin of this test fail on Windows. This
    // suite is Linux-only today, where the unlink would succeed regardless;
    // scoped anyway so it does not become a Windows failure the day it is not.
    {
        morph::offline::SqliteOfflineQueue const queue{dbPath, std::nullopt, ioOps};
    }

    REQUIRE(syncedPaths.size() == 1);
    CHECK(syncedPaths[0] == dbPath.parent_path());
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: an unsupported directory fsync warns instead of throwing", "[sqlite]") {
    // Same split as the file-backed queues: a directory fsync this platform or
    // mount cannot perform is a durability *ceiling*, not a failure, and must
    // not stop the database opening. kanban's enableOfflineQueue() builds one
    // of these from a user-supplied path, so throwing here would mean a data
    // directory on NFS could not open the app at all.
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);

    std::vector<std::string> warnings;
    morph::log::ScopedLoggerOverride const guard{[&warnings](morph::log::LogLevel level, std::string_view msg) {
        if (level == morph::log::LogLevel::warn) {
            warnings.emplace_back(msg);
        }
    }};

    morph::core::FileIoOps ioOps;
    ioOps.syncPath = [](const std::filesystem::path&) { return EACCES; };

    {
        morph::offline::SqliteOfflineQueue queue{dbPath, std::nullopt, ioOps};
        auto const id = queue.enqueue("payload");
        CHECK(queue.drain().size() == 1);
        queue.markDone(id);
        CHECK(queue.drain().empty());
    }

    REQUIRE_FALSE(warnings.empty());
    CHECK(warnings[0].contains("cannot fsync the directory"));
    removeDbFiles(dbPath);
}

TEST_CASE("morph::offline::SqliteOfflineQueue: Synchronous selects the level SQLite actually applies", "[sqlite]") {
    // `full` is opt-in because it costs ~18x per mutation, and every mutation
    // here is its own commit -- so the parameter only earns its place if it
    // actually reaches SQLite. Asserted against the level read back from the
    // queue's own connection, not against the argument that was passed in.
    //
    // SQLite's numeric levels: 0 OFF, 1 NORMAL, 2 FULL, 3 EXTRA.
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);

    {
        morph::offline::SqliteOfflineQueue queue{
            dbPath, std::nullopt, {}, morph::offline::SqliteOfflineQueue::Synchronous::full};
        CHECK(queue.synchronousLevel() == 2);
        // A working queue, not merely a constructible one.
        auto const id = queue.enqueue("durable-payload");
        CHECK(queue.drain().size() == 1);
        queue.markDone(id);
        CHECK(queue.drain().empty());
    }

    // The default, and the other side of the assertion: without it, a
    // read-back that always reported FULL would pass the check above.
    {
        morph::offline::SqliteOfflineQueue const queue{dbPath};
        CHECK(queue.synchronousLevel() == 1);
    }

    removeDbFiles(dbPath);
}

TEST_CASE(
    "morph::offline::SqliteOfflineQueue: a failing directory fsync during construction throws and leaks no "
    "connection",
    "[sqlite]") {
    auto dbPath = tempDbPath();
    removeDbFiles(dbPath);
    morph::core::FileIoOps ioOps;
    ioOps.syncPath = [](const std::filesystem::path&) { return -1; };

    REQUIRE_THROWS_AS(morph::offline::SqliteOfflineQueue(dbPath, std::nullopt, ioOps),
                      morph::offline::SqliteOfflineQueueError);

    // The failed construction must not have left the connection open -- a
    // fresh, real-I/O open of the same path must succeed cleanly.
    morph::offline::SqliteOfflineQueue reopened{dbPath};
    (void)reopened.enqueue("payload");
    REQUIRE(reopened.size() == 1);
    removeDbFiles(dbPath);
}
