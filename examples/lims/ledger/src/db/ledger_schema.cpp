// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlMigration.hpp>

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
// NOLINTEND(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)
