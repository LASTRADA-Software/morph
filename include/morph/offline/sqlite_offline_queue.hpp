// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <sqlite3.h>

#include <chrono>
#include <climits>
#include <core/platform/Clock.hpp>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "../attributes.hpp"
#include "../core/completion.hpp"
#include "../core/detail/owned_state.hpp"
#include "../core/executor.hpp"
#include "../core/file_io_ops.hpp"
#include "../core/logger.hpp"
#include "../core/observability.hpp"
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
    /// @brief Takes over finalization of @p stmt.
    /// @param stmt Statement to finalize on destruction. Borrowed for the
    ///             guard's whole lifetime — it is `sqlite3_finalize`d, not
    ///             copied, so it must stay valid until the guard is destroyed.
    explicit StatementGuard(sqlite3_stmt* stmt MORPH_LIFETIMEBOUND) : _stmt{stmt} {}
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
/// journal_mode=WAL` and `PRAGMA synchronous=FULL` (both set once, at
/// construction, and `journal_mode` read back and verified rather than
/// trusted -- some filesystems, e.g. NFS, silently fall back to `delete`
/// mode) give the durability. `PRAGMA busy_timeout` is also set at
/// construction: running every statement on one owner makes `SQLITE_BUSY`
/// unreachable for a single instance, but the class documents no single-opener restriction.
/// Construction also fsyncs the containing directory once, after
/// `sqlite3_open()` and every schema-setup `PRAGMA`/`CREATE` succeed, closing
/// the same fresh-file-creation directory-durability gap `FileIoOps::syncPath`
/// closes for `FileActionLog`/`FileOfflineQueue`.
///
/// @par One owner
/// The connection belongs to the executor given at construction (see
/// `IOfflineQueue`, "One owner"): every statement runs there, one at a time.
/// The application's enqueue-on-failure path and `SyncWorker`'s drain share
/// the queue by sharing that owner.
class SqliteOfflineQueue : public IOfflineQueue {
    class State;

public:
    using IOfflineQueue::drain;
    using IOfflineQueue::enqueue;  // keep the two-arg and completion overloads visible
    using IOfflineQueue::size;

    /// @brief `PRAGMA busy_timeout` set at construction: how long
    ///        a statement blocks on `SQLITE_BUSY` before giving up, in
    ///        milliseconds.
    static constexpr int kBusyTimeoutMillis = 5000;

    /// @brief How hard each committed statement is pushed onto the platter.
    enum class Synchronous : std::uint8_t {
        /// @brief `PRAGMA synchronous=NORMAL` — the default, and SQLite's own
        ///        recommendation under WAL. A crash or power loss can lose the
        ///        most recent commits, which this queue's at-least-once
        ///        delivery plus `idempotencyKey` dedup already absorb.
        normal,
        /// @brief `PRAGMA synchronous=FULL` — fsync on every committed
        ///        statement. Measured ~18x slower per mutation (~0.08ms to
        ///        ~1.44ms on NVMe/btrfs, sqlite 3.53.4), and every mutation here
        ///        is its own commit, so a 200-item `SyncWorker` drain goes from
        ///        ~16ms to ~290ms — all of it on the queue's owner, where it
        ///        holds up the producer's `enqueue()` too. Opt in when losing the
        ///        last few commits is genuinely unacceptable.
        full,
    };

    /// @brief SQLite's numeric `PRAGMA synchronous` level for this connection,
    ///        as read back at construction.
    ///
    /// `0` OFF, `1` NORMAL, `2` FULL, `3` EXTRA. Reported rather than inferred
    /// from the constructor argument: the argument is what was asked for, this
    /// is what SQLite confirmed.
    /// @return The level in force on this queue's own connection.
    [[nodiscard]] int synchronousLevel() const noexcept { return _synchronousLevel; }

    /// @brief The `journal_mode` this database actually ended up in.
    ///
    /// Normally `"wal"`. A filesystem without the shared-memory support WAL
    /// needs reports `"delete"`; `:memory:` and the temp spellings report
    /// `"memory"`. Construction warns rather than throws on anything but
    /// `"wal"` (durability is carried by `PRAGMA synchronous`), so this is how
    /// a caller that cares can tell which mode it got.
    /// @return The mode string as SQLite reported it at construction.
    [[nodiscard]] const std::string& journalMode() const noexcept { return _journalMode; }

