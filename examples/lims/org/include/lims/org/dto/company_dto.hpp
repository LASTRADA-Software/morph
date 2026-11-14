// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/records.hpp>
#include <string>
#include <vector>

#include "lims/org/authorization/declared_guard.hpp"
#include "lims/org/core/types.hpp"

/// @file
/// Companies and sites as the wire sees them.

namespace lims {

/// @brief Creates a company.
struct CreateCompany {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The company's name.
    std::string name;
    /// @brief The currency its books are kept in.
    CurrencyCode baseCurrency = CurrencyCode::EUR;
    /// @brief The month its fiscal year starts.
    Month fiscalYearStart = Month::January;

    /// @brief Whether the company can be created.
    /// @return `true` when it is named, in at most 128 bytes.
    [[nodiscard]] bool validate() const noexcept { return !name.empty() && name.size() <= 128; }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One company.
struct CompanyView {
    /// @brief Its id.
    CompanyId id;
    /// @brief Its name.
    std::string name;
    /// @brief The currency its books are kept in.
    CurrencyCode baseCurrency = CurrencyCode::EUR;
    /// @brief The month its fiscal year starts.
    Month fiscalYearStart = Month::January;
};

/// @brief Lists the companies.
struct ListCompanies {
    /// @brief Always ready.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The companies, by id.
struct ListCompaniesResult {
    /// @brief The companies.
    std::vector<CompanyView> companies;
};

/// @brief Creates a site in a company.
struct CreateSite {
    using guard = ::lims::DeclaredRuleGuard;

    /// @brief The company the site belongs to.
    CompanyId companyId;
    /// @brief The site's name.
    std::string name;

    /// @brief Whether the site can be created.
    /// @return `true` when a company is named and the site is named, in at most 128 bytes.
    [[nodiscard]] bool validate() const noexcept {
        return companyId.hasValue() && !name.empty() && name.size() <= 128;
    }

    /// @brief Sent by its button, never per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief One site.
struct SiteView {
    /// @brief Its id.
    SiteId id;
    /// @brief The company it belongs to.
    CompanyId companyId;
    /// @brief Its name.
    std::string name;
};

/// @brief Lists the sites.
struct ListSites {
    /// @brief Always ready.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief The sites, by id.
struct ListSitesResult {
    /// @brief The sites.
    std::vector<SiteView> sites;
};

}  // namespace lims

BRIDGE_REGISTER_RECORD(lims::CompanyView, "Org_Company")
BRIDGE_REGISTER_RECORD(lims::SiteView, "Org_Site")
