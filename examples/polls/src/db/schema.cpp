// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlMigration.hpp>
#include <Lightweight/SqlQuery/Migrate.hpp>

#include "polls/db/database.hpp"

namespace polls::db {

void setup(const std::string& connectionString) {
    Lightweight::SqlConnection::SetDefaultConnectionString(Lightweight::SqlConnectionString{connectionString});
    Lightweight::SqlMigration::MigrationManager::GetInstance().CreateMigrationHistory();
    Lightweight::SqlMigration::MigrationManager::GetInstance().ApplyPendingMigrations();
}

}  // namespace polls::db

// ─── Schema migration ────────────────────────────────────────────────────────
// LIGHTWEIGHT_SQL_MIGRATION auto-registers with the MigrationManager at
// static-init time; linking this TU into the binary makes the schema known.
// All six tables (`poll_entity.hpp`) are created in one migration, in
// dependency order, matching bookmarks' own single-migration schema.cpp.

using namespace Lightweight::SqlColumnTypeDefinitions;

LIGHTWEIGHT_SQL_MIGRATION(20260808000001, "Create polls tables") {
    plan.CreateTableIfNotExists("polls")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("poll_id", Varchar(22))
        .RequiredColumn("admin_token", Varchar(22))
        .RequiredColumn("participant_token", Varchar(22))
        .RequiredColumn("title", Varchar(200))
        .RequiredColumn("finalized", Bool())
        .RequiredColumn("finalized_option_id", Bigint())
        .RequiredColumn("created_at_ms", Bigint());
    // pollId is the shareable link id and both tokens gate admin/participant
    // actions (Task 5+) -- all three must be looked up by exact value alone.
    plan.CreateUniqueIndex("idx_polls_poll_id", "polls", {"poll_id"});
    plan.CreateUniqueIndex("idx_polls_admin_token", "polls", {"admin_token"});
    plan.CreateUniqueIndex("idx_polls_participant_token", "polls", {"participant_token"});

    const auto pollsRef = Lightweight::SqlForeignKeyReferenceDefinition{.tableName = "polls", .columnName = "id"};

    plan.CreateTableIfNotExists("poll_options")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("poll_id", Bigint(), pollsRef)
        .RequiredColumn("label", Varchar(100))
        .RequiredColumn("sort_order", Bigint());
    // GetPollState (Task 5) lists every option for a poll.
    plan.CreateIndex("idx_poll_options_poll", "poll_options", {"poll_id"});

    const auto optionsRef =
        Lightweight::SqlForeignKeyReferenceDefinition{.tableName = "poll_options", .columnName = "id"};

    plan.CreateTableIfNotExists("votes")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("poll_id", Bigint(), pollsRef)
        .RequiredForeignKey("option_id", Bigint(), optionsRef)
        .RequiredColumn("participant_name", Varchar(80))
        .RequiredColumn("choice", Tinyint());
    // A participant may cast exactly one current vote per option -- this is
    // what makes a retried SubmitVotes (Task 6) idempotent rather than a
    // duplicate row.
    plan.CreateUniqueIndex("idx_votes_poll_participant_option", "votes", {"poll_id", "participant_name", "option_id"});

    plan.CreateTableIfNotExists("comments")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("poll_id", Bigint(), pollsRef)
        .RequiredColumn("participant_name", Varchar(80))
        .RequiredColumn("body", Varchar(500))
        .RequiredColumn("created_at_ms", Bigint());
    plan.CreateIndex("idx_comments_poll", "comments", {"poll_id"});

    plan.CreateTableIfNotExists("vote_history")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("poll_id", Bigint(), pollsRef)
        .RequiredColumn("participant_name", Varchar(80))
        .RequiredColumn("previous_votes_json", NVarchar(0))
        .RequiredColumn("created_at_ms", Bigint());
    // UndoLastVoteChange (Task 8) looks up the calling participant's most
    // recent row for this poll.
    plan.CreateIndex("idx_vote_history_poll", "vote_history", {"poll_id"});

    plan.CreateTableIfNotExists("poll_events")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("poll_id", Bigint(), pollsRef)
        .RequiredColumn("kind", Varchar(32))
        .RequiredColumn("summary", NVarchar(0))
        .RequiredColumn("created_at_ms", Bigint());
    // GetEventsSince (Task 9) lists every event for a poll after a cursor.
    plan.CreateIndex("idx_poll_events_poll", "poll_events", {"poll_id"});
}
