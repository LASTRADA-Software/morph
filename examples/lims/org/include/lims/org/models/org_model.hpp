// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>
#include <optional>

#include "lims/org/dto/company_dto.hpp"

/// @file
/// `OrgModel`: the companies and sites every other module's records and roles are scoped by.

namespace lims {

/// @brief The organisation: companies and their sites.
///
/// Unkeyed: there is one organisation per deployment. Holds no connection; each `execute()` opens a
/// `Lightweight::DataMapper` for its own duration. Other modules never read the org tables; they call the static
/// lookups here.
class OrgModel {
public:
    /// @brief Creates a company.
    /// @param action Its name, base currency and fiscal-year start.
    /// @return The new company.
    /// @throws ValidationError if the name is empty or longer than 128 bytes.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    CompanyView execute(const CreateCompany& action);

    /// @brief Lists the companies.
    /// @param action Carries nothing.
    /// @return Every company, by id.
    ListCompaniesResult execute(const ListCompanies& action);

    /// @brief Creates a site in a company.
    /// @param action The company and the site's name.
    /// @return The new site.
    /// @throws ValidationError if no company is named, or the name is empty or longer than 128 bytes.
    /// @throws NotFound if the company does not exist.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SiteView execute(const CreateSite& action);

    /// @brief Lists the sites.
    /// @param action Carries nothing.
    /// @return Every site, by id.
    ListSitesResult execute(const ListSites& action);

    /// @brief The site with @p site's id, for a module that records where something happened.
    /// @param site The site.
    /// @return It, or nothing when @p site is empty or names no site.
    [[nodiscard]] static std::optional<SiteView> findSite(SiteId site);
};

}  // namespace lims

// The registration macros declare a non-const status object each.
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL(lims::OrgModel, "OrgModel")
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::CreateCompany, "CreateCompany")
BRIDGE_REGISTER_EFFECTS(lims::CreateCompany, ::morph::effects::changes<lims::CompanyView>)
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::ListCompanies, "ListCompanies", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::CreateSite, "CreateSite")
BRIDGE_REGISTER_EFFECTS(lims::CreateSite, ::morph::effects::changes<lims::SiteView>)
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::ListSites, "ListSites", ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
