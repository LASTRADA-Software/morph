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
    //
    // The spelling is Lightweight's, not a choice of ours, and the lint's
    // suggested fix is a runtime data bug. `RecordTableNameImpl` in
    // Lightweight/Utils.hpp reads the member *by name* —
    // `if constexpr (requires { Record::TableName; })` — and otherwise falls
    // back to `Reflection::TypeNameOf<Record>`. The repository-root
    // .clang-tidy sets `readability-identifier-naming.VariableCase: camelBack`,
    // so this line is reported as `invalid case style for variable
    // 'TableName'` with `tableName` offered as the fix. That rename compiles
    // and links: the `requires` clause goes false, the `else` branch runs, and
    // this record silently maps to a table called "LadderTestkitProbe" that no
    // migration ever creates. Suppressed rather than taken (morph#702).
    //
    // Per declaration, and deliberately not a directory .clang-tidy.
    // examples/bank/include/.clang-tidy exempts the same spelling with
    // `readability-identifier-naming.VariableIgnoredRegexp: '^TableName$'`,
    // and its own reasoning is what argues against copying it here: that file
    // covers twelve declarations in a header-only tree, reached as main files
    // and nothing else. This directory is the opposite shape. It holds
    // twenty-one Catch2 translation units (the count
    // scripts/check_rung_filters.sh reads back) and exactly two of these
    // declarations, and a .clang-tidy here is resolved for every one of them —
    // including for the include/morph/** headers they reach, which *do* match
    // the root `HeaderFilterRegex` (morph#632). It also already carries a
    // .clang-tidy whose whole justification is "this finding is Catch2 idiom",
    // a claim scripts/check_rung_filters.sh re-checks against every .cpp that
    // file governs; `TableName` is ORM protocol rather than Catch2 idiom, so
    // adding it there would put a second claim into a file whose gate
    // validates only the first. Two directives subtract one check on one line
    // each and reach nothing else.
    // NOLINTNEXTLINE(readability-identifier-naming)
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
