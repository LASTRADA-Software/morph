// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/Lightweight.hpp>
#include <Lightweight/SqlMigration.hpp>
#include <string>

#include "lims/org/core/types.hpp"
#include "lims/org/db/org_entity.hpp"

using namespace Lightweight::SqlColumnTypeDefinitions;
using Lightweight::SqlForeignKeyReferenceDefinition;

// The macro names a struct and a static object whose constructor registers the migration, so neither can be const,
// local to an anonymous namespace, or free of a throwing initializer.
// NOLINTBEGIN(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)
LIGHTWEIGHT_SQL_MIGRATION(20261109000001, "Create org_companies and org_sites, seeded with one of each") {
    plan.CreateTableIfNotExists("org_companies")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("name", Varchar(128))
        .RequiredColumn("base_currency", Integer())
        .RequiredColumn("fiscal_year_start", Integer());
    plan.CreateTableIfNotExists("org_sites")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("company_id", Bigint(),
                            SqlForeignKeyReferenceDefinition{.tableName = "org_companies", .columnName = "id"})
        .RequiredColumn("name", Varchar(128));
    // Every record that existed before companies needs one to belong to, so a deployment starts with one company
    // and one site, which a later migration moves the existing records to.
    plan.Insert("org_companies")
        .Set("id", lims::db::kSeededCompanyId)
        .Set("name", std::string{"Main laboratory"})
        .Set("base_currency", static_cast<int>(lims::CurrencyCode::EUR))
        .Set("fiscal_year_start", static_cast<int>(lims::Month::January));
    plan.Insert("org_sites")
        .Set("id", lims::db::kSeededSiteId)
        .Set("company_id", lims::db::kSeededCompanyId)
        .Set("name", std::string{"Main site"});
}

LIGHTWEIGHT_SQL_MIGRATION(20261109000003, "Create org_role_assignments") {
    plan.CreateTableIfNotExists("org_role_assignments")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("principal", Varchar(64))
        .RequiredColumn("role", Integer())
        .RequiredColumn("scope_kind", Integer())
        .RequiredColumn("scope_id", Bigint())
        .RequiredColumn("granted_by", Varchar(64))
        .RequiredColumn("granted_at", Bigint());
}
// NOLINTEND(bugprone-throwing-static-initialization,cert-err58-cpp,misc-use-internal-linkage,misc-use-anonymous-namespace,cppcoreguidelines-avoid-non-const-global-variables)
