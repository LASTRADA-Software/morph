// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlMigration.hpp>

using Lightweight::SqlForeignKeyReferenceDefinition;
using namespace Lightweight::SqlColumnTypeDefinitions;

// The macro names a struct and a static object whose constructor registers the migration, so neither can be const,
// local to an anonymous namespace, or free of a throwing initializer.
// NOLINTBEGIN(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)
LIGHTWEIGHT_SQL_MIGRATION(20261110000003, "Create ledger_accounts and ledger_periods") {
    plan.CreateTableIfNotExists("ledger_accounts")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("code", Varchar(16))
        .RequiredColumn("name", Varchar(128))
        .RequiredColumn("type", Integer());
    plan.CreateUniqueIndex("idx_ledger_accounts_company_code", "ledger_accounts", {"company_id", "code"});
    plan.CreateTableIfNotExists("ledger_periods")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("start_ms", Bigint())
        .RequiredColumn("state", Integer());
    plan.CreateUniqueIndex("idx_ledger_periods_company_start", "ledger_periods", {"company_id", "start_ms"});
}
LIGHTWEIGHT_SQL_MIGRATION(20261110000004, "Create ledger_entries, their lines, posting sources and period balances") {
    plan.CreateTableIfNotExists("ledger_entries")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredForeignKey("period_id", Bigint(),
                            SqlForeignKeyReferenceDefinition{.tableName = "ledger_periods", .columnName = "id"})
        .RequiredColumn("date_ms", Bigint())
        .RequiredColumn("state", Integer())
        .RequiredColumn("source_key", Varchar(128))
        .RequiredColumn("source_reference", Varchar(128))
        .RequiredColumn("memo", Varchar(255))
        .RequiredColumn("currency", Integer())
        .RequiredColumn("rate_num", Bigint())
        .RequiredColumn("rate_den", Bigint())
        .Column("reverses_id", Bigint())
        .Column("reversed_by_id", Bigint())
        .RequiredColumn("created_by", Varchar(64))
        .RequiredColumn("posted_by", Varchar(64));
    plan.CreateTableIfNotExists("ledger_entry_lines")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("entry_id", Bigint(),
                            SqlForeignKeyReferenceDefinition{.tableName = "ledger_entries", .columnName = "id"})
        .RequiredColumn("position", Integer())
        .RequiredForeignKey("account_id", Bigint(),
                            SqlForeignKeyReferenceDefinition{.tableName = "ledger_accounts", .columnName = "id"})
        .RequiredColumn("side", Integer())
        .RequiredColumn("amount_num", Bigint())
        .RequiredColumn("amount_den", Bigint())
        .RequiredColumn("company_num", Bigint())
        .RequiredColumn("company_den", Bigint());
    plan.CreateUniqueIndex("idx_ledger_entry_lines_entry_position", "ledger_entry_lines", {"entry_id", "position"});
    plan.CreateTableIfNotExists("ledger_posting_sources")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("scope", Varchar(32))
        .RequiredColumn("source_key", Varchar(128))
        .RequiredColumn("entry", Varchar(32));
    plan.CreateTableIfNotExists("ledger_period_balances")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("period_id", Bigint(),
                            SqlForeignKeyReferenceDefinition{.tableName = "ledger_periods", .columnName = "id"})
        .RequiredForeignKey("account_id", Bigint(),
                            SqlForeignKeyReferenceDefinition{.tableName = "ledger_accounts", .columnName = "id"})
        .RequiredColumn("debit_num", Bigint())
        .RequiredColumn("debit_den", Bigint())
        .RequiredColumn("credit_num", Bigint())
        .RequiredColumn("credit_den", Bigint());
}
// NOLINTEND(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)
