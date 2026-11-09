// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlMigration.hpp>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "testkit/db_fixture.hpp"

/// @file
/// A database of its own for a test of a data migration: rows are written in an older schema, and the newer
/// migrations are then applied over them, which the shared test database cannot do because `DbFixture` applies every
/// migration at once.

namespace lims::test {

/// @brief Points Lightweight's default connection at a fresh probe database migrated up to one timestamp, and back at
///        the shared test database when it goes out of scope.
///
/// Construct it after a `DbFixture`, which has already configured the shared connection this restores.
class MigrationProbe {
public:
    /// @param lastApplied The newest migration to apply now; the later ones wait for `applyRest()`.
    explicit MigrationProbe(std::uint64_t lastApplied) {
        removeFile();
        ::Lightweight::SqlConnection::SetDefaultConnectionString(
            ::Lightweight::SqlConnectionString{std::string{"DRIVER=SQLite3;Database="} + kFile});
        auto& migrations = ::Lightweight::SqlMigration::MigrationManager::GetInstance();
        migrations.CloseDataMapper();
        migrations.CreateMigrationHistory();
        migrations.ApplyPendingMigrationsUpTo(::Lightweight::SqlMigration::MigrationTimestamp{lastApplied});
        migrations.CloseDataMapper();
    }

    MigrationProbe(const MigrationProbe&) = delete;
    MigrationProbe& operator=(const MigrationProbe&) = delete;
    MigrationProbe(MigrationProbe&&) = delete;
    MigrationProbe& operator=(MigrationProbe&&) = delete;

    ~MigrationProbe() {
        ::Lightweight::SqlMigration::MigrationManager::GetInstance().CloseDataMapper();
        ::Lightweight::SqlConnection::SetDefaultConnectionString(
            ::Lightweight::SqlConnectionString{::morph::ladder::testkit::DbFixture::computeConnectionString(
                std::getenv("ODBC_CONNECTION_STRING"))});  // NOLINT(concurrency-mt-unsafe): nothing sets it
        removeFile();
    }

    /// @brief Applies every migration newer than the one the probe stopped at.
    void applyRest() {
        auto& migrations = ::Lightweight::SqlMigration::MigrationManager::GetInstance();
        migrations.ApplyPendingMigrations();
        migrations.CloseDataMapper();
    }

private:
    static constexpr char const* kFile = "lims_migration_probe.db";

    /// @brief Deletes the probe file; a file still open elsewhere is left for the next probe to replace.
    static void removeFile() {
        std::error_code ignored;
        std::filesystem::remove(kFile, ignored);
    }
};

}  // namespace lims::test
