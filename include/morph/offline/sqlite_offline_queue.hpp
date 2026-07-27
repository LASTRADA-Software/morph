// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "offline_queue.hpp"

namespace morph::offline {

/// @brief Thrown when a SQLite operation used by `SqliteOfflineQueue` fails.
struct SqliteOfflineQueueError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

namespace detail {

/// @brief `SQLITE_TRANSIENT` (`((sqlite3_destructor_type)-1)` in `sqlite3.h`)
///        re-expressed via `reinterpret_cast` instead of the macro, so no
///        C-style cast token ever appears at our call sites under
///        `-Wold-style-cast`/`-Weverything`. Tells SQLite to copy the bound
///        string immediately, since our `std::string` arguments may be gone
///        by the time a later `sqlite3_step` would otherwise read them.
inline const sqlite3_destructor_type kSqliteTransient = reinterpret_cast<sqlite3_destructor_type>(-1);

/// @brief RAII wrapper that finalizes a `sqlite3_stmt*` on scope exit,
///        including when an exception unwinds past it.
class StatementGuard {
public:
    explicit StatementGuard(sqlite3_stmt* stmt) : _stmt{stmt} {}
    ~StatementGuard() { sqlite3_finalize(_stmt); }

    StatementGuard(const StatementGuard&) = delete;
    StatementGuard& operator=(const StatementGuard&) = delete;
    StatementGuard(StatementGuard&&) = delete;
    StatementGuard& operator=(StatementGuard&&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return _stmt; }

private:
    sqlite3_stmt* _stmt;
};

}  // namespace detail

/// @brief Reference SQLite-backed `IOfflineQueue`: persists `payload`,
///        `idempotencyKey`, and `attempts` across process restarts.
///
/// Schema (one table, `morph_offline_queue`):
///
/// ```sql
/// CREATE TABLE IF NOT EXISTS morph_offline_queue (
///     id              INTEGER PRIMARY KEY AUTOINCREMENT,
///     payload         TEXT    NOT NULL,
///     idempotency_key TEXT    NOT NULL DEFAULT '',
///     attempts        INTEGER NOT NULL DEFAULT 0,
///     enqueued_at     INTEGER NOT NULL
/// );
/// CREATE UNIQUE INDEX IF NOT EXISTS ix_queue_idem
///     ON morph_offline_queue(idempotency_key) WHERE idempotency_key <> '';
/// ```
///
/// `id` is `AUTOINCREMENT`, so ids are never reused and a re-opened queue
/// re-presents each row under its **stored** id — stable across restarts.
/// `QueueItem::id` remains queue-local (per `docs/spec/offline/offline.md`);
/// cross-restart identity is carried by `idempotencyKey`, not `id`.
///
/// The partial unique index gives insert-time dedup for a non-empty
/// `idempotencyKey`: a re-enqueue of the same key is a no-op that returns the
/// existing row's id (`INSERT ... ON CONFLICT ... DO NOTHING`, then a lookup
/// on a no-op conflict). Empty keys (the default) are exempt, so keyless
/// items behave exactly as `InMemoryOfflineQueue` does — never deduplicated.
///
/// @par Crash safety
/// `drain()` never deletes, so a crash between `drain()` and `markDone()`
/// loses nothing. Each write (`enqueue`, `markDone`, `setAttempts`,
/// `setIdempotencyKey`) is its own committed statement; `PRAGMA
/// journal_mode=WAL` (set once, at construction) gives the durability.
///
/// @par Thread safety
/// All operations serialise on an internal mutex around the connection, so
/// this is safe to share between the application's enqueue-on-failure write
/// path and `SyncWorker`'s drain/replay read path.
class SqliteOfflineQueue : public IOfflineQueue {
public:
    using IOfflineQueue::enqueue;  // keep the two-arg overload visible

    /// @brief Opens (or creates) the queue database at @p path, creating the
    ///        schema if it does not already exist.
    /// @param path SQLite database file.
    /// @throws SqliteOfflineQueueError if the database cannot be opened or
    ///         the schema cannot be created.
    explicit SqliteOfflineQueue(std::filesystem::path path) : _path{std::move(path)} {
        if (sqlite3_open(_path.string().c_str(), &_db) != SQLITE_OK) {
            std::string msg = "SqliteOfflineQueue: failed to open " + _path.string() + ": " +
                              (_db != nullptr ? sqlite3_errmsg(_db) : "unknown error");
            if (_db != nullptr) {
                sqlite3_close(_db);
                _db = nullptr;
            }
            throw SqliteOfflineQueueError{msg};
        }
        execOrThrow("PRAGMA journal_mode=WAL;");
        execOrThrow(
            "CREATE TABLE IF NOT EXISTS morph_offline_queue ("
            "  id              INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  payload         TEXT    NOT NULL,"
            "  idempotency_key TEXT    NOT NULL DEFAULT '',"
            "  attempts        INTEGER NOT NULL DEFAULT 0,"
            "  enqueued_at     INTEGER NOT NULL"
            ");");
        execOrThrow(
            "CREATE UNIQUE INDEX IF NOT EXISTS ix_queue_idem "
            "ON morph_offline_queue(idempotency_key) WHERE idempotency_key <> '';");
    }

