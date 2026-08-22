// SPDX-License-Identifier: Apache-2.0
#include "ledger/db/database.hpp"  // pulls in schema.cpp's registrations via linkage
#include "ledger/db/ledger_entity.hpp"

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

TEST_CASE("AccountRecord round-trips through the ledgers/accounts tables", "[ledger][db]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;

    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    REQUIRE(ledgerRow.id.Value() != 0);

    ledger::db::AccountRecord accountRow;
    accountRow.ledger = ledgerRow;  // BelongsTo assignment: the whole parent record, per
                                     // polls::db::OptionRecord's real usage (opt.poll = poll;),
                                     // never a raw .SetKey(...) call
    accountRow.name = "Checking";
    accountRow.kind = 0;  // AccountKind::Asset
    accountRow.currencyCode = "USD";
    mapper.Create(accountRow);
    REQUIRE(accountRow.id.Value() != 0);

    auto loaded = mapper.Query<ledger::db::AccountRecord>()
                      .Where(::Lightweight::FieldNameOf<&ledger::db::AccountRecord::ledger>, "=", ledgerRow.id.Value())
                      .All();
    REQUIRE(loaded.size() == 1);
    CHECK(loaded.front().name.Value() == "Checking");
}
