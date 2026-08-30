// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlMigration.hpp>

#include "crm/db/database.hpp"

namespace crm::db {

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

}  // namespace crm::db

using namespace Lightweight::SqlColumnTypeDefinitions;
using Lightweight::SqlForeignKeyReferenceDefinition;

namespace {
constexpr auto crmAccountsRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "crm_accounts", .columnName = "id"};
}
}  // namespace

LIGHTWEIGHT_SQL_MIGRATION(20260828000001, "Create crm_accounts table") {
    plan.CreateTableIfNotExists("crm_accounts")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("name", Varchar(128))
        .Column("industry", Varchar(64))
        .Column("website", Varchar(255))
        .RequiredColumn("created_at", Bigint())
        .RequiredColumn("version", Integer());
}

LIGHTWEIGHT_SQL_MIGRATION(20260828000002, "Create crm_contacts table") {
    plan.CreateTableIfNotExists("crm_contacts")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("account_id", Bigint(), crmAccountsRef())
        .RequiredColumn("first_name", Varchar(64))
        .RequiredColumn("last_name", Varchar(64))
        .Column("email", Varchar(255))
        .Column("phone", Varchar(32))
        .RequiredColumn("created_at", Bigint())
        .RequiredColumn("version", Integer());
}

LIGHTWEIGHT_SQL_MIGRATION(20260828000003, "Create crm_leads table") {
    plan.CreateTableIfNotExists("crm_leads")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("company_name", Varchar(128))
        .RequiredColumn("contact_name", Varchar(128))
        .Column("email", Varchar(255))
        .RequiredColumn("status", Integer())
        .Column("converted_account_id", Bigint())
        .Column("converted_contact_id", Bigint())
        .Column("converted_opportunity_id", Bigint())
        .RequiredColumn("created_at", Bigint())
        .RequiredColumn("version", Integer());
}

LIGHTWEIGHT_SQL_MIGRATION(20260828000004, "Create crm_opportunities table") {
    plan.CreateTableIfNotExists("crm_opportunities")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("account_id", Bigint(), crmAccountsRef())
        .Column("primary_contact_id", Bigint())
        .RequiredColumn("name", Varchar(128))
        .RequiredColumn("stage", Integer())
        .RequiredColumn("created_at", Bigint())
        .RequiredColumn("version", Integer());
}

namespace {
constexpr auto crmOpportunitiesRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "crm_opportunities", .columnName = "id"};
}
}  // namespace

LIGHTWEIGHT_SQL_MIGRATION(20260828000005, "Create crm_applied_ops table") {
    plan.CreateTableIfNotExists("crm_applied_ops")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("opportunity_id", Bigint(), crmOpportunitiesRef())
        .RequiredColumn("op_id", Varchar(128))
        .RequiredColumn("result_json", NVarchar(0))
        .RequiredColumn("created_at", Bigint());
    plan.CreateUniqueIndex("idx_crm_applied_ops_opportunity_op", "crm_applied_ops", {"opportunity_id", "op_id"});
}

LIGHTWEIGHT_SQL_MIGRATION(20260828000006, "Create crm_quotes table") {
    plan.CreateTableIfNotExists("crm_quotes")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("opportunity_id", Bigint(), crmOpportunitiesRef())
        .RequiredColumn("status", Integer())
        .RequiredColumn("tax_rate_num", Bigint())
        .RequiredColumn("tax_rate_den", Bigint())
        .RequiredColumn("tax_rate_dp", Integer())
        .RequiredColumn("grand_total_num", Bigint())
        .RequiredColumn("grand_total_den", Bigint())
        .RequiredColumn("grand_total_dp", Integer())
        .RequiredColumn("created_at", Bigint())
        .RequiredColumn("version", Integer());
}

namespace {
constexpr auto crmQuotesRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "crm_quotes", .columnName = "id"};
}
}  // namespace

LIGHTWEIGHT_SQL_MIGRATION(20260828000007, "Create crm_quote_lines table") {
    plan.CreateTableIfNotExists("crm_quote_lines")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("quote_id", Bigint(), crmQuotesRef())
        .RequiredColumn("line_order", Integer())
        .RequiredColumn("product_name", Varchar(128))
        .RequiredColumn("quantity_num", Bigint())
        .RequiredColumn("quantity_den", Bigint())
        .RequiredColumn("quantity_dp", Integer())
        .RequiredColumn("unit_price_num", Bigint())
        .RequiredColumn("unit_price_den", Bigint())
        .RequiredColumn("unit_price_dp", Integer())
        .RequiredColumn("discount_num", Bigint())
        .RequiredColumn("discount_den", Bigint())
        .RequiredColumn("discount_dp", Integer())
        .RequiredColumn("total_num", Bigint())
        .RequiredColumn("total_den", Bigint())
        .RequiredColumn("total_dp", Integer());
}

