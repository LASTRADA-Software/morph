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
    uint64_t id;

    /// @brief Opaque serialised representation of the queued action.
    std::string payload;
};

// ── Interface ─────────────────────────────────────────────────────────────────

/// @brief Interface for durable storage of actions that could not be delivered.
///
/// Items accumulate while the system is offline and are replayed by `SyncWorker`
/// on reconnect. The interface is intentionally minimal so that implementations
/// can range from in-memory (`InMemoryOfflineQueue`) to SQLite or file-backed stores.
struct IOfflineQueue {
    virtual ~IOfflineQueue() = default;

    /// @brief Appends @p payload to the queue.
    ///
    /// @param payload Serialised action to persist.
    /// @return A stable id that can be passed to `markDone()`.
    virtual uint64_t enqueue(std::string payload) = 0;

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
};

// ── In-memory implementation ──────────────────────────────────────────────────

/// @brief Thread-safe in-memory implementation of `IOfflineQueue`.
///
/// Suitable for testing and for applications that do not require persistence
/// across process restarts. Items are stored in a `std::deque` protected by a mutex.
class InMemoryOfflineQueue : public IOfflineQueue {
public:
    /// @brief Appends @p payload and returns a monotonically increasing id.
    /// @param payload Serialised action to store.
    /// @return Unique id for this item.
    uint64_t enqueue(std::string payload) override {
        std::scoped_lock lock{_mtx};
        uint64_t itemId = ++_nextId;
        _items.push_back(QueueItem{.id = itemId, .payload = std::move(payload)});
        return itemId;
    }

    /// @brief Returns a snapshot of all pending items. Thread-safe.
    /// @return Copy of all items in insertion order.
    std::vector<QueueItem> drain() override {
        std::scoped_lock lock{_mtx};
        return std::vector<QueueItem>{_items.begin(), _items.end()};
    }

    /// @brief Removes the item with @p itemId from the queue. Thread-safe.
    ///
    /// No-op if @p itemId is not found.
    /// @param itemId Id of the item to remove.
    void markDone(uint64_t itemId) override {
        std::scoped_lock lock{_mtx};
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
