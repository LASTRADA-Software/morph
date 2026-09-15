// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>

#include "logger.hpp"

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace morph::core {

/// @brief The raw file-I/O primitives `morph::journal::FileActionLog` and
///        `morph::offline::FileOfflineQueue` both call, as an injectable
///        strategy. Every member defaults to the real syscall/stdlib call it
///        stands in for, so a default-constructed `FileIoOps` is byte-for-byte
///        what both classes called directly before this seam existed — no
///        behavior change for a normal caller.
///
/// @par Why this exists
/// Both classes have several branch arms that only run when a real OS-level
/// file-I/O call fails partway through an otherwise-successful operation
/// (disk full, fd closed underneath, a permission change racing an exact
/// window). None of those are reachable from a portable unit test without
/// this seam — see `LASTRADA-Software/morph#97`, which requested exactly
/// this for `FileActionLog`; `FileOfflineQueue` has the identical gap. A
/// test constructs a `FileIoOps` whose relevant member fails on demand (or
/// on the Nth call, or forever) and passes it to the class under test;
/// every other member stays at its real default, so the rest of the class's
/// I/O behaves normally around the one injected failure.
///
/// @par Thread safety
/// `FileIoOps` itself is a plain value type with no shared state — copying
/// or moving one has ordinary value semantics. Whether the *callbacks*
/// themselves are safe to call from multiple threads concurrently is up to
/// whatever a test installs; the real default callbacks are exactly the
/// real syscalls, which already have their own well-defined thread-safety.
struct FileIoOps {
    /// @brief Writes @p size bytes from @p buffer to @p file. Mirrors `std::fwrite`.
    /// @return The number of bytes actually written; short of @p size on failure.
    std::function<std::size_t(const void* buffer, std::size_t size, std::FILE* file)> fwrite =
        [](const void* buffer, std::size_t size, std::FILE* file) { return std::fwrite(buffer, 1, size, file); };

    /// @brief Flushes @p file's stdio buffer. Mirrors `std::fflush`.
    /// @return `0` on success, nonzero on failure.
    std::function<int(std::FILE* file)> fflush = [](std::FILE* file) { return std::fflush(file); };

    /// @brief Commits @p file's contents to durable storage. POSIX `fsync` /
    ///        Windows `_commit`, resolved from @p file via `fileno`/`_fileno`.
    /// @return `0` on success, nonzero on failure.
    std::function<int(std::FILE* file)> fsync = [](std::FILE* file) {
#ifdef _WIN32
        return _commit(_fileno(file));
#else
        return ::fsync(fileno(file));
#endif
    };

    /// @brief Opens @p path in mode @p mode. Mirrors `std::fopen`.
    /// @return The open file, or `nullptr` on failure.
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) — mirrors std::fopen's own raw-owning-pointer return
    std::function<std::FILE*(const std::string& path, const char* mode)> fopen = [](const std::string& path,
                                                                                    const char* mode) {
        // NOLINTNEXTLINE(cert-err33-c) — callers check the returned handle themselves
        return std::fopen(path.c_str(), mode);
    };

    /// @brief Reports whether @p path can be opened for reading right now.
    ///        Stands in for `std::ifstream{path}`'s own open-succeeded check
    ///        (`repairTornTail()`'s `if (!input)`) — a fault-injection test has
    ///        no way to make a *real* `std::ifstream` construction fail
    ///        without actually breaking the filesystem, so this predicate is
    ///        consulted first; the real default performs the real open
    ///        `std::ifstream` itself would.
    /// @return `true` if @p path is currently readable.
    std::function<bool(const std::filesystem::path& path)> canOpenForRead = [](const std::filesystem::path& path) {
        return static_cast<bool>(std::ifstream{path});
    };

    /// @brief Truncates/extends @p path to @p newSize bytes. Mirrors
    ///        `std::filesystem::resize_file`.
    /// @param path Path to resize.
    /// @param newSize Target size, in bytes.
    /// @param errorCode Set on failure, cleared on success — same contract as
    ///        `std::filesystem::resize_file`'s own `error_code` overload.
    std::function<void(const std::filesystem::path& path, std::uintmax_t newSize, std::error_code& errorCode)>
        resizeFile = [](const std::filesystem::path& path, std::uintmax_t newSize, std::error_code& errorCode) {
            std::filesystem::resize_file(path, newSize, errorCode);
        };

    /// @brief Commits a directory's own metadata (new/renamed entries within
    ///        it) to durable storage. `fsync` on a *file* makes only that
    ///        file's data durable -- not the directory entry that names it,
    ///        so a fresh file's creation or a rename can vanish on power loss
    ///        even after the file's own contents were fsynced (morph#532).
    ///        POSIX: `open(dir, O_RDONLY|O_DIRECTORY)` + `fsync` + `close`. A
    ///        no-op on Windows, documented as such rather than faked --
    ///        `FlushFileBuffers`'s semantics for a directory handle differ
    ///        enough from POSIX `fsync` that pretending otherwise would be
    ///        misleading, and this seam's Windows story is already
    ///        file-`fsync`-only.
    /// @param dir Directory whose entries were just mutated. A `path::parent_path()`
    ///        of a bare relative filename (e.g. `"queue.ndjson"`, with no
    ///        directory component) is the *empty* path, not `"."` -- treated
    ///        here as the current directory, the same resolution the shell
    ///        and every POSIX call already give an empty path's implicit
    ///        caller, so a relative, directory-less @p path still gets its
    ///        containing directory synced instead of failing `open()` with
    ///        `ENOENT` on every call site that derives @p dir from
    ///        `parent_path()`.
    /// @return `0` on success, nonzero on failure.
    std::function<int(const std::filesystem::path& dir)> syncPath = [](const std::filesystem::path& dir) {
#ifdef _WIN32
        (void)dir;
        return 0;
#else
        std::filesystem::path const resolved = dir.empty() ? std::filesystem::path{"."} : dir;
        // POSIX ::open is variadic only to make the third `mode` argument
        // optional; it is not passed here (no O_CREAT), and there is no
        // non-variadic spelling of the syscall to prefer.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        int const dirFd = ::open(resolved.c_str(), O_RDONLY | O_DIRECTORY);
        if (dirFd < 0) {
            return -1;
        }
        int const result = ::fsync(dirFd);
        ::close(dirFd);
        return result;
#endif
    };
};

