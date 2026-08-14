// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "testkit/db_busy_fixture.hpp"
#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlMigration.hpp>

#include <chrono>
#include <cstdlib>
#include <string>

// Not an anonymous namespace: reflection-cpp's `DataMapper` reflects on this
// struct via `Reflection::detail::External<T>`, which requires `T` to have
// external linkage — see test_db_fixture.cpp's identical comment on
// `LadderTestkitProbe` for the full explanation.
namespace ladder_testkit_busy_probe {

struct BusyProbe {
    static constexpr std::string_view TableName = "busy_fixture_probe";

    Lightweight::Field<uint64_t, Lightweight::PrimaryKey::AutoAssign> id;
    Lightweight::Field<std::string> label;
};

}  // namespace ladder_testkit_busy_probe

using ladder_testkit_busy_probe::BusyProbe;

LIGHTWEIGHT_SQL_MIGRATION(2, "busy_fixture_probe: create probe table")
{
    plan.CreateTable("busy_fixture_probe")
        .PrimaryKeyWithAutoIncrement("id")
        .Column("label", Lightweight::SqlColumnTypeDefinitions::Varchar{ 64 });
}

namespace {

/// @brief Same database `DbFixture` just migrated, but with a short
///        `Timeout=` — see db_busy_fixture.hpp's doc comment for why this
///        has to be set *at connect time*, in the connection string itself,
///        rather than via a later `PRAGMA busy_timeout` (which only shortens
///        SQLite's own per-attempt busy handler, not the sqliteodbc
///        driver's own outer retry ceiling — captured once at connect and
///        never re-read from the live connection afterward).
///
/// Derived from the process's actual active connection string (rather than
/// a hard-coded literal) so this stays correct if `ODBC_CONNECTION_STRING`
/// ever points somewhere other than `DbFixture`'s own SQLite-file default.
[[nodiscard]] std::string shortTimeoutConnectionString()
{
    std::string connStr =
        morph::ladder::testkit::DbFixture::computeConnectionString(std::getenv("ODBC_CONNECTION_STRING"));
    static constexpr std::string_view key = "Timeout=";
    if (auto const pos = connStr.find(key); pos != std::string::npos) {
        auto const valueStart = pos + key.size();
        auto valueEnd = connStr.find(';', valueStart);
        if (valueEnd == std::string::npos) {
            valueEnd = connStr.size();
        }
        connStr.replace(valueStart, valueEnd - valueStart, "200");
    } else {
        connStr += ";Timeout=200";
    }
    return connStr;
}

}  // namespace

TEST_CASE("DbBusyFixture forces a genuine SQLITE_BUSY on a concurrent write to the same table",
          "[ladder][testkit][db][busy]")
{
    morph::ladder::testkit::DbFixture fixture;
    {
        Lightweight::DataMapper mapper;
        BusyProbe row;
        row.label = "seed";
        mapper.Create(row);
    }

    morph::ladder::testkit::DbBusyFixture busy{ "busy_fixture_probe" };

    Lightweight::DataMapper mapper{ Lightweight::SqlConnectionString{ shortTimeoutConnectionString() } };
    // Lightweight::SqlConnection::PostConnect() unconditionally issues
    // `PRAGMA busy_timeout = 60000` for every SQLite connection right after
    // connect, which *does* win over whatever the connection string's
    // `Timeout=` set moments earlier for SQLite's own internal busy handler
    // (confirmed empirically: last PRAGMA busy_timeout call wins). Re-issue
    // it here, short, so the handler governing each individual retry attempt
    // is short too -- both this AND shortTimeoutConnectionString()'s short
    // `Timeout=` are required together (confirmed empirically): the
    // connection string alone shortens only the driver's outer retry
    // ceiling, which a single 60s-bounded inner attempt already blows past
    // before that ceiling is ever checked; the PRAGMA alone shortens only
    // the inner attempts, leaving the outer ceiling (5000ms by
    // DbFixture::computeConnectionString's own default) as the effective
    // total bound. Together, both bounds are short, and the racy write below
    // fails within a few hundred milliseconds.
    (void) Lightweight::SqlStatement{ mapper.Connection() }.ExecuteDirect("PRAGMA busy_timeout = 200");

    BusyProbe row;
    row.label = "should collide";
    auto const start = std::chrono::steady_clock::now();
    REQUIRE_THROWS_WITH(mapper.Create(row), Catch::Matchers::ContainsSubstring("database is locked"));
    auto const elapsed = std::chrono::steady_clock::now() - start;
    // Must fail fast, not after minutes -- otherwise this "test" would just
    // be a very slow way to prove the same thing (observed without the
    // combined override above: tens of seconds, occasionally exceeding even
    // the ladder_common_tests suite's 120s ctest TIMEOUT budget for a single
    // test case).
    REQUIRE(elapsed < std::chrono::seconds{ 5 });
}
