// SPDX-License-Identifier: Apache-2.0
//
// Saved views/filters (README build order §10, stretch): a stored
// ListOpportunities filter, executed by re-dispatching that same list
// action — always live against the pipeline's current state, never a cached
// result. Scoped per-principal, owner-only to delete.

#include <catch2/catch_test_macros.hpp>

#include "crm/core/errors.hpp"
#include "crm/models/account_model.hpp"
#include "crm/models/opportunity_model.hpp"
#include "crm/models/saved_view_model.hpp"
#include "crm_test_support.hpp"
#include "testkit/db_fixture.hpp"

using crm::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

namespace {

/// @brief One account with two opportunities in different stages, for the
///        saved-view filter tests to run against.
struct Pipeline {
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::AccountId accountId;
    crm::OpportunityId prospectingId;
    crm::OpportunityId negotiationId;

    Pipeline() {
        accountId = accounts.execute(crm::CreateAccount{.name = "Acme", .industry = "", .website = ""}).accountId;
        const auto choice = crm::OpportunityAccountChoice{std::to_string(*accountId)};
        prospectingId =
            opportunities.execute(crm::CreateOpportunity{.account = choice, .primaryContact = {}, .name = "Deal A"})
                .opportunityId;
        const auto created =
            opportunities.execute(crm::CreateOpportunity{.account = choice, .primaryContact = {}, .name = "Deal B"});
        negotiationId = created.opportunityId;
        opportunities.execute(
            crm::MoveOpportunityStage{.opportunityId = negotiationId, .stage = crm::OpportunityStage::Negotiation});
    }
};

}  // namespace

// ── ListOpportunities' new stage filter ─────────────────────────────────

TEST_CASE("ListOpportunities filters by stage", "[crm][saved_view]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Pipeline pipeline;

    const auto negotiating =
        pipeline.opportunities.execute(crm::ListOpportunities{.stage = crm::OpportunityStage::Negotiation});
    REQUIRE(negotiating.opportunities.size() == 1);
    CHECK(negotiating.opportunities[0].id == pipeline.negotiationId);

    const auto prospecting =
        pipeline.opportunities.execute(crm::ListOpportunities{.stage = crm::OpportunityStage::Prospecting});
    REQUIRE(prospecting.opportunities.size() == 1);
    CHECK(prospecting.opportunities[0].id == pipeline.prospectingId);
}

TEST_CASE("ListOpportunities combines account and stage filters", "[crm][saved_view]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Pipeline pipeline;

    const auto matched = pipeline.opportunities.execute(
        crm::ListOpportunities{.accountId = pipeline.accountId, .stage = crm::OpportunityStage::Negotiation});
    REQUIRE(matched.opportunities.size() == 1);
    CHECK(matched.opportunities[0].id == pipeline.negotiationId);

    // A different account entirely — engaged account filter narrows to zero
    // even though the stage filter alone would have matched.
    crm::AccountModel accounts;
    const auto otherAccountId =
        accounts.execute(crm::CreateAccount{.name = "Globex", .industry = "", .website = ""}).accountId;
    const auto unmatched = pipeline.opportunities.execute(
        crm::ListOpportunities{.accountId = otherAccountId, .stage = crm::OpportunityStage::Negotiation});
    CHECK(unmatched.opportunities.empty());
}

// ── CreateSavedView / ListSavedViews ─────────────────────────────────────

TEST_CASE("CreateSavedView saves a named filter, ListSavedViews returns it", "[crm][saved_view]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::SavedViewModel model;

    const auto created =
        model.execute(crm::CreateSavedView{.name = "My negotiations", .stage = crm::OpportunityStage::Negotiation});
    REQUIRE(created.savedViewId.hasValue());

    const auto listed = model.execute(crm::ListSavedViews{});
    REQUIRE(listed.views.size() == 1);
    CHECK(listed.views[0].name == "My negotiations");
    REQUIRE(listed.views[0].stage.has_value());
    CHECK(*listed.views[0].stage == crm::OpportunityStage::Negotiation);
}

TEST_CASE("CreateSavedView with no name is rejected", "[crm][saved_view]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::SavedViewModel model;
    CHECK_THROWS_AS(model.execute(crm::CreateSavedView{.name = ""}), crm::ValidationError);
}

TEST_CASE("ListSavedViews only returns the calling principal's own views", "[crm][saved_view]") {
    DbFixture fixture;
    {
        const ScopedPrincipal alice{"alice"};
        crm::SavedViewModel model;
        model.execute(crm::CreateSavedView{.name = "Alice's view"});
    }
    {
        const ScopedPrincipal bob{"bob"};
        crm::SavedViewModel model;
        model.execute(crm::CreateSavedView{.name = "Bob's view"});

        const auto listed = model.execute(crm::ListSavedViews{});
        REQUIRE(listed.views.size() == 1);
        CHECK(listed.views[0].name == "Bob's view");
    }
    {
        const ScopedPrincipal alice{"alice"};
        crm::SavedViewModel model;
        const auto listed = model.execute(crm::ListSavedViews{});
        REQUIRE(listed.views.size() == 1);
        CHECK(listed.views[0].name == "Alice's view");
    }
}

