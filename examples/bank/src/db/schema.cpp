// SPDX-License-Identifier: Apache-2.0

#include "bank/db/database.hpp"

#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlMigration.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace bank::db {

void configure(const std::string& connectionString) {
    // Each model opens its own SQLite connection to the same file. Give every
    // connection a busy timeout so that when they contend on SQLite's single
    // writer lock they wait-and-retry instead of failing immediately with
    // SQLITE_BUSY. (The SQLite ODBC driver reads `Timeout` in milliseconds.)
    std::string lower = connectionString;
    std::ranges::transform(lower, lower.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::string augmented = connectionString;
    if (lower.find("timeout=") == std::string::npos) {
        if (!augmented.empty() && augmented.back() != ';') {
            augmented.push_back(';');
        }
        augmented += "Timeout=5000";
    }
    Lightweight::SqlConnection::SetDefaultConnectionString(Lightweight::SqlConnectionString{augmented});
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
using Lightweight::SqlForeignKeyReferenceDefinition;

// Foreign-key references mirror the `BelongsTo<>` members on the entity records
// (see bank/db/*_entity.hpp). Tables are created in dependency order so a
// referenced table always exists first: users → accounts/payees → the rest.
// The declared constraints document the schema; SQLite only enforces them when
// `PRAGMA foreign_keys=ON`, which the example leaves at its default (off).

namespace {
constexpr auto usersRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "users", .columnName = "id"};
}
constexpr auto accountsRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "accounts", .columnName = "id"};
}
constexpr auto payeesRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "payees", .columnName = "id"};
}
}  // namespace

LIGHTWEIGHT_SQL_MIGRATION(20260630000001, "Create users table") {
    plan.CreateTableIfNotExists("users")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("username", Varchar(64))
        .RequiredColumn("password_hash", Varchar(32))
        .RequiredColumn("display_name", Varchar(128))
        .RequiredColumn("status", Integer());
}

LIGHTWEIGHT_SQL_MIGRATION(20260630000002, "Create accounts table") {
    plan.CreateTableIfNotExists("accounts")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("user_id", Bigint(), usersRef())
        .RequiredColumn("number", Varchar(34))
        .RequiredColumn("kind", Integer())
        .RequiredColumn("currency", Integer())
        .RequiredColumn("balance_minor", Bigint())
        .RequiredColumn("overdraft_minor", Bigint())
        .RequiredColumn("status", Integer())
        .RequiredColumn("interest_bps", Integer());
}

LIGHTWEIGHT_SQL_MIGRATION(20260630000003, "Create payees table") {
    plan.CreateTableIfNotExists("payees")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("name", Varchar(128))
        .RequiredColumn("iban", Varchar(34))
        .RequiredColumn("bank_name", Varchar(128))
        .RequiredForeignKey("user_id", Bigint(), usersRef());
}

LIGHTWEIGHT_SQL_MIGRATION(20260630000004, "Create transactions table") {
    plan.CreateTableIfNotExists("transactions")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        // Nullable: deposits/withdrawals have no counterparty.
        .ForeignKey("counterparty_id", Bigint(), accountsRef())
        .RequiredColumn("direction", Integer())
        .RequiredForeignKey("account_id", Bigint(), accountsRef())
        .RequiredColumn("kind", Integer())
        .RequiredColumn("amount_minor", Bigint())
        .RequiredColumn("currency", Integer())
        .RequiredColumn("balance_after_minor", Bigint())
        .RequiredColumn("description", Varchar(128))
        .RequiredColumn("created_at_ms", Bigint());
}

LIGHTWEIGHT_SQL_MIGRATION(20260630000005, "Create cards table") {
    plan.CreateTableIfNotExists("cards")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("account_id", Bigint(), accountsRef())
        .RequiredForeignKey("user_id", Bigint(), usersRef())
        .RequiredColumn("kind", Integer())
        .RequiredColumn("pan_last4", Varchar(4))
        .RequiredColumn("status", Integer())
        .RequiredColumn("daily_limit_minor", Bigint())
        .RequiredColumn("pin_hash", Varchar(16));
}

LIGHTWEIGHT_SQL_MIGRATION(20260630000006, "Create loans table") {
    plan.CreateTableIfNotExists("loans")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("user_id", Bigint(), usersRef())
        .RequiredForeignKey("account_id", Bigint(), accountsRef())
        .RequiredColumn("principal_minor", Bigint())
        .RequiredColumn("outstanding_minor", Bigint())
        .RequiredColumn("currency", Integer())
        .RequiredColumn("rate_bps", Integer())
        .RequiredColumn("term_months", Integer())
        .RequiredColumn("status", Integer())
        .RequiredColumn("created_at_ms", Bigint());
}

LIGHTWEIGHT_SQL_MIGRATION(20260630000007, "Create payments table") {
    plan.CreateTableIfNotExists("payments")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("user_id", Bigint(), usersRef())
        .RequiredColumn("currency", Integer())
        .RequiredColumn("amount_minor", Bigint())
        .RequiredForeignKey("from_account_id", Bigint(), accountsRef())
        .RequiredForeignKey("payee_id", Bigint(), payeesRef())
        .RequiredColumn("schedule", Integer())
        .RequiredColumn("status", Integer())
        .RequiredColumn("due_at_ms", Bigint())
        .RequiredColumn("interval_days", Integer())
        .RequiredColumn("description", Varchar(128));
}

LIGHTWEIGHT_SQL_MIGRATION(20260630000008, "Create budgets table") {
    plan.CreateTableIfNotExists("budgets")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("user_id", Bigint(), usersRef())
        .RequiredColumn("category", Varchar(64))
        .RequiredColumn("monthly_limit_minor", Bigint())
        .RequiredColumn("currency", Integer());
}

LIGHTWEIGHT_SQL_MIGRATION(20260630000009, "Create notifications table") {
    plan.CreateTableIfNotExists("notifications")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("user_id", Bigint(), usersRef())
        .RequiredColumn("severity", Integer())
        .RequiredColumn("message", Varchar(256))
        .RequiredColumn("is_read", Bool())
        .RequiredColumn("created_at_ms", Bigint());
}
