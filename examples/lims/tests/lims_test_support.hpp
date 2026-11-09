// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <morph/session/session.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "lims/org/core/types.hpp"
#include "lims/org/db/org_entity.hpp"
#include "lims/org/dto/role_dto.hpp"
#include "lims/org/models/org_model.hpp"

/// @file
/// The scaffolding every lims test needs: a scoped authenticated principal.
///
/// Models in this rung refuse to mutate anything without one
/// (`lims::requirePrincipal`), which is deliberate — the README names
/// empty-principal audit entries as disqualifying — so almost every test
/// declares one.

namespace lims::test {

/// @brief Builds a session context naming @p principal.
/// @param principal The authenticated principal's name.
/// @return The context.
[[nodiscard]] inline morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

/// @brief Installs @p principal as the authenticated principal for this scope.
///
/// `_ctx` is declared before `_scope` on purpose: `ScopedContext` holds a
/// reference to it, so the member order *is* the lifetime guarantee.
class ScopedPrincipal {
public:
    /// @param principal The name every action in this scope executes as.
    explicit ScopedPrincipal(std::string principal) : _ctx{contextFor(std::move(principal))}, _scope{_ctx} {}

private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

/// @brief The site the first org migration seeds, where every sample that predates sites is.
/// @return Its id.
[[nodiscard]] inline lims::SiteId seededSite() { return lims::SiteId{lims::db::kSeededSiteId}; }

/// @brief The company the first org migration seeds, which owns the seeded site.
/// @return Its id.
[[nodiscard]] inline lims::CompanyId seededCompany() { return lims::CompanyId{lims::db::kSeededCompanyId}; }

/// @brief Assigns @p role to @p principal in @p scope.
///
/// Made by `lab-supervisor`, a supervisor at the seeded company the helper appoints itself while the lab has none. A
/// test that appoints its own supervisor first assigns through `OrgModel` as that supervisor instead.
/// @param principal Who gets the role.
/// @param role The role.
/// @param scope Where; the seeded company unless said.
/// @return The assignment.
inline lims::RoleAssignmentView grant(std::string principal, lims::LimsRole role,
                                      lims::Scope scope = lims::Scope{seededCompany()}) {
    constexpr std::string_view kGranter = "lab-supervisor";
    ScopedPrincipal const granter{std::string{kGranter}};
    lims::OrgModel org;
    auto const held = lims::OrgModel::rolesIn(kGranter, lims::Scope{seededCompany()});
    if (std::ranges::find(held, lims::LimsRole::Supervisor) == held.end()) {
        org.execute(lims::AssignRole{.principal = std::string{kGranter},
                                     .role = lims::LimsRole::Supervisor,
                                     .company = seededCompany(),
                                     .site = {}});
    }
    auto const* company = std::get_if<lims::CompanyId>(&scope);
    return org.execute(lims::AssignRole{.principal = std::move(principal),
                                        .role = role,
                                        .company = company != nullptr ? *company : lims::CompanyId{},
                                        .site = company != nullptr ? lims::SiteId{} : std::get<lims::SiteId>(scope)});
}

}  // namespace lims::test
