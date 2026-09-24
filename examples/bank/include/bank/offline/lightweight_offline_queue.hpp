// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/Lightweight.hpp>
#include <cstddef>
#include <cstdint>
#include <morph/offline/offline_queue.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// A durable `morph::offline::IOfflineQueue` whose store is the LASTRADA
/// Lightweight ORM, living entirely on the consumer side of the seam.
///
/// morph owns the interface; the store is the application's. Nothing here is
/// visible to `include/morph/` — `grep -rn Lightweight include/morph/` returns
/// nothing, and that is the invariant this file exists to demonstrate rather
/// than to weaken. The framework's own durable queues
/// (`FileOfflineQueue`, `SqliteOfflineQueue`) are unchanged and unaffected: an
/// application that already has an ORM connection open does not need a second
/// persistence mechanism shipped by the framework, it needs the framework to
/// accept the one it has.

namespace bank::offline {

/// @brief Declared capacity of the payload column, in bytes.
///
/// SQLite ignores a declared column width, so this bounds nothing on that
/// backend; it is the width the migration declares, and it is what a
/// server-side backend (Lightweight speaks ODBC, so the same record works
/// against PostgreSQL or SQL Server) would enforce.
inline constexpr std::size_t kMaxPayloadBytes = 8000;

/// @brief One row of the `morph_offline_queue` table.
///
/// The schema *is* this type: `LIGHTWEIGHT_SQL_MIGRATION` builds the table from
/// it with `plan.CreateTable<OfflineQueueRecord>()`, so the column list, the
/// types and the record cannot drift apart.
///
/// The record has **no relations** — no `BelongsTo`, no `HasMany`. That is
/// deliberate and it is checked rather than assumed: `HasMany` resolves a
/// child's foreign key by matching ordinal member index, and the fluent
/// `Query`/`Update` surface does not accept `HasMany`-bearing records in a
/// non-reflection build. A queue row references nothing, so neither trap is
/// reachable here.
struct OfflineQueueRecord {
    /// @brief Table backing `IOfflineQueue` for this application.
    static constexpr std::string_view TableName = "morph_offline_queue";

    /// @brief Server-side autoincrement id, re-presented verbatim as
    ///        `QueueItem::id`.
    ///
    /// `drain()` orders by this column, which is what makes "pending items in
    /// enqueue order" true: the store assigns ids in insertion order and never
    /// reuses one.
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0

    /// @brief The opaque payload, stored as `VARBINARY` rather than text.
    ///
    /// `QueueItem::payload` is documented as an opaque string whose
    /// serialisation is the caller's choice, so it may contain an embedded
    /// `\0`. A character column would hand the byte run to the driver as a C
    /// string; `SqlDynamicBinary` binds `SQL_C_BINARY` with an explicit byte
    /// count in both directions, so the round trip is length-exact
    /// (`tests/offline_queue_conformance.hpp`'s `checkNulPayloadRoundTrip`).
    Light::Field<Light::SqlDynamicBinary<kMaxPayloadBytes>, Light::SqlRealName{"payload"}> payload;  // 1

    /// @brief The idempotency key, **hex-encoded**, or empty for "no key".
    ///
    /// Hex, not raw bytes, and the reason is a measured limitation of the ORM
    /// rather than a preference: enqueue-time dedup needs a `WHERE` on this
    /// column, and the fluent query builder accumulates its bound parameters in
    /// a `std::vector<Lightweight::SqlVariant>`
    /// (`SqlQuery/Core.hpp`'s `SqlSearchCondition::inputBindings`, appended to
    /// by `AppendLiteralValue`). `SqlVariant`'s alternative list carries no
    /// binary type, so a `SqlDynamicBinary` argument to `Where()` does not
    /// compile. Hex is NUL-free by construction, so a text column carries an
    /// arbitrary byte string without either truncating it or colliding two keys
    /// that share a NUL prefix.
    Light::Field<std::string, Light::SqlRealName{"idempotency_key_hex"}> idempotencyKeyHex;  // 2

