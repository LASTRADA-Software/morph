// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlMigration.hpp>
#include <string>

#include "lims/lab/db/database.hpp"
#include "lims/org/core/types.hpp"
#include "lims/org/db/org_entity.hpp"

namespace lims::db {

void configure(const std::string& connectionString) {
    Lightweight::SqlConnection::SetDefaultConnectionString(Lightweight::SqlConnectionString{connectionString});
}

void applyMigrations() {
    auto& migrations = Lightweight::SqlMigration::MigrationManager::GetInstance();
    migrations.CreateMigrationHistory();
    migrations.ApplyPendingMigrations();
    // The migration manager is a static that keeps a DataMapper open between calls;
    // one still open at exit is destroyed after Lightweight's default logger and
    // aborts the process. Close it once the migrations are applied.
    migrations.CloseDataMapper();
}

void setup(const std::string& connectionString) {
    configure(connectionString);
    applyMigrations();
}

}  // namespace lims::db

using namespace Lightweight::SqlColumnTypeDefinitions;
using Lightweight::SqlForeignKeyReferenceDefinition;

namespace {
constexpr auto limsClientsRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "lims_clients", .columnName = "id"};
}
constexpr auto limsAnalysesRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "lims_analyses", .columnName = "id"};
}
constexpr auto limsAnalysisVersionsRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "lims_analysis_versions", .columnName = "id"};
}
constexpr auto limsSamplesRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "lims_samples", .columnName = "id"};
}
constexpr auto limsResultsRef() {
    return SqlForeignKeyReferenceDefinition{.tableName = "lims_results", .columnName = "id"};
}
}  // namespace

LIGHTWEIGHT_SQL_MIGRATION(20260823000001, "Create lims_clients table") {
    plan.CreateTableIfNotExists("lims_clients")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("name", Varchar(128));
}

LIGHTWEIGHT_SQL_MIGRATION(20260823000002, "Create lims_analyses table") {
    plan.CreateTableIfNotExists("lims_analyses")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("name", Varchar(128));
}

LIGHTWEIGHT_SQL_MIGRATION(20260823000003, "Create lims_analysis_versions table") {
    // Rows here are append-only: editing an analysis inserts version N+1
    // rather than updating N, which is what keeps an old result bound to the
    // exact definition it was captured under.
    plan.CreateTableIfNotExists("lims_analysis_versions")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("analysis_id", Bigint(), limsAnalysesRef())
        .RequiredColumn("version", Integer())
        .RequiredColumn("canonical_unit", Varchar(32))
        .RequiredColumn("decimal_places", Integer())
        .Column("spec_low_num", Bigint())  // nullable: not every analysis has a
        .Column("spec_low_den", Bigint())  // specification range
        .Column("spec_high_num", Bigint())
        .Column("spec_high_den", Bigint())
        .Column("lod_num", Bigint())  // nullable: nor a detection limit
        .Column("lod_den", Bigint())
        .Column("udl_num", Bigint())
        .Column("udl_den", Bigint())
        .RequiredColumn("created_at", Bigint());
}

LIGHTWEIGHT_SQL_MIGRATION(20260823000004, "Create lims_samples table") {
    plan.CreateTableIfNotExists("lims_samples")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("client_id", Bigint(), limsClientsRef())
        .RequiredColumn("reference", Varchar(64))
        .RequiredColumn("state", Integer())
        .RequiredColumn("version", Integer())
        .RequiredColumn("registered_at", Bigint());
}

LIGHTWEIGHT_SQL_MIGRATION(20260823000005, "Create lims_results table") {
    plan.CreateTableIfNotExists("lims_results")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("sample_id", Bigint(), limsSamplesRef())
        // The *version*, never the analysis identity.
        .RequiredForeignKey("analysis_version_id", Bigint(), limsAnalysisVersionsRef())
        .RequiredColumn("qualifier", Integer())
        .Column("value_num", Bigint())  // nullable: null for every qualifier
        .Column("value_den", Bigint())  // except Measured
        .RequiredColumn("value_dp", Integer())
        // Out-of-specification is a flag on the reading, not a refusal of it:
        // see `ResultView::outOfSpec`.
        .RequiredColumn("out_of_spec", Integer())
        .RequiredColumn("captured_by", Varchar(64))
        .RequiredColumn("captured_at", Bigint());
}

LIGHTWEIGHT_SQL_MIGRATION(20260823000006, "Create lims_verifications table") {
    plan.CreateTableIfNotExists("lims_verifications")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("result_id", Bigint(), limsResultsRef())
        .RequiredColumn("verified_by", Varchar(64))
        .RequiredColumn("verified_at", Bigint());
}

