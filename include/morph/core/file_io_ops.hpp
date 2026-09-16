// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <cerrno>
#include <cstdint>
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

/// @brief Repeats @p operation while it fails with `EINTR`.
///
/// A syscall interrupted by a signal returns a negative result with `errno` set
/// to `EINTR`, having done nothing; the caller is expected to reissue it. Both
/// halves of the directory fsync below need that, and inlining the loop at each
/// put two copies of a two-branch retry in a header where no test can reach
/// either -- a signal arriving mid-`open` is not something a unit test can
/// arrange. Factored out, the loop is ordinary code a test drives with a
/// callable that fails once.
///
/// @tparam Operation Callable returning a POSIX-style `int`: negative on
///         failure, with `errno` set.
/// @param operation The syscall to issue, and reissue while it reports `EINTR`.
/// @return @p operation's first result that is not an `EINTR` failure.
template <typename Operation>
int retryOnEintr(Operation operation) {
    int result = operation();
    while (result < 0 && errno == EINTR) {
        result = operation();
    }
    return result;
}

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
    /// @return `0` on success, otherwise the `errno` that caused the failure.
    ///         Returning the code rather than a bare `-1` is what lets
    ///         `classifyDirectorySync()` tell "this platform cannot do it" from
    ///         "this platform tried and failed", which the call sites treat very
    ///         differently. A custom (test) sink is free to return any nonzero
    ///         value; anything it does not recognise counts as a real failure.
    std::function<int(const std::filesystem::path& dir)> syncPath = [](const std::filesystem::path& dir) {
#ifdef _WIN32
        (void)dir;
        return 0;
#else
        std::filesystem::path const resolved = dir.empty() ? std::filesystem::path{"."} : dir;
        // O_CLOEXEC: this fd exists for one fsync, and must not survive into a
        // child across a concurrent fork/exec.
        //
        // POSIX ::open is variadic only to make the third `mode` argument
        // optional; it is not passed here (no O_CREAT), and there is no
        // non-variadic spelling of the syscall to prefer.
        int const dirFd = retryOnEintr([&resolved] {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            return ::open(resolved.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        });
        if (dirFd < 0) {
            return errno;
        }
        int const result = retryOnEintr([dirFd] { return ::fsync(dirFd); });
        int const failure = result == 0 ? 0 : errno;
        ::close(dirFd);
        return failure;
#endif
    };
};

/// @brief What a nonzero `FileIoOps::syncPath` result means for durability.
enum class DirectorySync : std::uint8_t {
    /// @brief The directory entry is on stable storage.
    durable,
    /// @brief This platform or mount cannot fsync a directory, or will not let
    ///        this process open one for reading. Nothing is wrong with the
    ///        data; the extra durability simply is not available here.
    unsupported,
    /// @brief A real I/O failure. The directory entry may not survive a crash.
    failed,
};

/// @brief Classifies a `FileIoOps::syncPath` return value.
///
/// A directory fsync needs a **read** handle on the directory, which is a
/// strictly stronger permission than writing a file inside it. A mode-0300
/// spool directory (write + search, no read) — an ordinary hardened layout, and
/// what an SELinux/AppArmor write-without-read policy produces — lets
/// `fopen(path, "a")` succeed while `open(dir, O_RDONLY|O_DIRECTORY)` fails
/// `EACCES`. Treating that as fatal made `FileActionLog`, `FileOfflineQueue`
/// and `SqliteOfflineQueue` unconstructible on layouts where they had worked
/// for years, for the sake of a durability refinement.
///
/// `fsync` on a directory fd is also simply unimplemented on several mounts —
/// `EINVAL`/`ENOSYS`/`ENOTSUP` from sshfs, gvfs, Docker Desktop's gRPC-FUSE,
/// WSL drvfs/9p under `/mnt/c`, and some overlay and network filesystems.
///
/// None of those is a durability *failure*; they are a durability *ceiling*.
/// They warn and continue. `EIO` and anything else unrecognised is a genuine
/// failure and still throws.
///
/// @param syncPathResult The value `FileIoOps::syncPath` returned.
/// @return Which of the three cases @p syncPathResult falls into.
[[nodiscard]] inline DirectorySync classifyDirectorySync(int syncPathResult) noexcept {
    if (syncPathResult == 0) {
        return DirectorySync::durable;
    }
    // Not guarded on `_WIN32`. The real `syncPath` is a documented no-op there
    // and returns 0, so this only ever sees an injected value on Windows -- but
    // classifying the *value* rather than the platform is what keeps a test
    // that injects EACCES meaning the same thing everywhere. (It did not: with
    // the switch compiled out under _WIN32, every nonzero value fell through to
    // `failed`, and the unsupported-fsync test failed on the clangcl-debug leg
    // alone.) Every constant below is in <cerrno> on the Microsoft CRT too.
    switch (syncPathResult) {
        case EACCES:
        case EPERM:
        case EINVAL:
        case ENOSYS:
        case ENOTDIR:
#ifdef ENOTSUP
        case ENOTSUP:
#endif
#if defined(EOPNOTSUPP) && (!defined(ENOTSUP) || EOPNOTSUPP != ENOTSUP)
        case EOPNOTSUPP:
#endif
            return DirectorySync::unsupported;
        default:
            break;
    }
    return DirectorySync::failed;
}

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

