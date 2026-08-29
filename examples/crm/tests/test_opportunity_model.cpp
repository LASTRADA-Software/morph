// SPDX-License-Identifier: Apache-2.0
//
// OpportunityModel CRUD (README build order §1). The guarded pipeline-stage
// transition (§3) is not exercised here — step 1 stays plain CRUD.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <string>

#include "crm/core/errors.hpp"
#include "crm/models/account_model.hpp"
#include "crm/models/contact_model.hpp"
#include "crm/models/opportunity_model.hpp"
#include "crm_test_support.hpp"
#include "testkit/db_fixture.hpp"

using crm::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

namespace {
crm::AccountId createAcme(crm::AccountModel& accounts) {
    return accounts
        .execute(crm::CreateAccount{.name = "Acme Corp", .industry = "Manufacturing", .website = "acme.example"})
        .accountId;
}

crm::ContactId createAda(crm::ContactModel& contacts, crm::AccountId accountId) {
    return contacts
        .execute(crm::CreateContact{
            .account = crm::AccountChoice{std::to_string(*accountId)},
            .firstName = "Ada",
            .lastName = "Lovelace",
            .email = "ada@example.test",
            .phone = "",
        })
        .contactId;
}
}  // namespace

TEST_CASE("CreateOpportunity creates the opportunity in the Prospecting stage", "[crm][opportunity]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;

    const auto accountId = createAcme(accounts);
    const auto created = opportunities.execute(crm::CreateOpportunity{
        .account = crm::OpportunityAccountChoice{std::to_string(*accountId)},
        .primaryContact = {},
        .name = "Acme — Q1 renewal",
    });
    REQUIRE(created.opportunityId.hasValue());

    const auto view = opportunities.execute(crm::GetOpportunity{.opportunityId = created.opportunityId});
    CHECK(view.name == "Acme — Q1 renewal");
    CHECK(view.stage == crm::OpportunityStage::Prospecting);
    CHECK(view.accountId == accountId);
    CHECK_FALSE(view.primaryContactId.has_value());
    CHECK(view.version == 1);
}

TEST_CASE("CreateOpportunity with a primary contact records the link", "[crm][opportunity]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::ContactModel contacts;
    crm::OpportunityModel opportunities;

    const auto accountId = createAcme(accounts);
    const auto contactId = createAda(contacts, accountId);
    const auto created = opportunities.execute(crm::CreateOpportunity{
        .account = crm::OpportunityAccountChoice{std::to_string(*accountId)},
        .primaryContact = crm::PrimaryContactChoice{std::to_string(*contactId)},
        .name = "Acme — Q1 renewal",
    });

    const auto view = opportunities.execute(crm::GetOpportunity{.opportunityId = created.opportunityId});
    REQUIRE(view.primaryContactId.has_value());
    CHECK(*view.primaryContactId == contactId);
}

TEST_CASE("CreateOpportunity naming a nonexistent account is NotFound", "[crm][opportunity]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::OpportunityModel opportunities;

    CHECK_THROWS_AS(opportunities.execute(crm::CreateOpportunity{
                        .account = crm::OpportunityAccountChoice{std::string{"999"}},
                        .primaryContact = {},
                        .name = "Nobody",
                    }),
                    crm::NotFound);
}

TEST_CASE("CreateOpportunity naming a nonexistent primary contact is NotFound", "[crm][opportunity]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;

    const auto accountId = createAcme(accounts);
    CHECK_THROWS_AS(opportunities.execute(crm::CreateOpportunity{
                        .account = crm::OpportunityAccountChoice{std::to_string(*accountId)},
                        .primaryContact = crm::PrimaryContactChoice{std::string{"999"}},
                        .name = "Acme deal",
                    }),
                    crm::NotFound);
}

TEST_CASE("Creating an opportunity with no principal is refused", "[crm][opportunity][audit]") {
    DbFixture fixture;
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::AccountId accountId;
    {
        const ScopedPrincipal alice{"alice"};
        accountId = createAcme(accounts);
    }
    CHECK_THROWS_AS(opportunities.execute(crm::CreateOpportunity{
                        .account = crm::OpportunityAccountChoice{std::to_string(*accountId)},
                        .primaryContact = {},
                        .name = "Acme deal",
                    }),
                    crm::EmptyPrincipalError);
}

