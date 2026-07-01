// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/Lightweight.hpp>

#include <cstdint>
#include <string_view>

namespace bank::db {

// Forward declaration: `UserRecord::accounts` is the "one" side of a 1-to-many
// relationship. `HasMany<AccountRecord>` only needs the type to be declared
// here; the full definition is pulled in (via `entities.hpp`) by any TU that
// actually loads the relation.
struct AccountRecord;

/// @brief The `users` table as a navigable aggregate: scalar columns plus the
///        `HasMany<AccountRecord>` inverse of `AccountRecord::user`.
///
/// @note Member order is significant. Lightweight resolves `HasMany<Child>` by
/// the child column at the SAME ordinal member index as the relation field, so
/// `accounts` sits at index 5 to match `AccountRecord::user`'s index.
///
/// @warning In this Lightweight version's non-reflection build, a record that
/// has a `HasMany` member can only be used with `Create`, `Delete`, and
/// `QuerySingle`-by-primary-key (+ relation navigation). The fluent
/// `Query<T>()` builder and `Update` enumerate *all* members (no
/// `FieldWithStorage` guard), so they emit/inspect the relation as if it were a
/// column. Use the relation-free `UserRow` projection below for fluent lookups
/// and credential updates; reserve `UserRecord` for navigation (see
/// `AccountModel::ListAccounts`, `StatementModel`).
struct UserRecord {
    static constexpr std::string_view TableName = "users";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"username"}> username;                       // 1
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"password_hash"}> passwordHash;            // 2
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"display_name"}> displayName;             // 3
    Light::Field<int, Light::SqlRealName{"status"}> status{0};                                            // 4
    /// All accounts owned by this user (inverse of `AccountRecord::user`).
    Light::HasMany<AccountRecord> accounts;  // 5
};

/// @brief A relation-free projection over the same `users` table.
///
/// Carries every scalar column but no `HasMany`, so it works with the fluent
/// `Query<>()` builder and `Update` (which `UserRecord` cannot — see the warning
/// above). All CRUD and by-username lookups go through this; `UserRecord` is the
/// read-side aggregate used only to navigate the `accounts` relation. This is
/// the library's documented multiple-structs-per-table pattern.
struct UserRow {
    static constexpr std::string_view TableName = "users";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"username"}> username;
    Light::Field<Light::SqlAnsiString<32>, Light::SqlRealName{"password_hash"}> passwordHash;
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"display_name"}> displayName;
    Light::Field<int, Light::SqlRealName{"status"}> status{0};
};

}  // namespace bank::db
