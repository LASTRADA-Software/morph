// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>
#include <optional>
#include <string_view>
#include <vector>

#include "lims/org/dto/company_dto.hpp"
#include "lims/org/dto/role_dto.hpp"

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
    /// @throws Conflict if a company already has that name.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    CompanyView execute(const CreateCompany& action);

    /// @brief Lists the companies.
    /// @param action Carries nothing.
    /// @return Every company, by id.
    /// @throws ValidationError if a stored row holds a currency or month outside its enumeration.
    ListCompaniesResult execute(const ListCompanies& action);

    /// @brief Creates a site in a company.
    /// @param action The company and the site's name.
    /// @return The new site.
    /// @throws ValidationError if no company is named, or the name is empty or longer than 128 bytes.
    /// @throws NotFound if the company does not exist.
    /// @throws Conflict if the company already has a site with that name.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SiteView execute(const CreateSite& action);

    /// @brief Lists the sites.
    /// @param action Carries nothing.
    /// @return Every site, by id.
    ListSitesResult execute(const ListSites& action);

    /// @brief Assigns a role to a principal in a company or a site. Assigning a role the principal already holds in
    ///        that scope changes nothing and answers the existing assignment.
    /// @param action The principal, the role and the scope.
    /// @return The assignment.
    /// @throws ValidationError if no principal, or not exactly one scope, is named.
    /// @throws NotFound if the scope does not exist.
    /// @throws Forbidden if a supervisor exists and the caller is not one in that scope.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    RoleAssignmentView execute(const AssignRole& action);

    /// @brief Revokes one assignment.
    /// @param action The assignment.
    /// @return Which one went.
    /// @throws ValidationError if no assignment is named.
    /// @throws NotFound if it does not exist.
    /// @throws Forbidden if the caller is not a supervisor in its scope.
    /// @throws Conflict if it is the last supervisor's: without one, nobody could assign roles again.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    RevokeRoleResult execute(const RevokeRole& action);

    /// @brief Lists the role assignments.
    /// @param action Carries nothing.
    /// @return Every assignment, by id.
    /// @throws ValidationError if a stored row holds a role or scope kind outside its enumeration.
    ListRoleAssignmentsResult execute(const ListRoleAssignments& action);

    /// @brief Lists the users: the principals holding an assignment.
    /// @param action Carries nothing.
    /// @return Every user, by principal.
    /// @throws ValidationError if a stored row holds a role or scope kind outside its enumeration.
    ListUsersResult execute(const ListUsers& action);

    /// @brief The roles @p principal holds in @p scope. In a site, that is the roles held at the site and at its
    ///        company; in a company, the roles held at the company.
    /// @param principal The principal.
    /// @param scope The scope.
    /// @return The roles, each once, in the order first assigned; none for an empty or unknown scope.
    /// @throws ValidationError if a stored row holds a role or scope kind outside its enumeration.
    [[nodiscard]] static std::vector<LimsRole> rolesIn(std::string_view principal, Scope const& scope);

    /// @brief The roles @p principal holds in any scope: all the server edge can ask, since it never sees a payload.
    /// @param principal The principal.
    /// @return The roles, each once, in the order first assigned.
    /// @throws ValidationError if a stored row holds a role or scope kind outside its enumeration.
    [[nodiscard]] static std::vector<LimsRole> rolesAnywhere(std::string_view principal);

    /// @brief Whether any principal holds `Supervisor` anywhere.
    /// @return Whether one does.
    [[nodiscard]] static bool supervisorExists();

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
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::AssignRole, "AssignRole")
BRIDGE_REGISTER_EFFECTS(lims::AssignRole, ::morph::effects::changes<lims::RoleAssignmentView>,
                        ::morph::effects::changes<lims::UserView>)
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::RevokeRole, "RevokeRole")
BRIDGE_REGISTER_EFFECTS(lims::RevokeRole, ::morph::effects::changes<lims::RoleAssignmentView>,
                        ::morph::effects::changes<lims::UserView>)
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::ListRoleAssignments, "ListRoleAssignments", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::OrgModel, lims::ListUsers, "ListUsers", ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
