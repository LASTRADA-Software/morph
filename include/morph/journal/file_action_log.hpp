// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "../core/file_io_ops.hpp"
#include "../core/logger.hpp"
#include "action_log.hpp"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace morph::journal {

/// @brief Append-only, newline-delimited-JSON `IActionLog` backed by a local file.
///
/// Each entry is written as one `journal::toJson`-encoded line. `flush()` flushes
/// the C stdio buffer and then issues a real fsync (POSIX `fsync` / Windows
/// `_commit`), so a crash immediately after `flush()` returns cannot lose data —
/// this is the "local file" sink from the original request, and the natural
/// target for `SessionLog::checkpoint()` at a "Save" action. Dedups `append()` on
/// a non-empty `LogEntry::idempotencyKey`, rebuilding the seen-key set from disk
/// at open time — see `IActionLog`'s class docs and the constructor's docs.
///
/// @par Process-local `seq`
/// Like `InMemoryActionLog`, `seq` is assigned fresh per process instance — it
/// does not resume from the highest `seq` already on disk if the file is
/// reopened. Entries remain correctly ordered on disk regardless (append-only),
/// but `seq` alone is not a cross-restart unique key; use `entries()`' natural
/// file order for that.
///
/// @par Thread safety
/// All public methods are thread-safe (guarded by an internal mutex). Safe to
/// use from multiple threads within one process; not safe for multiple
/// processes to append to the same path concurrently.
///
/// @par Rotation
/// `rotate()` seals the active file (rename to a host-chosen path) and
/// reopens a fresh, empty one at the original path — the seam a host uses to
/// implement its own retention policy. See `rotate()`'s own docs.
class FileActionLog : public IActionLog {
public:
    /// @brief Opens (creating if necessary) @p path for appending.
    ///
    /// Also rebuilds the `idempotencyKey` dedup set (see `append()`) from
    /// whatever is already on disk at @p path — an O(n) scan of the existing
    /// file's contents, paid once here, not on every `append()`.
    /// @param path File to append entries to.
    /// @param ioOps Injectable file-I/O primitives; defaults to the real
    ///        syscalls. Test-only seam — see `morph::core::FileIoOps`'s own
    ///        docs — for forcing the failure branches that need a real
    ///        OS-level I/O error to reach.
    /// @throws std::runtime_error if the file cannot be opened.
    /// @throws SerializationError if an existing file at @p path has a malformed
    ///         *interior* line (a malformed trailing line is tolerated — see
    ///         `entries()`).
    explicit FileActionLog(std::filesystem::path path, ::morph::core::FileIoOps ioOps = {})
        : _path{std::move(path)}, _io{std::move(ioOps)} {
        // Discard a torn trailing record before anything else touches the file.
        // The file is opened "a", so the next append() would otherwise start
        // writing at the exact byte the truncated JSON stopped at, with no
        // separating newline: the two would merge into one line that swallows
        // the new entry, and once a *further* append pushes that merged line out
        // of trailing position, entries()' tolerance no longer applies and it
        // throws -- from this very constructor, leaving the journal permanently
        // unopenable. FileOfflineQueue heals the same damage in compact(); this
        // is FileActionLog's equivalent.
        repairTornTail();

        // Rebuild the idempotencyKey dedup set from whatever is already durably on
        // disk, so a re-relayed outbox row is recognised even after this process
        // restarts (not just within one FileActionLog instance's lifetime). Reuses
        // entries()'s existing torn-trailing-line tolerance; a malformed *interior*
        // line still throws SerializationError here, same as calling entries()
        // directly would (see entries()'s docs).
        //
        // Done before _file is opened: entries() reads through its own
        // std::ifstream, independent of _file, so a throw here leaves no file
        // handle open. Opening _file first and rebuilding the dedup set after
        // would leak it on this path -- the constructor never completes, so
        // the destructor never runs to close what fopen() already opened.
        // entries() is deliberately the final overrider here: copy/move are
        // deleted and nothing derives from this class, so there is no more-
        // derived override for the call to bypass.
        // NOLINTNEXTLINE(clang-analyzer-optin.cplusplus.VirtualCall)
        for (const auto& existing : entries()) {
            if (!existing.idempotencyKey.empty()) {
                _seenIdempotencyKeys.insert(existing.idempotencyKey);
            }
        }
        _file = _io.fopen(_path.string(), "a");
        if (_file == nullptr) {
            throw std::runtime_error("FileActionLog: failed to open " + _path.string());
        }
    }

