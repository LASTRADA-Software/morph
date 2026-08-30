// SPDX-License-Identifier: Apache-2.0
//
// LeadModel: capture/edit/list/mark-lost (README build order §1). Not
// covered here: `ConvertLead` (§3, multi-model transactional action).

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <string>

#include "crm/core/errors.hpp"
#include "crm/models/lead_model.hpp"
#include "crm_test_support.hpp"
#include "testkit/db_fixture.hpp"

using crm::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

namespace {
crm::CreateLead prospect() {
    return crm::CreateLead{.companyName = "Initech", .contactName = "Bill Lumbergh", .email = "bill@initech.example"};
}
}  // namespace

TEST_CASE("CreateLead creates the lead in New status", "[crm][lead]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::LeadModel model;

    const auto created = model.execute(prospect());
    REQUIRE(created.leadId.hasValue());

    const auto view = model.execute(crm::GetLead{.leadId = created.leadId});
    CHECK(view.companyName == "Initech");
    CHECK(view.contactName == "Bill Lumbergh");
    CHECK(view.status == crm::LeadStatus::New);
    CHECK_FALSE(view.convertedAccountId.has_value());
    CHECK(view.version == 1);
}

TEST_CASE("CreateLead with an empty company or contact name is rejected", "[crm][lead]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::LeadModel model;

    auto badCompany = prospect();
    badCompany.companyName.clear();
    CHECK_THROWS_AS(model.execute(badCompany), crm::ValidationError);

    auto badContact = prospect();
    badContact.contactName.clear();
    CHECK_THROWS_AS(model.execute(badContact), crm::ValidationError);
}

TEST_CASE("Creating a lead with no principal is refused", "[crm][lead][audit]") {
    DbFixture fixture;
    crm::LeadModel model;
    CHECK_THROWS_AS(model.execute(prospect()), crm::EmptyPrincipalError);
}

TEST_CASE("UpdateLead replaces fields and bumps the version", "[crm][lead]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::LeadModel model;

    const auto created = model.execute(prospect());
    const auto updated = model.execute(crm::UpdateLead{
        .leadId = created.leadId,
        .companyName = "Initech",
        .contactName = "Bill Lumbergh Jr.",
        .email = "bill@initech.example",
        .expectedVersion = 1,
    });
    CHECK(updated.lead.contactName == "Bill Lumbergh Jr.");
    CHECK(updated.lead.version == 2);
}

TEST_CASE("UpdateLead with a stale version is a Conflict", "[crm][lead]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::LeadModel model;

    const auto created = model.execute(prospect());
    CHECK_THROWS_AS(model.execute(crm::UpdateLead{
                        .leadId = created.leadId,
                        .companyName = "Initech",
                        .contactName = "Bill Lumbergh Jr.",
                        .email = "bill@initech.example",
                        .expectedVersion = 0,  // stale
                    }),
                    crm::Conflict);
}

TEST_CASE("MarkLeadLost transitions the lead to Lost", "[crm][lead]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::LeadModel model;

    const auto created = model.execute(prospect());
    const auto lost = model.execute(crm::MarkLeadLost{.leadId = created.leadId});
    CHECK(lost.lead.status == crm::LeadStatus::Lost);
}

TEST_CASE("A Lost lead is terminal: editing it throws IllegalTransition", "[crm][lead]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::LeadModel model;

    const auto created = model.execute(prospect());
    model.execute(crm::MarkLeadLost{.leadId = created.leadId});

    CHECK_THROWS_AS(model.execute(crm::UpdateLead{
                        .leadId = created.leadId,
                        .companyName = "Initech",
                        .contactName = "Someone Else",
                        .email = "",
                        .expectedVersion = 2,
                    }),
                    crm::IllegalTransition);
}

TEST_CASE("Marking an already-Lost lead lost again throws IllegalTransition", "[crm][lead]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::LeadModel model;

    const auto created = model.execute(prospect());
    model.execute(crm::MarkLeadLost{.leadId = created.leadId});
    CHECK_THROWS_AS(model.execute(crm::MarkLeadLost{.leadId = created.leadId}), crm::IllegalTransition);
}

TEST_CASE("ListLeads lists every lead", "[crm][lead]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::LeadModel model;

    model.execute(prospect());
    model.execute(crm::CreateLead{.companyName = "Globex", .contactName = "Hank Scorpio", .email = ""});

    const auto listed = model.execute(crm::ListLeads{});
    REQUIRE(listed.leads.size() == 2);
}

TEST_CASE("LeadModel journals its edits against the attached identity", "[crm][lead][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::LeadModel model;
    model.attachActionLog(log, std::string{"leads"});

    const auto created = model.execute(prospect());
    model.execute(crm::MarkLeadLost{.leadId = created.leadId});

    const auto entries = log->entries("leads");
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].actionType == "CreateLead");
    CHECK(entries[1].actionType == "MarkLeadLost");
    for (const auto& entry : entries) {
        CHECK(entry.modelType == "LeadModel");
        CHECK(entry.principal == "alice");
        CHECK(entry.outcome == morph::journal::Outcome::Succeeded);
    }
}
