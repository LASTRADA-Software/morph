// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlMigration.hpp>

using Lightweight::SqlForeignKeyReferenceDefinition;
using namespace Lightweight::SqlColumnTypeDefinitions;

// The macro names a struct and a static object whose constructor registers the migration, so neither can be const,
// local to an anonymous namespace, or free of a throwing initializer.
// NOLINTBEGIN(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)
LIGHTWEIGHT_SQL_MIGRATION(20261110000006, "Create billing settings, clients, price lists and prices") {
    plan.CreateTableIfNotExists("billing_settings")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("receivable_account", Varchar(16))
        .RequiredColumn("revenue_account", Varchar(16))
        .RequiredColumn("tax_account", Varchar(16))
        .RequiredColumn("rounding_account", Varchar(16))
        .RequiredColumn("tax_rate_num", Bigint())
        .RequiredColumn("tax_rate_den", Bigint())
        .RequiredColumn("approval_hours", Bigint())
        .RequiredColumn("billing_day", Bigint());
    plan.CreateUniqueIndex("idx_billing_settings_company", "billing_settings", {"company_id"});
    plan.CreateTableIfNotExists("billing_clients")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("client_id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("name", Varchar(128))
        .RequiredColumn("currency", Integer());
    plan.CreateUniqueIndex("idx_billing_clients_client_company", "billing_clients", {"client_id", "company_id"});
    plan.CreateTableIfNotExists("billing_price_lists")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("name", Varchar(128))
        .RequiredColumn("currency", Integer())
        .RequiredColumn("valid_from_ms", Bigint());
    // One list per company, currency and start: the unique index is what makes "the list in force" a single answer.
    plan.CreateUniqueIndex("idx_billing_price_lists_company_currency_start", "billing_price_lists",
                           {"company_id", "currency", "valid_from_ms"});
    plan.CreateTableIfNotExists("billing_prices")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("price_list_id", Bigint(),
                            SqlForeignKeyReferenceDefinition{.tableName = "billing_price_lists", .columnName = "id"})
        .RequiredColumn("analysis_id", Bigint())
        .RequiredColumn("client_id", Bigint())
        .RequiredColumn("amount_num", Bigint())
        .RequiredColumn("amount_den", Bigint());
    // client_id is 0 for the list's own price: a NULL would be distinct from every other NULL in a unique index.
    plan.CreateUniqueIndex("idx_billing_prices_list_analysis_client", "billing_prices",
                           {"price_list_id", "analysis_id", "client_id"});
}
// NOLINTEND(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)