    /// @brief Closes the underlying file.
    ///
    /// `_file` is normally non-null: the constructor either finishes with a
    /// valid handle or throws before completing (in which case this destructor
    /// never runs), and copy/move are deleted. The one exception is a `rotate()`
    /// whose reopen failed after the rename succeeded — it throws with the
    /// handle left null rather than dangling, so the null check here is
    /// reachable and load-bearing, not defensive noise.
    // NOLINTNEXTLINE(cert-err33-c) — destructor context, can't propagate errors
    ~FileActionLog() override {
        if (_file != nullptr) {
            // Destructor context: there is nothing to propagate a failure to.
            // NOLINTNEXTLINE(cert-err33-c, cppcoreguidelines-owning-memory)
            std::fclose(_file);
        }
    }

    FileActionLog(const FileActionLog&) = delete;
    FileActionLog& operator=(const FileActionLog&) = delete;
    FileActionLog(FileActionLog&&) = delete;
    FileActionLog& operator=(FileActionLog&&) = delete;

    /// @brief Appends @p entry as one JSON line. Buffered until `flush()`. Thread-safe.
    ///
    /// The entry's `idempotencyKey` is only remembered as *seen* once `flush()`
    /// confirms it reached the disk (see `flush()`), so a write that fails is
    /// still retryable. Recording the key up front — before the bytes were
    /// durable — silently deduplicated the retry of a row that was never
    /// written, turning a transient I/O error into permanent data loss.
    ///
    /// @param entry Entry to append; `seq` is overwritten regardless of the input value.
    /// @throws std::runtime_error if the write fails, or if the log has no open
    ///         file (only reachable after a `rotate()` that failed to reopen).
    void append(LogEntry entry) override {
        std::scoped_lock const lock{_mtx};
        requireOpen("append");
        if (!entry.idempotencyKey.empty() && (_seenIdempotencyKeys.contains(entry.idempotencyKey) ||
                                              _unflushedIdempotencyKeys.contains(entry.idempotencyKey))) {
            return;  // already recorded; a re-relayed duplicate is a safe no-op
        }
        entry.seq = ++_nextSeq;
        auto line = toJson(entry);
        line.push_back('\n');
        if (_io.fwrite(line.data(), line.size(), _file) != line.size()) {
            throw std::runtime_error("FileActionLog::append: short write to " + _path.string());
        }
        if (!entry.idempotencyKey.empty()) {
            _unflushedIdempotencyKeys.insert(std::move(entry.idempotencyKey));
        }
    }

    /// @brief Flushes stdio's buffer, then fsyncs the file descriptor. Thread-safe.
    ///
    /// Throws rather than returning a status because the interface is
    /// `void`-returning and the failure is not one a caller may ignore:
    /// `OutboxRelay::relay()` calls `markRelayed()` immediately after this, and
    /// on a silent failure would record rows as relayed in the model's own store
    /// while nothing reached the durable sink — the rows are then gone from the
    /// outbox and absent from the log. Throwing aborts `relay()` before
    /// `markRelayed`, leaving the batch to be retried.
    ///
    /// On failure the keys buffered since the last successful flush are
    /// forgotten, so a retry writes them again instead of being deduplicated
    /// away. A duplicated audit row is recoverable; a dropped one is not.
    ///
    /// @throws std::runtime_error if the buffer flush or the fsync fails, or if
    ///         the log has no open file.
    void flush() override {
        std::scoped_lock const lock{_mtx};
        requireOpen("flush");
        if (_io.fflush(_file) != 0) {
            _unflushedIdempotencyKeys.clear();
            throw std::runtime_error("FileActionLog::flush: failed to flush " + _path.string());
        }
        if (_io.fsync(_file) != 0) {
            _unflushedIdempotencyKeys.clear();
            throw std::runtime_error("FileActionLog::flush: failed to fsync " + _path.string());
        }
        // Durable now: promote this window's keys so they survive as dedup state.
        for (const auto& key : _unflushedIdempotencyKeys) {
            _seenIdempotencyKeys.insert(key);
        }
        _unflushedIdempotencyKeys.clear();
    }

