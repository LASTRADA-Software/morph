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
    /// @throws std::runtime_error if the file cannot be opened.
    /// @throws SerializationError if an existing file at @p path has a malformed
    ///         *interior* line (a malformed trailing line is tolerated — see
    ///         `entries()`).
    explicit FileActionLog(std::filesystem::path path) : _path{std::move(path)} {
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
        for (const auto& existing : entries()) {
            if (!existing.idempotencyKey.empty()) {
                _seenIdempotencyKeys.insert(existing.idempotencyKey);
            }
        }
        _file = std::fopen(_path.string().c_str(), "a");
        if (_file == nullptr) {
            throw std::runtime_error("FileActionLog: failed to open " + _path.string());
        }
    }

    /// @brief Closes the underlying file.
    ///
    /// `_file` is always non-null here: the constructor either finishes with a
    /// valid handle or throws before completing (in which case this destructor
    /// never runs), and copy/move are deleted, so there is no path that could
    /// null it out afterwards.
    // NOLINTNEXTLINE(cert-err33-c) — destructor context, can't propagate errors
    ~FileActionLog() override { std::fclose(_file); }

    FileActionLog(const FileActionLog&) = delete;
    FileActionLog& operator=(const FileActionLog&) = delete;
    FileActionLog(FileActionLog&&) = delete;
    FileActionLog& operator=(FileActionLog&&) = delete;

    /// @brief Appends @p entry as one JSON line. Buffered until `flush()`. Thread-safe.
    /// @param entry Entry to append; `seq` is overwritten regardless of the input value.
    void append(LogEntry entry) override {
        std::scoped_lock const lock{_mtx};
        if (!entry.idempotencyKey.empty() && !_seenIdempotencyKeys.insert(entry.idempotencyKey).second) {
            return;  // already durably recorded; a re-relayed duplicate is a safe no-op
        }
        entry.seq = ++_nextSeq;
        auto line = toJson(entry);
        line.push_back('\n');
        // NOLINTNEXTLINE(cert-err33-c) — append-only; fwrite errors checked via subsequent fsync
        std::fwrite(line.data(), 1, line.size(), _file);
    }

    /// @brief Flushes stdio's buffer, then fsyncs the file descriptor. Thread-safe.
    void flush() override {
        std::scoped_lock const lock{_mtx};
        // NOLINTNEXTLINE(cert-err33-c) — errors logged by caller after flush
        std::fflush(_file);
#ifdef _WIN32
        _commit(_fileno(_file));
#else
        ::fsync(fileno(_file));
#endif
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
    /// @throws std::runtime_error if renaming to @p sealedPath fails — in that
    ///         case the *original* active file is reopened in place (still
    ///         holding every entry recorded before the call, so no data is
    ///         lost; the rotation simply did not happen) — or if reopening the
    ///         active path afterward fails outright (only possible if the
    ///         rename itself succeeded and the original path's directory then
    ///         became unwritable).
    void rotate(const std::filesystem::path& sealedPath) {
        std::scoped_lock const lock{_mtx};
        // Same durability steps as flush()'s body, inlined here because
        // flush() itself takes _mtx and this is already under it.
        std::fflush(_file);
#ifdef _WIN32
        _commit(_fileno(_file));
#else
        ::fsync(fileno(_file));
#endif
        std::fclose(_file);
        _file = nullptr;

        std::error_code ec;
        std::filesystem::rename(_path, sealedPath, ec);

        // Reopen the active path regardless of the rename's outcome: on
        // success this creates a fresh empty file; on failure it reopens the
        // same pre-rotation file (still holding every prior entry), so a
        // failed rotation never leaves the log unusable.
        _file = std::fopen(_path.string().c_str(), "a");
        if (_file == nullptr) {
            throw std::runtime_error("FileActionLog::rotate: failed to reopen " + _path.string() + " after " +
                                     (ec ? "a failed" : "a successful") + " rename to " + sealedPath.string());
        }
        if (ec) {
            throw std::runtime_error("FileActionLog::rotate: failed to rename " + _path.string() + " to " +
                                     sealedPath.string() + ": " + ec.message());
        }
    }

private:
    std::filesystem::path _path;
    std::FILE* _file = nullptr;
    mutable std::mutex _mtx;
    uint64_t _nextSeq{0};
    std::unordered_set<std::string> _seenIdempotencyKeys;
};

}  // namespace morph::journal
