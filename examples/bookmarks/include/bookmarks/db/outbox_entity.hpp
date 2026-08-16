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
///
/// Every string column is a Lightweight strong string type
/// (`docs/superpowers/specs/2026-08-11-strong-storage-types-design.md` item
/// 3). `modelType`/`entityKey`/`actionType`/`principal` are
/// `Light::SqlAnsiString<64>` each — short, program-controlled identifiers
/// (a model's own type name, an owner principal, an action-dispatch tag),
/// never free text. `idempotencyKey` is `Light::SqlAnsiString<128>` — every
/// call site builds it as `owner + "-" + actionTag + "-" + nowMs() + "-" +
/// seq` (see `bookmark_model.cpp`/`tag_model.cpp`'s `writeOutboxEntry`/
/// idempotency-key construction), which bounds it in practice well under
/// 128 bytes. `payload`/`result` are `Light::SqlMaxDynamicAnsiString` —
/// serialized JSON of arbitrary action/result shapes, unbounded like
/// `journal::LogEntry`'s own payload field.
struct BookmarkOutboxRecord {
    static constexpr std::string_view TableName = "bookmark_outbox";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"model_type"}> modelType;  // 1
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"entity_key"}> entityKey;  // 2
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"action_type"}> actionType;  // 3
    Light::Field<Light::SqlMaxDynamicAnsiString, Light::SqlRealName{"payload"}> payload;  // 4
    Light::Field<Light::SqlMaxDynamicAnsiString, Light::SqlRealName{"result"}> result;  // 5
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"principal"}> principal;  // 6
    Light::Field<std::int64_t, Light::SqlRealName{"timestamp_ms"}> timestampMs{0};  // 7
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"idempotency_key"}> idempotencyKey;  // 8
};

}  // namespace bookmarks::db
