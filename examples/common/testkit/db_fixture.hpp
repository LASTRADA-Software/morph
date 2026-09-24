// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlMigration.hpp>
#include <Lightweight/SqlSchema.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "db/pool_transaction_audit.hpp"

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
        try {
            dropAllTables(stmt);
        } catch (const std::exception& error) {
            clearAndReport(error.what());  // never returns
        } catch (...) {
            clearAndReport("a non-std::exception was thrown");  // never returns
        }
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

    /// @brief The on-disk file an ODBC connection string names, resolved to an
    ///        absolute path so a report naming it can be acted on from any
    ///        directory.
    ///
    /// Pure and public for `computeConnectionString`'s reason: the failure
    /// report below is the fixture's whole contribution when the database
    /// turns out to be unusable, and a report is worth what its worst
    /// component is. This is the component that turns `morph_ladder_test.db` —
    /// a relative name, resolved against whatever working directory ctest
    /// chose — into something the reader can actually delete.
    ///
    /// @param connectionString An ODBC connection string, e.g.
    ///        `DRIVER=SQLite3;Database=morph_ladder_test.db;Timeout=5000`.
    /// @return The absolute path of the `Database=` token's value; that
    ///         token's raw value if it cannot be made absolute; or an empty
    ///         string when there is no `Database=` token at all (a
    ///         server-hosted DSN names no file).
    [[nodiscard]] static std::string databaseFileOf(std::string_view connectionString) {
        constexpr std::string_view marker = "Database=";
        const auto markerAt = connectionString.find(marker);
        if (markerAt == std::string_view::npos) {
            return {};
        }
        auto value = connectionString.substr(markerAt + marker.size());
        if (const auto end = value.find(';'); end != std::string_view::npos) {
            value = value.substr(0, end);
        }
        if (value.empty()) {
            return {};
        }
        std::error_code failure;
        const auto resolved = std::filesystem::absolute(std::filesystem::path{value}, failure);
        if (failure) {
            return std::string{value};
        }
        return resolved.string();
    }

