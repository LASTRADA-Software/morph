// SPDX-License-Identifier: Apache-2.0

#include "scenario/scenario.hpp"

#include <algorithm>

#include "lims/org/models/org_model.hpp"
#include "scenario/billing_scenario.hpp"
#include "scenario/inventory_scenario.hpp"
#include "scenario/lab_scenario.hpp"
#include "scenario/ledger_scenario.hpp"
#include "scenario/org_scenario.hpp"
#include "scenario/scenario_client.hpp"

namespace lims::screens {

void seedScenario() {
    ScenarioClient anna{"anna"};
    auto const companies = anna.call<OrgModel>(ListCompanies{}).companies;
    bool const seeded = std::ranges::any_of(
        companies, [](CompanyView const& company) { return company.name == "Rheinlab Analytik GmbH"; });
    if (seeded) {
        return;
    }
    seedOrgScenario();
    seedLabScenario();
    seedBillingScenario();
    seedLedgerScenario();
    seedInventoryScenario();
}

}  // namespace lims::screens
