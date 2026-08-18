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
// All eight tables in one migration, in dependency order, matching
// bookmarks'/polls' own single-migration schema.cpp. Bounded columns use
// Varchar(N) matching their entity's SqlAnsiString<N> capacity (Task 4);
// unbounded columns use NVarchar(0), never Text() -- the fix already applied
// to bookmarks (PR #90) and polls (PR #91) for this exact DDL/entity
// mismatch (design spec §7).

using namespace Lightweight::SqlColumnTypeDefinitions;

LIGHTWEIGHT_SQL_MIGRATION(20260817000001, "Create kanban tables") {
    plan.CreateTableIfNotExists("projects")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("name", Varchar(200))
        .RequiredColumn("archived", Bool())
        .RequiredColumn("created_at_ms", Bigint());

    plan.CreateTableIfNotExists("project_has_roles")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("project_id", Bigint(),
                             Lightweight::SqlForeignKeyReferenceDefinition{.tableName = "projects", .columnName = "id"})
        .RequiredColumn("principal", Varchar(64))
        .RequiredColumn("role", Varchar(16));
    // One role row per (project, principal) -- a re-grant overwrites, never
    // duplicates; ProjectAdminModel's own role-change action does an
    // upsert-shaped delete-then-recreate against this index.
    plan.CreateUniqueIndex("idx_project_roles_project_principal", "project_has_roles", {"project_id", "principal"});

    const auto projectsRef =
        Lightweight::SqlForeignKeyReferenceDefinition{.tableName = "projects", .columnName = "id"};

    plan.CreateTableIfNotExists("board_columns")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("project_id", Bigint(), projectsRef)
        .RequiredColumn("name", Varchar(100))
        .RequiredColumn("wip_limit", Bigint())  // 0 = unlimited
        .RequiredColumn("sort_order", Bigint());
    plan.CreateIndex("idx_board_columns_project", "board_columns", {"project_id"});

    plan.CreateTableIfNotExists("swimlanes")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("project_id", Bigint(), projectsRef)
        .RequiredColumn("name", Varchar(100))
        .RequiredColumn("sort_order", Bigint());
    plan.CreateIndex("idx_swimlanes_project", "swimlanes", {"project_id"});

    const auto columnsRef =
        Lightweight::SqlForeignKeyReferenceDefinition{.tableName = "board_columns", .columnName = "id"};
    const auto swimlanesRef =
        Lightweight::SqlForeignKeyReferenceDefinition{.tableName = "swimlanes", .columnName = "id"};

    plan.CreateTableIfNotExists("tasks")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("project_id", Bigint(), projectsRef)
        .RequiredForeignKey("column_id", Bigint(), columnsRef)
        .RequiredForeignKey("swimlane_id", Bigint(), swimlanesRef)
        .RequiredColumn("title", Varchar(200))
        .RequiredColumn("position", Bigint())
        .RequiredColumn("created_at_ms", Bigint());
    // GetBoard lists every task for a project; MoveTaskPosition renumbers
    // within one (column, swimlane) pair.
    plan.CreateIndex("idx_tasks_project", "tasks", {"project_id"});
    plan.CreateIndex("idx_tasks_column_swimlane", "tasks", {"column_id", "swimlane_id"});

    plan.CreateTableIfNotExists("comments")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("task_id", Bigint(),
                             Lightweight::SqlForeignKeyReferenceDefinition{.tableName = "tasks", .columnName = "id"})
        .RequiredColumn("principal", Varchar(64))
        .RequiredColumn("body", NVarchar(0))
        .RequiredColumn("created_at_ms", Bigint());
    plan.CreateIndex("idx_comments_task", "comments", {"task_id"});

    // Exactly-once ledger (design spec §1): one row per (board, opId),
    // storing the full serialized GetBoardResult the original call produced.
    plan.CreateTableIfNotExists("board_applied_ops")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("project_id", Bigint(), projectsRef)
        .RequiredColumn("op_id", Varchar(128))
        .RequiredColumn("result_json", NVarchar(0))
        .RequiredColumn("created_at_ms", Bigint());
    plan.CreateUniqueIndex("idx_board_applied_ops_project_op", "board_applied_ops", {"project_id", "op_id"});

    // Event log (design spec §1's "GetEventsSince is a real table" decision):
    // table-wide autoincrement id is the wire cursor, mirroring
    // polls::db::PollEventRecord exactly.
    plan.CreateTableIfNotExists("board_events")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("project_id", Bigint(), projectsRef)
        .RequiredColumn("kind", Varchar(32))
        .RequiredColumn("summary", NVarchar(0))
        .RequiredColumn("created_at_ms", Bigint());
    plan.CreateIndex("idx_board_events_project", "board_events", {"project_id"});
}

// Automation rules (README build-order step 6, design spec §9):
// event->condition->mutation rules, storage and DTO surface only -- rule
// *evaluation* is a later task. A separate migration, matching bookmarks'
// own precedent (schema.cpp's 20260807000002 outbox-table addition) of
// appending a new LIGHTWEIGHT_SQL_MIGRATION block rather than editing an
// already-shipped one.
LIGHTWEIGHT_SQL_MIGRATION(20260818000001, "Create kanban rules table") {
    const auto projectsRef =
        Lightweight::SqlForeignKeyReferenceDefinition{.tableName = "projects", .columnName = "id"};

    plan.CreateTableIfNotExists("rules")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("project_id", Bigint(), projectsRef)
        .RequiredColumn("trigger_event", Varchar(32))
        .RequiredColumn("condition_field", Varchar(32))
        .RequiredColumn("condition_value", Varchar(100))
        .RequiredColumn("mutation_type", Varchar(16))
        .RequiredColumn("mutation_value", Varchar(100));
    plan.CreateIndex("idx_rules_project", "rules", {"project_id"});
}

// Task 14: rule evaluation needs "add tag"/"remove tag" to mean something
// concrete. kanban has no `tags` table and `RuleRecord::mutationValue` is a
// bare string (not a `TagId`), so a task's tags are a denormalized (task_id,
// tag) join table -- the smallest storage answer that makes a fired rule
// observable in `TaskView::tags`. A separate migration, same "append, don't
// edit a shipped block" precedent as 20260818000001 above.
LIGHTWEIGHT_SQL_MIGRATION(20260818000002, "Create kanban task_tags table") {
    plan.CreateTableIfNotExists("task_tags")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("task_id", Bigint(),
                             Lightweight::SqlForeignKeyReferenceDefinition{.tableName = "tasks", .columnName = "id"})
        .RequiredColumn("tag", Varchar(100));
    plan.CreateIndex("idx_task_tags_task", "task_tags", {"task_id"});
}
