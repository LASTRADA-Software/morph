// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>
#include <string>
#include <variant>

#include "lims/org/core/strong_id.hpp"

/// @file
/// The organisation's identities and closed sets: companies, sites, the currencies a company keeps its books in and
/// the month its fiscal year starts.

namespace lims {

LIMS_DEFINE_STRONG_ID(CompanyId);
LIMS_DEFINE_STRONG_ID(SiteId);
LIMS_DEFINE_STRONG_ID(RoleAssignmentId);

/// @brief A currency a company keeps its books in. JPY has no minor unit, so the zero-decimal case is always present.
enum class CurrencyCode : std::uint8_t {
    EUR,  ///< Euro.
    USD,  ///< US dollar.
    GBP,  ///< Pound sterling.
    CHF,  ///< Swiss franc.
    JPY,  ///< Japanese yen.
};

/// @brief A calendar month: the first month of a company's fiscal year.
enum class Month : std::uint8_t {
    January,    ///< January.
    February,   ///< February.
    March,      ///< March.
    April,      ///< April.
    May,        ///< May.
    June,       ///< June.
    July,       ///< July.
    August,     ///< August.
    September,  ///< September.
    October,    ///< October.
    November,   ///< November.
    December,   ///< December.
};

/// @brief What a role assignment's scope is.
enum class ScopeKind : std::uint8_t {
    Company,  ///< A company, and every site of it.
    Site,     ///< One site.
};

/// @brief A scope a role is held in or an action is judged in: a company or a site.
using Scope = std::variant<CompanyId, SiteId>;

/// @brief Names @p scope for a message: `company 1`, `site 2`.
/// @param scope The scope.
/// @return Its name.
[[nodiscard]] inline std::string scopeName(Scope const& scope) {
    if (auto const* company = std::get_if<CompanyId>(&scope)) {
        return company->hasValue() ? "company " + std::to_string(**company) : std::string{"no company"};
    }
    auto const& site = std::get<SiteId>(scope);
    return site.hasValue() ? "site " + std::to_string(*site) : std::string{"an unassigned site"};
}

}  // namespace lims

LIMS_DEFINE_STRONG_ID_WIRE(CompanyId);
LIMS_DEFINE_STRONG_ID_WIRE(SiteId);
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(CompanyId, "lims.companyId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(SiteId, "lims.siteId");
LIMS_DEFINE_STRONG_ID_WIRE(RoleAssignmentId);
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(RoleAssignmentId, "lims.roleAssignmentId");

/// @brief Reflects `ScopeKind` as its enumerator name.
template <>
struct glz::meta<lims::ScopeKind> {
    using enum lims::ScopeKind;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Company, Site);
};

/// @brief Reflects `CurrencyCode` as its enumerator name.
template <>
struct glz::meta<lims::CurrencyCode> {
    using enum lims::CurrencyCode;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(EUR, USD, GBP, CHF, JPY);
};

/// @brief Reflects `Month` as its enumerator name.
template <>
struct glz::meta<lims::Month> {
    using enum lims::Month;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(January, February, March, April, May, June, July, August, September,
                                                 October, November, December);
};
