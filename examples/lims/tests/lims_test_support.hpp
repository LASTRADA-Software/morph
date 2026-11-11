// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/datetime.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "lims/app/lims_authorizer.hpp"
#include "lims/org/core/types.hpp"
#include "lims/org/db/org_entity.hpp"
#include "lims/org/dto/role_dto.hpp"
#include "lims/org/models/org_model.hpp"
#include "testkit/db_fixture.hpp"

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

/// @brief A fresh database in which the lab's bench staff, `alice`, `fiona`, `gerard` and `tech`, are analysts at the
///        seeded company, which holds at every one of its sites.
///
/// Every write on a sample needs some role at the sample's site, so a test that registers, transitions or captures as
/// one of them needs these grants as setup. A test about what a principal without a role may do uses a plain
/// `DbFixture`.
class LabFixture : public ::morph::ladder::testkit::DbFixture {
public:
    LabFixture() {
        for (std::string const staff : {"alice", "fiona", "gerard", "tech"}) {
            static_cast<void>(grant(staff, lims::LimsRole::Analyst));
        }
    }
};

/// @brief A UTC instant on a calendar day.
/// @param year The year.
/// @param month The month, 1 to 12.
/// @param day The day.
/// @param hour The hour.
/// @return The instant.
[[nodiscard]] inline morph::time::DateTime at(int year, unsigned month, unsigned day, int hour = 9) {
    return morph::time::DateTime{std::chrono::year{year},  std::chrono::month{month}, std::chrono::day{day},
                                 std::chrono::hours{hour}, std::chrono::minutes{0},   std::chrono::seconds{0}};
}

/// @brief The whole UTC second of @p at as ISO 8601 text, the form the table engine's date parser reads; an operand
///        built from it falls exactly on the second the engine floors the instant to.
/// @param at The instant.
/// @return For example `2026-11-10T09:00:00Z`.
[[nodiscard]] inline std::string isoSecond(morph::time::Timestamp const& at) {
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::floor<std::chrono::seconds>((*at).value));
}

/// @brief A signed session for @p principal.
/// @param issuer The issuer.
/// @param principal The principal.
/// @return The context, with the token and the principal set.
[[nodiscard]] inline morph::session::Context signedAs(morph::session::TokenIssuer const& issuer,
                                                      std::string principal) {
    morph::session::Context ctx;
    ctx.principal = principal;
    ctx.token = issuer.issue(morph::session::SessionToken{
        .principal = std::move(principal), .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
    return ctx;
}

/// @brief The server's authorizer, reading roles where production does.
/// @param secret The token secret.
/// @return The authorizer.
[[nodiscard]] inline std::shared_ptr<lims::auth::LimsAuthorizer> limsAuthorizer(std::string_view secret) {
    return std::make_shared<lims::auth::LimsAuthorizer>(
        std::string{secret}, [](std::string_view principal) { return lims::OrgModel::rolesAnywhere(principal); });
}

}  // namespace lims::test
