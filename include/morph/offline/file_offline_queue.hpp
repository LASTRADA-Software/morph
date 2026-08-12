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

/// @brief Write options that escape ASCII control bytes as `\\uXXXX` sequences.
///
/// glaze 7.4 leaves control bytes (0x00-0x1F) unescaped by default, which
/// breaks a `FileQueueRecord` carrying one in `payload`/`idempotencyKey` two
/// ways: RFC 8259 requires those bytes escaped, so the raw byte alone yields
/// JSON `fromJson`'s `glz::read` throws on for any non-trailing line (a
/// permanent `FileOfflineQueueError` on every later `open()`, per this file's
/// torn-trailing-line tolerance); worse, once the same string also contains an
/// escaped `\` or `"`, glaze's chunked writer path silently rewrites the
/// control byte as two 0x00 bytes, corrupting the payload before it ever
/// reaches disk. Mirrors `morph::wire::detail::EscapingWriteOpts` (`core/wire.hpp`)
/// exactly; duplicated here (rather than shared) so this header stays free of
/// a `core/` dependency. Escaping is lossless, so any such byte still
/// round-trips through `fromJson` unchanged.
struct EscapingWriteOpts : glz::opts {
    /// @brief Emit control bytes as `\\uXXXX` rather than raw.
    // NOLINTNEXTLINE(readability-identifier-naming) — glaze's option name, matched by name.
    bool escape_control_characters = true;
};

/// @brief Encodes @p record as JSON, escaping control bytes in `payload`/`idempotencyKey`.
inline std::string toJson(const FileQueueRecord& record) {
    std::string out;
    throwOnGlazeError(glz::write<EscapingWriteOpts{}>(record, out), out);
    return out;
}

inline FileQueueRecord fromJson(std::string_view json) {
    FileQueueRecord record{};
    // null_terminated=false: json is a caller-supplied view with no guaranteed
    // trailing '\0' — see the identical fix + rationale on morph::wire::decode
    // (wire.hpp), whose fuzz harness found the resulting heap-buffer-overflow in
    // glaze's skip_ws. glz::read_json (used elsewhere in this file) hardcodes
    // glz::opts{} and offers no way to override this, hence the explicit glz::read<>.
    static constexpr glz::opts kUnpadded{.null_terminated = false};
    throwOnGlazeError(glz::read<kUnpadded>(record, json), json);
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

    /// Every mutation is documented as a committed transaction by the time the
    /// call returns, so a failure to get the bytes down has to be raised rather
    /// than swallowed: a caller told an item was enqueued, or marked done, must
    /// not have that silently be untrue after a restart.
    void writeLine(const std::string& json) {
        std::string line = json;
        line.push_back('\n');
        if (std::fwrite(line.data(), 1, line.size(), _file) != line.size()) {
            throw std::runtime_error("FileOfflineQueue: short write to " + _path.string());
        }
        syncFile(_file, _path.string());
    }

    static void syncFile(std::FILE* file, const std::string& what) {
        if (std::fflush(file) != 0) {
            throw std::runtime_error("FileOfflineQueue: failed to flush " + what);
        }
#ifdef _WIN32
        int const syncResult = _commit(_fileno(file));
#else
        int const syncResult = ::fsync(fileno(file));
#endif
        if (syncResult != 0) {
            throw std::runtime_error("FileOfflineQueue: failed to fsync " + what);
        }
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
        auto writeRecord = [&](const detail::FileQueueRecord& record) {
            std::string outLine = detail::toJson(record);
            outLine.push_back('\n');
            if (std::fwrite(outLine.data(), 1, outLine.size(), out) != outLine.size()) {
                // Closing on the way out of a throw; nothing to report a
                // close failure to.
                // NOLINTNEXTLINE(cert-err33-c, cppcoreguidelines-owning-memory)
                std::fclose(out);
                throw std::runtime_error("FileOfflineQueue: short write during compaction of " + _path.string());
            }
        };

        for (const auto& [entryId, item] : _items) {
            writeRecord(detail::FileQueueRecord{.op = "put",
                                                .id = item.id,
                                                .payload = item.payload,
                                                .idempotencyKey = item.idempotencyKey,
                                                .attempts = item.attempts});
        }

        // Carry the id high-water mark across the rewrite. `load()` derives
        // _nextId from the ids it sees, and compaction drops every tombstone, so
        // without this the mark silently regresses to the highest *surviving*
        // id: enqueue 1 and 2, markDone(2), restart (compacts to just id 1),
        // restart again -> _nextId == 1 and the next enqueue reissues id 2, the
        // id of an item that was completed and acknowledged. That breaks the
        // "new ids never collide with an old tombstone" invariant this class
        // documents, and a stale in-flight reference to the old id 2 would then
        // silently address a different item.
        //
        // Recorded as a "done" for the mark itself rather than a new record
        // type: `load()` already raises highestId for every id it reads and
        // erasing an id that is not present is a no-op, so this needs no reader
        // change and stays readable by an older build. Emitted only when the
        // mark exceeds every surviving id -- writing "done" for an id that a
        // "put" line above just restored would delete it on the next load.
        uint64_t const maxSurviving = _items.empty() ? 0 : _items.rbegin()->first;
        if (_nextId > maxSurviving) {
            writeRecord(detail::FileQueueRecord{
                .op = "done", .id = _nextId, .payload = {}, .idempotencyKey = {}, .attempts = 0});
        }

        syncFile(out, tmp);
        // NOLINTNEXTLINE(cert-err33-c, cppcoreguidelines-owning-memory) — the data is already fsynced above
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
