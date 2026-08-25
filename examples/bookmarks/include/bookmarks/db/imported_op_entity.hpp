// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <string_view>

namespace bookmarks::db {

/// @brief One applied `ImportBookmarks` chunk, keyed by `(owner_principal,
///        op_id)` — Task 11's idempotency check: a repeated chunk with the
///        same `opId` after a dropped connection finds its row already
///        present and is a safe no-op.
///
/// Both string columns are Lightweight strong string types
/// (`docs/superpowers/specs/2026-08-11-strong-storage-types-design.md` item
/// 3): `ownerPrincipal` is `Light::SqlAnsiString<64>`, matching
/// `bookmarks_authorizer.hpp`'s `kMaxPrincipalBytes`; `opId` is
/// `Light::SqlAnsiString<128>` — it is a small caller-chosen idempotency
/// token, not free text, so a fixed bound fits it the same way.
struct ImportedOpRecord {
    static constexpr std::string_view TableName = "imported_ops";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"owner_principal"}> ownerPrincipal;          // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"op_id"}> opId;                             // 2
    Light::Field<std::int64_t, Light::SqlRealName{"applied_at_ms"}> appliedAtMs{0};                        // 3
};

}  // namespace bookmarks::db
