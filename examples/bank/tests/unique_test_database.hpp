// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <format>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

/// @file
/// A SQLite database file that belongs to one process and no other.
///
/// `catch_discover_tests()` registers **one ctest case per `TEST_CASE`**, and
/// ctest runs each of those as its own process. Every bank test binary used to
/// name its database by a fixed path under `temp_directory_path()`, and each
/// process deleted and re-migrated that file on the way in — which is correct
/// under a serial `ctest` and nothing else. Under `ctest -j`, 21 processes
/// unlinked and re-created one file concurrently and 21 of 21 cases failed with
/// `HY000 (10) - [SQLite]disk I/O error (10)` (morph#682).
///
/// The remedy the ladder took for the same hazard is `RESOURCE_LOCK` (see
/// `cmake/morph_add_rung.cmake`), which serialises the cases instead. Bank does
/// not need to pay that: nothing here shares state *between* cases — each one
/// already started from an empty schema, and tests isolate themselves by owner
/// principal — so giving each process a path of its own restores correctness
/// without giving up the parallelism.

namespace bank::testing {

namespace detail {

/// @brief Creates a directory under the system temp directory that no other
///        process holds.
///
/// `std::filesystem::create_directory` reports whether *this* call created the
/// directory, and the underlying `mkdir`/`CreateDirectoryW` is atomic against
/// other processes, so a `true` return is an exclusive claim — which a
/// "generate a name, then check whether it exists" scheme would not be.
///
/// @return The path of the freshly created, exclusively owned directory.
/// @throws std::runtime_error if no candidate name could be claimed.
[[nodiscard]] inline std::filesystem::path createExclusiveTempDirectory() {
    std::random_device entropy;
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    std::error_code lastError;
    for (int attempt = 0; attempt < 64; ++attempt) {
        const auto token = (static_cast<std::uint64_t>(entropy()) << 32U) | static_cast<std::uint64_t>(entropy());
        const std::filesystem::path candidate = base / std::format("morph_bank_tests-{:016x}", token);
        std::error_code err;
        if (std::filesystem::create_directory(candidate, err)) {
            return candidate;
        }
        lastError = err;
    }
    throw std::runtime_error("bank tests: could not create a private database directory under " + base.string() +
                             ": " + lastError.message());
}

/// @brief Owns the private directory for the lifetime of the process and
///        removes it on the way out.
class PrivateDatabase {
public:
    /// @brief Claims a directory of this process's own and names a database
    ///        file inside it.
    PrivateDatabase()
        : _directory(createExclusiveTempDirectory()),
          _connection("DRIVER=SQLite3;Database=" + (_directory / "bank.db").string()) {}

    PrivateDatabase(const PrivateDatabase&) = delete;
    PrivateDatabase(PrivateDatabase&&) = delete;
    PrivateDatabase& operator=(const PrivateDatabase&) = delete;
    PrivateDatabase& operator=(PrivateDatabase&&) = delete;

    /// @brief Deletes the directory and everything SQLite left in it.
    ///
    /// Best effort: a process killed outright (a sanitizer `halt_on_error`
    /// abort, say) leaves the directory behind, which is a stale temp
    /// directory and not a failed run.
    ~PrivateDatabase() {
        std::error_code err;
        std::filesystem::remove_all(_directory, err);
    }

    /// @brief The ODBC connection string for the private database file.
    /// @return A connection string naming a path no other process uses.
    [[nodiscard]] const std::string& connection() const noexcept { return _connection; }

private:
    std::filesystem::path _directory;
    std::string _connection;
};

}  // namespace detail

/// @brief The ODBC connection string for this process's own SQLite file.
///
/// Stable for the life of the process and different in every other process, so
/// two ctest cases running concurrently cannot touch the same file. The file
/// itself is not created here — the caller's first connection does that, and
/// the containing directory is already empty.
///
/// @return The connection string, owned by a function-local static.
[[nodiscard]] inline const std::string& uniqueDatabaseConnection() {
    static const detail::PrivateDatabase database;
    return database.connection();
}

}  // namespace bank::testing
