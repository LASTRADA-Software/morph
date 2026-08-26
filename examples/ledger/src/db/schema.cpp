// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlMigration.hpp>

#include "ledger/db/database.hpp"

namespace ledger::db {

void configure(const std::string& connectionString) {
    Lightweight::SqlConnection::SetDefaultConnectionString(Lightweight::SqlConnectionString{connectionString});
}

void applyMigrations() {
    auto& migrations = Lightweight::SqlMigration::MigrationManager::GetInstance();
    migrations.CreateMigrationHistory();
    migrations.ApplyPendingMigrations();
}

void setup(const std::string& connectionString) {
    configure(connectionString);
    applyMigrations();
}

}  // namespace ledger::db

using namespace Lightweight::SqlColumnTypeDefinitions;
using Lightweight::SqlForeignKeyReferenceDefinition;

// Every method/type below is verified verbatim against real usage in
// examples/bank/src/db/schema.cpp, examples/bookmarks/src/db/schema.cpp,
// and examples/pastebin/src/db/schema.cpp: RequiredForeignKey(col, Type(),
// ref()) creates a NOT-NULL FK column in one call; ForeignKey(col, Type(),
// ref()) (no Required prefix) creates a nullable FK column (bank's own
// nullable `counterparty_id`); Column(name, Type()) (no Required prefix)
// creates a nullable plain column (pastebin's own `expires_at_ms`);
// CreateUniqueIndex(name, table, {cols...}) is a separate plan call, not
// chained onto CreateTableIfNotExists (bookmarks' own imported_ops table).

namespace {
constexpr auto ledgersRef() { return SqlForeignKeyReferenceDefinition{.tableName = "ledgers", .columnName = "id"}; }
constexpr auto accountsRef() { return SqlForeignKeyReferenceDefinition{.tableName = "accounts", .columnName = "id"}; }
constexpr auto transactionJournalsRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "transaction_journals", .columnName = "id"};
}
constexpr auto categoriesRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "categories", .columnName = "id"};
}
constexpr auto budgetsRef() { return SqlForeignKeyReferenceDefinition{.tableName = "budgets", .columnName = "id"}; }
}  // namespace

