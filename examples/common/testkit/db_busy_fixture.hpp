// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "db_fixture.hpp"

#include <Lightweight/Lightweight.hpp>

#include <format>
#include <string>

/// @file
/// The SQLITE_BUSY-provoking counterpart to `db_fault_fixture.hpp`'s
/// advisory-lock contention, which cannot fault an ordinary `DataMapper`
/// call (see `examples/TESTING.md`'s testkit section): holds a genuine,
/// uncommitted write transaction open on a second SqlConnection to the
/// shared test database, for the fixture's lifetime, so a concurrent write
/// from the code under test's own connection collides for real — no mock,
/// no simulated driver. See `DbBusyFixture`'s doc comment for the verified
/// locking recipe and test_db_busy_fixture.cpp for the observed exception
/// this produces and how the *other* connection (the one under test) must
/// shorten its own busy-timeout to fail fast.

namespace morph::ladder::testkit {

/// @brief Holds an open write transaction on @p tableName for its lifetime,
///        forcing a concurrent write from a different connection to that
///        same table to observe `SQLITE_BUSY`.
///
/// Verified empirically against the real sqliteodbc driver this repo tests
/// against:
///
/// - A plain `BEGIN` (or `Lightweight::SqlTransaction`, which only flips
///   `SQL_ATTR_AUTOCOMMIT` off via ODBC and issues no `BEGIN` of its own)
///   defers SQLite's actual lock acquisition to the connection's first
///   statement that touches data. `BEGIN IMMEDIATE`, sent as a raw
///   statement via `SqlStatement::ExecuteDirect` *before* any other
///   statement on this connection, is what forces SQLite's RESERVED write
///   lock to be taken immediately, so there is no race between this
///   constructor returning and a concurrent writer starting elsewhere. The
///   follow-up no-op `UPDATE ... SET id = id` isn't load-bearing for the
///   lock itself (`BEGIN IMMEDIATE` alone already reserves it) but exercises
///   the same code path a real write would, and gives a second, independent
///   confirmation the transaction is live.
/// - The destructor issues an explicit `ROLLBACK` rather than relying on
///   `_lockingConnection`'s own destructor to release the lock on
///   disconnect: ODBC disconnect-with-open-transaction behavior is
///   driver-defined, and an explicit release is unambiguous (the same
///   reasoning `DbFaultFixture`'s `SqlScopedLock`-based release already
///   follows).
///
/// A gotcha this fixture's own consumer must handle, *not* something this
/// class can fix on the other connection's behalf: `Lightweight::SqlConnection
/// ::PostConnect()` unconditionally issues `PRAGMA busy_timeout = 60000` on
/// every new SQLite connection, regardless of the connection string's own
/// `Timeout=` parameter (which the ODBC driver would otherwise honor, but
/// Lightweight's PRAGMA runs after connect and wins). That means a
/// concurrent write against this fixture's lock does not fail fast by
/// default — it genuinely blocks for up to 60 real seconds before SQLite
/// gives up and returns `SQLITE_BUSY`. A caller that wants the fast,
/// deterministic failure a unit test needs must re-issue `PRAGMA
/// busy_timeout = N` (a small value) directly on *its own* connection before
/// attempting the racy write (see test_db_busy_fixture.cpp) — the
/// `ODBC_CONNECTION_STRING`/`Timeout=` override this file's task brief
/// originally proposed does not work, because the PRAGMA is not derived
/// from it.
class DbBusyFixture {
  public:
    /// @param tableName Table to lock — must already exist (construct this
    ///        fixture after a `DbFixture` has applied migrations) and must
    ///        have an `id` column (every ladder entity to date does).
    explicit DbBusyFixture(std::string tableName): _tableName{ std::move(tableName) }, _lockingConnection{}
    {
        ::Lightweight::SqlStatement stmt{ _lockingConnection };
        (void) stmt.ExecuteDirect("BEGIN IMMEDIATE");
        (void) stmt.ExecuteDirect(std::format("UPDATE \"{}\" SET id = id", _tableName));
    }

    /// @brief Rolls back the held transaction explicitly — see the class
    ///        doc comment for why this doesn't rely on the connection's own
    ///        destructor instead.
    ~DbBusyFixture()
    {
        ::Lightweight::SqlStatement stmt{ _lockingConnection };
        (void) stmt.ExecuteDirect("ROLLBACK");
    }

    DbBusyFixture(const DbBusyFixture&) = delete;
    DbBusyFixture& operator=(const DbBusyFixture&) = delete;
    DbBusyFixture(DbBusyFixture&&) = delete;
    DbBusyFixture& operator=(DbBusyFixture&&) = delete;

  private:
    std::string _tableName;
    ::Lightweight::SqlConnection _lockingConnection;
};

}  // namespace morph::ladder::testkit
