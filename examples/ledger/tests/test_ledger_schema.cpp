// SPDX-License-Identifier: Apache-2.0
#include "ledger/db/database.hpp"  // pulls in schema.cpp's registrations via linkage

#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlSchema.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("ledger schema migrations create every expected table", "[ledger][db]") {
    morph::ladder::testkit::DbFixture fixture;  // configures the connection + applies migrations

    Lightweight::SqlStatement stmt;
    const auto tables = Lightweight::SqlSchema::ReadAllTables(stmt, stmt.Connection().DatabaseName());
    auto hasTable = [&](std::string_view name) {
        return std::ranges::any_of(tables, [&](const auto& t) { return t.name == name; });
    };
    CHECK(hasTable("ledgers"));
    CHECK(hasTable("accounts"));
    CHECK(hasTable("transaction_journals"));
    CHECK(hasTable("transaction_legs"));
    CHECK(hasTable("categories"));
    CHECK(hasTable("budgets"));
    CHECK(hasTable("budget_limits"));
    CHECK(hasTable("rules"));
    CHECK(hasTable("ledger_imported_ops"));
    CHECK(hasTable("ledger_imported_txn_hashes"));
    CHECK(hasTable("ledger_report_jobs"));
}
