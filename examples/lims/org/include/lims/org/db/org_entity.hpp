// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
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

}  // namespace lims::db