    /// @brief Closes the underlying SQLite connection.
    ~SqliteOfflineQueue() override {
        if (_db != nullptr) {
            sqlite3_close(_db);
        }
    }

    SqliteOfflineQueue(const SqliteOfflineQueue&) = delete;
    SqliteOfflineQueue& operator=(const SqliteOfflineQueue&) = delete;
    SqliteOfflineQueue(SqliteOfflineQueue&&) = delete;
    SqliteOfflineQueue& operator=(SqliteOfflineQueue&&) = delete;

    /// @brief Inserts @p payload with an empty idempotency key.
    /// @param payload Serialised action to persist.
    /// @return The new row's id (`SELECT last_insert_rowid()`).
    uint64_t enqueue(std::string payload) override {
        std::scoped_lock const lock{_mtx};
        detail::StatementGuard guard{
            prepare("INSERT INTO morph_offline_queue (payload, idempotency_key, attempts, enqueued_at) "
                    "VALUES (?, '', 0, ?);")};
        bindText(guard.get(), 1, payload);
        bindInt64(guard.get(), 2, nowMillis());
        stepOrThrow(guard.get(), "enqueue");
        return static_cast<uint64_t>(sqlite3_last_insert_rowid(_db));
    }

    /// @brief Inserts @p payload carrying @p idempotencyKey in one write. A
    ///        non-empty key already present on a row is deduplicated: the
    ///        existing row's id is returned and nothing new is inserted.
    /// @param payload        Serialised action to persist.
    /// @param idempotencyKey Stable dedup token; empty means "no dedup".
    /// @return The new row's id, or the existing row's id on a dedup hit.
    uint64_t enqueue(std::string payload, std::string idempotencyKey) override {
        std::scoped_lock const lock{_mtx};
        if (idempotencyKey.empty()) {
            detail::StatementGuard guard{
                prepare("INSERT INTO morph_offline_queue (payload, idempotency_key, attempts, enqueued_at) "
                        "VALUES (?, '', 0, ?);")};
            bindText(guard.get(), 1, payload);
            bindInt64(guard.get(), 2, nowMillis());
            stepOrThrow(guard.get(), "enqueue");
            return static_cast<uint64_t>(sqlite3_last_insert_rowid(_db));
        }

        detail::StatementGuard insertGuard{
            prepare("INSERT INTO morph_offline_queue (payload, idempotency_key, attempts, enqueued_at) "
                    "VALUES (?, ?, 0, ?) "
                    "ON CONFLICT(idempotency_key) WHERE idempotency_key <> '' DO NOTHING;")};
        bindText(insertGuard.get(), 1, payload);
        bindText(insertGuard.get(), 2, idempotencyKey);
        bindInt64(insertGuard.get(), 3, nowMillis());
        stepOrThrow(insertGuard.get(), "enqueue");
        if (sqlite3_changes(_db) > 0) {
            return static_cast<uint64_t>(sqlite3_last_insert_rowid(_db));
        }

        // Conflict fired (DO NOTHING) -- a row for this key already exists.
        detail::StatementGuard lookupGuard{prepare("SELECT id FROM morph_offline_queue WHERE idempotency_key = ?;")};
        bindText(lookupGuard.get(), 1, idempotencyKey);
        int const lookupResult = sqlite3_step(lookupGuard.get());
        if (lookupResult != SQLITE_ROW) {
            // The conflict proved a row exists, so anything but a row here is a
            // real failure. Returning the `0` this used to fall through with
            // would hand the caller an id that matches nothing: `markDone(0)`
            // silently deletes no row, and the item is stranded in the queue
            // forever with no error ever reported.
            throw SqliteOfflineQueueError{std::string{"SqliteOfflineQueue: enqueue could not resolve the existing id "
                                                      "for a deduplicated idempotency key: "} +
                                          sqlite3_errmsg(_db)};
        }
        return static_cast<uint64_t>(sqlite3_column_int64(lookupGuard.get(), 0));
    }

    /// @brief Returns all pending rows in ascending-id (enqueue) order.
    /// @return Snapshot of all pending items; the table is unchanged.
    std::vector<QueueItem> drain() override {
        std::scoped_lock const lock{_mtx};
        detail::StatementGuard guard{
            prepare("SELECT id, payload, idempotency_key, attempts FROM morph_offline_queue ORDER BY id;")};
        std::vector<QueueItem> out;
        for (;;) {
            int const stepResult = sqlite3_step(guard.get());
            if (stepResult == SQLITE_DONE) {
                break;
            }
            if (stepResult != SQLITE_ROW) {
                // `while (step() == SQLITE_ROW)` treated an I/O error, a corrupt
                // page, or SQLITE_BUSY as "no more rows", so drain() returned a
                // silently truncated set that the caller takes for the complete
                // list of pending work -- and SyncWorker then reports a clean
                // pass over a queue it never fully read.
                throw SqliteOfflineQueueError{std::string{"SqliteOfflineQueue: drain failed part-way through after "} +
                                              std::to_string(out.size()) + " row(s): " + sqlite3_errmsg(_db)};
            }
            QueueItem item;
            item.id = static_cast<uint64_t>(sqlite3_column_int64(guard.get(), 0));
            item.payload = textColumn(guard.get(), 1);
            item.idempotencyKey = textColumn(guard.get(), 2);
            item.attempts = static_cast<uint32_t>(sqlite3_column_int64(guard.get(), 3));
            out.push_back(std::move(item));
        }
        return out;
    }

