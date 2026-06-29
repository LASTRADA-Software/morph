// SPDX-License-Identifier: Apache-2.0

#include "bank/db/database.hpp"

#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlMigration.hpp>

namespace bank::db {

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

}  // namespace bank::db

// ─── Schema migrations ───────────────────────────────────────────────────────
// Each LIGHTWEIGHT_SQL_MIGRATION auto-registers with the MigrationManager at
// static-init time; linking this TU into the binary makes the schema known.
// Timestamps are monotonically increasing and group by domain (accounts use the
// 0001 slot; later parts claim higher slots).

using namespace Lightweight::SqlColumnTypeDefinitions;

LIGHTWEIGHT_SQL_MIGRATION(20260629000001, "Create accounts table") {
    plan.CreateTableIfNotExists("accounts")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("owner", Varchar(64))
        .RequiredColumn("number", Varchar(34))
        .RequiredColumn("kind", Integer())
        .RequiredColumn("currency", Integer())
        .RequiredColumn("balance_minor", Bigint())
        .RequiredColumn("overdraft_minor", Bigint())
        .RequiredColumn("status", Integer())
        .RequiredColumn("interest_bps", Integer());
}

LIGHTWEIGHT_SQL_MIGRATION(20260629000002, "Create users table") {
    plan.CreateTableIfNotExists("users")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("username", Varchar(64))
        .RequiredColumn("password_hash", Varchar(32))
        .RequiredColumn("display_name", Varchar(128))
        .RequiredColumn("status", Integer());
}

LIGHTWEIGHT_SQL_MIGRATION(20260629000003, "Create transactions table") {
    plan.CreateTableIfNotExists("transactions")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("account_id", Bigint())
        .RequiredColumn("counterparty_id", Bigint())
        .RequiredColumn("direction", Integer())
        .RequiredColumn("kind", Integer())
        .RequiredColumn("amount_minor", Bigint())
        .RequiredColumn("currency", Integer())
        .RequiredColumn("balance_after_minor", Bigint())
        .RequiredColumn("description", Varchar(128))
        .RequiredColumn("created_at_ms", Bigint());
}

LIGHTWEIGHT_SQL_MIGRATION(20260629000004, "Create payees table") {
    plan.CreateTableIfNotExists("payees")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("owner", Varchar(64))
        .RequiredColumn("name", Varchar(128))
        .RequiredColumn("iban", Varchar(34))
        .RequiredColumn("bank_name", Varchar(128));
}

LIGHTWEIGHT_SQL_MIGRATION(20260629000005, "Create payments table") {
    plan.CreateTableIfNotExists("payments")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("owner", Varchar(64))
        .RequiredColumn("from_account_id", Bigint())
        .RequiredColumn("payee_id", Bigint())
        .RequiredColumn("amount_minor", Bigint())
        .RequiredColumn("currency", Integer())
        .RequiredColumn("schedule", Integer())
        .RequiredColumn("status", Integer())
        .RequiredColumn("due_at_ms", Bigint())
        .RequiredColumn("interval_days", Integer())
        .RequiredColumn("description", Varchar(128));
}

LIGHTWEIGHT_SQL_MIGRATION(20260629000006, "Create cards table") {
    plan.CreateTableIfNotExists("cards")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("owner", Varchar(64))
        .RequiredColumn("account_id", Bigint())
        .RequiredColumn("kind", Integer())
        .RequiredColumn("pan_last4", Varchar(4))
        .RequiredColumn("status", Integer())
        .RequiredColumn("daily_limit_minor", Bigint())
        .RequiredColumn("pin_hash", Varchar(16));
}

LIGHTWEIGHT_SQL_MIGRATION(20260629000007, "Create loans table") {
    plan.CreateTableIfNotExists("loans")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("owner", Varchar(64))
        .RequiredColumn("account_id", Bigint())
        .RequiredColumn("principal_minor", Bigint())
        .RequiredColumn("outstanding_minor", Bigint())
        .RequiredColumn("currency", Integer())
        .RequiredColumn("rate_bps", Integer())
        .RequiredColumn("term_months", Integer())
        .RequiredColumn("status", Integer())
        .RequiredColumn("created_at_ms", Bigint());
}

LIGHTWEIGHT_SQL_MIGRATION(20260629000008, "Create budgets table") {
    plan.CreateTableIfNotExists("budgets")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("owner", Varchar(64))
        .RequiredColumn("category", Varchar(64))
        .RequiredColumn("monthly_limit_minor", Bigint())
        .RequiredColumn("currency", Integer());
}

LIGHTWEIGHT_SQL_MIGRATION(20260629000009, "Create notifications table") {
    plan.CreateTableIfNotExists("notifications")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("owner", Varchar(64))
        .RequiredColumn("severity", Integer())
        .RequiredColumn("message", Varchar(256))
        .RequiredColumn("is_read", Bool())
        .RequiredColumn("created_at_ms", Bigint());
}
