// SPDX-License-Identifier: Apache-2.0
#include "shell_people_seed.hpp"

#include <catch2/catch_test_macros.hpp>

#include "lims/ledger/dto/account_dto.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/org/models/org_model.hpp"
#include "scenario/org_scenario.hpp"
#include "scenario/scenario_client.hpp"

namespace lims::shelltest {

void seedPeople() {
    {
        lims::screens::ScenarioClient anna{"anna"};
        static_cast<void>(anna.call<lims::OrgModel>(lims::AssignRole{.principal = "anna",
                                                                     .role = lims::LimsRole::Supervisor,
                                                                     .company = lims::OrgModel::seededCompany(),
                                                                     .site = {}}));
    }
    lims::screens::seedOrgScenario();
    // One account in the first company julia sees, so a search for "a" finds a hit the ledger's source answers.
    lims::screens::ScenarioClient julia{"julia"};
    auto const scopes = julia.call<lims::OrgModel>(lims::ListMyScopes{});
    REQUIRE_FALSE(scopes.scopes.empty());
    static_cast<void>(julia.call<lims::LedgerModel>(lims::CreateAccount{.company = scopes.scopes.front().company,
                                                                        .code = "1200",
                                                                        .name = "Accounts receivable",
                                                                        .type = lims::AccountType::Asset}));
}

}  // namespace lims::shelltest
