// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "../core/observability.hpp"

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
    /// `docs/spec/offline/offline.md`, "`idempotencyKey`: deduping against the journal").
    /// The value is opaque to the queue; a good choice is a stable content hash
    /// or a client-minted operation id (e.g. a UUID) reused if the same op is
    /// re-enqueued.
    ///
    /// The queue never *interprets* the key, and the interface does not
    /// **require** an implementation to enforce uniqueness on it — a replay
    /// consumer must therefore always dedup on the key itself, because a
    /// conforming queue may hand it the same key twice. An implementation is
    /// nonetheless **permitted** to dedup a non-empty key at enqueue time as a
    /// strengthening of that floor; see `IOfflineQueue::enqueue(std::string,
    /// std::string)` for exactly what a dedup hit does.
    std::string idempotencyKey;

    /// @brief Durable retry count for this item, authoritative when the queue
    ///        persists it.
    ///
    /// Defaults to `0`. A queue that overrides `IOfflineQueue::setAttempts()`
    /// to store this value makes the retry budget survive a process restart:
    /// `SyncWorker` seeds its own attempt counter from the larger of this
    /// field and its in-memory count, and writes the updated count back via
    /// `setAttempts()` after every failed replay. A queue that leaves
    /// `setAttempts()` as the default no-op never advances this field, so
    /// `SyncWorker`'s in-memory counter stays authoritative — today's
    /// process-local retry behavior, unchanged.
    uint32_t attempts{0};
};

/// @brief Thrown by `enqueue()` when the queue is at its configured `maxDepth()`.
///
/// `IOfflineQueue` enforces a reject-newest overflow policy: once the queue
/// holds `maxDepth()` items, a further `enqueue()` throws instead of silently
/// evicting an older item or invoking an app-defined callback. Evicting the
/// oldest item would destroy data the caller believes durable and break
/// replay ordering with no error raised anywhere; a host that genuinely wants
/// different eviction semantics already has the seam for it — subclass
/// `IOfflineQueue` directly rather than layering a second policy mechanism on
/// top of this one.
struct OfflineQueueFullError : std::runtime_error {
    /// @param maxDepthValue    The configured capacity that was reached.
    /// @param currentSizeValue Number of pending items at the time of rejection
    ///                         (equal to maxDepth for a well-behaved implementation).
    OfflineQueueFullError(std::size_t maxDepthValue, std::size_t currentSizeValue)
        : std::runtime_error("IOfflineQueue: enqueue rejected, queue is at capacity (" +
                             std::to_string(currentSizeValue) + "/" + std::to_string(maxDepthValue) + ")"),
          maxDepth{maxDepthValue},
          currentSize{currentSizeValue} {}

    /// @brief The configured capacity that was reached.
    std::size_t maxDepth;
    /// @brief Number of pending items at the time of rejection.
    std::size_t currentSize;
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
    [[nodiscard]] virtual uint64_t enqueue(std::string payload) = 0;