/// @brief Returns @p file's current stdio position, wide enough to represent
///        files past the ~2 GiB `std::ftell` can address through its 32-bit
///        `long` on an LLP64 platform (Windows, in every build configuration
///        this project ships — `long` stays 32-bit there even in a 64-bit
///        binary). A long-lived `FileActionLog`/`FileOfflineQueue` sink with
///        infrequent rotation can exceed that well within its lifetime; past
///        it, `std::ftell` returns `-1` (`EOVERFLOW`) on every call, which
///        `rollBackShortWrite()` already treats as "no offset to roll back
///        to" and silently skips — quietly reviving the exact torn-tail
///        merge morph#530 fixed, on every platform where this actually
///        matters, for as long as the process keeps running (the next
///        restart's `repairTornTail()` is still a backstop, but a long-lived
///        process that never restarts gets no benefit from it).
/// @param file Open stdio handle to query.
/// @return The current position, or a negative value on failure — same
///         convention as `std::ftell`.
inline long long wideFtell(std::FILE* file) {
#ifdef _WIN32
    return _ftelli64(file);
#else
    return ::ftello(file);
#endif
}

/// @brief Rolls @p file/@p path back to @p offsetBeforeWrite bytes after a
///        short write, best-effort (morph#530).
///
/// `resizeFile` truncates the file by path, not through @p file's own file
/// descriptor, so @p file's buffered stdio position (what a later `ftell`
/// returns) is never resynced by the truncation itself. Left alone, that
/// stale position would be captured as the *next* write's "offset before
/// write" — and if that next write also fails short, the rollback would
/// truncate/pad to the wrong, stale offset instead of the file's real
/// current size. The trailing `fseek` closes that gap: it forces `file`'s
/// stdio position back in sync with the (possibly just-truncated) file on
/// disk, so every subsequent `ftell` on @p file is trustworthy again
/// regardless of how many short writes happen back to back.
///
/// @param ioOps             Injectable I/O primitives to use for the flush/resize.
/// @param file              Open stdio handle the short write happened on.
/// @param path              Path @p file was opened from.
/// @param offsetBeforeWrite `wideFtell(file)` as captured immediately before
///                          the short write; negative (a failed query) skips
///                          the resize, since there is no offset to roll back to.
inline void rollBackShortWrite(FileIoOps& ioOps, std::FILE* file, const std::filesystem::path& path,
                               long long offsetBeforeWrite) {
    ioOps.fflush(file);
    if (offsetBeforeWrite >= 0) {
        std::error_code errorCode;
        ioOps.resizeFile(path, static_cast<std::uintmax_t>(offsetBeforeWrite), errorCode);
    }
    // Best-effort resync, regardless of whether the resize above ran or
    // succeeded: cheaper and safer than conditioning it on that outcome, and
    // a stream position that is merely still-correct is a harmless no-op.
    // The return value is deliberately unchecked: this is already the failure
    // path, a failed reposition leaves the caller no better recovery than the
    // throw it is about to do anyway, and repairTornTail() is the backstop.
    // NOLINTNEXTLINE(cert-err33-c)
    std::fseek(file, 0, SEEK_END);
}

