// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlMigration.hpp>
#include <Lightweight/SqlQuery/Migrate.hpp>

#include "pastebin/db/database.hpp"

namespace pastebin::db {

void setup(const std::string& connectionString) {
    Lightweight::SqlConnection::SetDefaultConnectionString(Lightweight::SqlConnectionString{connectionString});
    Lightweight::SqlMigration::MigrationManager::GetInstance().CreateMigrationHistory();
    Lightweight::SqlMigration::MigrationManager::GetInstance().ApplyPendingMigrations();
}

}  // namespace pastebin::db

// ─── Schema migration ────────────────────────────────────────────────────────
// LIGHTWEIGHT_SQL_MIGRATION auto-registers with the MigrationManager at
// static-init time; linking this TU into the binary makes the schema known.
//
// `.PrimaryKey("id", Varchar(32))` (as opposed to `.PrimaryKeyWithAutoIncrement`)
// is the manual/caller-assigned primary key column — confirmed against
// `Lightweight/SqlQuery/Migrate.hpp`'s `SqlCreateTableQueryBuilder::PrimaryKey`
// overload, which is exactly what a `Field<..., Light::PrimaryKey::AutoAssign, ...>`
// member (see `paste_entity.hpp`) needs.

using namespace Lightweight::SqlColumnTypeDefinitions;

LIGHTWEIGHT_SQL_MIGRATION(20260806000001, "Create pastes table") {
    plan.CreateTableIfNotExists("pastes")
        .PrimaryKey("id", Varchar(32))
        .RequiredColumn("content", NVarchar(0))
        .RequiredColumn("syntax", Varchar(32))
        .RequiredColumn("created_at_ms", Bigint())
        .Column("expires_at_ms", Bigint())
        .Column("burn_after_reads", Bigint())
        .RequiredColumn("read_count", Bigint())
        .RequiredColumn("is_private", Bool())
        .RequiredColumn("is_editable", Bool());
}
