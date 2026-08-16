// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>

#include <cstdint>
#include <string_view>

namespace bookmarks::db {

/// @brief One row of the `tags` table. Every string column is a Lightweight
///        strong string type
///        (`docs/superpowers/specs/2026-08-11-strong-storage-types-design.md`
///        item 3): `ownerPrincipal` is `Light::SqlAnsiString<64>`, matching
///        `bookmarks_authorizer.hpp`'s `kMaxPrincipalBytes`, and `name` is
///        `Light::SqlAnsiString<kMaxTagNameBytes>` (128,
///        `bookmarks/dto/tag_dto.hpp`) — `tag_model.cpp`'s static_assert
///        pins the column capacity to that same constant. No relation-typed
///        member — see `bookmark_entity.hpp`'s file comment.
struct TagRecord {
    static constexpr std::string_view TableName = "tags";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"owner_principal"}> ownerPrincipal;  // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;  // 2
};

}  // namespace bookmarks::db
