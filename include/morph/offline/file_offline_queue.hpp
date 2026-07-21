// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <glaze/glaze.hpp>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../core/logger.hpp"
#include "offline_queue.hpp"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace morph::offline {

/// @brief Thrown by `FileOfflineQueue` when its on-disk NDJSON cannot be
///        opened, or a non-trailing line is malformed.
struct FileOfflineQueueError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

namespace detail {

/// @brief One NDJSON line of `FileOfflineQueue`'s on-disk log.
///
/// `op == "put"` upserts `id`'s current `(payload, idempotencyKey, attempts)`;
/// `op == "done"` tombstones `id`. Replaying every line in file order and
/// applying each in turn (a later "put" overwrites an earlier one for the same
/// `id`; a "done" removes it) reconstructs the live item set — the
/// last-write-wins-per-id compaction `docs/spec/offline/offline.md` describes
/// for this variant.
struct FileQueueRecord {
    std::string op;
    uint64_t id{};
    std::string payload;
    std::string idempotencyKey;
    uint32_t attempts{0};
};

inline void throwOnGlazeError(const glz::error_ctx& errCode, std::string_view context) {
    if (errCode) {
        throw FileOfflineQueueError{glz::format_error(errCode, context)};
    }
}

inline std::string toJson(const FileQueueRecord& record) {
    std::string out;
    throwOnGlazeError(glz::write_json(record, out), out);
    return out;
}

inline FileQueueRecord fromJson(std::string_view json) {
    FileQueueRecord record{};
    throwOnGlazeError(glz::read_json(record, json), json);
    return record;
}

}  // namespace detail

/// @brief Reference append-only, NDJSON-backed `IOfflineQueue` that persists
///        `payload`, `idempotencyKey`, and `attempts` across process restarts
///        with no third-party dependency.
///
/// Each mutation (`enqueue`, `markDone`, `setAttempts`, `setIdempotencyKey`)
/// appends one JSON line and immediately `fflush`+`fsync`s it, so it is a
/// committed transaction before the call returns. On construction, the file
/// is replayed line by line (last-write-wins per id — a later "put" overwrites
/// an earlier one, a "done" tombstones it) and then rewritten in compacted
/// form (one "put" line per surviving item), which both bounds file growth and
/// heals a torn trailing line left by a crash mid-write — the same tolerance
/// `FileActionLog::entries()` (`include/morph/journal/file_action_log.hpp`)
/// gives a malformed *trailing* line: it is logged and skipped, not thrown: a
/// malformed line anywhere else is genuine corruption and is rethrown.
///
/// New ids resume from the highest id ever seen in the file (including
/// tombstoned ones), so a fresh item can never collide with an old tombstone
/// — unlike `FileActionLog`'s process-local `seq`, which does not need this
/// property because it never reuses/removes entries.
///
/// @par Thread safety
/// All public methods are thread-safe (guarded by an internal mutex). Not
/// safe for multiple processes to open the same path concurrently — same
/// restriction as `FileActionLog`.
///
/// @par Idempotency-key dedup
/// A keyed `enqueue` does a linear scan over the currently-pending items to
/// look for a matching `idempotencyKey` — O(pending items) per call. That is
/// fine at the queue depths this reference implementation targets; a host
/// with high-volume keyed enqueues should prefer `SqliteOfflineQueue`, whose
/// dedup is index-backed.
class FileOfflineQueue : public IOfflineQueue {
public:
    using IOfflineQueue::enqueue;  // keep the two-arg overload visible

    /// @brief Opens (or creates) the queue log at @p path, replaying and
    ///        compacting whatever is already there.
    /// @param path NDJSON file to store queue state in.
    /// @throws FileOfflineQueueError if @p path exists but contains a
    ///         malformed non-trailing line.
    /// @throws std::runtime_error if @p path cannot be opened/rewritten.
    explicit FileOfflineQueue(std::filesystem::path path) : _path{std::move(path)} {
        load();
        compact();
        _file = std::fopen(_path.string().c_str(), "a");
        if (_file == nullptr) {
            throw std::runtime_error("FileOfflineQueue: failed to open " + _path.string());
        }
    }

    /// @brief Closes the underlying file.
    // NOLINTNEXTLINE(cert-err33-c) — destructor context, can't propagate errors
    ~FileOfflineQueue() override {
        if (_file != nullptr) {
            std::fclose(_file);
        }
    }

    FileOfflineQueue(const FileOfflineQueue&) = delete;
    FileOfflineQueue& operator=(const FileOfflineQueue&) = delete;
    FileOfflineQueue(FileOfflineQueue&&) = delete;
    FileOfflineQueue& operator=(FileOfflineQueue&&) = delete;

    /// @brief Appends @p payload with no idempotency key.
    /// @param payload Serialised action to persist.
    /// @return A stable id that can be passed to `markDone()`.
    uint64_t enqueue(std::string payload) override { return enqueue(std::move(payload), {}); }

    /// @brief Appends @p payload carrying @p idempotencyKey. A non-empty key
    ///        already present on a pending item is deduplicated: the existing
    ///        item's id is returned and nothing new is written.
    /// @param payload        Serialised action to persist.
    /// @param idempotencyKey Stable dedup token; empty means "no dedup".
    /// @return The new item's id, or the existing item's id on a dedup hit.
    uint64_t enqueue(std::string payload, std::string idempotencyKey) override {
        std::scoped_lock const lock{_mtx};
        if (!idempotencyKey.empty()) {
            for (const auto& [existingId, item] : _items) {
                if (item.idempotencyKey == idempotencyKey) {
                    return existingId;
                }
            }
        }
        uint64_t const itemId = ++_nextId;
        QueueItem item{.id = itemId, .payload = std::move(payload), .idempotencyKey = std::move(idempotencyKey)};
        appendPut(item);
        _items.emplace(itemId, std::move(item));
        return itemId;
    }

