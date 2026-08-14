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
///
/// `content` is `Light::SqlMaxDynamicWideString`, not `std::string`: a paste
/// body is unbounded free-form Unicode text, and `db_fixture.hpp`'s
/// `computeConnectionString()` lets `ODBC_CONNECTION_STRING` point this same
/// suite at a SQL Server backend instead of its SQLite default (per
/// `examples/LADDER.md`'s security matrix, which expects rungs to eventually
/// gain non-SQLite CI legs). On that backend, `Light::SqlText`/bare
/// `std::string` — both `char`-based — render as `VARCHAR(MAX)`, a
/// single-byte-collation column: non-ASCII paste content would not
/// round-trip correctly there. `SqlMaxDynamicWideString` is `wchar_t`-based,
/// so its `SqlBasicStringOperations` specialization self-declares `NVarchar`
/// as its column type instead of `Varchar`/`Text`, which every dialect's
/// formatter renders as an unbounded Unicode column (`NVARCHAR(MAX)` on SQL
/// Server once size exceeds `SqlOptimalMaxColumnSize`; SQLite ignores
/// declared length as pure type-affinity and stores UTF-8 natively either
/// way). The model converts at the DTO boundary
/// (`Lightweight::ToStdWideString`/`Lightweight::ToUtf8`), since the wire
/// DTOs stay UTF-8 `std::string` per IMPLEMENTATION.md rule 4 — only this
/// entity field's storage representation is wide. `id`/`syntax` use
/// `Light::SqlAnsiString<32>` instead, matching bank's convention for
/// fixed-width/ASCII/token-shaped columns, where the ASCII assumption is
/// actually true.

namespace pastebin::db {

/// @brief One row of the `pastes` table.
struct PasteRecord {
    static constexpr std::string_view TableName = "pastes";

    /// The animal-name id; caller-assigned, not auto-incremented.
    Light::Field<Light::SqlAnsiString<32>, Light::PrimaryKey::AutoAssign, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlMaxDynamicWideString, Light::SqlRealName{"content"}> content;  // 1
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
