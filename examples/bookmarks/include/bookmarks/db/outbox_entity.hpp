// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>

#include <cstdint>
#include <string_view>

namespace bookmarks::db {

/// @brief `BookmarkModel`'s own transactional outbox — a row written inside
///        the same `SqlTransaction` as a multi-row mutation
///        (`BulkEdit`; `TagModel`'s `RenameTag`/`MergeTags`, Task 9, uses
///        the identical table), drained by `journal::OutboxRelay` (Task 12)
///        into the durable `FileActionLog`. Shaped after
///        `journal::LogEntry` (`include/morph/journal/action_log.hpp`) —
///        only the fields a relay actually needs, not a 1:1 mirror. A row
///        is deleted once relayed rather than flagged, so the table only
///        ever holds genuinely-unrelayed work.
struct BookmarkOutboxRecord {
    static constexpr std::string_view TableName = "bookmark_outbox";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::string, Light::SqlRealName{"model_type"}> modelType;  // 1
    Light::Field<std::string, Light::SqlRealName{"entity_key"}> entityKey;  // 2
    Light::Field<std::string, Light::SqlRealName{"action_type"}> actionType;  // 3
    Light::Field<std::string, Light::SqlRealName{"payload"}> payload;  // 4
    Light::Field<std::string, Light::SqlRealName{"result"}> result;  // 5
    Light::Field<std::string, Light::SqlRealName{"principal"}> principal;  // 6
    Light::Field<std::int64_t, Light::SqlRealName{"timestamp_ms"}> timestampMs{0};  // 7
    Light::Field<std::string, Light::SqlRealName{"idempotency_key"}> idempotencyKey;  // 8
};

}  // namespace bookmarks::db