LIGHTWEIGHT_SQL_MIGRATION(20260828000008, "Create crm_account_roles table") {
    plan.CreateTableIfNotExists("crm_account_roles")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("account_id", Bigint(), crmAccountsRef())
        .RequiredColumn("principal", Varchar(64))
        .RequiredColumn("role", Varchar(16));
    plan.CreateUniqueIndex("idx_crm_account_roles_account_principal", "crm_account_roles",
                           {"account_id", "principal"});
}

LIGHTWEIGHT_SQL_MIGRATION(20260828000009, "Add expected_close_value to crm_opportunities") {
    plan.AlterTable("crm_opportunities")
        .AddNotRequiredColumn("expected_close_value_num", Bigint())
        .AddNotRequiredColumn("expected_close_value_den", Bigint());
}

LIGHTWEIGHT_SQL_MIGRATION(20260828000010, "Create crm_opportunity_conflicts table") {
    plan.CreateTableIfNotExists("crm_opportunity_conflicts")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("opportunity_id", Bigint(), crmOpportunitiesRef())
        .RequiredColumn("base_version", Integer())
        .RequiredColumn("server_version", Integer())
        .RequiredColumn("reason", Integer())
        .RequiredColumn("status", Integer())
        .RequiredColumn("payload", NVarchar(0))
        .RequiredColumn("detected_by", Varchar(64))
        .RequiredColumn("detected_at", Bigint())
        .Column("resolved_by", Varchar(64))
        .RequiredColumn("resolved_at", Bigint())
        .Column("resolution_note", Varchar(255));
}

LIGHTWEIGHT_SQL_MIGRATION(20260829000003, "Create crm_saved_views table") {
    plan.CreateTableIfNotExists("crm_saved_views")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("owner", Varchar(64))
        .RequiredColumn("name", Varchar(128))
        .Column("account_id", Bigint())
        .Column("stage", Integer());
}

LIGHTWEIGHT_SQL_MIGRATION(20260829000001, "Create crm_custom_field_defs table") {
    plan.CreateTableIfNotExists("crm_custom_field_defs")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("entity", Integer())
        .RequiredColumn("name", Varchar(64))
        .RequiredColumn("type", Integer())
        .RequiredColumn("required", Bool());
    plan.CreateUniqueIndex("idx_crm_custom_field_defs_entity_name", "crm_custom_field_defs", {"entity", "name"});
}

LIGHTWEIGHT_SQL_MIGRATION(20260829000002, "Create crm_account_custom_values table") {
    plan.CreateTableIfNotExists("crm_account_custom_values")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("account_id", Bigint(), crmAccountsRef())
        .RequiredColumn("field_name", Varchar(64))
        .RequiredColumn("value_json", NVarchar(1024));
    plan.CreateUniqueIndex("idx_crm_account_custom_values_account_field", "crm_account_custom_values",
                           {"account_id", "field_name"});
}

LIGHTWEIGHT_SQL_MIGRATION(20260829000004, "Add per-field authz and choice options to crm_custom_field_defs") {
    // AddNotRequiredColumn (nullable), not AddRequiredColumn — Lightweight's
    // AlterTable has no "required column with a default" primitive, and any
    // pre-existing row (none yet in this still-unreleased rung, but the
    // migration itself must be written as if there could be) would have
    // nothing to backfill a NOT NULL column with. CustomFieldDefRecord's own
    // fields are nullable to match; the model translates a null read back to
    // Role::Member / "" the same way it already does for OpportunityRecord's
    // nullable expectedCloseValue columns.
    plan.AlterTable("crm_custom_field_defs")
        .AddNotRequiredColumn("min_role_to_edit", Integer())
        .AddNotRequiredColumn("choice_options_json", NVarchar(1024));
}

LIGHTWEIGHT_SQL_MIGRATION(20260828000011, "Create crm_opportunity_replayed_ops table") {
    // Idempotency-key enforcement is the replay consumer's job, not the
    // queue's (docs/spec/offline/offline.md), and this is where this consumer
    // keeps it durably — no unique index: `alreadyDecided()` is the actual
    // enforcement, queried before a row is ever written, matching
    // lims_replayed_ops's identical, deliberately-unconstrained shape.
    plan.CreateTableIfNotExists("crm_opportunity_replayed_ops")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("op_key", Varchar(128))
        .RequiredColumn("decided_at", Bigint());
}