    /// @brief Opens (or creates) the queue database at @p path, creating the
    ///        schema if it does not already exist; the queue belongs to @p owner.
    ///
    /// Runs on the constructing thread, before anything else can reach the queue.
    /// @param owner    The executor every statement runs on; must run one task
    ///        at a time. Borrowed: it must outlive this queue and run what it posts.
    /// @param path     SQLite database file.
    /// @param maxDepth Maximum number of pending rows `enqueue()` will admit
    ///        before throwing `OfflineQueueFullError`; `std::nullopt` (the
    ///        default) means unbounded. Not persisted in the database itself
    ///        — a per-construction parameter, so a reopen must pass it again
    ///        to keep the same cap enforced.
    /// @param ioOps Injectable directory-fsync primitive (`FileIoOps::syncPath`
    ///        only -- the rest of this class talks to SQLite directly, never
    ///        through `FileIoOps`); defaults to the real syscall. Test-only
    ///        seam, mirroring `FileActionLog`/`FileOfflineQueue`'s own
    ///        `FileIoOps` parameter, for forcing the directory-fsync failure
    ///        branch below without needing a real OS-level failure.
    /// @param synchronous `PRAGMA synchronous` level; see `Synchronous`.
    ///        Defaults to `normal`.
    /// @param busyTimeout How long a statement waits out a lock another
    ///        connection holds before returning `SQLITE_BUSY`. Defaults to
    ///        `kBusyTimeoutMillis`. One owner running every statement makes
    ///        `SQLITE_BUSY` unreachable for a single instance, so the timeout
    ///        only matters when something else has the database open — but
    ///        when it does, the wait happens on the owner and holds up every
    ///        other caller of this instance, a Qt GUI thread included. Pass `0` to restore
    ///        fail-fast.
    /// @param wallClock Clock the `enqueued_at` column is stamped from; the
    ///        system clock by default. Borrowed, so it must outlive the queue.
    /// @throws SqliteOfflineQueueError if the database cannot be opened, the
    ///         schema cannot be created, or the containing directory cannot
    ///         be fsynced after `sqlite3_open()` creates a brand-new file. A
    ///         `journal_mode` that does not end up as `wal` is **warned about,
    ///         not thrown** — see the constructor body.
    explicit SqliteOfflineQueue(::morph::exec::IExecutor& owner MORPH_LIFETIMEBOUND, std::filesystem::path path,
                                std::optional<std::size_t> maxDepth = std::nullopt,
                                ::morph::core::FileIoOps ioOps = {}, Synchronous synchronous = Synchronous::normal,
                                std::chrono::milliseconds busyTimeout = std::chrono::milliseconds{kBusyTimeoutMillis},
                                ::core::platform::WallClockRef wallClock = ::core::platform::defaultSystemWallClock())
        : SqliteOfflineQueue{owner, std::make_shared<State>(std::move(path), maxDepth, std::move(ioOps), synchronous,
                                                            busyTimeout, wallClock)} {}

    ~SqliteOfflineQueue() override = default;

    SqliteOfflineQueue(const SqliteOfflineQueue&) = delete;
    SqliteOfflineQueue& operator=(const SqliteOfflineQueue&) = delete;
    SqliteOfflineQueue(SqliteOfflineQueue&&) = delete;
    SqliteOfflineQueue& operator=(SqliteOfflineQueue&&) = delete;

    /// @brief Inserts @p payload with an empty idempotency key. On the owner.
    /// @param payload Serialised action to persist.
    /// @return The new row's id (`SELECT last_insert_rowid()`).
    /// @throws OfflineQueueFullError if the queue is already at `maxDepth()`.
    [[nodiscard]] uint64_t enqueue(std::string payload) override {
        return _owned.read("SqliteOfflineQueue::enqueue").enqueue(std::move(payload));
    }

