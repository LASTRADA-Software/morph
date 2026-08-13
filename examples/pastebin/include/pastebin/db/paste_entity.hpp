// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>

#include <cstdint>
#include <optional>
#include <string>

/// @file
/// PasteRecord: the one Lightweight entity this rung needs, kept strictly
/// separate from the wire DTOs (pastebin/dto/paste_dto.hpp) per
/// IMPLEMENTATION.md rule 4's two-type-layer architecture. `id` is the
/// animal-name key itself (the primary key IS the public id — no separate
/// surrogate integer key), so it is a plain string primary key, not
/// auto-incremented: `Light::PrimaryKey::AutoAssign` is Lightweight's
/// enumerator for "primary key, caller supplies the value" (its doc comment:
/// "If the field is neither auto-incrementable nor a GUID, it must be
/// manually set" — exactly this column). There is no `ManualAssign`
/// enumerator; `Light::PrimaryKey` has exactly three values: `No`,
/// `AutoAssign`, `ServerSideAutoIncrement` (the latter is what bank's
/// surrogate integer keys use).

namespace pastebin::db {

/// @brief One row of the `pastes` table.
struct PasteRecord {
    static constexpr std::string_view TableName = "pastes";

    /// The animal-name id; caller-assigned, not auto-incremented.
    Light::Field<Light::SqlAnsiString<32>, Light::PrimaryKey::AutoAssign, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::string, Light::SqlRealName{"content"}> content;  // 1
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"syntax"}> syntax;  // 2
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};  // 3
    /// `std::nullopt` = never expires.
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"expires_at_ms"}> expiresAtMs;  // 4
    /// `std::nullopt` = no burn limit.
    Light::Field<std::optional<std::int64_t>, Light::SqlRealName{"burn_after_reads"}> burnAfterReads;  // 5
    Light::Field<std::int64_t, Light::SqlRealName{"read_count"}> readCount{0};  // 6
    Light::Field<bool, Light::SqlRealName{"is_private"}> isPrivate{false};  // 7
    Light::Field<bool, Light::SqlRealName{"is_editable"}> isEditable{false};  // 8
};

}  // namespace pastebin::db