/// @brief Positions a just-opened append-mode stream at end-of-file.
///
/// C11 leaves an append-mode stream's *initial* position implementation-defined,
/// and the implementations genuinely differ. glibc seeks to end at `fopen`, so
/// `ftell` immediately reports the file's size. The Microsoft CRT documents the
/// opposite in as many words -- "If no I/O operation has yet occurred on a file
/// opened for appending, the file position is the beginning of the file" -- and
/// musl behaves the same way. Every write still lands at the end regardless;
/// what differs is only what `ftell` reports before the first one.
///
/// That difference is load-bearing here, because `rollBackShortWrite` takes the
/// pre-write `wideFtell` as the offset to roll back to. On the Microsoft CRT the
/// *first* write after any open would report offset 0, and a short write there
/// would roll the file back to zero bytes -- destroying an entire audit journal
/// or queue backlog rather than removing one partial record. Linux CI cannot
/// catch it: glibc makes the bug unreachable, and the `cl-debug`/`cl-release`/
/// `clangcl-*` legs where it is reachable have no test that short-writes before
/// writing successfully first.
///
/// Calling this once after each successful append-mode `fopen` normalises the
/// position across CRTs, so `wideFtell` means the same thing everywhere.
///
/// @param file Stream to position; a null handle is ignored, so this can be
///             called before the caller's own open-failure check.
inline void positionAtEnd(std::FILE* file) noexcept {
    if (file != nullptr) {
        // NOLINTNEXTLINE(cert-err33-c)
        std::fseek(file, 0, SEEK_END);
    }
}

/// @brief Rolls @p file/@p path back to @p offsetBeforeWrite bytes after a
///        short write, best-effort (morph#530).
///
/// `resizeFile` truncates the file by path, not through @p file's own file
/// descriptor, so @p file's buffered stdio position (what a later `ftell`
/// returns) is never resynced by the truncation itself. Left alone, that
/// stale position would be captured as the *next* write's "offset before
/// write" — and if that next write also fails short, the rollback would
/// truncate to the wrong, stale offset instead of the file's real current
/// size. The trailing `fseek` closes that gap.
///
/// @par Why the flush must succeed before anything is truncated
/// @p offsetBeforeWrite is a *stream* position, and callers buffer: a
/// `FileActionLog::append()` is documented as buffered-until-`flush()`, so
/// `ftell` routinely runs ahead of the file's real on-disk size. Truncating to
/// a stream offset that exceeds the on-disk size does not shrink the file —
/// `std::filesystem::resize_file` **grows** it, padding with NUL bytes, and any
/// later flush then appends the buffered record *after* that padding. The
/// result is a NUL-bearing interior line that the caller's own reader rejects
/// for the life of the file: precisely the bricking morph#530 exists to
/// prevent, manufactured by the rollback meant to prevent it. (Measured:
/// `ftell`=30 against an on-disk size of 10, `resize_file(30)` yielding a
/// 30-byte file, and a final 50-byte file of data + 20 NULs + the flushed
/// record.)
///
/// So the flush comes first and its result is checked. If it fails — which on
/// a full disk is the *likely* case, since that is what made the write short —
/// nothing is truncated at all: the on-disk contents are unknowable, the
/// buffered bytes cannot portably be discarded, and a later flush would
/// re-append them after whatever this call had removed. The file is left as it
/// is for `repairTornTail()` to trim at the next open. The truncation is also
/// clamped to the file's actual size, so it can only ever shrink.
///
/// `clearerr` is called first because a real short write latches the stream's
/// error indicator, and `fseek` does not clear it. glibc keeps accepting writes
/// on an error-flagged stream, but the standard does not require that, and on a
/// CRT that refuses them one transient `ENOSPC` would leave the log permanently
/// throwing for the life of the process.
///
/// @param ioOps             Injectable I/O primitives to use for the flush/resize.
/// @param file              Open stdio handle the short write happened on.
/// @param path              Path @p file was opened from.
/// @param offsetBeforeWrite `wideFtell(file)` as captured immediately before
///                          the short write; negative (a failed query) skips
///                          the resize, since there is no offset to roll back to.
inline void rollBackShortWrite(FileIoOps& ioOps, std::FILE* file, const std::filesystem::path& path,
                               long long offsetBeforeWrite) {
    std::clearerr(file);
    if (ioOps.fflush(file) != 0) {
        // Cannot reason about what reached disk, and cannot drop what did not.
        // Leaving the file untouched is strictly safer than truncating to an
        // offset that may exceed its real size -- see the note above.
        return;
    }
    if (offsetBeforeWrite < 0) {
        return;  // no offset to roll back to
    }
    std::error_code errorCode;
    auto const onDisk = std::filesystem::file_size(path, errorCode);
    auto target = static_cast<std::uintmax_t>(offsetBeforeWrite);
    if (!errorCode) {
        target = std::min(target, onDisk);  // only ever shrink
    }
    ioOps.resizeFile(path, target, errorCode);
    // Resync `file`'s stdio position with the (now shorter) file on disk. Safe
    // to flush against here, and only here: the buffer was emptied above, so
    // this cannot re-append anything the resize just removed. The return value
    // is deliberately unchecked -- this is already the failure path, and a
    // failed reposition leaves the caller no better recovery than the throw it
    // is about to do anyway.
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
/// Called from `morph::journal::FileActionLog`'s constructor, which is where
/// this scan has always lived; lifted out of that class so the logic has one
/// home. `morph::offline::FileOfflineQueue` deliberately does **not** call it --
/// see the note in its constructor for why running it there both failed to fix
/// the case it was added for and cost a documented invariant.
///
/// @warning It trims by newline, not by parse, so a *complete* final record
/// whose only missing byte is the terminating newline is discarded along with a
/// genuinely torn one. Callers that accept externally-appended records need to
/// know that before adopting it.
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