    /// @brief Inserts @p payload carrying @p idempotencyKey in one write. On
    ///        the owner. A non-empty key already present on a row is
    ///        deduplicated: the existing row's id is returned and nothing new
    ///        is inserted.
    /// @param payload        Serialised action to persist.
    /// @param idempotencyKey Stable dedup token; empty means "no dedup".
    /// @return The new row's id, or the existing row's id on a dedup hit.
    /// @throws OfflineQueueFullError if the queue is already at `maxDepth()`.
    ///         Checked before the insert is attempted, so a call that would
    ///         have resolved to a dedup hit (inserting nothing) can also be
    ///         rejected when the queue happens to be full at the same time —
    ///         a documented, accepted conservatism rather than an extra
    ///         round trip to special-case it.
    [[nodiscard]] uint64_t enqueue(std::string payload, std::string idempotencyKey) override {
        return _owned.read("SqliteOfflineQueue::enqueue").enqueue(std::move(payload), std::move(idempotencyKey));
    }

    /// @brief Returns all pending rows in ascending-id (enqueue) order. On the owner.
    /// @return Snapshot of all pending items; the table is unchanged.
    [[nodiscard]] std::vector<QueueItem> drain() const override {
        return _owned.read("SqliteOfflineQueue::drain").drain();
    }

    /// @brief Deletes the row identified by @p itemId, on the owner. No-op if absent.
    ///
    /// Called off the owner, the delete is posted there and this returns at
    /// once; a statement that fails there is logged, and the row stays queued.
    /// @param itemId Id returned by the corresponding `enqueue()` call.
    void markDone(uint64_t itemId) override {
        _owned.apply("SqliteOfflineQueue::markDone", [itemId](State& state) { state.markDone(itemId); });
    }

    /// @brief Persists an updated attempt count for @p itemId, on the owner.
    ///        No-op if absent.
    /// @param itemId   Id of the item whose count changed.
    /// @param attempts New cumulative attempt count to store.
    void setAttempts(uint64_t itemId, Attempts attempts) override {
        _owned.apply("SqliteOfflineQueue::setAttempts",
                     [itemId, attempts](State& state) { state.setAttempts(itemId, attempts); });
    }

    /// @brief Returns the number of pending rows. On the owner.
    /// @return Current pending item count (`COUNT(*)` against the table).
    [[nodiscard]] std::size_t size() const override { return _owned.read("SqliteOfflineQueue::size").size(); }

    /// @brief Returns the configured maximum depth, or `std::nullopt` if
    ///        unbounded. Callable anywhere: fixed at construction.
    /// @return The capacity `enqueue()` enforces, or `std::nullopt` if none.
    [[nodiscard]] std::optional<std::size_t> maxDepth() const override { return _maxDepth; }

protected:
    /// @brief Stamps an idempotency key onto an already-inserted row, on the
    ///        owner. No-op if @p itemId is absent. Reachable only if a caller
    ///        invokes the base `IOfflineQueue::enqueue(payload, key)` default
    ///        through an `IOfflineQueue&` -- this class's own
    ///        `enqueue(payload, key)` override above stamps the key inline in
    ///        the same INSERT instead.
    /// @param itemId         Id of the row to stamp.
    /// @param idempotencyKey Key to store.
    void setIdempotencyKey(uint64_t itemId, std::string idempotencyKey) override {
        _owned.apply("SqliteOfflineQueue::setIdempotencyKey",
                     [itemId, idempotencyKey = std::move(idempotencyKey)](State& state) mutable {
                         state.setIdempotencyKey(itemId, std::move(idempotencyKey));
                     });
    }

    /// @brief Enqueues on the owner, answered on @p replyExec.
    /// @param replyExec      Where the answer is delivered.
    /// @param payload        Serialised action to persist.
    /// @param idempotencyKey Stable dedup token; may be empty.
    /// @return The row's id, or the enqueue's error.
    [[nodiscard]] ::morph::async::Completion<uint64_t> askEnqueue(::morph::exec::IExecutor& replyExec,
                                                                  std::string payload,
                                                                  std::string idempotencyKey) override {
        return _owned.ask<uint64_t>(
            "SqliteOfflineQueue::enqueue", replyExec,
            [payload = std::move(payload), idempotencyKey = std::move(idempotencyKey)](State& state) mutable {
                return state.enqueue(std::move(payload), std::move(idempotencyKey));
            });
    }

