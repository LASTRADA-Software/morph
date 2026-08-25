// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlMigration.hpp>
#include <Lightweight/SqlSchema.hpp>
#include <cstdlib>
#include <string>

/// @file
/// Real on-disk SQLite database, shared per test binary — mirrors
/// Lightweight's own `SqlTestFixture` (Lightweight/src/tests/Utils.hpp) and
/// examples/bank/tests/bank_test_support.hpp's `ensureDatabase()`, not a
/// per-fixture temp file. Every rung's LIGHTWEIGHT_SQL_MIGRATION-registered
/// schema (examples/IMPLEMENTATION.md rule 4) is picked up automatically:
/// MigrationManager is a process-wide singleton every linked-in schema.cpp
/// registers against at static-init time.

namespace morph::ladder::testkit {

/// @brief Drops every table in the shared on-disk test database and
///        re-applies pending migrations, for the lifetime of one fixture.
///
/// Construct one per `TEST_CASE` (matching `TEST_CASE_METHOD(SqlTestFixture,
/// ...)`'s usage in Lightweight's own suite) so every test starts from a
/// clean, real schema on the same real connection.
class DbFixture {
public:
    DbFixture() {
        ensureConnectionConfigured();
        ::Lightweight::SqlStatement stmt;
        dropAllTables(stmt);
        ::Lightweight::SqlMigration::MigrationManager::GetInstance().ApplyPendingMigrations();
    }

    DbFixture(const DbFixture&) = delete;
    DbFixture& operator=(const DbFixture&) = delete;
    DbFixture(DbFixture&&) = delete;
    DbFixture& operator=(DbFixture&&) = delete;
    ~DbFixture() = default;

public:
    /// @brief Pure decision logic behind `ensureConnectionConfigured()`,
    ///        factored out so it is directly unit-testable: that function
    ///        applies its result behind a `static const` guard that runs
    ///        exactly once per *process* (parallel binaries — not parallel
    ///        test cases within one binary — are what that guard needs to
    ///        survive; Catch2 runs sections sequentially), so no test can
    ///        ever be first to observe a particular `ODBC_CONNECTION_STRING`
    ///        value once some earlier test (or the very first `DbFixture` in
    ///        the binary) has already forced the default-SQLite path. Taking
    ///        the raw env value as a parameter instead of reading it
    ///        internally sidesteps that: a test calls this with whatever
    ///        string it likes, no process boundary required.
    /// @param envValue `ODBC_CONNECTION_STRING`'s raw value (as
    ///        `std::getenv` would return it), or `nullptr`/empty if unset.
    /// @return @p envValue verbatim if non-empty (parity with Lightweight's
    ///         own override convention, so the same ladder suite can later
    ///         run a CI leg against Postgres/MSSQL the way
    ///         `examples/LADDER.md`'s security matrix expects other rungs to
    ///         gain non-SQLite legs); otherwise a real file named
    ///         `morph_ladder_test.db` in the current working directory.
    [[nodiscard]] static std::string computeConnectionString(const char* envValue) {
        if (envValue != nullptr && *envValue != '\0') {
            return envValue;
        }
        return "DRIVER=SQLite3;Database=morph_ladder_test.db;Timeout=5000";
    }

private:
    /// @brief Points Lightweight's default connection at the connection
    ///        string `computeConnectionString` computes, exactly once per
    ///        process. All the interesting logic (env value set vs. not)
    ///        lives in that function above; this applies the result and has
    ///        no branch of its own left to miss.
    static void ensureConnectionConfigured() {
        static const bool once = [] {
            ::Lightweight::SqlConnection::SetDefaultConnectionString(
                ::Lightweight::SqlConnectionString{computeConnectionString(std::getenv("ODBC_CONNECTION_STRING"))});
            ::Lightweight::SqlMigration::MigrationManager::GetInstance().CreateMigrationHistory();
            return true;
        }();
        (void)once;
    }

    /// @brief `DROP TABLE IF EXISTS` every table currently in the database.
    ///
    /// Simplified relative to `SqlTestFixture::DropAllTablesInDatabase`
    /// (Lightweight/src/tests/Utils.hpp): that version recursively orders
    /// drops around foreign-key cycles (needed for Chinook-shaped schemas
    /// with self- and cross-references). Rung 0 has no schema of its own and
    /// no ladder rung has shipped a cyclic-FK schema yet, so this toggles
    /// SQLite's `PRAGMA foreign_keys` off for the sweep instead — correct for
    /// any acyclic schema, and simpler. If a future rung's schema is cyclic,
    /// port `SqlTestFixture`'s recursive algorithm here rather than
    /// reinventing one; note that as a one-line addition to this comment when
    /// it happens, not a silent behavior change.
    static void dropAllTables(::Lightweight::SqlStatement& stmt) {
        const bool isSqlite = stmt.Connection().ServerType() == ::Lightweight::SqlServerType::SQLITE;
        if (isSqlite) {
            (void)stmt.ExecuteDirect("PRAGMA foreign_keys = OFF");
        }
        // Lightweight's own SQLite table enumeration (SqlSchema.cpp's
        // ReadAllTablesLegacy) already excludes sqlite_sequence — SQLite's
        // autoincrement bookkeeping table — before it ever reaches an
        // EventHandler, so it never appears in this list to begin with; no
        // skip of our own is needed.
        const auto tables = ::Lightweight::SqlSchema::ReadAllTables(stmt, stmt.Connection().DatabaseName());
        for (const auto& table : tables) {
            (void)stmt.ExecuteDirect("DROP TABLE IF EXISTS \"" + table.name + "\"");
        }
        if (isSqlite) {
            (void)stmt.ExecuteDirect("PRAGMA foreign_keys = ON");
        }
    }
};

}  // namespace morph::ladder::testkit
