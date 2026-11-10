// SPDX-License-Identifier: Apache-2.0

#include "scenario/org_scenario.hpp"

#include <cstdint>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "lims/org/core/money.hpp"
#include "lims/org/dto/company_dto.hpp"
#include "lims/org/dto/limit_dto.hpp"
#include "lims/org/dto/role_dto.hpp"
#include "lims/org/models/org_model.hpp"
#include "scenario/scenario_client.hpp"

namespace lims::screens {

namespace {

/// @brief A value limit of @p units hundredths of the company's currency.
/// @param units The amount in minor units: 1,000,000 is 10,000.00.
/// @return The limit.
[[nodiscard]] Amount limitOf(std::int64_t units) {
    return Amount{morph::math::Rational{units, morph::math::DecimalPlaces{2}}};
}

}  // namespace

void seedOrgScenario() {
    ScenarioClient anna{"anna"};
    // Whoever creates a company becomes its supervisor, so anna administers both companies.
    auto const analytik = anna.call<OrgModel>(CreateCompany{
        .name = "Rheinlab Analytik GmbH", .baseCurrency = CurrencyCode::EUR, .fiscalYearStart = Month::January});
    auto const cologne = anna.call<OrgModel>(CreateSite{.companyId = analytik.id, .name = "Köln"});
    static_cast<void>(anna.call<OrgModel>(CreateSite{.companyId = analytik.id, .name = "Leverkusen"}));
    auto const japan = anna.call<OrgModel>(CreateCompany{
        .name = "Rheinlab Japan K.K.", .baseCurrency = CurrencyCode::JPY, .fiscalYearStart = Month::April});
    auto const osaka = anna.call<OrgModel>(CreateSite{.companyId = japan.id, .name = "Osaka"});

    auto const atCompany = [&anna](std::string const& login, LimsRole role, CompanyId company) {
        static_cast<void>(
            anna.call<OrgModel>(AssignRole{.principal = login, .role = role, .company = company, .site = {}}));
    };
    auto const atSite = [&anna](std::string const& login, LimsRole role, SiteId site) {
        static_cast<void>(
            anna.call<OrgModel>(AssignRole{.principal = login, .role = role, .company = {}, .site = site}));
    };
    auto const limit = [&anna](std::string const& login, CompanyId company, std::int64_t units) {
        static_cast<void>(
            anna.call<OrgModel>(SetValueLimit{.principal = login, .company = company, .limit = limitOf(units)}));
    };

    // Dr. Anna Meyer, lab director: the supervisor of both companies, a verifier, an invoice approver to 10,000.00.
    // A supervisor cannot assign a role to herself, so her other roles come from julia below.
    // Julia Schneider, finance controller: the second supervisor, accountant, invoice approver to 50,000.00.
    atCompany("julia", LimsRole::Supervisor, analytik.id);
    atCompany("julia", LimsRole::Supervisor, japan.id);
    atCompany("julia", LimsRole::Accountant, analytik.id);
    atCompany("julia", LimsRole::InvoiceApprover, analytik.id);
    limit("julia", analytik.id, 5000000);

    ScenarioClient julia{"julia"};
    static_cast<void>(julia.call<OrgModel>(
        AssignRole{.principal = "anna", .role = LimsRole::Verifier, .company = analytik.id, .site = {}}));
    static_cast<void>(julia.call<OrgModel>(
        AssignRole{.principal = "anna", .role = LimsRole::InvoiceApprover, .company = analytik.id, .site = {}}));
    static_cast<void>(
        julia.call<OrgModel>(SetValueLimit{.principal = "anna", .company = analytik.id, .limit = limitOf(1000000)}));

    // Manpreet Kaur, analyst, at the Köln site.
    atSite("manpreet", LimsRole::Analyst, cologne.id);
    // Lena Fischer and Markus Weber, billing clerks.
    atCompany("lena", LimsRole::BillingClerk, analytik.id);
    atCompany("markus", LimsRole::BillingClerk, analytik.id);
    // Kenji Sato, Osaka: an analyst at the site and a billing clerk at the subsidiary.
    atSite("kenji", LimsRole::Analyst, osaka.id);
    atCompany("kenji", LimsRole::BillingClerk, japan.id);
    // Tobias Krüger, stores, holds no role yet: the inventory module's roles are written with the inventory scenario.
}

}  // namespace lims::screens
