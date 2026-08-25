// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
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
};

}  // namespace morph::core
