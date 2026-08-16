// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/db/database.hpp"

#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlMigration.hpp>
#include <Lightweight/SqlQuery/Migrate.hpp>

namespace bookmarks::db {

void setup(const std::string& connectionString) {
    Lightweight::SqlConnection::SetDefaultConnectionString(Lightweight::SqlConnectionString{connectionString});
    Lightweight::SqlMigration::MigrationManager::GetInstance().CreateMigrationHistory();
    Lightweight::SqlMigration::MigrationManager::GetInstance().ApplyPendingMigrations();
}

}  // namespace bookmarks::db

using namespace Lightweight::SqlColumnTypeDefinitions;

LIGHTWEIGHT_SQL_MIGRATION(20260807000001, "Create bookmarks tables") {
    plan.CreateTableIfNotExists("bookmarks")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("owner_principal", Varchar(64))
        .RequiredColumn("url", Varchar(2048))
        .RequiredColumn("title", Varchar(512))
        .RequiredColumn("description", NVarchar(0))
        .RequiredColumn("notes", NVarchar(0))
        .RequiredColumn("is_unread", Bool())
        .RequiredColumn("is_archived", Bool())
        .RequiredColumn("is_shared", Bool())
        .RequiredColumn("created_at_ms", Bigint())
        .RequiredColumn("updated_at_ms", Bigint())
        .RequiredColumn("favicon_path", Varchar(2048));
    // Every list/get/edit/archive query filters on owner_principal first;
    // the changes-since poll (Task 7) additionally filters on
    // updated_at_ms, and the shared feed (Task 10) on is_shared alone.
    plan.CreateIndex("idx_bookmarks_owner", "bookmarks", {"owner_principal"});
    plan.CreateIndex("idx_bookmarks_owner_updated", "bookmarks", {"owner_principal", "updated_at_ms"});
    plan.CreateIndex("idx_bookmarks_shared", "bookmarks", {"is_shared"});

    plan.CreateTableIfNotExists("tags")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("owner_principal", Varchar(64))
        .RequiredColumn("name", Varchar(128));
    // Tag names are unique per owner, not globally -- two different users
    // may both have a tag named "work".
    plan.CreateUniqueIndex("idx_tags_owner_name", "tags", {"owner_principal", "name"});

    const auto bookmarksRef = Lightweight::SqlForeignKeyReferenceDefinition{.tableName = "bookmarks", .columnName = "id"};
    const auto tagsRef = Lightweight::SqlForeignKeyReferenceDefinition{.tableName = "tags", .columnName = "id"};
    plan.CreateTableIfNotExists("bookmark_tags")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("bookmark_id", Bigint(), bookmarksRef)
        .RequiredForeignKey("tag_id", Bigint(), tagsRef);
    // A bookmark may never carry the same tag twice -- this is what makes
    // TagModel::execute(const MergeTags&)'s "INSERT OR IGNORE"-shaped
    // dedup (Task 9) meaningful rather than a defensive no-op.
    plan.CreateUniqueIndex("idx_bookmark_tags_pair", "bookmark_tags", {"bookmark_id", "tag_id"});

    plan.CreateTableIfNotExists("imported_ops")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("owner_principal", Varchar(64))
        .RequiredColumn("op_id", Varchar(128))
        .RequiredColumn("applied_at_ms", Bigint());
    plan.CreateUniqueIndex("idx_imported_ops_owner_op", "imported_ops", {"owner_principal", "op_id"});
}

LIGHTWEIGHT_SQL_MIGRATION(20260807000002, "Create bookmarks outbox table") {
    plan.CreateTableIfNotExists("bookmark_outbox")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("model_type", Varchar(64))
        .RequiredColumn("entity_key", Varchar(64))
        .RequiredColumn("action_type", Varchar(64))
        .RequiredColumn("payload", NVarchar(0))
        .RequiredColumn("result", NVarchar(0))
        .RequiredColumn("principal", Varchar(64))
        .RequiredColumn("timestamp_ms", Bigint())
        .RequiredColumn("idempotency_key", Varchar(128));
    plan.CreateUniqueIndex("idx_bookmark_outbox_idempotency", "bookmark_outbox", {"idempotency_key"});
}
