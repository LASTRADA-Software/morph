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

LIGHTWEIGHT_SQL_MIGRATION(20261110000008, "Create billing_lines and billing_consumed_events") {
    plan.CreateTableIfNotExists("billing_lines")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("site_id", Bigint())
        .RequiredColumn("client_id", Bigint())
        .RequiredColumn("sample_id", Bigint())
        .RequiredColumn("result_id", Bigint())
        .RequiredColumn("analysis_id", Bigint())
        .RequiredColumn("published_at", Bigint())
        .RequiredColumn("state", Integer())
        .Column("amount_num", Bigint())
        .Column("amount_den", Bigint())
        .RequiredColumn("currency", Integer())
        .Column("invoice_id", Bigint())
        .RequiredColumn("event_id", Varchar(128));
    plan.CreateTableIfNotExists("billing_consumed_events")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("scope", Varchar(32))
        .RequiredColumn("event_id", Varchar(128))
        .RequiredColumn("line", Varchar(32));
    // An event is consumed once per scope: the index refuses the second of two deliveries that raced past the lookup.
    plan.CreateUniqueIndex("idx_billing_consumed_events_scope_event", "billing_consumed_events",
                           {"scope", "event_id"});
}

LIGHTWEIGHT_SQL_MIGRATION(20261110000009, "Create billing_invoices") {
    plan.CreateTableIfNotExists("billing_invoices")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_id", Bigint())
        .RequiredColumn("site_id", Bigint())
        .RequiredColumn("client_id", Bigint())
        .RequiredColumn("currency", Integer())
        .RequiredColumn("state", Integer())
        .RequiredColumn("number", Varchar(32))
        .RequiredColumn("net_num", Bigint())
        .RequiredColumn("net_den", Bigint())
        .RequiredColumn("tax_num", Bigint())
        .RequiredColumn("tax_den", Bigint())
        .RequiredColumn("gross_num", Bigint())
        .RequiredColumn("gross_den", Bigint())
        .RequiredColumn("reference", Varchar(128))
        .RequiredColumn("created_by", Varchar(64))
        .RequiredColumn("submitted_by", Varchar(64))
        .RequiredColumn("approved_by", Varchar(64))
        .RequiredColumn("posted_by", Varchar(64))
        .RequiredColumn("created_at", Bigint())
        .RequiredColumn("submitted_at", Bigint())
        .RequiredColumn("deadline_at", Bigint())
        .Column("gross_company_num", Bigint())
        .Column("gross_company_den", Bigint())
        .Column("entry_id", Bigint());
}
// NOLINTEND(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)