    /// @brief Appends @p payload carrying the cross-subsystem @p idempotencyKey.
    ///
    /// The key is stored on the resulting `QueueItem::idempotencyKey` so a
    /// replay consumer can dedup this op against ones the journal (or a prior
    /// replay) already applied (see `QueueItem::idempotencyKey` and
    /// `docs/spec/offline/offline.md`).
    ///
    /// @par Uniqueness is a floor, not a prohibition
    /// The queue never interprets the key, and this interface does not
    /// *require* uniqueness enforcement: a conforming implementation may store
    /// the same non-empty key twice, so a replay consumer must dedup on the key
    /// regardless. Enqueue-time dedup is an allowed **strengthening**, not a
    /// contract violation. Of the implementations morph ships,
    /// `InMemoryOfflineQueue` never dedups, while `FileOfflineQueue` (linear
    /// scan) and `SqliteOfflineQueue` (partial unique index) both do.
    ///
    /// @par What a dedup hit does, in every implementation that dedups
    /// - It applies only to a **non-empty** key already carried by a *pending*
    ///   item. An empty key is never a dedup token — two empty-key enqueues
    ///   always produce two distinct items, in every implementation.
    /// - The call **succeeds** and returns the **existing** item's id rather
    ///   than a fresh one, so the return value is not a reliable signal that
    ///   anything was stored.
    /// - It is **first-write-wins with silent payload loss**: @p payload is
    ///   discarded, the pending item keeps the payload it already had, and no
    ///   error is raised and nothing is reported to the caller. A caller that
    ///   re-enqueues a *corrected* payload under an unchanged key therefore
    ///   loses the correction — mint a new key when the payload changes.
    /// - `markDone()` releases the key: once the pending item is gone, the same
    ///   key enqueues normally again.
    /// - It survives a restart in a durable queue — re-enqueuing a key that a
    ///   pending persisted item still carries is a hit after a reopen.
    ///
    /// These guarantees are pinned for every shipped implementation by
    /// `tests/offline_queue_conformance.hpp`, which is told which policy each
    /// implementation has and asserts it.
    ///
    /// The default implementation delegates to `enqueue(std::move(payload))`
    /// and then stamps the key via `setIdempotencyKey`, so existing
    /// `IOfflineQueue` implementations keep working without overriding it.
    /// @param payload        Serialised action to persist.
    /// @param idempotencyKey Stable dedup token for the logical op; may be empty.
    /// @return A stable id that can be passed to `markDone()`.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
    [[nodiscard]] virtual uint64_t enqueue(std::string payload, std::string idempotencyKey) {
        const uint64_t itemId = enqueue(std::move(payload));
        setIdempotencyKey(itemId, std::move(idempotencyKey));
        return itemId;
    }
#pragma GCC diagnostic pop

    /// @brief Returns all pending items in enqueue order without removing them.
    ///
    /// Items remain in the queue until `markDone()` is called. It is safe to
    /// call `drain()` multiple times — items survive a crash between `drain()`
    /// and the corresponding `markDone()` call.
    /// @return Snapshot of all pending items.
    [[nodiscard]] virtual std::vector<QueueItem> drain() const = 0;

    /// @brief Removes the item identified by @p itemId.
    ///
    /// No-op if @p itemId is not found.
    /// @param itemId Id returned by the corresponding `enqueue()` call.
    virtual void markDone(uint64_t itemId) = 0;

    /// @brief Returns the number of pending items without removing them.
    ///
    /// Default implementation calls `drain().size()` — correct but O(n) and
    /// allocates a full snapshot vector to answer a size query. Override for
    /// an O(1) or index-backed answer.
    /// @return Current pending item count.
    [[nodiscard]] virtual std::size_t size() const { return drain().size(); }

    /// @brief Returns the configured maximum depth, or `std::nullopt` if unbounded.
    ///
    /// Default: `std::nullopt` (unbounded) — preserves current behavior for any
    /// `IOfflineQueue` subclass that predates this method.
    /// @return The capacity `enqueue()` enforces, or `std::nullopt` if none.
    [[nodiscard]] virtual std::optional<std::size_t> maxDepth() const { return std::nullopt; }

    /// @brief Persists an updated attempt count for an item. Default: no-op.
    ///
    /// A durable queue overrides this to store the count so the retry budget
    /// survives a restart — `SyncWorker` calls it after every failed replay,
    /// and reads the persisted value back through `QueueItem::attempts` on
    /// the next `drain()` (this run, or after a restart). `InMemoryOfflineQueue`
    /// overrides it to update the in-deque item; a queue that leaves it as
    /// the default no-op keeps the pre-existing process-local retry behavior
    /// (`SyncWorker`'s own in-memory counter is then always authoritative,
    /// since `QueueItem::attempts` never advances).
    /// @param itemId   Id of the item whose attempt count changed.
    /// @param attempts New cumulative attempt count to persist.
    virtual void setAttempts([[maybe_unused]] uint64_t itemId, [[maybe_unused]] uint32_t attempts) {}

protected:
    /// @brief Stamps an idempotency key onto an already-enqueued item.
    ///
    /// Called by the default `enqueue(payload, key)` overload so implementations
    /// that only override the single-argument `enqueue` still support keys. The
    /// default is a no-op (an implementation with no per-item key storage simply
    /// drops it); `InMemoryOfflineQueue` overrides it to record the key.
    /// @param itemId         Id of the item to stamp.
    /// @param idempotencyKey Key to store; ignored by the default no-op.
    virtual void setIdempotencyKey([[maybe_unused]] uint64_t itemId, [[maybe_unused]] std::string idempotencyKey) {}
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