    /// @brief Reads every pending row on the owner, answered on @p replyExec.
    /// @param replyExec Where the answer is delivered.
    /// @return The pending items, or the read's error.
    [[nodiscard]] ::morph::async::Completion<std::vector<QueueItem>> askDrain(
        ::morph::exec::IExecutor& replyExec) const override {
        return _owned.ask<std::vector<QueueItem>>("SqliteOfflineQueue::drain", replyExec,
                                                  [](State& state) { return state.drain(); });
    }

    /// @brief Counts the rows on the owner, answered on @p replyExec.
    /// @param replyExec Where the answer is delivered.
    /// @return The pending item count.
    [[nodiscard]] ::morph::async::Completion<std::size_t> askSize(::morph::exec::IExecutor& replyExec) const override {
        return _owned.ask<std::size_t>("SqliteOfflineQueue::size", replyExec,
                                       [](State& state) { return state.size(); });
    }

private:
    /// Adopts @p state, built by the public constructor, and copies out what
    /// is fixed at construction so it can be read from any thread.
    SqliteOfflineQueue(::morph::exec::IExecutor& owner, std::shared_ptr<State> state)
        : _maxDepth{state->maxDepth()},
          _synchronousLevel{state->synchronousLevel()},
          _journalMode{state->journalMode()},
          _owned{owner, std::move(state)} {}