/// @brief Truncates any bytes following the last newline in @p path.
///
/// A crash between a caller's `fwrite` and its next `fsync` (or a short
/// write, before morph#530's fix) can leave a partial record at the end.
/// Because every complete record is written newline-terminated in a single
/// `fwrite`, whatever follows the final newline is by construction an
/// incomplete record and never a whole one -- which makes discarding it
/// safe: it can only remove bytes that no reader could ever have decoded.
/// Complete records, including a malformed *interior* line, are left exactly
/// as they are; diagnosing those is the caller's own read path's job.
///
/// Shared between `morph::journal::FileActionLog` and
/// `morph::offline::FileOfflineQueue`'s constructors, both of which used to
/// carry an identical copy of this scan.
///
/// @param ioOps       Injectable I/O primitives to use for the read-check/resize.
/// @param path        File to check and, if needed, truncate.
/// @param logComponent Name to prefix warning log lines with (the calling
///        class's own name), so a log reader can tell which file the
///        warning is about without `path` alone disambiguating it.
inline void repairTornTail(FileIoOps& ioOps, const std::filesystem::path& path, std::string_view logComponent) {
    std::error_code errorCode;
    auto const size = std::filesystem::file_size(path, errorCode);
    if (errorCode || size == 0) {
        return;  // absent or empty: nothing to repair
    }
    if (!ioOps.canOpenForRead(path)) {
        return;
    }
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        // The probe above said readable and this open still failed (a
        // permission change or fd exhaustion landing in between). Falling
        // through would scan nothing, leave `intactEnd` at 0, and truncate
        // the whole file as if it were one torn record -- so bail instead.
        // The safety argument above holds only for bytes actually read.
        ::morph::log::logWarn(std::string{logComponent} + ": could not read " + path.string() +
                              " to check for a torn trailing record; leaving it untouched");
        return;
    }
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
    if (input.bad()) {
        // Terminated by an I/O error rather than by end-of-file, so
        // everything past `intactEnd` is unread rather than established to
        // be torn. Truncating here would discard complete, fsynced records.
        ::morph::log::logWarn(std::string{logComponent} + ": read error while checking " + path.string() +
                              " for a torn trailing record; leaving it untouched");
        return;
    }
    if (intactEnd == size) {
        return;
    }
    ioOps.resizeFile(path, intactEnd, errorCode);
    if (errorCode) {
        ::morph::log::logWarn(std::string{logComponent} + ": could not truncate torn trailing record in " +
                              path.string() + ": " + errorCode.message());
        return;
    }
    ::morph::log::logWarn(std::string{logComponent} + ": discarded " + std::to_string(size - intactEnd) +
                          " byte(s) of a torn trailing record in " + path.string());
}

}  // namespace morph::core