    /// @brief Re-reads the file from disk and decodes every line. Thread-safe.
    ///
    /// Reads whatever is currently on disk, including anything written but not
    /// yet `flush()`ed if the platform's stdio buffering has already handed it
    /// to the OS — callers that need a guaranteed-durable view should `flush()`
    /// first.
    /// @param entityKey If non-empty, restricts the result to that entity's entries.
    /// @return Matching entries, in on-disk (append) order.
    [[nodiscard]] std::vector<LogEntry> entries(std::string_view entityKey = {}) const override {
        std::scoped_lock const lock{_mtx};
        std::ifstream in{_path};
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) {
                lines.push_back(line);
            }
        }
        std::vector<LogEntry> out;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            LogEntry entry;
            try {
                entry = fromJson(lines[i]);
            } catch (const std::exception& exc) {
                // A crash between `append`'s `fwrite` and the next flush can leave
                // a truncated final line. Tolerate exactly that — skip a malformed
                // *trailing* line so the rest of the log stays readable — but a
                // malformed line mid-file is genuine corruption and is re-thrown.
                if (i + 1 == lines.size()) {
                    ::morph::log::logWarn("FileActionLog: skipping malformed trailing line in " + _path.string() +
                                          ": " + std::string{exc.what()});
                    break;
                }
                throw;
            }
            if (entityKey.empty() || entry.entityKey == entityKey) {
                out.push_back(std::move(entry));
            }
        }
        return out;
    }

    /// @brief Seals the active file and reopens a fresh, empty one at the same path.
    ///
    /// Flushes (`fflush` + `fsync`/`_commit`) and closes the current active
    /// file, renames it to @p sealedPath, then reopens a fresh, empty active
    /// file at the original path. Thread-safe — guarded by the same mutex as
    /// `append()`/`flush()`/`entries()`, so no in-flight `append()` call is
    /// ever split across the sealed and the new active file. `entries()`
    /// keeps reading only the (post-rotation, empty) active file; composing
    /// full history across a rotation is a host-side recipe (concatenate each
    /// sealed segment's `entries()` oldest-to-newest, then the active file's),
    /// not a new API.
    ///
    /// @par Crash safety
    /// The rename is a single atomic filesystem operation. A crash before it
    /// completes leaves the pre-rotation active file untouched, as if
    /// `rotate()` was never called; a crash after leaves the sealed file plus
    /// a freshly recreated, empty active file. Either way no line is ever torn
    /// across the two files, so the existing torn-line rule keeps applying
    /// independently per file.
    ///
    /// @param sealedPath Destination path for the sealed segment. Platform
    ///        `rename` semantics decide what happens if a filesystem entry
    ///        already exists there (POSIX silently replaces it) — pass a path
    ///        that does not collide with an existing segment.
    /// @throws std::runtime_error if the pre-rotation flush/fsync fails (raised
    ///         before anything is closed or renamed, so no segment is ever
    ///         sealed around entries that never reached the disk); if renaming
    ///         to @p sealedPath fails — in that case the *original* active file
    ///         is reopened in place (still holding every entry recorded before
    ///         the call, so no data is lost; the rotation simply did not
    ///         happen) — or if reopening the active path afterward fails
    ///         outright (only possible if the rename itself succeeded and the
    ///         original path's directory then became unwritable). After that
    ///         last case the log has no open file: `append()`, `flush()`, and a
    ///         further `rotate()` all throw with that diagnosis rather than
    ///         dereferencing a null handle, and destruction is still safe.
    void rotate(const std::filesystem::path& sealedPath) {
        std::scoped_lock const lock{_mtx};
        requireOpen("rotate");
        // Same durability steps as flush()'s body, inlined here because
        // flush() itself takes _mtx and this is already under it. A failure is
        // raised before the file is closed and renamed, so a segment is never
        // sealed around entries that never reached the disk.
        if (_io.fflush(_file) != 0) {
            throw std::runtime_error("FileActionLog::rotate: failed to flush " + _path.string());
        }
        if (_io.fsync(_file) != 0) {
            throw std::runtime_error("FileActionLog::rotate: failed to fsync " + _path.string());
        }
        // Everything buffered is now durable in the segment about to be sealed.
        for (const auto& key : _unflushedIdempotencyKeys) {
            _seenIdempotencyKeys.insert(key);
        }
        _unflushedIdempotencyKeys.clear();
        // NOLINTNEXTLINE(cert-err33-c) — the data is already fsynced above
        std::fclose(_file);
        _file = nullptr;

        std::error_code renameError;
        std::filesystem::rename(_path, sealedPath, renameError);

        // Reopen the active path regardless of the rename's outcome: on
        // success this creates a fresh empty file; on failure it reopens the
        // same pre-rotation file (still holding every prior entry), so a
        // failed rotation never leaves the log unusable.
        _file = _io.fopen(_path.string(), "a");
        if (_file == nullptr) {
            throw std::runtime_error("FileActionLog::rotate: failed to reopen " + _path.string() + " after " +
                                     (renameError ? "a failed" : "a successful") + " rename to " +
                                     sealedPath.string());
        }
        if (renameError) {
            throw std::runtime_error("FileActionLog::rotate: failed to rename " + _path.string() + " to " +
                                     sealedPath.string() + ": " + renameError.message());
        }
    }