    /// @brief Durable retry count, written by `setAttempts()`.
    Light::Field<std::uint32_t, Light::SqlRealName{"attempts"}> attempts{0};  // 3
};

/// @brief A `DataMapper`-backed durable offline queue.
///
/// Dedup policy: `KeyDedup::onPendingItems` — a non-empty key already carried
/// by a pending row is a hit, which returns that row's id and discards the new
/// payload (first-write-wins), exactly as `FileOfflineQueue` and
/// `SqliteOfflineQueue` do. The check is a `SELECT` under this instance's mutex
/// rather than a partial unique index, which makes it as strong as
/// `FileOfflineQueue`'s linear scan (single process) and deliberately not as
/// strong as `SqliteOfflineQueue`'s index (any writer): a partial unique index
/// — `UNIQUE(idempotency_key_hex) WHERE idempotency_key_hex <> ''` — has no
/// expression in Lightweight's migration DSL, and an unconditional unique index
/// would make the *second* empty-key enqueue fail, which the interface forbids.
class LightweightOfflineQueue final : public morph::offline::IOfflineQueue {
public:
    /// @brief Opens the queue over Lightweight's default connection.
    ///
    /// The table must already exist; the `LIGHTWEIGHT_SQL_MIGRATION` in this
    /// component's translation unit creates it when the application applies its
    /// pending migrations.
    /// @param maxDepth Maximum number of pending items `enqueue()` will admit
    ///        before throwing `morph::offline::OfflineQueueFullError`;
    ///        `std::nullopt` (the default) means unbounded.
    explicit LightweightOfflineQueue(std::optional<std::size_t> maxDepth = std::nullopt);

    /// @brief Opens the queue over its own connection to @p connectionString.
    /// @param connectionString ODBC connection string for the store.
    /// @param maxDepth Maximum number of pending items `enqueue()` will admit
    ///        before throwing `morph::offline::OfflineQueueFullError`;
    ///        `std::nullopt` means unbounded.
    explicit LightweightOfflineQueue(Lightweight::SqlConnectionString connectionString,
                                     std::optional<std::size_t> maxDepth = std::nullopt);

    /// @brief Appends @p payload with no idempotency key.
    /// @param payload Serialised action to persist.
    /// @return The stored row's id.
    /// @throws morph::offline::OfflineQueueFullError if the queue is at `maxDepth()`.
    [[nodiscard]] std::uint64_t enqueue(std::string payload) override;

    /// @brief Appends @p payload carrying @p idempotencyKey, storing both in
    ///        **one** `INSERT`.
    ///
    /// Overridden rather than inherited, and the base class asks implementors to
    /// do exactly this. The base default is two separate virtual calls
    /// (`enqueue`, then `setIdempotencyKey`), which for a SQL store writes the
    /// row and then rewrites it, is not atomic across the two, and —
    /// decisively — cannot dedup at all, because by the time the key is stamped
    /// the row is already in the table. The dedup lookup and the capacity check
    /// are separate statements, but they run inside this call's single lock
    /// acquisition, so no concurrent `enqueue`/`markDone` on this queue can
    /// interleave with them.
    /// @param payload        Serialised action to persist.
    /// @param idempotencyKey Stable dedup token for the logical op; may be empty.
    /// @return The stored row's id, or the existing pending row's id on a dedup hit.
    /// @throws morph::offline::OfflineQueueFullError if the queue is at `maxDepth()`
    ///         and the call is not a dedup hit.
    [[nodiscard]] std::uint64_t enqueue(std::string payload, std::string idempotencyKey) override;

    /// @brief Returns every pending row, ordered by id, without removing any.
    /// @return Snapshot of all pending items in enqueue order.
    [[nodiscard]] std::vector<morph::offline::QueueItem> drain() const override;

    /// @brief Deletes the row identified by @p itemId. No-op if absent.
    /// @param itemId Id returned by the corresponding `enqueue()` call.
    void markDone(std::uint64_t itemId) override;

    /// @brief Returns the pending row count via `SELECT COUNT(*)`.
    ///
    /// Overridden rather than inherited: the base default is `drain().size()`,
    /// which materialises every payload in the table to answer a scalar
    /// question.
    /// @return Current pending item count.
    [[nodiscard]] std::size_t size() const override;

    /// @brief Returns the configured maximum depth, or `std::nullopt`.
    /// @return The capacity `enqueue()` enforces, or `std::nullopt` if none.
    [[nodiscard]] std::optional<std::size_t> maxDepth() const override;

    /// @brief Persists @p attempts on the row identified by @p itemId.
    ///
    /// No-op if the row is gone, so a `SyncWorker` that writes a count back
    /// after the item was marked done does not fail.
    /// @param itemId   Id of the item whose attempt count changed.
    /// @param attempts New cumulative attempt count to persist.
    void setAttempts(std::uint64_t itemId, morph::offline::Attempts attempts) override;

private:
    mutable std::mutex _mtx;
    /// Mutable because `drain()` and `size()` are `const` on the interface
    /// while every `DataMapper` query mutates the mapper's owned statement.
    /// The framework's sqlite backend gets away without this only because it
    /// holds a raw `sqlite3*`, where constness stops at the pointer.
    mutable Lightweight::DataMapper _mapper;
    std::optional<std::size_t> _maxDepth;
};

}  // namespace bank::offline
