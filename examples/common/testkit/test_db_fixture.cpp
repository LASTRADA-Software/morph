// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlMigration.hpp>
#include <catch2/catch_test_macros.hpp>

#include "testkit/db_fixture.hpp"

// Not an anonymous namespace: reflection-cpp's `DataMapper` reflects on this
// struct via `Reflection::detail::External<T>`, which requires `T` to have
// external linkage — a type declared inside an unnamed namespace has internal
// linkage and fails to compile (`used but not defined in this translation
// unit, and cannot be defined in any other translation unit because its type
// does not have linkage`). Lightweight's own reflection-backed test fixtures
// hit the same constraint and use a named namespace instead (see
// `Lightweight/src/tests/MigrationReflectionTests.cpp`'s `ReflectionTests`);
// this mirrors that, scoped to this test file only by the uncommon name.
namespace ladder_testkit_probe {

struct LadderTestkitProbe {
    // Reflection's default table name is the (unqualified) struct name, i.e.
    // "LadderTestkitProbe" — explicit here so DataMapper targets the same
    // "ladder_testkit_probe" table the migration below creates.
    static constexpr std::string_view TableName = "ladder_testkit_probe";

    Lightweight::Field<uint64_t, Lightweight::PrimaryKey::AutoAssign> id;
    Lightweight::Field<std::string> label;
};

}  // namespace ladder_testkit_probe

using ladder_testkit_probe::LadderTestkitProbe;

LIGHTWEIGHT_SQL_MIGRATION(1, "ladder_testkit_probe: create probe table") {
    plan.CreateTable("ladder_testkit_probe")
        .PrimaryKeyWithAutoIncrement("id")
        .Column("label", Lightweight::SqlColumnTypeDefinitions::Varchar{64});
}

TEST_CASE("DbFixture resets the shared database: a row from a prior fixture is gone", "[ladder][testkit][db]") {
    {
        morph::ladder::testkit::DbFixture fixture;
        Lightweight::DataMapper mapper;
        LadderTestkitProbe row;
        row.label = "left-over-from-first-fixture";
        mapper.Create(row);
    }
    // A fresh fixture drops+recreates the table — the row above must not survive.
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<LadderTestkitProbe>().All();
    REQUIRE(rows.empty());
}

TEST_CASE("DbFixture applies pending migrations so a registered table exists and is writable",
          "[ladder][testkit][db]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    LadderTestkitProbe row;
    row.label = "probe";
    mapper.Create(row);
    auto rows = mapper.Query<LadderTestkitProbe>().All();
    REQUIRE(rows.size() == 1);
    REQUIRE(rows.front().label.Value() == "probe");
}

// ensureConnectionConfigured() applies its result behind a `static const`
// guard that runs exactly once per *process*, so no test can ever be first
// to observe a particular ODBC_CONNECTION_STRING value once some earlier
// test has already forced the default-SQLite path. computeConnectionString
// takes the raw env value as a parameter instead, so it's directly testable
// without a process boundary — see db_fixture.hpp's comment on it.
TEST_CASE("DbFixture::computeConnectionString falls back to the default SQLite file when unset",
          "[ladder][testkit][db]") {
    REQUIRE(morph::ladder::testkit::DbFixture::computeConnectionString(nullptr) ==
            "DRIVER=SQLite3;Database=morph_ladder_test.db;Timeout=5000");
    REQUIRE(morph::ladder::testkit::DbFixture::computeConnectionString("") ==
            "DRIVER=SQLite3;Database=morph_ladder_test.db;Timeout=5000");
}

TEST_CASE("DbFixture::computeConnectionString uses ODBC_CONNECTION_STRING verbatim when set",
          "[ladder][testkit][db]") {
    REQUIRE(morph::ladder::testkit::DbFixture::computeConnectionString("DRIVER=PostgreSQL;Database=whatever") ==
            "DRIVER=PostgreSQL;Database=whatever");
}

TEST_CASE("DbFixture's table-drop sweep is unaffected by SQLite's own sqlite_sequence bookkeeping table",
          "[ladder][testkit][db]") {
    {
        morph::ladder::testkit::DbFixture fixture;
        // Lightweight's PrimaryKeyWithAutoIncrement() emits a plain SQLite
        // rowid-alias `INTEGER PRIMARY KEY` (no sqlite_sequence involved) —
        // the probe table above never triggers this. The literal
        // `AUTOINCREMENT` keyword is what makes SQLite create and maintain
        // its own `sqlite_sequence` bookkeeping table, so force that here.
        Lightweight::SqlStatement stmt;
        (void)stmt.ExecuteDirect("CREATE TABLE ladder_autoincrement_probe (id INTEGER PRIMARY KEY AUTOINCREMENT)");
        (void)stmt.ExecuteDirect("INSERT INTO ladder_autoincrement_probe DEFAULT VALUES");
    }
    // A fresh fixture's drop sweep runs with sqlite_sequence now present in
    // the database (created as a side effect above) — this must not throw
    // (Lightweight's own ReadAllTables never surfaces sqlite_sequence as a
    // table to drop in the first place — see db_fixture.hpp's comment on
    // dropAllTables), and the migrated probe table must still come back
    // clean.
    REQUIRE_NOTHROW([] { morph::ladder::testkit::DbFixture fixture; }());

    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    REQUIRE(mapper.Query<LadderTestkitProbe>().All().empty());
}
