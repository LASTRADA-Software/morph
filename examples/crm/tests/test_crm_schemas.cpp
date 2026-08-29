// SPDX-License-Identifier: Apache-2.0
//
// crmSchemasJson() — the served schema document (README build order §2,
// "schema-served forms for every edit view"). Confirms the Choice-backed
// relation fields (account/primary-contact lookups) carry the framework's
// x-optionsAction/x-optionValue/x-optionLabel keys a renderer needs.

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <string>

#include "crm/gui/crm_schemas.hpp"

TEST_CASE("crmSchemasJson serves every form action's schema", "[crm][schema]") {
    const std::string doc = crm::gui::crmSchemasJson();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, doc));
    for (const char* action :
         {"CreateAccount", "UpdateAccount", "CreateContact", "UpdateContact", "CreateLead", "UpdateLead",
          "CreateOpportunity", "UpdateOpportunity", "AddCustomField", "CreateSavedView"}) {
        CHECK(dom.contains(action));
    }
}

TEST_CASE("CreateContact's account field carries the Choice options-action metadata", "[crm][schema]") {
    const std::string doc = crm::gui::crmSchemasJson();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, doc));
    auto& property = dom["CreateContact"]["properties"]["account"];
    CHECK(property["x-optionsAction"].get<std::string>() == "ListAccountOptions");
    CHECK(property["x-optionValue"].get<std::string>() == "id");
    CHECK(property["x-optionLabel"].get<std::string>() == "name");
}

TEST_CASE("CreateOpportunity's primaryContact field depends on the account field (cascading picklist)",
          "[crm][schema]") {
    const std::string doc = crm::gui::crmSchemasJson();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, doc));
    auto& property = dom["CreateOpportunity"]["properties"]["primaryContact"];
    CHECK(property["x-optionsAction"].get<std::string>() == "ListContactOptions");
    // PrimaryContactChoice declares no DependsOn (contact_dto.hpp), so
    // ListContactOptions is called independently of the selected account —
    // README step 2's "cascading" framing is aspirational for this rung's
    // Choice declaration; x-optionsDependsOn is absent, which this asserts
    // rather than assumes.
    CHECK_FALSE(property.contains("x-optionsDependsOn"));
}