    /// @brief Returns all pending items in ascending-id (enqueue) order.
    /// @return Snapshot of all pending items; the file itself is unchanged.
    std::vector<QueueItem> drain() override {
        std::scoped_lock const lock{_mtx};
        std::vector<QueueItem> out;
        out.reserve(_items.size());
        for (const auto& [id, item] : _items) {
            out.push_back(item);
        }
        return out;
    }

    /// @brief Tombstones @p itemId. No-op if not found.
    /// @param itemId Id returned by the corresponding `enqueue()` call.
    void markDone(uint64_t itemId) override {
        std::scoped_lock const lock{_mtx};
        if (_items.erase(itemId) == 0) {
            return;
        }
        appendDone(itemId);
    }

    /// @brief Persists an updated attempt count for @p itemId. No-op if not found.
    /// @param itemId   Id of the item whose count changed.
    /// @param attempts New cumulative attempt count to store.
    void setAttempts(uint64_t itemId, uint32_t attempts) override {
        std::scoped_lock const lock{_mtx};
        auto iter = _items.find(itemId);
        if (iter == _items.end()) {
            return;
        }
        iter->second.attempts = attempts;
        appendPut(iter->second);
    }

protected:
    /// @brief Stamps an idempotency key onto an already-enqueued item. No-op
    ///        if @p itemId is not found. Reachable only if a caller invokes
    ///        the base `IOfflineQueue::enqueue(payload, key)` default through
    ///        an `IOfflineQueue&` — this class's own `enqueue(payload, key)`
    ///        override above stamps the key inline instead.
    /// @param itemId         Id of the item to stamp.
    /// @param idempotencyKey Key to store.
    void setIdempotencyKey(uint64_t itemId, std::string idempotencyKey) override {
        std::scoped_lock const lock{_mtx};
        auto iter = _items.find(itemId);
        if (iter == _items.end()) {
            return;
        }
        iter->second.idempotencyKey = std::move(idempotencyKey);
        appendPut(iter->second);
    }

private:
    void appendPut(const QueueItem& item) {
        detail::FileQueueRecord const record{.op = "put",
                                             .id = item.id,
                                             .payload = item.payload,
                                             .idempotencyKey = item.idempotencyKey,
                                             .attempts = item.attempts};
        writeLine(detail::toJson(record));
    }

    void appendDone(uint64_t itemId) {
        detail::FileQueueRecord const record{
            .op = "done", .id = itemId, .payload = {}, .idempotencyKey = {}, .attempts = 0};
        writeLine(detail::toJson(record));
    }

    void writeLine(const std::string& json) {
        std::string line = json;
        line.push_back('\n');
        // NOLINTNEXTLINE(cert-err33-c) — durability checked via fflush/fsync below
        std::fwrite(line.data(), 1, line.size(), _file);
        syncFile(_file);
    }

    static void syncFile(std::FILE* file) {
        // NOLINTNEXTLINE(cert-err33-c)
        std::fflush(file);
#ifdef _WIN32
        _commit(_fileno(file));
#else
        ::fsync(fileno(file));
#endif
    }

    /// @brief Reads whatever is on disk and replays it into `_items`/`_nextId`.
    void load() {
        if (!std::filesystem::exists(_path)) {
            return;
        }
        std::ifstream in{_path};
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) {
                lines.push_back(line);
            }
        }
        uint64_t highestId = 0;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            detail::FileQueueRecord record;
            try {
                record = detail::fromJson(lines[i]);
            } catch (const std::exception& exc) {
                if (i + 1 == lines.size()) {
                    ::morph::log::logWarn("FileOfflineQueue: skipping malformed trailing line in " + _path.string() +
                                          ": " + std::string{exc.what()});
                    break;
                }
                throw;
            }
            highestId = std::max(highestId, record.id);
            if (record.op == "done") {
                _items.erase(record.id);
            } else {
                _items[record.id] = QueueItem{.id = record.id,
                                              .payload = record.payload,
                                              .idempotencyKey = record.idempotencyKey,
                                              .attempts = record.attempts};
            }
        }
        _nextId = highestId;
    }

    /// @brief Rewrites the file with exactly one "put" line per surviving
    ///        item, collapsing whatever history `load()` just replayed.
    ///        Called once from the constructor, after `load()` and before the
    ///        append-mode `_file` handle is opened for new writes.
    void compact() {
        std::string const tmp = _path.string() + ".compact-tmp";
        std::FILE* out = std::fopen(tmp.c_str(), "w");
        if (out == nullptr) {
            throw std::runtime_error("FileOfflineQueue: failed to open " + tmp + " for compaction");
        }
        for (const auto& [id, item] : _items) {
            detail::FileQueueRecord const record{.op = "put",
                                                 .id = item.id,
                                                 .payload = item.payload,
                                                 .idempotencyKey = item.idempotencyKey,
                                                 .attempts = item.attempts};
            std::string outLine = detail::toJson(record);
            outLine.push_back('\n');
            // NOLINTNEXTLINE(cert-err33-c)
            std::fwrite(outLine.data(), 1, outLine.size(), out);
        }
        syncFile(out);
        std::fclose(out);
        std::filesystem::rename(tmp, _path);
    }

    std::filesystem::path _path;
    std::FILE* _file = nullptr;
    std::mutex _mtx;
    std::map<uint64_t, QueueItem> _items;
    uint64_t _nextId{0};
};

}  // namespace morph::offline
