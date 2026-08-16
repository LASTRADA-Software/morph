// SPDX-License-Identifier: Apache-2.0
#include "kanban/db/database.hpp"

#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlMigration.hpp>
#include <Lightweight/SqlQuery/Migrate.hpp>

namespace kanban::db {

void setup(const std::string& connectionString) {
    Lightweight::SqlConnection::SetDefaultConnectionString(Lightweight::SqlConnectionString{connectionString});
    Lightweight::SqlMigration::MigrationManager::GetInstance().CreateMigrationHistory();
    Lightweight::SqlMigration::MigrationManager::GetInstance().ApplyPendingMigrations();
}

}  // namespace kanban::db

// ─── Schema migration ────────────────────────────────────────────────────────
// LIGHTWEIGHT_SQL_MIGRATION auto-registers with the MigrationManager at
// static-init time; linking this TU into the binary makes the schema known.
// Task 3+ will add migrations here using SqlColumnTypeDefinitions.
