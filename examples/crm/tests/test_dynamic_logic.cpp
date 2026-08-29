// SPDX-License-Identifier: Apache-2.0
//
// Dynamic logic (README build order §7): CreateOpportunity/UpdateOpportunity
// declare requiredWhen(primaryContact, engaged(expectedCloseValue)) — the
// shipped And/Or/Not-capable rule combinator vocabulary (morph#78), used
// here as a plain requiredWhen (no compound and/or/not needed for this
// particular rule, since the fields available don't support the
// stage-comparison this rung originally considered — see
// opportunity_dto.hpp's doc comment for why).

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/session/session.hpp>
#include <morph/util/rational.hpp>
#include <string>

#include "crm/core/errors.hpp"
#include "crm/gui/crm_schemas.hpp"
#include "crm/models/account_model.hpp"
#include "crm/models/contact_model.hpp"
#include "crm/models/opportunity_model.hpp"
#include "crm_test_support.hpp"
#include "testkit/db_fixture.hpp"

using crm::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Rational;

namespace {
crm::AccountId createAcme(crm::AccountModel& model) {
    return model.execute(crm::CreateAccount{.name = "Acme Corp", .industry = "", .website = ""}).accountId;
}
}  // namespace

// ── Schema: the served schema carries the requiredWhen rule ────────────────

TEST_CASE("CreateOpportunity's served schema carries the requiredWhen rule for primaryContact",
          "[crm][dynamic_logic][schema]") {
    const std::string schema = crm::gui::crmSchemasJson();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));
    auto& rules = dom["CreateOpportunity"]["x-rules"];
    REQUIRE(rules.holds<glz::generic_u64::array_t>());
    bool found = false;
    for (auto const& rule : rules.get<glz::generic_u64::array_t>()) {
        if (rule.contains("kind") && rule.at("kind").get<std::string>() == "requiredWhen") {
            found = true;
            REQUIRE(rule.at("fields").holds<glz::generic_u64::array_t>());
            CHECK(rule.at("fields").get<glz::generic_u64::array_t>().front().get<std::string>() == "primaryContact");
            CHECK(rule.at("when").at("kind").get<std::string>() == "engaged");
        }
    }
    CHECK(found);
}

// ── Enforcement: validate()/allRulesSatisfied() actually gates it ─────────

TEST_CASE("CreateOpportunity with no expectedCloseValue does not require primaryContact",
          "[crm][dynamic_logic][enforcement]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;

    const auto accountId = createAcme(accounts);
    // No expectedCloseValue, no primaryContact — legal: the rule's
    // condition (engaged(expectedCloseValue)) is not satisfied, so
    // requiredWhen imposes nothing.
    const auto created = opportunities.execute(crm::CreateOpportunity{
        .account = crm::OpportunityAccountChoice{std::to_string(*accountId)}, .primaryContact = {}, .name = "Deal"});
    CHECK(created.opportunityId.hasValue());
}

TEST_CASE("CreateOpportunity with expectedCloseValue but no primaryContact is rejected",
          "[crm][dynamic_logic][enforcement]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;

    const auto accountId = createAcme(accounts);
    CHECK_THROWS_AS(opportunities.execute(crm::CreateOpportunity{
                        .account = crm::OpportunityAccountChoice{std::to_string(*accountId)},
                        .primaryContact = {},
                        .name = "Big deal",
                        .expectedCloseValue = crm::Money{Rational{50000, DecimalPlaces{2}}},
                    }),
                    crm::ValidationError);
}

TEST_CASE("CreateOpportunity with expectedCloseValue AND primaryContact succeeds",
          "[crm][dynamic_logic][enforcement]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::ContactModel contacts;
    crm::OpportunityModel opportunities;

    const auto accountId = createAcme(accounts);
    const auto contactId = contacts
                               .execute(crm::CreateContact{
                                   .account = crm::AccountChoice{std::to_string(*accountId)},
                                   .firstName = "Ada",
                                   .lastName = "Lovelace",
                                   .email = "",
                                   .phone = "",
                               })
                               .contactId;

    const auto created = opportunities.execute(crm::CreateOpportunity{
        .account = crm::OpportunityAccountChoice{std::to_string(*accountId)},
        .primaryContact = crm::PrimaryContactChoice{std::to_string(*contactId)},
        .name = "Big deal",
        .expectedCloseValue = crm::Money{Rational{50000, DecimalPlaces{2}}},
    });
    const auto view = opportunities.execute(crm::GetOpportunity{.opportunityId = created.opportunityId});
    REQUIRE(view.expectedCloseValue.hasValue());
    CHECK(*view.expectedCloseValue == Rational{50000, DecimalPlaces{2}});
    REQUIRE(view.primaryContactId.has_value());
}

TEST_CASE("UpdateOpportunity adding expectedCloseValue without a primaryContact is rejected",
          "[crm][dynamic_logic][enforcement]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;

    const auto accountId = createAcme(accounts);
    const auto created = opportunities.execute(crm::CreateOpportunity{
        .account = crm::OpportunityAccountChoice{std::to_string(*accountId)}, .primaryContact = {}, .name = "Deal"});

    CHECK_THROWS_AS(opportunities.execute(crm::UpdateOpportunity{
                        .opportunityId = created.opportunityId,
                        .account = crm::OpportunityAccountChoice{std::to_string(*accountId)},
                        .primaryContact = {},
                        .name = "Deal",
                        .expectedCloseValue = crm::Money{Rational{1000, DecimalPlaces{2}}},
                        .expectedVersion = 1,
                    }),
                    crm::ValidationError);
}

TEST_CASE("Server re-evaluates the rule even if a client's schema check were bypassed",
          "[crm][dynamic_logic][enforcement]") {
    // docs/spec/forms/forms.md: "The server never trusts the client's
    // evaluation of x-rules; it re-runs A::formRules itself." This test
    // exercises exactly that: calling execute() directly (as every ladder
    // model test does, examples/IMPLEMENTATION.md rule 5) with no schema
    // involved at all — the rejection below comes from validate() ->
    // allRulesSatisfied(), the server's own re-check, not from any client
    // that could have been bypassed.
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;

    const auto accountId = createAcme(accounts);
    CHECK_THROWS_AS(opportunities.execute(crm::CreateOpportunity{
                        .account = crm::OpportunityAccountChoice{std::to_string(*accountId)},
                        .primaryContact = {},
                        .name = "Deal",
                        .expectedCloseValue = crm::Money{Rational{1, DecimalPlaces{2}}},
                    }),
                    crm::ValidationError);
}
