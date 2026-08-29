// SPDX-License-Identifier: Apache-2.0
//
// MoveOpportunityStage: the guarded, journaled pipeline-stage transition
// (README build order §3), modelled on
// kanban::BoardModel::execute(MoveTaskPosition)'s validate/idempotency-ledger/
// re-check/journal sequence.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <string>

#include "crm/core/errors.hpp"
#include "crm/models/account_model.hpp"
#include "crm/models/opportunity_model.hpp"
#include "crm_test_support.hpp"
#include "testkit/db_fixture.hpp"

using crm::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

namespace {
crm::OpportunityId createDeal(crm::AccountModel& accounts, crm::OpportunityModel& opportunities) {
    const auto accountId =
        accounts.execute(crm::CreateAccount{.name = "Acme Corp", .industry = "", .website = ""}).accountId;
    return opportunities
        .execute(crm::CreateOpportunity{.account = crm::OpportunityAccountChoice{std::to_string(*accountId)},
                                        .primaryContact = {},
                                        .name = "Deal"})
        .opportunityId;
}
}  // namespace

TEST_CASE("MoveOpportunityStage moves the opportunity and bumps the version", "[crm][pipeline]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;

    const auto opportunityId = createDeal(accounts, opportunities);
    const auto moved = opportunities.execute(
        crm::MoveOpportunityStage{.opportunityId = opportunityId, .stage = crm::OpportunityStage::Qualification});
    CHECK(moved.stage == crm::OpportunityStage::Qualification);
    CHECK(moved.version == 2);

    const auto view = opportunities.execute(crm::GetOpportunity{.opportunityId = opportunityId});
    CHECK(view.stage == crm::OpportunityStage::Qualification);
}

TEST_CASE("MoveOpportunityStage out of a terminal stage (Won) throws IllegalTransition", "[crm][pipeline]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;

    const auto opportunityId = createDeal(accounts, opportunities);
    opportunities.execute(
        crm::MoveOpportunityStage{.opportunityId = opportunityId, .stage = crm::OpportunityStage::Won});

    CHECK_THROWS_AS(opportunities.execute(crm::MoveOpportunityStage{.opportunityId = opportunityId,
                                                                    .stage = crm::OpportunityStage::Prospecting}),
                    crm::IllegalTransition);
}

TEST_CASE("MoveOpportunityStage out of a terminal stage (Lost) throws IllegalTransition", "[crm][pipeline]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;

    const auto opportunityId = createDeal(accounts, opportunities);
    opportunities.execute(
        crm::MoveOpportunityStage{.opportunityId = opportunityId, .stage = crm::OpportunityStage::Lost});

    CHECK_THROWS_AS(opportunities.execute(crm::MoveOpportunityStage{.opportunityId = opportunityId,
                                                                    .stage = crm::OpportunityStage::Prospecting}),
                    crm::IllegalTransition);
}

TEST_CASE("MoveOpportunityStage naming a nonexistent opportunity is NotFound", "[crm][pipeline]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::OpportunityModel opportunities;

    CHECK_THROWS_AS(opportunities.execute(crm::MoveOpportunityStage{.opportunityId = crm::OpportunityId{999},
                                                                    .stage = crm::OpportunityStage::Qualification}),
                    crm::NotFound);
}

TEST_CASE("Moving a stage with no principal is refused", "[crm][pipeline][audit]") {
    DbFixture fixture;
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::OpportunityId opportunityId;
    {
        const ScopedPrincipal alice{"alice"};
        opportunityId = createDeal(accounts, opportunities);
    }
    CHECK_THROWS_AS(opportunities.execute(crm::MoveOpportunityStage{.opportunityId = opportunityId,
                                                                    .stage = crm::OpportunityStage::Qualification}),
                    crm::EmptyPrincipalError);
}

// ── Exactly-once: a retried opId must not double-apply the move ─────────────

TEST_CASE("MoveOpportunityStage: a retried opId replays the stored result instead of moving again",
          "[crm][pipeline][exactly-once]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;

    const auto opportunityId = createDeal(accounts, opportunities);
    const auto first = opportunities.execute(crm::MoveOpportunityStage{
        .opportunityId = opportunityId, .stage = crm::OpportunityStage::Qualification, .opId = "op-1"});
    CHECK(first.version == 2);

    // Retry with the same opId: must return the *same* stored result (still
    // version 2), not apply a second bump.
    const auto retried = opportunities.execute(crm::MoveOpportunityStage{
        .opportunityId = opportunityId, .stage = crm::OpportunityStage::Qualification, .opId = "op-1"});
    CHECK(retried.version == 2);
    CHECK(retried.stage == crm::OpportunityStage::Qualification);

    // Confirm the DB itself only advanced once.
    const auto view = opportunities.execute(crm::GetOpportunity{.opportunityId = opportunityId});
    CHECK(view.version == 2);
}

TEST_CASE("MoveOpportunityStage: a different opId on the same opportunity is a fresh move",
          "[crm][pipeline][exactly-once]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;

    const auto opportunityId = createDeal(accounts, opportunities);
    opportunities.execute(crm::MoveOpportunityStage{
        .opportunityId = opportunityId, .stage = crm::OpportunityStage::Qualification, .opId = "op-1"});
    const auto second = opportunities.execute(crm::MoveOpportunityStage{
        .opportunityId = opportunityId, .stage = crm::OpportunityStage::Proposal, .opId = "op-2"});
    CHECK(second.version == 3);
    CHECK(second.stage == crm::OpportunityStage::Proposal);
}

TEST_CASE("MoveOpportunityStage journals its moves against the attached identity", "[crm][pipeline][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::OpportunityModel opportunities;
    opportunities.attachActionLog(log, std::string{"opportunities"});

    const auto opportunityId = createDeal(accounts, opportunities);
    opportunities.execute(
        crm::MoveOpportunityStage{.opportunityId = opportunityId, .stage = crm::OpportunityStage::Qualification});

    const auto entries = log->entries("opportunities");
    REQUIRE(entries.size() == 2);  // CreateOpportunity, then MoveOpportunityStage
    CHECK(entries[1].actionType == "MoveOpportunityStage");
    CHECK(entries[1].outcome == morph::journal::Outcome::Succeeded);
}
