// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>

#include <cstdint>
#include <string_view>

/// @file
/// Kanban's eight entities. Every child table (`ColumnRecord`,
/// `SwimlaneRecord`, `TaskRecord`, `CommentRecord`, `AppliedOpRecord`,
/// `BoardEventRecord`, `ProjectRoleRecord`) deliberately carries **zero**
/// relation-typed members beyond `BelongsTo` (no `HasMany`, no
/// `HasManyThrough`) -- see `bookmarks::db::BookmarkRecord`'s identical file
/// comment for the verified reason: `DataMapper::Update()`'s non-reflection
/// path calls `field.IsModified()` on every member via
/// `EnumerateRecordMembers` (which does not filter by field kind), and
/// neither relation type declares that method, so a record embedding one
/// fails to compile the instant `Update()` is instantiated for it.

namespace kanban::db {

/// @brief One row of the `projects` table.
struct ProjectRecord {
    static constexpr std::string_view TableName = "projects";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<200>, Light::SqlRealName{"name"}> name;  // 1
    Light::Field<bool, Light::SqlRealName{"archived"}> archived{false};  // 2
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};  // 3
};

/// @brief One row of the `project_has_roles` table -- one per
///        (project, principal); `role` stores `kanban::roleToString`'s
///        output, converted back via `roleFromString` at the model
///        boundary (mirrors how `bookmarks::db::BookmarkRecord::isUnread`
///        stores an enum-shaped concept as its own primitive column type,
///        rather than storing `Role` directly).
struct ProjectRoleRecord {
    static constexpr std::string_view TableName = "project_has_roles";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&ProjectRecord::id, Light::SqlRealName{"project_id"}> project;  // 1
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"principal"}> principal;  // 2
    Light::Field<Light::SqlAnsiString<16>, Light::SqlRealName{"role"}> role;  // 3
};

/// @brief One row of the `board_columns` table. `wipLimit == 0` means
///        unlimited (mirrors `PollRecord::finalizedOptionId`'s "0 =
///        not-applicable" sentinel convention).
struct ColumnRecord {
    static constexpr std::string_view TableName = "board_columns";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&ProjectRecord::id, Light::SqlRealName{"project_id"}> project;  // 1
    Light::Field<Light::SqlAnsiString<100>, Light::SqlRealName{"name"}> name;  // 2
    Light::Field<std::int64_t, Light::SqlRealName{"wip_limit"}> wipLimit{0};  // 3
    Light::Field<std::int64_t, Light::SqlRealName{"sort_order"}> sortOrder{0};  // 4
};

/// @brief One row of the `swimlanes` table.
struct SwimlaneRecord {
    static constexpr std::string_view TableName = "swimlanes";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&ProjectRecord::id, Light::SqlRealName{"project_id"}> project;  // 1
    Light::Field<Light::SqlAnsiString<100>, Light::SqlRealName{"name"}> name;  // 2
    Light::Field<std::int64_t, Light::SqlRealName{"sort_order"}> sortOrder{0};  // 3
};

/// @brief One row of the `tasks` table. `position` is dense within its
///        `(columnId, swimlaneId)` pair -- see design spec §2's
///        delete-then-recreate renumbering decision.
struct TaskRecord {
    static constexpr std::string_view TableName = "tasks";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&ProjectRecord::id, Light::SqlRealName{"project_id"}> project;  // 1
    Light::BelongsTo<&ColumnRecord::id, Light::SqlRealName{"column_id"}> column;  // 2
    Light::BelongsTo<&SwimlaneRecord::id, Light::SqlRealName{"swimlane_id"}> swimlane;  // 3
    Light::Field<Light::SqlAnsiString<200>, Light::SqlRealName{"title"}> title;  // 4
    Light::Field<std::int64_t, Light::SqlRealName{"position"}> position{0};  // 5
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};  // 6
};

/// @brief One row of the `comments` table. `body` is
///        `Light::SqlMaxDynamicAnsiString` (unbounded) -- no DTO-level cap
///        exists on comment length, so none is invented at storage (design
///        spec §7's "Unbounded fields" note).
struct CommentRecord {
    static constexpr std::string_view TableName = "comments";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&TaskRecord::id, Light::SqlRealName{"task_id"}> task;  // 1
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"principal"}> principal;  // 2
    Light::Field<Light::SqlMaxDynamicAnsiString, Light::SqlRealName{"body"}> body;  // 3
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};  // 4
};

/// @brief One row of the `board_applied_ops` exactly-once ledger (design
///        spec §1). `resultJson` is the full serialized `GetBoardResult`
///        the original call produced -- unbounded, like
///        `polls::db::VoteHistoryRecord::previousVotesJson`.
struct AppliedOpRecord {
    static constexpr std::string_view TableName = "board_applied_ops";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&ProjectRecord::id, Light::SqlRealName{"project_id"}> project;  // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"op_id"}> opId;  // 2
    Light::Field<Light::SqlMaxDynamicAnsiString, Light::SqlRealName{"result_json"}> resultJson;  // 3
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};  // 4
};

/// @brief One row of the `board_events` append-only log (design spec §1's
///        "`GetEventsSince` is a real table" decision) -- mirrors
///        `polls::db::PollEventRecord` exactly: table-wide autoincrement
///        `id` is the wire cursor.
struct BoardEventRecord {
    static constexpr std::string_view TableName = "board_events";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&ProjectRecord::id, Light::SqlRealName{"project_id"}> project;  // 1
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"kind"}> kind;  // 2
    Light::Field<Light::SqlMaxDynamicAnsiString, Light::SqlRealName{"summary"}> summary;  // 3
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};  // 4
};

}  // namespace kanban::db
