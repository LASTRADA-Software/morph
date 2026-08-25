// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <string_view>

#include "bookmarks/db/bookmark_entity.hpp"
#include "bookmarks/db/tag_entity.hpp"

namespace bookmarks::db {

/// @brief The bookmark<->tag many-to-many junction (`IMPLEMENTATION.md`
///        rule 4's "real Lightweight idiom" clause — this is an ordinary
///        `BelongsTo`-pair entity, not the sanctioned raw-SQL escape tier).
///        `BelongsTo<>` supports `Update()` (unlike `HasMany`/
///        `HasManyThrough` — see `bookmark_entity.hpp`'s file comment), but
///        this record never needs it: tag assignment/removal is always a
///        `Create`/delete of a whole row (`BookmarkModel::execute`, Task 6).
struct BookmarkTagRecord {
    static constexpr std::string_view TableName = "bookmark_tags";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&BookmarkRecord::id, Light::SqlRealName{"bookmark_id"}> bookmark;                     // 1
    Light::BelongsTo<&TagRecord::id, Light::SqlRealName{"tag_id"}> tag;                                    // 2
};

}  // namespace bookmarks::db