private:
    /// Throws if no file handle is open. Reachable only after a `rotate()` whose
    /// reopen failed: that path deliberately leaves `_file` null rather than
    /// dangling, so every entry point has to say so instead of dereferencing it.
    void requireOpen(std::string_view what) const {
        if (_file == nullptr) {
            throw std::runtime_error("FileActionLog::" + std::string{what} + ": " + _path.string() +
                                     " is not open (a previous rotate() failed to reopen it)");
        }
    }

    /// Truncates any bytes following the last newline in the file.
    ///
    /// A crash between `append()`'s `fwrite` and the next `flush()` can leave a
    /// partial record at the end. Because every complete record is written
    /// newline-terminated in a single `fwrite`, whatever follows the final
    /// newline is by construction an incomplete record and never a whole one —
    /// which makes discarding it safe: it can only remove bytes that no reader
    /// could ever have decoded. Complete records, including a malformed
    /// *interior* line, are left exactly as they are; diagnosing those stays
    /// `entries()`' job.
    void repairTornTail() const {
        std::error_code errorCode;
        auto const size = std::filesystem::file_size(_path, errorCode);
        if (errorCode || size == 0) {
            return;  // absent or empty: nothing to repair
        }
        if (!_io.canOpenForRead(_path)) {
            return;
        }
        std::ifstream input{_path, std::ios::binary};
        std::uintmax_t intactEnd = 0;
        std::uintmax_t offset = 0;
        std::string line;
        while (std::getline(input, line)) {
            offset += line.size();
            if (input.eof()) {
                break;  // no trailing newline: this line is the torn remainder
            }
            ++offset;  // the '\n' getline consumed
            intactEnd = offset;
        }
        if (intactEnd == size) {
            return;
        }
        _io.resizeFile(_path, intactEnd, errorCode);
        if (errorCode) {
            ::morph::log::logWarn("FileActionLog: could not truncate torn trailing record in " + _path.string() +
                                  ": " + errorCode.message());
            return;
        }
        ::morph::log::logWarn("FileActionLog: discarded " + std::to_string(size - intactEnd) +
                              " byte(s) of a torn trailing record in " + _path.string());
    }

    std::filesystem::path _path;
    ::morph::core::FileIoOps _io;
    std::FILE* _file = nullptr;
    mutable std::mutex _mtx;
    uint64_t _nextSeq{0};
    /// Keys confirmed durable by a successful `flush()`.
    std::unordered_set<std::string> _seenIdempotencyKeys;
    /// Keys written since the last successful `flush()`. Promoted into
    /// `_seenIdempotencyKeys` on success, discarded on failure so the
    /// corresponding rows stay retryable.
    std::unordered_set<std::string> _unflushedIdempotencyKeys;
};

}  // namespace morph::journal