    /// The connection and everything configured on it; touched only on the
    /// owner once the constructor has returned. Closes the connection when the
    /// last task that holds it, or the queue, lets go.
    class State {
    public:
        State(std::filesystem::path path, std::optional<std::size_t> maxDepth, ::morph::core::FileIoOps ioOps,
              Synchronous synchronous, std::chrono::milliseconds busyTimeout, ::core::platform::WallClockRef wallClock)
            : _path{std::move(path)},
              _maxDepth{maxDepth},
              _io{std::move(ioOps)},
              _synchronous{synchronous},
              _busyTimeout{busyTimeout},
              _wallClock{wallClock} {
            if (sqlite3_open(_path.string().c_str(), &_db) != SQLITE_OK) {
                std::string msg = "SqliteOfflineQueue: failed to open " + _path.string() + ": " +
                                  (_db != nullptr ? sqlite3_errmsg(_db) : "unknown error");
                if (_db != nullptr) {
                    sqlite3_close(_db);
                    _db = nullptr;
                }
                throw SqliteOfflineQueueError{msg};
            }
            // _db is a live, open connection from here on -- if any schema-setup
            // statement below throws, this constructor never completes, so
            // ~State() never runs to close it. Close it here before
            // rethrowing, the same discipline the open-failure branch above
            // already applies to its own failure path.
            try {
                // FIRST, before anything that can return SQLITE_BUSY. Converting a
                // database to WAL needs an exclusive lock, so with a second
                // connection open `PRAGMA journal_mode=WAL` is itself a BUSY
                // candidate -- issued after the timeout it waits, issued before it
                // fails instantly with "database is locked" (measured: 12ms to
                // throw without, a full 1001ms wait with a 1000ms timeout set
                // first). Setting it last, as an earlier revision did, left the
                // multi-opener case it was added for failing exactly as before.
                execOrThrow(("PRAGMA busy_timeout=" + std::to_string(_busyTimeout.count()) + ";").c_str());

                // Before journal_mode, and unconditional. SQLite's default for the
                // rollback journal is already FULL; it is WAL that lowers it to
                // NORMAL, so raising it here first means the setting holds whether
                // or not WAL takes. Ordered the other way round, a database that
                // fell back to the rollback journal kept whatever synchronous it
                // happened to have.
                execOrThrow(
                    ("PRAGMA synchronous=" + std::string{_synchronous == Synchronous::full ? "FULL" : "NORMAL"} + ";")
                        .c_str());
                // Read back for the same reason journal_mode is: sqlite3_exec
                // discards the row a PRAGMA returns, so a setting that did not take
                // would otherwise be invisible. `synchronous` is a *connection*
                // property, not a database one, so this is also the only way a
                // caller can confirm the level it asked for is the level in force.
                {
                    detail::StatementGuard const guard{prepare("PRAGMA synchronous;")};
                    if (sqlite3_step(guard.get()) != SQLITE_ROW) {
                        throw SqliteOfflineQueueError{"SqliteOfflineQueue: failed to read back synchronous"};
                    }
                    _synchronousLevel = sqlite3_column_int(guard.get(), 0);
                }

                execOrThrow("PRAGMA journal_mode=WAL;");
                // execOrThrow() discards sqlite3_exec's row callback, so a silent
                // fallback would otherwise go unnoticed. Read the
                // pragma back through a real prepared statement rather than
                // trusting the set.
                //
                // Warn, do not throw. An earlier revision made a non-`wal` result a
                // hard construction failure, which refused configurations that were
                // never actually less durable: `journal_mode` reports `memory` for
                // `:memory:` and the temp/"" spellings, and `delete` on filesystems
                // without the shared-memory WAL needs (NFS, CIFS/SMB, some overlay,
                // 9p and Docker mounts). All of those persist correctly through the
                // rollback journal -- whose durability is exactly what the
                // `synchronous` pragma above now guarantees regardless of mode. The
                // real call sites are not hypothetical either: kanban's
                // `enableOfflineQueue()` builds one of these from a user-supplied
                // path, so a throw here means an NFS home directory cannot open the
                // app at all.
                {
                    detail::StatementGuard const guard{prepare("PRAGMA journal_mode;")};
                    if (sqlite3_step(guard.get()) != SQLITE_ROW) {
                        throw SqliteOfflineQueueError{"SqliteOfflineQueue: failed to read back journal_mode"};
                    }
                    _journalMode = textColumn(guard.get(), 0);
                    if (_journalMode != "wal") {
                        ::morph::log::logWarn(
                            "SqliteOfflineQueue: PRAGMA journal_mode=WAL did not take for {} (got '{}'); continuing "
                            "on "
                            "that mode -- durability is carried by PRAGMA synchronous, but concurrent readers and the "
                            "writer will not overlap as they do under WAL",
                            _path.string(), _journalMode);
                    }
                }
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
                // sqlite3_open() above creates `_path` (and, once journal_mode=WAL
                // took, its "-wal"/"-shm" siblings) if it did not already exist --
                // a fresh directory entry that SQLite's own internal fsyncs of the
                // *file's* contents never make durable -- the identical
                // directory-vs-file-fsync gap
                // `FileActionLog`/`FileOfflineQueue` close (see `FileIoOps::syncPath`'s
                // own docs). Unconditional: harmless when the file already
                // existed, since syncing an unchanged directory is a cheap no-op.
                //
                // Which directory, though, is SQLite's to answer rather than
                // `_path`'s. `:memory:`, `""` and the `file::memory:` URI spellings
                // open no file at all, and `parent_path()` is empty for them --
                // which `FileIoOps::syncPath` resolves to `"."`, so deriving the
                // directory from `_path` meant fsyncing the *process's current
                // working directory* and reporting the result as this queue's: a
                // warning naming a database that is not on disk where a CWD cannot
                // be fsynced, and a refusal to construct an in-memory queue at all
                // where that fsync genuinely fails. `sqlite3_db_filename` reports
                // an empty name for exactly those spellings, so it distinguishes
                // "no backing file, nothing to sync" from a real path without this
                // class having to re-parse SQLite's own filename grammar.
                const char* const backingFile = sqlite3_db_filename(_db, "main");
                if (backingFile != nullptr && *backingFile != '\0') {
                    std::filesystem::path const backingPath{backingFile};
                    auto const dirSync = ::morph::core::classifyDirectorySync(_io.syncPath(backingPath.parent_path()));
                    if (dirSync == ::morph::core::DirectorySync::failed) {
                        throw SqliteOfflineQueueError{"SqliteOfflineQueue: failed to fsync directory after creating " +
                                                      backingPath.string()};
                    }
                    if (dirSync == ::morph::core::DirectorySync::unsupported) {
                        ::morph::log::logWarn(
                            "SqliteOfflineQueue: cannot fsync the directory containing {}; SQLite's own fsyncs still "
                            "cover the database contents, but its directory entry is only as durable as this "
                            "filesystem "
                            "makes it",
                            backingPath.string());
                    }
                }
            } catch (...) {
                sqlite3_close(_db);
                _db = nullptr;
                throw;
            }
        }

        /// @brief Closes the underlying SQLite connection.
        ~State() {
            if (_db != nullptr) {
                sqlite3_close(_db);
            }
        }

