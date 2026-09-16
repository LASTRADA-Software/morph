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
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../core/file_io_ops.hpp"
#include "../core/logger.hpp"
#include "../core/observability.hpp"
#include "offline_queue.hpp"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace morph::offline {

/// @brief Thrown by `FileOfflineQueue` when a non-trailing line of its on-disk
///        NDJSON is malformed (via `detail::throwOnGlazeError`). Note the
///        "cannot be opened" paths throw plain `std::runtime_error`, not this
///        type — see the constructor's own exception documentation.
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
/// exactly; duplicated here (rather than shared) so this header does not pull in
/// `core/wire.hpp`'s envelope machinery for a four-line options struct (it
/// already depends on `core/file_io_ops.hpp`, `core/logger.hpp` and
/// `core/observability.hpp`, so it is this one header that is being avoided, not
/// `core/` as such). Escaping is lossless, so any such byte still
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
    /// @param ioOps Injectable file-I/O primitives; defaults to the real
    ///        syscalls. Test-only seam — see `morph::core::FileIoOps`'s own
    ///        docs — for forcing the failure branches that need a real
    ///        OS-level I/O error to reach.
    /// @param maxDepth Maximum number of pending items `enqueue()` will admit
    ///        before throwing `OfflineQueueFullError`; `std::nullopt` (the
    ///        default) means unbounded. Not persisted in the file itself — a
    ///        per-construction parameter, so a reopen must pass it again to
    ///        keep the same cap enforced.
    /// @throws FileOfflineQueueError if @p path exists but contains a
    ///         malformed non-trailing line.
    /// @throws std::runtime_error if @p path cannot be opened/rewritten.
    explicit FileOfflineQueue(std::filesystem::path path, ::morph::core::FileIoOps ioOps = {},
                              std::optional<std::size_t> maxDepth = std::nullopt)
        : _path{std::move(path)}, _io{std::move(ioOps)}, _maxDepth{maxDepth} {
        // No constructor-time repairTornTail() here, deliberately.
        //
        // An earlier revision of morph#530 called it before load(), to heal an
        // "interior merge from a doubled-up short write" that load() would
        // otherwise reject. It cannot do that: repairTornTail only trims bytes
        // after the final newline, and says so itself -- "Complete records,
        // including a malformed *interior* line, are left exactly as they are."
        // So it never fixed the case it was added for.
        //
        // It did break two things. It is the constructor's only file mutation
        // that can run *before* load() throws, which costs morph#494's
        // guarantee that a failed construction leaves the queue file
        // byte-identical (a file with both a malformed interior line and a torn
        // tail would come back truncated *and* throw). And it discards a
        // complete final record whose only missing byte is the trailing newline
        // -- which load() decodes perfectly well -- wiping the file outright
        // when that is the only line.
        //
        // What actually prevents the doubled-up short write is the rollback in
        // writeLine() below, which leaves no partial bytes for a later write to
        // merge with; load()+compact() heal an ordinary torn tail as they always
        // have. FileActionLog keeps its own long-standing call: that is
        // pre-existing behaviour there, not something this change introduced.
        load();
        compact();
        _file = _io.fopen(_path.string(), "a");
        ::morph::core::positionAtEnd(_file);
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
    [[nodiscard]] uint64_t enqueue(std::string payload) override { return enqueue(std::move(payload), {}); }

    /// @brief Appends @p payload carrying @p idempotencyKey. A non-empty key
    ///        already present on a pending item is deduplicated: the existing
    ///        item's id is returned and nothing new is written.
    /// @param payload        Serialised action to persist.
    /// @param idempotencyKey Stable dedup token; empty means "no dedup".
    /// @return The new item's id, or the existing item's id on a dedup hit.
    /// @throws OfflineQueueFullError if the queue is already at `maxDepth()`
    ///         (a dedup hit above bypasses this check and always succeeds).
    [[nodiscard]] uint64_t enqueue(std::string payload, std::string idempotencyKey) override {
        std::scoped_lock const lock{_mtx};
        if (!idempotencyKey.empty()) {
            for (const auto& [existingId, item] : _items) {
                if (item.idempotencyKey == idempotencyKey) {
                    return existingId;
                }
            }
        }
        if (_maxDepth && _items.size() >= *_maxDepth) {
            ::morph::observe::detail::emitMetric(::morph::observe::Metric::queueOverflow,
                                                 static_cast<double>(_items.size()));
            throw OfflineQueueFullError{*_maxDepth, _items.size()};
        }
        uint64_t const itemId = ++_nextId;
        QueueItem item{.id = itemId, .payload = std::move(payload), .idempotencyKey = std::move(idempotencyKey)};
        appendPut(item);
        _items.emplace(itemId, std::move(item));
        return itemId;
    }

    /// @brief Returns all pending items in ascending-id (enqueue) order.
    /// @return Snapshot of all pending items; the file itself is unchanged.
    [[nodiscard]] std::vector<QueueItem> drain() const override {
        std::scoped_lock const lock{_mtx};
        std::vector<QueueItem> out;
        out.reserve(_items.size());
        for (const auto& [id, item] : _items) {
            out.push_back(item);
        }
        return out;
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

    /// @brief Tombstones @p itemId. No-op if not found.
    /// @param itemId Id returned by the corresponding `enqueue()` call.
    void markDone(uint64_t itemId) override {
        std::scoped_lock const lock{_mtx};
        auto iter = _items.find(itemId);
        if (iter == _items.end()) {
            return;
        }
        // Durable first, then in-memory -- the same order `enqueue()` uses, and
        // for the mirror-image reason. `appendDone` throws on a short write, a
        // failed fflush or a failed fsync; erasing before it means this process
        // would never replay the item again while no tombstone reached disk, so
        // a restart resurrects it and applies it a second time. Erasing after
        // means a failure leaves the item live in both places, which replays
        // once too often at worst -- and `idempotencyKey` exists to absorb that.
        appendDone(itemId);
        _items.erase(iter);
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
        // Durable first, for the same reason as `markDone` above: a throwing
        // `appendPut` must not leave memory claiming a count that never reached
        // disk. Write from a copy so `_items` is only updated once the record is
        // durable.
        auto updated = iter->second;
        updated.attempts = attempts;
        appendPut(updated);
        iter->second.attempts = attempts;
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
        long long const offsetBeforeWrite = ::morph::core::wideFtell(_file);
        // The rollback below has to cover the *flush* too, not only a short
        // fwrite. A queue record is a few hundred bytes -- far under BUFSIZ --
        // so fwrite is a memcpy into the stdio buffer and returns the full
        // count even on a full disk; the write(2) that actually fails happens
        // inside syncFile's fflush. Wired to the short-write branch alone, an
        // ENOSPC there threw with a truncated line already on disk, at exactly
        // the offset the next writeLine resumes from and with no separating
        // newline -- the identical merge morph#530 exists to prevent, and the
        // *common* manifestation of a full disk rather than an exotic one.
        auto const rollBackAndThrow = [&](const std::string& what) {
            ::morph::core::rollBackShortWrite(_io, _file, _path, offsetBeforeWrite);
            throw std::runtime_error("FileOfflineQueue: " + what + " " + _path.string());
        };
        if (_io.fwrite(line.data(), line.size(), _file) != line.size()) {
            // The file is opened "a" (append), so a short write's partial bytes
            // sit right where the *next* writeLine would otherwise resume, with
            // no separating newline -- merging into one line load() can only
            // tolerate while it stays the trailing line, and stops being able
            // to the moment a further write pushes it into an interior position
            // (morph#530). Roll the file back to its pre-write length instead,
            // so a failed write leaves no trace at all for the next one to
            // merge with. Best-effort: this is already the failure path, and
            // when the rollback's own flush cannot complete (the disk that made
            // the write short is still full) it deliberately truncates nothing
            // -- load()'s tolerance of a torn *trailing* line, plus the
            // rewrite compact() performs on the next open, is what heals it
            // then. See `rollBackShortWrite`'s own doc comment for why the
            // flush has to succeed before anything is truncated, and why it
            // resyncs `_file`'s stdio position afterwards.
            rollBackAndThrow("short write to");
        }
        if (_io.fflush(_file) != 0) {
            rollBackAndThrow("failed to flush");
        }
        if (_io.fsync(_file) != 0) {
            // fsync failing after a successful flush means the bytes are in the
            // page cache but may not reach the platter. They are a *complete*
            // record, so rolling back is still the right call: this mutation is
            // documented as committed once it returns, and a caller told the
            // enqueue failed must not find it replayed after a restart.
            rollBackAndThrow("failed to fsync");
        }
    }

    void syncFile(std::FILE* file, const std::string& what) const {
        if (_io.fflush(file) != 0) {
            throw std::runtime_error("FileOfflineQueue: failed to flush " + what);
        }
        if (_io.fsync(file) != 0) {
            throw std::runtime_error("FileOfflineQueue: failed to fsync " + what);
        }
    }

    /// @brief Reads whatever is on disk and replays it into `_items`/`_nextId`.
    void load() {
        if (!std::filesystem::exists(_path)) {
            return;
        }
        std::ifstream in{_path};
        if (!in) {
            // The file exists (checked above) but cannot be read. Returning an
            // empty `_items` here is not "an empty queue": the constructor calls
            // compact() straight after load(), which would rewrite `_path` from
            // that empty set and destroy every pending item. Throw so the caller
            // learns the queue could not be opened, instead of being handed one
            // that silently reports no work.
            throw std::runtime_error("FileOfflineQueue: cannot read " + _path.string());
        }
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) {
                lines.push_back(line);
            }
        }
        uint64_t highestId = 0;
        if (in.bad()) {
            // A read error mid-file, not end-of-file: `lines` is a prefix of the
            // queue, and compact() would commit that prefix over the whole file.
            throw std::runtime_error("FileOfflineQueue: read error on " + _path.string());
        }
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
        std::FILE* out = _io.fopen(tmp, "w");
        if (out == nullptr) {
            throw std::runtime_error("FileOfflineQueue: failed to open " + tmp + " for compaction");
        }

        // Every exit below this point closes `out` and, unless the rename
        // committed, removes `tmp`. Before this guard existed only the
        // short-write branch cleaned up: `syncFile(out, tmp)` threw straight out
        // of compact(), leaking the handle and orphaning the temp file. That is
        // not a theoretical path -- the fault-injection test "a failing
        // fflush() during construction-time compaction throws" drives it on
        // every run, which had left 31 stray *.compact-tmp files in /tmp on the
        // machine this was found on. The leaked handle would also block the
        // unlink on Windows.
        class TempFileGuard {
        public:
            TempFileGuard(std::FILE* file, std::string path) : _file{file}, _path{std::move(path)} {}

            ~TempFileGuard() {
                if (_file != nullptr) {
                    // Unwinding already; there is nothing to report a close
                    // failure to.
                    // NOLINTNEXTLINE(cert-err33-c, cppcoreguidelines-owning-memory)
                    std::fclose(_file);
                }
                if (!_committed) {
                    std::error_code errorCode;
                    std::filesystem::remove(_path, errorCode);
                }
            }

            TempFileGuard(const TempFileGuard&) = delete;
            TempFileGuard& operator=(const TempFileGuard&) = delete;
            TempFileGuard(TempFileGuard&&) = delete;
            TempFileGuard& operator=(TempFileGuard&&) = delete;

            /// @brief Hands the handle back to the caller, which closes it.
            void releaseHandle() noexcept { _file = nullptr; }
            /// @brief Marks the temp file as renamed away, so it is not removed.
            void commit() noexcept { _committed = true; }

        private:
            std::FILE* _file;
            std::string _path;
            bool _committed = false;
        };
        TempFileGuard guard{out, tmp};

        auto writeRecord = [&](const detail::FileQueueRecord& record) {
            std::string outLine = detail::toJson(record);
            outLine.push_back('\n');
            if (_io.fwrite(outLine.data(), outLine.size(), out) != outLine.size()) {
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
        guard.releaseHandle();  // closed here; the rename needs the handle gone on Windows
        std::filesystem::rename(tmp, _path);
        guard.commit();  // `tmp` no longer exists under that name
        // The rename is a directory mutation, not a file-content one -- fsync
        // on `out` above made the compacted *data* durable, but not the
        // directory entry that now names it `_path` instead of the tmp name
        // (morph#532). Surfaced rather than swallowed, same as every other
        // fsync failure in this class; safe to throw here, since compact()
        // always runs before `_file` is opened -- nothing left dangling.
        auto const dirSync = ::morph::core::classifyDirectorySync(_io.syncPath(_path.parent_path()));
        if (dirSync == ::morph::core::DirectorySync::failed) {
            throw std::runtime_error("FileOfflineQueue: failed to fsync directory after compacting " + _path.string());
        }
        if (dirSync == ::morph::core::DirectorySync::unsupported) {
            ::morph::log::logWarn(
                "FileOfflineQueue: cannot fsync the directory containing {}; the queue's contents are still fsynced, "
                "but its directory entry is only as durable as this filesystem makes it",
                _path.string());
        }
    }

    std::filesystem::path _path;
    ::morph::core::FileIoOps _io;
    std::FILE* _file = nullptr;
    mutable std::mutex _mtx;
    std::map<uint64_t, QueueItem> _items;
    uint64_t _nextId{0};
    std::optional<std::size_t> _maxDepth;
};

}  // namespace morph::offline