    /// @brief Deletes the row identified by @p itemId. No-op if absent.
    /// @param itemId Id returned by the corresponding `enqueue()` call.
    void markDone(uint64_t itemId) override {
        std::scoped_lock const lock{_mtx};
        detail::StatementGuard guard{prepare("DELETE FROM morph_offline_queue WHERE id = ?;")};
        bindInt64(guard.get(), 1, static_cast<std::int64_t>(itemId));
        stepOrThrow(guard.get(), "markDone");
    }

    /// @brief Persists an updated attempt count for @p itemId. No-op if absent.
    /// @param itemId   Id of the item whose count changed.
    /// @param attempts New cumulative attempt count to store.
    void setAttempts(uint64_t itemId, uint32_t attempts) override {
        std::scoped_lock const lock{_mtx};
        detail::StatementGuard guard{prepare("UPDATE morph_offline_queue SET attempts = ? WHERE id = ?;")};
        bindInt64(guard.get(), 1, static_cast<std::int64_t>(attempts));
        bindInt64(guard.get(), 2, static_cast<std::int64_t>(itemId));
        stepOrThrow(guard.get(), "setAttempts");
    }

protected:
    /// @brief Stamps an idempotency key onto an already-inserted row. No-op if
    ///        @p itemId is absent. Reachable only if a caller invokes the base
    ///        `IOfflineQueue::enqueue(payload, key)` default through an
    ///        `IOfflineQueue&` -- this class's own `enqueue(payload, key)`
    ///        override above stamps the key inline in the same INSERT instead.
    /// @param itemId         Id of the row to stamp.
    /// @param idempotencyKey Key to store.
    void setIdempotencyKey(uint64_t itemId, std::string idempotencyKey) override {
        std::scoped_lock const lock{_mtx};
        detail::StatementGuard guard{prepare("UPDATE morph_offline_queue SET idempotency_key = ? WHERE id = ?;")};
        bindText(guard.get(), 1, idempotencyKey);
        bindInt64(guard.get(), 2, static_cast<std::int64_t>(itemId));
        stepOrThrow(guard.get(), "setIdempotencyKey");
    }

private:
    void execOrThrow(const char* sql) {
        char* errMsg = nullptr;
        if (sqlite3_exec(_db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
            std::string msg = errMsg != nullptr ? errMsg : "unknown sqlite error";
            sqlite3_free(errMsg);
            throw SqliteOfflineQueueError{"SqliteOfflineQueue: " + msg};
        }
    }

    sqlite3_stmt* prepare(const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw SqliteOfflineQueueError{std::string{"SqliteOfflineQueue: prepare failed: "} + sqlite3_errmsg(_db)};
        }
        return stmt;
    }

    void bindText(sqlite3_stmt* stmt, int index, const std::string& value) {
        if (sqlite3_bind_text(stmt, index, value.c_str(), -1, detail::kSqliteTransient) != SQLITE_OK) {
            throw SqliteOfflineQueueError{std::string{"SqliteOfflineQueue: bind failed: "} + sqlite3_errmsg(_db)};
        }
    }

    void bindInt64(sqlite3_stmt* stmt, int index, std::int64_t value) {
        if (sqlite3_bind_int64(stmt, index, value) != SQLITE_OK) {
            throw SqliteOfflineQueueError{std::string{"SqliteOfflineQueue: bind failed: "} + sqlite3_errmsg(_db)};
        }
    }

    void stepOrThrow(sqlite3_stmt* stmt, const char* what) {
        // A busy/error code is treated the same as reaching the end -- a
        // production consumer wanting to distinguish SQLITE_BUSY should retry
        // instead, but a single in-process mutex around the whole connection
        // makes SQLITE_BUSY practically unreachable for this reference queue.
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            throw SqliteOfflineQueueError{std::string{"SqliteOfflineQueue: "} + what +
                                          " failed: " + sqlite3_errmsg(_db)};
        }
    }

    static std::string textColumn(sqlite3_stmt* stmt, int index) {
        const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, index));
        return text != nullptr ? std::string{text} : std::string{};
    }

    static std::int64_t nowMillis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    std::filesystem::path _path;
    sqlite3* _db = nullptr;
    std::mutex _mtx;
};

}  // namespace morph::offline