TEST_CASE("UpdateOpportunity replaces fields and bumps the version", "[crm][opportunity]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;

    const auto accountId = createAcme(accounts);
    const auto created = opportunities.execute(crm::CreateOpportunity{
        .account = crm::OpportunityAccountChoice{std::to_string(*accountId)},
        .primaryContact = {},
        .name = "Acme — Q1 renewal",
    });

    const auto updated = opportunities.execute(crm::UpdateOpportunity{
        .opportunityId = created.opportunityId,
        .account = crm::OpportunityAccountChoice{std::to_string(*accountId)},
        .primaryContact = {},
        .name = "Acme — Q1 renewal (revised)",
        .expectedVersion = 1,
    });
    CHECK(updated.opportunity.name == "Acme — Q1 renewal (revised)");
    CHECK(updated.opportunity.version == 2);
}

TEST_CASE("UpdateOpportunity with a stale version is a Conflict", "[crm][opportunity]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;

    const auto accountId = createAcme(accounts);
    const auto created = opportunities.execute(crm::CreateOpportunity{
        .account = crm::OpportunityAccountChoice{std::to_string(*accountId)},
        .primaryContact = {},
        .name = "Acme — Q1 renewal",
    });

    CHECK_THROWS_AS(opportunities.execute(crm::UpdateOpportunity{
                        .opportunityId = created.opportunityId,
                        .account = crm::OpportunityAccountChoice{std::to_string(*accountId)},
                        .primaryContact = {},
                        .name = "Acme — Q1 renewal (revised)",
                        .expectedVersion = 0,  // stale
                    }),
                    crm::Conflict);
}

TEST_CASE("ListOpportunities filters by account when accountId is engaged", "[crm][opportunity]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;

    const auto acmeId = createAcme(accounts);
    const auto globexId =
        accounts.execute(crm::CreateAccount{.name = "Globex", .industry = "Retail", .website = "globex.example"})
            .accountId;

    opportunities.execute(crm::CreateOpportunity{
        .account = crm::OpportunityAccountChoice{std::to_string(*acmeId)}, .primaryContact = {}, .name = "Acme deal"});
    opportunities.execute(crm::CreateOpportunity{.account = crm::OpportunityAccountChoice{std::to_string(*globexId)},
                                                 .primaryContact = {},
                                                 .name = "Globex deal"});

    const auto acmeDeals = opportunities.execute(crm::ListOpportunities{.accountId = acmeId});
    REQUIRE(acmeDeals.opportunities.size() == 1);
    CHECK(acmeDeals.opportunities.front().name == "Acme deal");

    const auto allDeals = opportunities.execute(crm::ListOpportunities{});
    CHECK(allDeals.opportunities.size() == 2);
}

TEST_CASE("OpportunityModel journals its edits against the attached identity", "[crm][opportunity][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::OpportunityModel opportunities;
    opportunities.attachActionLog(log, std::string{"opportunities"});

    const auto accountId = createAcme(accounts);
    const auto created = opportunities.execute(crm::CreateOpportunity{
        .account = crm::OpportunityAccountChoice{std::to_string(*accountId)},
        .primaryContact = {},
        .name = "Acme deal",
    });
    opportunities.execute(crm::UpdateOpportunity{
        .opportunityId = created.opportunityId,
        .account = crm::OpportunityAccountChoice{std::to_string(*accountId)},
        .primaryContact = {},
        .name = "Acme deal (revised)",
        .expectedVersion = 1,
    });

    const auto entries = log->entries("opportunities");
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].actionType == "CreateOpportunity");
    CHECK(entries[1].actionType == "UpdateOpportunity");
    for (const auto& entry : entries) {
        CHECK(entry.modelType == "OpportunityModel");
        CHECK(entry.principal == "alice");
        CHECK(entry.outcome == morph::journal::Outcome::Succeeded);
    }
}