LIGHTWEIGHT_SQL_MIGRATION(20260819000001, "Create ledgers table") {
    plan.CreateTableIfNotExists("ledgers")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("name", Varchar(128));
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000002, "Create accounts table") {
    plan.CreateTableIfNotExists("accounts")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("ledger_id", Bigint(), ledgersRef())
        .RequiredColumn("name", Varchar(128))
        .RequiredColumn("kind", Integer())
        .RequiredColumn("currency_code", Varchar(3));
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000003, "Create transaction_journals table") {
    plan.CreateTableIfNotExists("transaction_journals")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("ledger_id", Bigint(), ledgersRef())
        .RequiredColumn("description", Varchar(256))
        .RequiredColumn("date", Bigint())          // Timestamp at rest -- epoch millis, per morph::time convention
        .Column("causal_parent_id", Varchar(64));  // nullable: NULL means "no parent" -- the entity layer wraps this
                                                   // as std::optional<SqlAnsiString<64>>, not an empty-string sentinel
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000004, "Create transaction_legs table") {
    plan.CreateTableIfNotExists("transaction_legs")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("journal_id", Bigint(), transactionJournalsRef())
        .RequiredForeignKey("account_id", Bigint(), accountsRef())
        .RequiredColumn("amount_num", Bigint())
        .RequiredColumn("amount_den", Bigint())
        .RequiredColumn("amount_dp", Integer())
        .RequiredColumn("currency_code", Varchar(3))
        .Column("foreign_amount_num", Bigint())  // nullable triple -- present only on a
        .Column("foreign_amount_den", Bigint())  // foreign-amount-pair leg (design spec §1 step 3)
        .Column("foreign_amount_dp", Integer())
        .Column("foreign_currency_code", Varchar(3));
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000005, "Create categories table") {
    plan.CreateTableIfNotExists("categories")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("ledger_id", Bigint(), ledgersRef())
        .RequiredColumn("name", Varchar(128));
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000006, "Create budgets table") {
    plan.CreateTableIfNotExists("budgets")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("ledger_id", Bigint(), ledgersRef())
        .RequiredColumn("name", Varchar(128))
        .RequiredForeignKey("category_id", Bigint(), categoriesRef());
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000007, "Create budget_limits table") {
    plan.CreateTableIfNotExists("budget_limits")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("budget_id", Bigint(), budgetsRef())
        .RequiredColumn("month", Varchar(7))  // "YYYY-MM"
        .RequiredColumn("limit_num", Bigint())
        .RequiredColumn("limit_den", Bigint())
        .RequiredColumn("limit_dp", Integer())
        .RequiredColumn("currency_code", Varchar(3));
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000008, "Create rules table") {
    plan.CreateTableIfNotExists("rules")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("ledger_id", Bigint(), ledgersRef())
        .RequiredColumn("trigger", Integer())
        .RequiredColumn("match_text", Varchar(256))
        .RequiredColumn("action", Integer())
        .RequiredColumn("action_value", Varchar(256))
        .RequiredColumn("version", Integer());  // default applied at insert time (=1), not a DDL DEFAULT
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000009, "Create ledger_imported_ops table") {
    // Mirrors bookmarks::db::ImportedOpRecord's exact migration shape
    // (examples/bookmarks/src/db/schema.cpp's "Create imported_ops table",
    // design spec §8): op-id ledger for chunk-retry dedup, keyed by
    // (owner_principal, op_id).
    plan.CreateTableIfNotExists("ledger_imported_ops")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("owner_principal", Varchar(64))
        .RequiredColumn("op_id", Varchar(128))
        .RequiredColumn("applied_at_ms", Bigint());
    plan.CreateUniqueIndex("idx_ledger_imported_ops_owner_op", "ledger_imported_ops", {"owner_principal", "op_id"});
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000010, "Create ledger_imported_txn_hashes table") {
    // Cross-import duplicate detection (design spec §8) -- distinct from
    // ledger_imported_ops: this catches "same statement re-uploaded under a
    // different opId", not "same chunk retried under the same opId".
    plan.CreateTableIfNotExists("ledger_imported_txn_hashes")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("ledger_id", Bigint(), ledgersRef())
        .RequiredColumn("hash", Varchar(64));
    plan.CreateUniqueIndex("idx_ledger_imported_txn_hashes_ledger_hash", "ledger_imported_txn_hashes",
                           {"ledger_id", "hash"});
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000011, "Create ledger_report_jobs table") {
    plan.CreateTableIfNotExists("ledger_report_jobs")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("ledger_id", Bigint(), ledgersRef())
        .RequiredColumn("job_id", Varchar(64))
        .RequiredColumn("kind", Integer())
        .RequiredColumn("status", Integer())
        .Column("result_json", NVarchar(0))  // nullable, unbounded -- absent until the job completes; NVarchar(0) is
                                             // the ladder-wide "unbounded text" convention (IMPLEMENTATION.md rule 3,
                                             // never Text() -- see pastebin's own `content` column)
        .RequiredColumn("created_at_ms", Bigint());
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000012, "Add category_id to accounts") {
    // First ALTER TABLE migration in this codebase (Task 10): a nullable FK
    // column added to an already-created table, rather than a column
    // declared as part of CREATE TABLE. AlterTable(std::string_view) returns
    // a SqlAlterTableQueryBuilder (Lightweight/SqlQuery/Migrate.hpp);
    // AddNotRequiredForeignKeyColumn(columnName, columnType, referencedColumn)
    // is its nullable-FK-column-add method, verified directly against that
    // header.
    plan.AlterTable("accounts").AddNotRequiredForeignKeyColumn("category_id", Bigint(), categoriesRef());
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000013, "Create ledger_applied_ops table") {
    // Exactly-once ledger (Task 11b): one row per (ledger, opId), storing
    // the full serialized GetLedgerResult the original StoreTransaction
    // call produced. Mirrors kanban::db's own board_applied_ops migration
    // shape exactly (ladder-kanban-impl:examples/kanban/src/db/schema.cpp).
    plan.CreateTableIfNotExists("ledger_applied_ops")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("ledger_id", Bigint(), ledgersRef())
        .RequiredColumn("op_id", Varchar(128))
        .RequiredColumn("result_json", NVarchar(0))
        .RequiredColumn("created_at_ms", Bigint());
    plan.CreateUniqueIndex("idx_ledger_applied_ops_ledger_op", "ledger_applied_ops", {"ledger_id", "op_id"});
}

LIGHTWEIGHT_SQL_MIGRATION(20260819000014, "Store SubmitReport params with the job row") {
    // The job row has to be self-describing now that the aggregation no
    // longer runs inside SubmitReport's own call frame (morph#160): the
    // params used to be decoded on the caller's thread and captured into the
    // posted lambda, so nothing needed to persist them. With the run moved
    // to ledger::app::App's runner -- possibly in a different process, and
    // certainly after a restart -- the row is the only record of what was
    // asked for.
    //
    // Nullable (AddNotRequiredColumn, not AddColumn) because SQLite cannot
    // add a NOT NULL column to a table that may already hold rows without a
    // default value. Every row this rung writes populates it; std::nullopt
    // means "submitted before this column existed", which
    // decodeMonthlyParams already treats as the all-time fallback.
    plan.AlterTable("ledger_report_jobs").AddNotRequiredColumn("params_json", NVarchar(0));
}