    /// @brief Constructs an in-memory queue, optionally bounded.
    /// @param maxDepth Maximum number of pending items `enqueue()` will admit
    ///        before throwing `OfflineQueueFullError`; `std::nullopt` (the
    ///        default) means unbounded.
    explicit InMemoryOfflineQueue(std::optional<std::size_t> maxDepth = std::nullopt) : _maxDepth{maxDepth} {}

    /// @brief Appends @p payload and returns a monotonically increasing id.
    /// @param payload Serialised action to store.
    /// @return Unique id for this item.
    [[nodiscard]] uint64_t enqueue(std::string payload) override { return enqueue(std::move(payload), {}); }

    /// @brief Appends @p payload carrying @p idempotencyKey and returns a
    ///        monotonically increasing id.
    /// @param payload        Serialised action to store.
    /// @param idempotencyKey Stable dedup token; stored verbatim on the item.
    /// @return Unique id for this item.
    /// @throws OfflineQueueFullError if the queue is already at `maxDepth()`.
    [[nodiscard]] uint64_t enqueue(std::string payload, std::string idempotencyKey) override {
        std::scoped_lock const lock{_mtx};
        if (_maxDepth && _items.size() >= *_maxDepth) {
            ::morph::observe::detail::emitMetric(::morph::observe::Metric::queueOverflow,
                                                 static_cast<double>(_items.size()));
            throw OfflineQueueFullError{*_maxDepth, _items.size()};
        }
        uint64_t const itemId = ++_nextId;
        _items.push_back(
            QueueItem{.id = itemId, .payload = std::move(payload), .idempotencyKey = std::move(idempotencyKey)});
        return itemId;
    }

    /// @brief Returns a snapshot of all pending items. Thread-safe.
    /// @return Copy of all items in insertion order.
    [[nodiscard]] std::vector<QueueItem> drain() const override {
        std::scoped_lock const lock{_mtx};
        return std::vector<QueueItem>{_items.begin(), _items.end()};
    }

    /// @brief Returns the number of pending items. Thread-safe.
    /// @return Current pending item count.
    [[nodiscard]] std::size_t size() const override {
        std::scoped_lock const lock{_mtx};
        return _items.size();
    }

    /// @brief Returns the configured maximum depth, or `std::nullopt` if unbounded.
    /// @return The capacity `enqueue()` enforces, or `std::nullopt` if none.
    [[nodiscard]] std::optional<std::size_t> maxDepth() const override { return _maxDepth; }

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

    /// @brief Updates the persisted attempt count on the in-deque item with
    ///        @p itemId. No-op if @p itemId is not found. Thread-safe.
    ///
    /// `InMemoryOfflineQueue` does not itself survive a process restart, but
    /// overriding this hook lets a *fresh* `SyncWorker` constructed over the
    /// same instance observe a durable-style cumulative attempt count — used
    /// to simulate cross-restart dead-lettering in tests.
    /// @param itemId   Id of the item to update.
    /// @param attempts New attempt count to store.
    void setAttempts(uint64_t itemId, uint32_t attempts) override {
        std::scoped_lock const lock{_mtx};
        auto iter = std::ranges::find_if(_items, [itemId](const QueueItem& item) { return item.id == itemId; });
        if (iter != _items.end()) {
            iter->attempts = attempts;
        }
    }

private:
    mutable std::mutex _mtx;
    std::deque<QueueItem> _items;
    uint64_t _nextId{0};
    std::optional<std::size_t> _maxDepth;
};

}  // namespace morph::offline