private:
    /// @brief One row of SQLite's own `sqlite_master` catalogue: a table, and
    ///        the DDL that created it.
    struct TableSchema {
        std::string name;  ///< The table's name.
        std::string sql;   ///< Its `CREATE TABLE` statement, or empty.
    };

    /// @brief The connection string this process is using, read from the
    ///        environment exactly once.
    ///
    /// One `std::getenv` for the whole fixture, cached: `ensureConnectionConfigured`
    /// applies it and the failure report below quotes it, and two independent
    /// reads could disagree if anything in the process called `setenv` between
    /// them — which would put one database in the report and another on the
    /// connection.
    ///
    /// @return The string `computeConnectionString` returned for this
    ///         process's `ODBC_CONNECTION_STRING`.
    [[nodiscard]] static const std::string& activeConnectionString() {
        // concurrency-mt-unsafe: `std::getenv` is flagged because a concurrent
        // `setenv` would race it. This runs once, inside a function-local
        // static's initialiser, before any ladder test has started a thread --
        // and reading it once here is what removes the second read the report
        // path would otherwise make.
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        static const std::string applied = computeConnectionString(std::getenv("ODBC_CONNECTION_STRING"));
        return applied;
    }

    /// @brief Points Lightweight's default connection at the connection
    ///        string `computeConnectionString` computes, exactly once per
    ///        process. All the interesting logic (env value set vs. not)
    ///        lives in that function above; this applies the result and has
    ///        no branch of its own left to miss.
    static void ensureConnectionConfigured() {
        static const bool once = [] {
            // Installed here as well as in every rung's own
            // `db::setup()`/`db::configure()`, because no ladder test goes
            // through those -- this fixture points Lightweight at the test
            // database itself. Without it the audit would be live only in
            // binaries the suite never runs, which is a control that measures
            // nothing. With it, every ladder test case in the suite runs under
            // the check.
            (void)::morph::ladder::db::installPoolTransactionAudit();
            ::Lightweight::SqlConnection::SetDefaultConnectionString(
                ::Lightweight::SqlConnectionString{activeConnectionString()});
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
    ///
    /// The pragma is restored on the way out of a throw as well as on the way
    /// out of a return. It is connection state and this connection returns to
    /// Lightweight's pool, so leaking `foreign_keys = OFF` into the pool would
    /// disarm every FK constraint for whichever later test drew that
    /// connection — a far quieter defect than the one that made the sweep
    /// throw in the first place.
    ///
    /// @param stmt A statement on the shared test connection.
    static void dropAllTables(::Lightweight::SqlStatement& stmt) {
        const bool isSqlite = stmt.Connection().ServerType() == ::Lightweight::SqlServerType::SQLITE;
        if (isSqlite) {
            (void)stmt.ExecuteDirect("PRAGMA foreign_keys = OFF");
        }
        try {
            // Lightweight's own SQLite table enumeration (SqlSchema.cpp's
            // ReadAllTablesLegacy) already excludes sqlite_sequence — SQLite's
            // autoincrement bookkeeping table — before it ever reaches an
            // EventHandler, so it never appears in this list to begin with; no
            // skip of our own is needed.
            const auto tables = ::Lightweight::SqlSchema::ReadAllTables(stmt, stmt.Connection().DatabaseName());
            for (const auto& table : tables) {
                (void)stmt.ExecuteDirect("DROP TABLE IF EXISTS \"" + table.name + "\"");
            }
        } catch (...) {
            if (isSqlite) {
                (void)stmt.ExecuteDirect("PRAGMA foreign_keys = ON");
            }
            throw;
        }
        if (isSqlite) {
            (void)stmt.ExecuteDirect("PRAGMA foreign_keys = ON");
        }
    }

    /// @brief Reads every user table out of SQLite's own catalogue, with the
    ///        DDL that created it.
    ///
    /// Deliberately not `SqlSchema::ReadAllTables`: that is the function whose
    /// failure brings us here. It resolves every foreign key's target through
    /// a name map built from the tables it enumerated, so a foreign key naming
    /// a table that is no longer there reaches a `map::at` and throws
    /// `std::out_of_range("map::at")`. `sqlite_master` resolves nothing and
    /// cannot fail that way, which is exactly the property a recovery path
    /// needs.
    ///
    /// @param stmt A usable statement on the shared test connection.
    /// @return Every table except SQLite's own `sqlite_%` bookkeeping ones.
    [[nodiscard]] static std::vector<TableSchema> readSqliteTables(::Lightweight::SqlStatement& stmt) {
        std::vector<TableSchema> tables;
        auto cursor = stmt.ExecuteDirect(
            "SELECT name, COALESCE(sql, '') FROM sqlite_master "
            "WHERE type = 'table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
        while (cursor.FetchRow()) {
            tables.push_back(
                TableSchema{.name = cursor.GetColumn<std::string>(1), .sql = cursor.GetColumn<std::string>(2)});
        }
        return tables;
    }

    /// @brief The table names one table's foreign keys point at.
    /// @param stmt A usable statement on the shared test connection.
    /// @param table The table to interrogate.
    /// @return One entry per foreign key, in `PRAGMA foreign_key_list`'s order.
    [[nodiscard]] static std::vector<std::string> foreignKeyTargetsOf(::Lightweight::SqlStatement& stmt,
                                                                      const std::string& table) {
        std::vector<std::string> targets;
        auto cursor = stmt.ExecuteDirect("PRAGMA foreign_key_list(\"" + table + "\")");
        while (cursor.FetchRow()) {
            // `PRAGMA foreign_key_list`'s third column is the referenced table.
            targets.push_back(cursor.GetColumn<std::string>(3));
        }
        return targets;
    }

    /// @brief Collapses every run of whitespace in @p text to one space, and
    ///        trims the ends.
    ///
    /// SQLite hands back `CREATE TABLE` DDL exactly as it was written,
    /// newlines and all. A multi-line value inside an exception message is
    /// what Catch2's console reporter re-indents into something unreadable,
    /// so the dump puts one table on one line.
    ///
    /// @param text The DDL, as `sqlite_master` recorded it.
    /// @return The same statement on a single line.
    [[nodiscard]] static std::string oneLine(std::string_view text) {
        std::string flattened;
        flattened.reserve(text.size());
        bool pendingSpace = false;
        for (const char character : text) {
            if (std::isspace(static_cast<unsigned char>(character)) != 0) {
                pendingSpace = !flattened.empty();
                continue;
            }
            if (pendingSpace) {
                flattened.push_back(' ');
                pendingSpace = false;
            }
            flattened.push_back(character);
        }
        return flattened;
    }

    /// @brief ASCII case-insensitive equality, for matching a foreign key's
    ///        target against a table name: SQLite reports a target in the case
    ///        the DDL *wrote* it, which need not be the case the table was
    ///        created with.
    /// @param left One name.
    /// @param right The other.
    /// @return `true` when they differ only in ASCII case.
    [[nodiscard]] static bool equalsIgnoringCase(std::string_view left, std::string_view right) {
        return std::ranges::equal(left, right, [](char lhs, char rhs) {
            return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
        });
    }

    /// @brief Names every foreign key whose target table is absent — the exact
    ///        condition that makes `SqlSchema::ReadAllTables` throw.
    /// @param stmt A usable statement on the shared test connection.
    /// @param tables The catalogue `readSqliteTables` returned.
    /// @return One indented line per dangling foreign key, or an empty string
    ///         if none was found — in which case the DDL dump the caller
    ///         prints unconditionally beside this is the whole record.
    [[nodiscard]] static std::string danglingForeignKeysIn(::Lightweight::SqlStatement& stmt,
                                                           const std::vector<TableSchema>& tables) {
        std::string found;
        for (const auto& table : tables) {
            std::vector<std::string> targets;
            try {
                targets = foreignKeyTargetsOf(stmt, table.name);
            } catch (...) {
                // A driver that will not run the PRAGMA costs this line of the
                // report and nothing else: the DDL dump still names the table.
                continue;
            }
            for (const auto& target : targets) {
                const bool present = std::ranges::any_of(tables, [&target](const TableSchema& candidate) {
                    return equalsIgnoringCase(candidate.name, target);
                });
                if (!present) {
                    found += "    " + table.name + " -> " + target + " (no such table)\n";
                }
            }
        }
        return found;
    }

    /// @brief Drops every table named in @p tables, foreign keys disarmed and
    ///        restored on both the throwing and the returning path.
    /// @param stmt A usable statement on the shared test connection.
    /// @param tables The catalogue `readSqliteTables` returned.
    static void dropSqliteTables(::Lightweight::SqlStatement& stmt, const std::vector<TableSchema>& tables) {
        (void)stmt.ExecuteDirect("PRAGMA foreign_keys = OFF");
        try {
            for (const auto& table : tables) {
                (void)stmt.ExecuteDirect("DROP TABLE IF EXISTS \"" + table.name + "\"");
            }
        } catch (...) {
            (void)stmt.ExecuteDirect("PRAGMA foreign_keys = ON");
            throw;
        }
        (void)stmt.ExecuteDirect("PRAGMA foreign_keys = ON");
    }

    /// @brief Reports an unusable shared database and — on the default SQLite
    ///        file — empties it first, so that the tests after this one are
    ///        not reported against it.
    ///
    /// ── Why it both repairs *and* fails ────────────────────────────────────
    ///
    /// The database is one real file (`morph_ladder_test.db` by default),
    /// shared by every test in the binary and kept between runs by design —
    /// see this file's `@file` comment. So a state the drop sweep cannot
    /// handle is not one test's problem: it is every later test's, in this run
    /// and in every run after it, until somebody deletes a file that nothing
    /// told them about. That is the expensive half of the failure; the cheap
    /// half is a throw that says only `map::at`, attributed to the `TEST_CASE`
    /// line, naming neither the fixture, nor the file, nor the fact that the
    /// state is on disk at all.
    ///
    /// Emptying the database quietly would trade that for something worse: a
    /// schema defect that reproduces every run becomes one that reproduces
    /// once and then hides, and the evidence goes with it. So this does
    /// neither thing alone. It reads the schema out first and puts it *in the
    /// failure message* — the evidence moves from the file, where nobody looks,
    /// into the run's log, where everybody does — then empties the database,
    /// then throws. The run goes red exactly once, at whichever test
    /// constructed the first fixture; the tests after it construct their
    /// fixtures against an empty database and stand or fall on their own code;
    /// and the next run is clean unless something is actively recreating the
    /// bad state, which the message names as the finding worth filing.
    ///
    /// Nothing is destroyed that the fixture was not about to destroy anyway:
    /// `dropAllTables` drops every table on every single construction. What is
    /// lost is the *forensic* copy only, and that is precisely what the dump
    /// preserves.
    ///
    /// Not attempted when `ODBC_CONNECTION_STRING` is set: a deliberately
    /// overridden database is not this fixture's to empty, and the catalogue
    /// query here is SQLite's own. That path reports and stops.
    ///
    /// @param cause What the drop sweep threw, as text.
    [[noreturn]] static void clearAndReport(std::string_view cause) {
        const std::string& connectionString = activeConnectionString();
        // "Was it overridden?" is read off the result rather than off a second
        // `std::getenv`: `computeConnectionString(nullptr)` *is* the default,
        // so anything else came from `ODBC_CONNECTION_STRING`, and there is
        // then no way for the check and the connection to disagree.
        const bool overridden = connectionString != computeConnectionString(nullptr);
        const std::string file = databaseFileOf(connectionString);
        const std::string named = file.empty() ? std::string{"  the database file"} : ("  " + file);

        // Every literal line below stays inside ~66 columns. Catch2's console
        // reporter re-wraps an exception message to the terminal width and
        // indents each continuation, so a line that overflows comes out
        // broken mid-token -- which is how a copy-pasteable path stops being
        // one. The paths themselves can still overflow; nothing here can fix
        // that, and everything else is kept short so that they are the only
        // thing that ever does.
        std::string report =
            "\n"
            "DbFixture could not prepare the shared ladder test database.\n"
            "This is NOT a failure of the test case named above: the throw\n"
            "came out of the fixture's constructor, before the test body ran.\n"
            "\n";
        report += "  error:      ";
        report += cause;
        report += "\n  connection: " + connectionString + "\n";
        if (!file.empty()) {
            report += "  file:       " + file + "\n";
        }
        report +=
            "\n"
            "That database is a real file, shared by every test in this\n"
            "binary and kept between runs by design, so a state the drop\n"
            "sweep cannot handle outlives the run that created it. `map::at`\n"
            "is Lightweight's SqlSchema::ReadAllTables failing to resolve a\n"
            "foreign key whose target table is missing.\n";

        if (overridden) {
            report +=
                "\n"
                "Left untouched: ODBC_CONNECTION_STRING is set, so this\n"
                "database is not the fixture's own to empty.\n"
                "\n"
                "Remedy: clear its schema by hand, or unset\n"
                "ODBC_CONNECTION_STRING to fall back to the default SQLite\n"
                "file.\n";
            throw std::runtime_error{report};
        }

        try {
            ::Lightweight::SqlStatement stmt;
            const auto tables = readSqliteTables(stmt);
            const std::string dangling = danglingForeignKeysIn(stmt, tables);
            if (!dangling.empty()) {
                report += "\n  dangling foreign keys:\n" + dangling;
            }
            report +=
                "\n"
                "The schema that was there, of which this message is now the\n"
                "only record:\n";
            for (const auto& table : tables) {
                report +=
                    "    " + table.name + ": " + (table.sql.empty() ? "<no DDL recorded>" : oneLine(table.sql)) + "\n";
            }
            dropSqliteTables(stmt, tables);
            report +=
                "\n"
                "Every table above has now been dropped, so the tests after\n"
                "this one run against an empty database and are not failures\n"
                "of their own code.\n"
                "\n"
                "Remedy: re-run. If this fires again, the run itself is\n"
                "recreating the bad state rather than inheriting it -- file\n"
                "that, and delete\n";
            report += named;
            report += "\nby hand before investigating.\n";
        } catch (const std::exception& recoveryError) {
            report += "\nEmptying it failed too (";
            report += recoveryError.what();
            report += ").\n\nRemedy: delete\n";
            report += named;
            report += "\nand re-run.\n";
        }
        throw std::runtime_error{report};
    }
};

}  // namespace morph::ladder::testkit