LIGHTWEIGHT_SQL_MIGRATION(20260823000007, "Create lims_offline_conflicts table") {
    // A stale-base field update is flagged for a human, never silently merged
    // and never silently dropped (README build order §7). The queued payload
    // is kept verbatim so the resolver sees what the field client actually
    // sent, not a re-encoding of it.
    plan.CreateTableIfNotExists("lims_offline_conflicts")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("sample_id", Bigint(), limsSamplesRef())
        .RequiredColumn("base_version", Integer())
        .RequiredColumn("server_version", Integer())
        .RequiredColumn("reason", Integer())
        .RequiredColumn("status", Integer())
        .RequiredColumn("payload", Text(4096))
        .RequiredColumn("detected_by", Varchar(64))
        .RequiredColumn("detected_at", Bigint())
        .RequiredColumn("resolved_by", Varchar(64))
        .RequiredColumn("resolved_at", Bigint())
        .RequiredColumn("resolution_note", Varchar(255));
}

LIGHTWEIGHT_SQL_MIGRATION(20260823000008, "Create lims_replayed_ops table") {
    // Idempotency-key enforcement is the replay consumer's job, not the
    // queue's (docs/spec/offline/offline.md). This is where this consumer
    // keeps it, durably: a redelivered field update is skipped, not acted on
    // twice. A row is written once the operation is *decided* -- applied or
    // flagged -- not only when it is applied.
    plan.CreateTableIfNotExists("lims_replayed_ops")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("op_key", Varchar(128))
        .RequiredColumn("decided_at", Bigint());
}

LIGHTWEIGHT_SQL_MIGRATION(20260823000009, "Create lims_operators table") {
    // The single source of truth for roles: the authorizer at the RemoteServer
    // edge and the model on every dispatch path both read this one table,
    // rather than each keeping its own idea of who may verify a result.
    plan.CreateTableIfNotExists("lims_operators")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("principal", Varchar(64))
        .RequiredColumn("role", Integer())
        .RequiredColumn("granted_by", Varchar(64))
        .RequiredColumn("granted_at", Bigint());
}

// The macro names a struct and a static object whose constructor registers the migration, so neither can be const,
// local to an anonymous namespace, or free of a throwing initializer.
// NOLINTBEGIN(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)
LIGHTWEIGHT_SQL_MIGRATION(20261014000001, "Create lims_sample_entries table") {
    plan.CreateTableIfNotExists("lims_sample_entries")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("sample_id", Bigint(), limsSamplesRef())
        .RequiredColumn("payload", Text(4096));
}

LIGHTWEIGHT_SQL_MIGRATION(20261107000001, "Create lims_ignition_tests table") {
    plan.CreateTableIfNotExists("lims_ignition_tests")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .Column("mass_container", Real())
        .Column("mass_before", Real())
        .Column("mass_after", Real());
}

// Must run after org's companies/sites migration (20261109000001): the foreign key names org_sites and the backfill
// names the seeded site, both of which that migration creates. Migrations run in timestamp order, and the
// timestamps are what put this one second.
LIGHTWEIGHT_SQL_MIGRATION(20261109000002, "Give every lims_samples row its site") {
    plan.AlterTable("lims_samples")
        .AddNotRequiredForeignKeyColumn(
            "site_id", Bigint(),
            SqlForeignKeyReferenceDefinition{.tableName = std::string{lims::db::SiteRecord::TableName},
                                             .columnName = "id"});
    // Every sample that predates sites was registered at the one place the lab had, which is the seeded site.
    plan.Update("lims_samples").Set("site_id", lims::db::kSeededSiteId).WhereExpression("site_id IS NULL");
}

// Must run after org's org_role_assignments migration (20261109000003), which creates the table this fills.
LIGHTWEIGHT_SQL_MIGRATION(20261109000004, "Move lims_operators into org_role_assignments at the seeded company") {
    // A role granted before roles were scoped was granted for the whole lab, which is the seeded company. Copying rows
    // between tables is INSERT ... SELECT, which Lightweight's migration builder cannot express (its Insert takes
    // literal values only), so this statement is raw SQL, the escape tier rule 4 allows with a finding recorded.
    static_assert(static_cast<int>(lims::ScopeKind::Company) == 0 && lims::db::kSeededCompanyId == 1,
                  "the statement spells the company scope kind and the seeded company's id as literals");
    plan.RawSql(
        "INSERT INTO org_role_assignments (principal, role, scope_kind, scope_id, granted_by, granted_at) "
        "SELECT principal, role, 0, 1, granted_by, granted_at FROM lims_operators");
    plan.DropTable("lims_operators");
}

LIGHTWEIGHT_SQL_MIGRATION(20261110000007, "Create lims_outbox") {
    plan.CreateTableIfNotExists("lims_outbox")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("event_id", Varchar(128))
        .RequiredColumn("event_type", Varchar(64))
        .RequiredColumn("entity_key", Varchar(64))
        .RequiredColumn("payload", Varchar(4096))
        .RequiredColumn("principal", Varchar(64))
        .RequiredColumn("recorded_at", Bigint());
    // An event id names one result at one sample version, so it is written once; the index refuses a second row.
    plan.CreateUniqueIndex("idx_lims_outbox_event", "lims_outbox", {"event_id"});
}
// NOLINTEND(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)
