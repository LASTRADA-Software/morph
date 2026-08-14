// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>

#include <cstdint>
#include <string_view>

namespace bookmarks::db {

/// @brief One row of the `tags` table. `name` is a plain variable-length
///        `TEXT` column, not a fixed `SqlAnsiString` — see
///        `bookmarks/dto/tag_dto.hpp`'s file comment for why (tag names are
///        free-form Unicode text; truncating one is exactly the harm this
///        session's `pastebin::EditPaste`/`syntax` fix eliminated
///        elsewhere). No relation-typed member — see `bookmark_entity.hpp`'s
///        file comment.
struct TagRecord {
    static constexpr std::string_view TableName = "tags";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::string, Light::SqlRealName{"owner_principal"}> ownerPrincipal;  // 1
    Light::Field<std::string, Light::SqlRealName{"name"}> name;  // 2
};

}  // namespace bookmarks::db