        State(const State&) = delete;
        State& operator=(const State&) = delete;
        State(State&&) = delete;
        State& operator=(State&&) = delete;

        /// @brief Inserts @p payload with an empty idempotency key.
        /// @param payload Serialised action to persist.
        /// @return The new row's id (`SELECT last_insert_rowid()`).
        /// @throws OfflineQueueFullError if the queue is already at `maxDepth()`.
        [[nodiscard]] uint64_t enqueue(std::string payload) {
            checkCapacity();
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
        /// @throws OfflineQueueFullError if the queue is already at `maxDepth()`.
        ///         Checked before the insert is attempted, so a call that would
        ///         have resolved to a dedup hit (inserting nothing) can also be
        ///         rejected when the queue happens to be full at the same time —
        ///         a documented, accepted conservatism rather than an extra
        ///         round trip to special-case it.
        [[nodiscard]] uint64_t enqueue(std::string payload, std::string idempotencyKey) {
            if (idempotencyKey.empty()) {
                checkCapacity();
                detail::StatementGuard guard{
                    prepare("INSERT INTO morph_offline_queue (payload, idempotency_key, attempts, enqueued_at) "
                            "VALUES (?, '', 0, ?);")};
                bindText(guard.get(), 1, payload);
                bindInt64(guard.get(), 2, nowMillis());
                stepOrThrow(guard.get(), "enqueue");
                return static_cast<uint64_t>(sqlite3_last_insert_rowid(_db));
            }

            checkCapacity();
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
            detail::StatementGuard lookupGuard{
                prepare("SELECT id FROM morph_offline_queue WHERE idempotency_key = ?;")};
            bindText(lookupGuard.get(), 1, idempotencyKey);
            int const lookupResult = sqlite3_step(lookupGuard.get());
            if (lookupResult != SQLITE_ROW) {
                // The conflict proved a row exists, so anything but a row here is a
                // real failure. Returning the `0` this used to fall through with
                // would hand the caller an id that matches nothing: `markDone(0)`
                // silently deletes no row, and the item is stranded in the queue
                // forever with no error ever reported.
                //
                // COVERAGE GAP, documented not closed: reaching this branch needs
                // a second writer on the same underlying file to delete this
                // exact row in the narrow window between the INSERT above and
                // this SELECT -- a genuine cross-connection race, not reachable
                // through this class's one owner or its own
                // single-connection API at all. Unlike the drain()/prepare()/
                // stepOrThrow() gaps closed elsewhere in this file via a
                // second-connection helper, this window opens and closes within
                // one function call on one thread, with no existing seam to pause
                // mid-statement and no SQLite-level hook (commit hook / custom
                // VFS) in this codebase to force it deterministically. A tight
                // busy-loop second thread could occasionally win the race, but
                // that is a fragile, non-deterministic test for one low-value
                // branch arm -- left open and documented rather than forced.
                throw SqliteOfflineQueueError{
                    std::string{"SqliteOfflineQueue: enqueue could not resolve the existing id "
                                "for a deduplicated idempotency key: "} +
                    sqlite3_errmsg(_db)};
            }
            return static_cast<uint64_t>(sqlite3_column_int64(lookupGuard.get(), 0));
        }

        /// @brief Returns all pending rows in ascending-id (enqueue) order.
        /// @return Snapshot of all pending items; the table is unchanged.
        [[nodiscard]] std::vector<QueueItem> drain() const {
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
                    throw SqliteOfflineQueueError{
                        std::string{"SqliteOfflineQueue: drain failed part-way through after "} +
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
        void markDone(uint64_t itemId) {
            detail::StatementGuard guard{prepare("DELETE FROM morph_offline_queue WHERE id = ?;")};
            bindInt64(guard.get(), 1, static_cast<std::int64_t>(itemId));
            stepOrThrow(guard.get(), "markDone");
        }

        /// @brief Persists an updated attempt count for @p itemId. No-op if absent.
        /// @param itemId   Id of the item whose count changed.
        /// @param attempts New cumulative attempt count to store.
        void setAttempts(uint64_t itemId, Attempts attempts) {
            detail::StatementGuard const guard{prepare("UPDATE morph_offline_queue SET attempts = ? WHERE id = ?;")};
            bindInt64(guard.get(), 1, static_cast<std::int64_t>(attempts.value()));
            bindInt64(guard.get(), 2, static_cast<std::int64_t>(itemId));
            stepOrThrow(guard.get(), "setAttempts");
        }

        /// @brief Returns the number of pending rows.
        /// @return Current pending item count (`COUNT(*)` against the table).
        [[nodiscard]] std::size_t size() const { return countRows(); }

        /// @brief Returns the configured maximum depth, or `std::nullopt` if unbounded.
        /// @return The capacity `enqueue()` enforces, or `std::nullopt` if none.
        [[nodiscard]] std::optional<std::size_t> maxDepth() const { return _maxDepth; }

        [[nodiscard]] int synchronousLevel() const noexcept { return _synchronousLevel; }

        [[nodiscard]] const std::string& journalMode() const noexcept { return _journalMode; }

        /// @brief Stamps an idempotency key onto an already-inserted row. No-op if
        ///        @p itemId is absent. Reachable only if a caller invokes the base
        ///        `IOfflineQueue::enqueue(payload, key)` default through an
        ///        `IOfflineQueue&` -- this class's own `enqueue(payload, key)`
        ///        override above stamps the key inline in the same INSERT instead.
        /// @param itemId         Id of the row to stamp.
        /// @param idempotencyKey Key to store.
        void setIdempotencyKey(uint64_t itemId, std::string idempotencyKey) {
            // `WHERE NOT EXISTS (...)` rather than a bare UPDATE: the partial
            // unique index `ix_queue_idem` rejects stamping a non-empty key a
            // pending row already holds, and a bare UPDATE would turn that into a
            // thrown SqliteOfflineQueueError -- while this class's own
            // `enqueue(payload, key)` resolves the identical conflict silently, by
            // keeping the existing row. Same conflict, two answers, and only one of
            // them matches the documented dedup contract.
            //
            // Throwing also bought nothing. This hook is called by the *base*
            // `IOfflineQueue::enqueue(payload, key)` default, which has already
            // inserted the row by the time it stamps; on a conflict the row exists
            // either way, so the throw added an exception to an outcome it could
            // not undo. Leaving the key unset is the same end state without it.
            detail::StatementGuard const guard{
                prepare("UPDATE morph_offline_queue SET idempotency_key = ?1 WHERE id = ?2 "
                        "AND (?1 = '' OR NOT EXISTS (SELECT 1 FROM morph_offline_queue "
                        "WHERE idempotency_key = ?1 AND id <> ?2));")};
            bindText(guard.get(), 1, idempotencyKey);
            bindInt64(guard.get(), 2, static_cast<std::int64_t>(itemId));
            stepOrThrow(guard.get(), "setIdempotencyKey");
        }

        void execOrThrow(const char* sql) {
            char* errMsg = nullptr;
            if (sqlite3_exec(_db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
                std::string msg = errMsg != nullptr ? errMsg : "unknown sqlite error";
                sqlite3_free(errMsg);
                throw SqliteOfflineQueueError{"SqliteOfflineQueue: " + msg};
            }
        }

        sqlite3_stmt* prepare(const char* sql) const {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
                throw SqliteOfflineQueueError{std::string{"SqliteOfflineQueue: prepare failed: "} +
                                              sqlite3_errmsg(_db)};
            }
            return stmt;
        }

        // COVERAGE GAP, documented not closed: this throw's only realistic
        // non-misuse trigger is SQLITE_TOOBIG, raised when `value` exceeds
        // SQLite's SQLITE_LIMIT_LENGTH -- confirmed ~2 GB on this SDK's SQLite
        // 3.51 via sqlite3_limit() (a 1,000,000,001-byte string was confirmed to
        // still bind SQLITE_OK). This class does not expose the raw sqlite3*
        // handle for a test to lower that limit via sqlite3_limit(), so the only
        // way to reach this branch is allocating and binding an actual >2 GB
        // std::string -- kSqliteTransient below means SQLite additionally copies
        // it, so the peak footprint is >4 GB for one assertion. Evaluated and
        // declined as not worth the memory/CI risk on an 8 GB development
        // machine for a single low-value branch that a legitimate offline-queue
        // payload will never approach in practice.
        void bindText(sqlite3_stmt* stmt, int index, const std::string& value) const {
            // An explicit length (not -1) is required so a NUL inside `value` --
            // legitimate, since payload/idempotencyKey are opaque strings the
            // caller controls the serialisation of -- doesn't tell
            // SQLite to measure only up to that byte and silently truncate.
            if (value.size() > static_cast<std::size_t>(INT_MAX)) {
                throw SqliteOfflineQueueError{"SqliteOfflineQueue: value exceeds INT_MAX bytes"};
            }
            if (sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()),
                                  detail::kSqliteTransient) != SQLITE_OK) {
                throw SqliteOfflineQueueError{std::string{"SqliteOfflineQueue: bind failed: "} + sqlite3_errmsg(_db)};
            }
        }

        void bindInt64(sqlite3_stmt* stmt, int index, std::int64_t value) const {
            if (sqlite3_bind_int64(stmt, index, value) != SQLITE_OK) {
                throw SqliteOfflineQueueError{std::string{"SqliteOfflineQueue: bind failed: "} + sqlite3_errmsg(_db)};
            }
        }

        void stepOrThrow(sqlite3_stmt* stmt, const char* what) const {
            // Anything but SQLITE_DONE throws, so SQLITE_BUSY is not distinguished
            // from a genuine error -- a production consumer wanting to retry on busy
            // would need to split them. The one owner every statement runs on
            // makes SQLITE_BUSY practically unreachable for this
            // reference queue, which is why the distinction is not made here.
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                throw SqliteOfflineQueueError{std::string{"SqliteOfflineQueue: "} + what +
                                              " failed: " + sqlite3_errmsg(_db)};
            }
        }