// ── RunSavedView: always live, never a cached snapshot ───────────────────

TEST_CASE("RunSavedView re-dispatches ListOpportunities with the stored filter", "[crm][saved_view]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Pipeline pipeline;
    crm::SavedViewModel views;

    const auto saved = views.execute(crm::CreateSavedView{.name = "Negotiating deals at Acme",
                                                          .accountId = pipeline.accountId,
                                                          .stage = crm::OpportunityStage::Negotiation});

    const auto result = views.execute(crm::RunSavedView{.savedViewId = saved.savedViewId});
    REQUIRE(result.opportunities.size() == 1);
    CHECK(result.opportunities[0].id == pipeline.negotiationId);
}

TEST_CASE("RunSavedView reflects the pipeline's current state, not a cached result", "[crm][saved_view]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    Pipeline pipeline;
    crm::SavedViewModel views;

    const auto saved =
        views.execute(crm::CreateSavedView{.name = "Negotiating", .stage = crm::OpportunityStage::Negotiation});

    // No matches yet at creation-adjacent time other than the one already
    // moved — move a second deal into Negotiation *after* the view was
    // created, and confirm the saved view picks it up without being
    // recreated: a stored definition, not a stored result.
    pipeline.opportunities.execute(crm::MoveOpportunityStage{.opportunityId = pipeline.prospectingId,
                                                             .stage = crm::OpportunityStage::Negotiation});

    const auto result = views.execute(crm::RunSavedView{.savedViewId = saved.savedViewId});
    CHECK(result.opportunities.size() == 2);
}

TEST_CASE("RunSavedView naming a nonexistent view is NotFound", "[crm][saved_view]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::SavedViewModel model;
    CHECK_THROWS_AS(model.execute(crm::RunSavedView{.savedViewId = crm::SavedViewId{999}}), crm::NotFound);
}

TEST_CASE("RunSavedView on another principal's view is NotFound, not leaked", "[crm][saved_view]") {
    DbFixture fixture;
    crm::SavedViewId savedViewId;
    {
        const ScopedPrincipal alice{"alice"};
        crm::SavedViewModel model;
        savedViewId = model.execute(crm::CreateSavedView{.name = "Alice's view"}).savedViewId;
    }
    {
        const ScopedPrincipal mallory{"mallory"};
        crm::SavedViewModel model;
        CHECK_THROWS_AS(model.execute(crm::RunSavedView{.savedViewId = savedViewId}), crm::NotFound);
    }
}

// ── DeleteSavedView: owner-only ──────────────────────────────────────────

TEST_CASE("DeleteSavedView removes the view", "[crm][saved_view]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::SavedViewModel model;
    const auto saved = model.execute(crm::CreateSavedView{.name = "Temp"});

    model.execute(crm::DeleteSavedView{.savedViewId = saved.savedViewId});
    CHECK(model.execute(crm::ListSavedViews{}).views.empty());
}

TEST_CASE("DeleteSavedView on another principal's view is Forbidden", "[crm][saved_view]") {
    DbFixture fixture;
    crm::SavedViewId savedViewId;
    {
        const ScopedPrincipal alice{"alice"};
        crm::SavedViewModel model;
        savedViewId = model.execute(crm::CreateSavedView{.name = "Alice's view"}).savedViewId;
    }
    {
        const ScopedPrincipal mallory{"mallory"};
        crm::SavedViewModel model;
        CHECK_THROWS_AS(model.execute(crm::DeleteSavedView{.savedViewId = savedViewId}), crm::Forbidden);
    }
    // Untouched — Alice can still see and run it.
    {
        const ScopedPrincipal alice{"alice"};
        crm::SavedViewModel model;
        CHECK(model.execute(crm::ListSavedViews{}).views.size() == 1);
    }
}

TEST_CASE("DeleteSavedView naming a nonexistent view is NotFound", "[crm][saved_view]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::SavedViewModel model;
    CHECK_THROWS_AS(model.execute(crm::DeleteSavedView{.savedViewId = crm::SavedViewId{999}}), crm::NotFound);
}

TEST_CASE("Every mutating SavedView action refuses an empty principal", "[crm][saved_view][audit]") {
    DbFixture fixture;
    crm::SavedViewModel model;
    CHECK_THROWS_AS(model.execute(crm::CreateSavedView{.name = "x"}), crm::EmptyPrincipalError);
    CHECK_THROWS_AS(model.execute(crm::ListSavedViews{}), crm::EmptyPrincipalError);
    CHECK_THROWS_AS(model.execute(crm::RunSavedView{.savedViewId = crm::SavedViewId{1}}), crm::EmptyPrincipalError);
    CHECK_THROWS_AS(model.execute(crm::DeleteSavedView{.savedViewId = crm::SavedViewId{1}}), crm::EmptyPrincipalError);
}
