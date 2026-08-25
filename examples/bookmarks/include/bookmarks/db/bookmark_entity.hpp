// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <string_view>

/// @file
/// `BookmarkRecord` deliberately carries **zero** relation-typed members
/// (no `HasMany`, no `HasManyThrough`) — see this plan's Global Constraints
/// section for the verified reason: `DataMapper::Update()`'s
/// non-reflection path calls `field.IsModified()` on every member via
/// `EnumerateRecordMembers` (which does not filter by field kind), and
/// neither relation type declares that method, so a record embedding one
/// fails to compile the instant `Update()` is instantiated for it — exactly
/// what `examples/bank/include/bank/db/account_entity.hpp`'s own comment
/// independently documents for `HasMany`. Tag associations are read via a
/// plain `Query<BookmarkTagRecord>()` call in the model (`bookmark_model.cpp`,
/// Task 6), never through a relation field on this record.
///
/// Every string column is a Lightweight strong string type, not
/// `std::string` (`docs/superpowers/specs/2026-08-11-strong-storage-types-design.md`
/// item 3): `ownerPrincipal` is `Light::SqlAnsiString<64>` — `bookmarks_authorizer.hpp`'s
/// `kMaxPrincipalBytes` already bounds every principal this rung accepts to
/// 64 ASCII bytes, so the column mirrors that bound exactly rather than
/// storing an unbounded string for a value that is already validated short.
/// `url`/`faviconPath` are `Light::SqlAnsiString<kMaxUrlBytes>` and `title`
/// is `Light::SqlAnsiString<kMaxTitleBytes>` — both DTO-level caps
/// (`bookmark_dto.hpp`) already exist; `bookmark_model.cpp`'s static_asserts
/// pin the column capacities to those same constants. `description`/`notes`
/// are `Light::SqlMaxDynamicAnsiString`: the DTO layer has never bounded
/// either (`bookmark_dto.hpp`'s `CreateBookmark`/`EditBookmark::validate()`
/// checks neither), so no new business limit is invented at the storage
/// layer where none exists on the wire.

namespace bookmarks::db {

/// @brief One row of the `bookmarks` table.
struct BookmarkRecord {
    static constexpr std::string_view TableName = "bookmarks";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    /// Authenticated owner (`session::Context::principal`) — every query the
    /// model issues filters on this column; see Task 6's `execute()` bodies.
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"owner_principal"}> ownerPrincipal;  // 1
    Light::Field<Light::SqlAnsiString<2048>, Light::SqlRealName{"url"}> url;                       // 2
    Light::Field<Light::SqlAnsiString<512>, Light::SqlRealName{"title"}> title;                    // 3
    Light::Field<Light::SqlMaxDynamicAnsiString, Light::SqlRealName{"description"}> description;   // 4
    Light::Field<Light::SqlMaxDynamicAnsiString, Light::SqlRealName{"notes"}> notes;               // 5
    Light::Field<bool, Light::SqlRealName{"is_unread"}> isUnread{true};                            // 6
    Light::Field<bool, Light::SqlRealName{"is_archived"}> isArchived{false};                       // 7
    Light::Field<bool, Light::SqlRealName{"is_shared"}> isShared{false};                           // 8
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};                // 9
    Light::Field<std::int64_t, Light::SqlRealName{"updated_at_ms"}> updatedAtMs{0};                // 10
    /// Empty = no favicon fetched yet. Path, not bytes — the metadata
    /// worker's own doc comment (Task 12) explains why blobs never travel
    /// the action protocol. Bounded the same as `url` (`kMaxUrlBytes`) since
    /// a favicon path is itself a URL.
    Light::Field<Light::SqlAnsiString<2048>, Light::SqlRealName{"favicon_path"}> faviconPath;  // 11
};

}  // namespace bookmarks::db