        static std::string textColumn(sqlite3_stmt* stmt, int index) {
            // sqlite3_column_bytes() gives the real stored length; constructing a
            // std::string from the raw `const char*` alone would stop at the
            // first NUL and silently truncate a NUL-bearing payload or
            // idempotency key on the way back out -- the read-side half of the
            // same truncation bindText() above prevents on the write side.
            const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, index));
            return text != nullptr ? std::string{text, static_cast<std::size_t>(sqlite3_column_bytes(stmt, index))}
                                   : std::string{};
        }

        [[nodiscard]] std::int64_t nowMillis() const noexcept {
            return std::chrono::duration_cast<std::chrono::milliseconds>(_wallClock.now().time_since_epoch()).count();
        }

        /// @brief Returns the current row count. On the owner.
        /// @return `COUNT(*)` against `morph_offline_queue`.
        std::size_t countRows() const {
            detail::StatementGuard guard{prepare("SELECT COUNT(*) FROM morph_offline_queue;")};
            sqlite3_step(guard.get());
            return static_cast<std::size_t>(sqlite3_column_int64(guard.get(), 0));
        }

        /// @brief Throws `OfflineQueueFullError` if the queue is already at
        ///        `maxDepth()`. On the owner. No-op if unbounded.
        void checkCapacity() const {
            if (!_maxDepth) {
                return;
            }
            std::size_t const current = countRows();
            if (current >= *_maxDepth) {
                ::morph::observe::detail::emitMetric(::morph::observe::Metric::queueOverflow,
                                                     static_cast<double>(current));
                throw OfflineQueueFullError{*_maxDepth, current};
            }
        }

        std::filesystem::path _path;
        mutable sqlite3* _db = nullptr;
        std::optional<std::size_t> _maxDepth;
        ::morph::core::FileIoOps _io;
        Synchronous _synchronous{Synchronous::normal};
        std::chrono::milliseconds _busyTimeout{kBusyTimeoutMillis};
        ::core::platform::WallClockRef _wallClock;
        // SQLite's numeric synchronous level, read back at construction.
        int _synchronousLevel{-1};
        // The mode the database actually ended up in, as read back at construction
        // -- "wal" normally, something else on a filesystem that cannot support it.
        std::string _journalMode;
    };

    std::optional<std::size_t> _maxDepth;
    // SQLite's numeric synchronous level, read back at construction.
    int _synchronousLevel{-1};
    // The mode the database actually ended up in, as read back at construction
    // -- "wal" normally, something else on a filesystem that cannot support it.
    std::string _journalMode;
    ::morph::exec::detail::OwnedState<State> _owned;
};

}  // namespace morph::offline
