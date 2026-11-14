// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <optional>
#include <string_view>

/// @file
/// The org module's tables, prefixed `org_`. No other module reads them; they ask `OrgModel`.

namespace lims::db {

/// @brief The company the first org migration seeds. Every record that existed before companies belongs to it.
inline constexpr std::int64_t kSeededCompanyId = 1;

/// @brief The site the first org migration seeds in the seeded company. Every sample that existed before sites is
///        at it.
inline constexpr std::int64_t kSeededSiteId = 1;

/// @brief One company: a legal unit with its own books.
struct CompanyRecord {
    static constexpr std::string_view TableName = "org_companies";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 1
    /// @brief `CurrencyCode` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"base_currency"}> baseCurrency{0};  // 2
    /// @brief `Month` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"fiscal_year_start"}> fiscalYearStart{0};  // 3
};

/// @brief One site: a place a company works at.
struct SiteRecord {
    static constexpr std::string_view TableName = "org_sites";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&CompanyRecord::id, Light::SqlRealName{"company_id"}> company;                        // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;                              // 2
};

/// @brief One principal's role in one scope. The single source of truth for what a principal may do, read by the
///        server edge's authorizer and by every model's own check.
struct RoleAssignmentRecord {
    static constexpr std::string_view TableName = "org_role_assignments";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"principal"}> principal;                     // 1
    /// @brief `LimsRole` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"role"}> role{0};  // 2
    /// @brief `ScopeKind` as its underlying integer.
    Light::Field<int, Light::SqlRealName{"scope_kind"}> scopeKind{0};  // 3
    /// @brief The company's or the site's id, as `scope_kind` says.
    Light::Field<std::int64_t, Light::SqlRealName{"scope_id"}> scopeId{0};               // 4
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"granted_by"}> grantedBy;  // 5
    Light::Field<std::int64_t, Light::SqlRealName{"granted_at"}> grantedAt{0};           // 6
};

/// @brief One principal's display name: at most one row per principal.
struct PersonRecord {
    static constexpr std::string_view TableName = "org_people";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"principal"}> principal;                     // 1
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"display_name"}> displayName;               // 2
};

/// @brief One principal's value limit at one company for one role: at most one row per triple.
struct ValueLimitRecord {
    static constexpr std::string_view TableName = "org_value_limits";
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"principal"}> principal;                     // 1
    Light::BelongsTo<&CompanyRecord::id, Light::SqlRealName{"company_id"}> company;                        // 2
    Light::Field<std::int64_t, Light::SqlRealName{"limit_num"}> limitNum{0};                               // 3
    Light::Field<std::int64_t, Light::SqlRealName{"limit_den"}> limitDen{1};                               // 4
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"set_by"}> setBy;                            // 5
    Light::Field<std::int64_t, Light::SqlRealName{"set_at"}> setAt{0};                                     // 6
    /// @brief `LimsRole` as its underlying integer; the migration that added the column fills every earlier row.
    Light::Field<std::optional<int>, Light::SqlRealName{"role"}> role;  // 7
};

}  // namespace lims::db
