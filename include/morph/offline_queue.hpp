// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace morph::offline {

/// @brief An item stored in the offline queue.
///
/// The payload is an opaque string — the caller controls the serialisation
/// format (JSON, binary-hex, plain text, etc.).
struct QueueItem {
    /// @brief Stable identifier assigned at enqueue time.
    ///
    /// Local to *this* queue instance; it is **not** a cross-subsystem
    /// idempotency key (a durable queue re-presents the same logical op with a
    /// fresh `id` after a restart, and the journal never sees it). Use
    /// `idempotencyKey` to dedup a replay against already-applied ops.
    uint64_t id{};

    /// @brief Opaque serialised representation of the queued action.
    std::string payload;

    /// @brief Optional caller-supplied idempotency key that is **stable across
    ///        subsystems and process restarts** for one logical operation.
    ///
    /// Empty by default. When set, it is the shared dedup token that lets a
    /// replay consumer recognise an op the journal (or a previous replay) has
    /// already applied and skip it, instead of double-applying. The offline
    /// queue and the journal replay have no shared identity otherwise — the
    /// queue's `id` is queue-local and the journal's `seq` is journal-local —
    /// so a host that replays through both paths must wire this key (see
    /// `docs/spec/offline.md`, "Idempotency key: deduping against the journal").
    /// The value is opaque to the queue; a good choice is a stable content hash
    /// or a client-minted operation id (e.g. a UUID) reused if the same op is
    /// re-enqueued. The queue does not interpret, require, or enforce
    /// uniqueness on it — enforcement is the replay consumer's job.
    std::string idempotencyKey;
};

// ── Interface ─────────────────────────────────────────────────────────────────

/// @brief Interface for durable storage of actions that could not be delivered.
///
/// Items accumulate while the system is offline and are replayed by `SyncWorker`
/// on reconnect. The interface is intentionally minimal so that implementations
/// can range from in-memory (`InMemoryOfflineQueue`) to SQLite or file-backed stores.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
struct IOfflineQueue {
    virtual ~IOfflineQueue() = default;

    /// @brief Appends @p payload to the queue with no idempotency key.
    ///
    /// @param payload Serialised action to persist.
    /// @return A stable id that can be passed to `markDone()`.
    virtual uint64_t enqueue(std::string payload) = 0;

    /// @brief Appends @p payload carrying the cross-subsystem @p idempotencyKey.
    ///
    /// The key is stored on the resulting `QueueItem::idempotencyKey` so a
    /// replay consumer can dedup this op against ones the journal (or a prior
    /// replay) already applied (see `QueueItem::idempotencyKey` and
    /// `docs/spec/offline.md`). The queue neither interprets nor enforces
    /// uniqueness on the key.
    ///
    /// The default implementation delegates to `enqueue(std::move(payload))`
    /// and then stamps the key via `setIdempotencyKey`, so existing
    /// `IOfflineQueue` implementations keep working without overriding it.
    /// @param payload        Serialised action to persist.
    /// @param idempotencyKey Stable dedup token for the logical op; may be empty.
    /// @return A stable id that can be passed to `markDone()`.
    virtual uint64_t enqueue(std::string payload, std::string idempotencyKey) {
        const uint64_t itemId = enqueue(std::move(payload));
        setIdempotencyKey(itemId, std::move(idempotencyKey));
        return itemId;
    }

    /// @brief Returns all pending items in enqueue order without removing them.
    ///
    /// Items remain in the queue until `markDone()` is called. It is safe to
    /// call `drain()` multiple times — items survive a crash between `drain()`
    /// and the corresponding `markDone()` call.
    /// @return Snapshot of all pending items.
    virtual std::vector<QueueItem> drain() = 0;

    /// @brief Removes the item identified by @p itemId.
    ///
    /// No-op if @p itemId is not found.
    /// @param itemId Id returned by the corresponding `enqueue()` call.
    virtual void markDone(uint64_t itemId) = 0;

protected:
    /// @brief Stamps an idempotency key onto an already-enqueued item.
    ///
    /// Called by the default `enqueue(payload, key)` overload so implementations
    /// that only override the single-argument `enqueue` still support keys. The
    /// default is a no-op (an implementation with no per-item key storage simply
    /// drops it); `InMemoryOfflineQueue` overrides it to record the key.
    /// @param itemId         Id of the item to stamp.
    /// @param idempotencyKey Key to store; ignored by the default no-op.
    virtual void setIdempotencyKey([[maybe_unused]] uint64_t itemId,
                                   [[maybe_unused]] std::string idempotencyKey) {}
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

// ── In-memory implementation ──────────────────────────────────────────────────

/// @brief Thread-safe in-memory implementation of `IOfflineQueue`.
///
/// Suitable for testing and for applications that do not require persistence
/// across process restarts. Items are stored in a `std::deque` protected by a mutex.
class InMemoryOfflineQueue : public IOfflineQueue {
public:
    using IOfflineQueue::enqueue;  // keep the two-arg overload visible

    /// @brief Appends @p payload and returns a monotonically increasing id.
    /// @param payload Serialised action to store.
    /// @return Unique id for this item.
    uint64_t enqueue(std::string payload) override { return enqueue(std::move(payload), {}); }

    /// @brief Appends @p payload carrying @p idempotencyKey and returns a
    ///        monotonically increasing id.
    /// @param payload        Serialised action to store.
    /// @param idempotencyKey Stable dedup token; stored verbatim on the item.
    /// @return Unique id for this item.
    uint64_t enqueue(std::string payload, std::string idempotencyKey) override {
        std::scoped_lock const lock{_mtx};
        uint64_t const itemId = ++_nextId;
        _items.push_back(QueueItem{
            .id = itemId, .payload = std::move(payload), .idempotencyKey = std::move(idempotencyKey)});
        return itemId;
    }

    /// @brief Returns a snapshot of all pending items. Thread-safe.
    /// @return Copy of all items in insertion order.
    std::vector<QueueItem> drain() override {
        std::scoped_lock const lock{_mtx};
        return std::vector<QueueItem>{_items.begin(), _items.end()};
    }

    /// @brief Removes the item with @p itemId from the queue. Thread-safe.
    ///
    /// No-op if @p itemId is not found.
    /// @param itemId Id of the item to remove.
    void markDone(uint64_t itemId) override {
        std::scoped_lock const lock{_mtx};
        auto iter = std::ranges::find_if(_items, [itemId](const QueueItem& item) { return item.id == itemId; });
        if (iter != _items.end()) {
            _items.erase(iter);
        }
    }

private:
    std::mutex _mtx;
    std::deque<QueueItem> _items;
    uint64_t _nextId{0};
};

}  // namespace morph::offline
