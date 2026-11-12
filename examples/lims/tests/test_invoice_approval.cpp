// SPDX-License-Identifier: Apache-2.0
//
// Invoice approval. Hypothesis 5: an approval that lasts days and passes between principals is ordinary actions and
// the declared authorization. Hypothesis 6, second instance: billing's create/approve and submit/approve pairs and the
// approver's value limit, which the server edge cannot see. Escalation at a deadline, which the design listed as
// hypothesis 4's second instance, with nothing scheduled.

#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <functional>
#include <morph/core/action_guard.hpp>
#include <morph/core/registry.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/app/service_principals.hpp"
#include "lims/billing/core/fault_hook.hpp"
#include "lims/billing/models/billing_run_model.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/org/dto/company_dto.hpp"
#include "lims/org/dto/limit_dto.hpp"
#include "lims/org/models/org_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::Mode;
using morph::math::DecimalPlaces;
using morph::math::Rational;

namespace {

constexpr std::string_view kSecret = "lims-invoice-approval-test-secret-32-bytes";

/// @brief Clears the fault hook when the case ends, however it ends.
struct FaultHookGuard {
    FaultHookGuard() = default;
    FaultHookGuard(FaultHookGuard const&) = delete;
    FaultHookGuard& operator=(FaultHookGuard const&) = delete;
    FaultHookGuard(FaultHookGuard&&) = delete;
    FaultHookGuard& operator=(FaultHookGuard&&) = delete;
    ~FaultHookGuard() { lims::setFaultHook({}); }
};

/// @brief Runs @p sql on the fixture's database.
void execute(std::string const& sql) {
    Lightweight::SqlStatement statement;
    (void)statement.ExecuteDirect(sql);
}

/// @brief Makes @p sql run on a connection of its own, committed, when the next invoice state change is about to be
///        written, as a concurrent action that committed first would have.
void raceWith(std::string sql) {
    lims::setFaultHook([sql = std::move(sql)](std::string_view point, Lightweight::SqlConnection&) {
        if (point == "invoice.beforeStateChange") {
            Lightweight::SqlConnection other;
            Lightweight::SqlStatement statement{other};
            (void)statement.ExecuteDirect(sql);
        }
    });
}

[[nodiscard]] lims::InvoiceView read(lims::InvoiceId id) {
    ScopedPrincipal const bea{"bea"};
    return lims::InvoiceModel{}.execute(lims::GetInvoice{.invoiceId = id});
}

}  // namespace

// Mutation: drop the separation-of-duties members from Authorization<ApproveInvoice> (bea approves her own invoice).
TEST_CASE("invoice approval: four eyes and a value limit, judged in the model", "[lims][billing][approval]") {
    lims::test::LabFixture const fixture;
    auto const invoice = lims::test::draftedInvoice();
    lims::test::submit(invoice);
    ScopedClockOverride const clock{lims::test::at(2026, 11, 11)};
    lims::InvoiceModel invoices;
    {
        ScopedPrincipal const bea{"bea"};
        CHECK_THROWS_WITH(::morph::model::executeChecked(invoices, lims::ApproveInvoice{.invoiceId = invoice.id}),
                          Catch::Matchers::ContainsSubstring("created this invoice"));
    }
    {
        ScopedPrincipal const tom{"tom"};
        CHECK_THROWS_WITH(::morph::model::executeChecked(invoices, lims::ApproveInvoice{.invoiceId = invoice.id}),
                          Catch::Matchers::ContainsSubstring("value limit"));
    }
    {
        ScopedPrincipal const nobody{"nora"};
        CHECK_THROWS_AS(::morph::model::executeChecked(invoices, lims::ApproveInvoice{.invoiceId = invoice.id}),
                        lims::Forbidden);
    }
    ScopedPrincipal const paula{"paula"};
    auto const approved = invoices.execute(lims::ApproveInvoice{.invoiceId = invoice.id});
    CHECK(approved.state == lims::InvoiceState::Approved);
    CHECK(approved.approvedBy == "paula");
    CHECK_THROWS_AS(::morph::model::executeChecked(invoices, lims::ApproveInvoice{.invoiceId = invoice.id}),
                    lims::IllegalTransition);
}

// Mutation: drop `submitted_by` from the submission's update (the row records no submitter).
TEST_CASE("invoice approval: submission records the deadline, the submitter and the company-currency gross",
          "[lims][billing][approval]") {
    lims::test::LabFixture const fixture;
    auto const invoice = lims::test::draftedInvoice();
    CHECK(invoice.state == lims::InvoiceState::Draft);
    lims::test::submit(invoice);
    auto const submitted = read(invoice.id);
    CHECK(submitted.state == lims::InvoiceState::Submitted);
    CHECK(submitted.submittedBy == "bea");
    // 09:00 on 10 November plus the company's 48 approval hours.
    CHECK(submitted.approvalDeadline == morph::time::Timestamp{lims::test::at(2026, 11, 12)});
    CHECK(submitted.submittedAt == morph::time::Timestamp{lims::test::at(2026, 11, 10)});
    // A Task handler: only a dispatch drives it.
    using lims::test::dispatchAs;
    CHECK_THROWS_AS(dispatchAs<lims::InvoiceModel>("bea", lims::SubmitInvoice{.invoiceId = invoice.id}),
                    lims::IllegalTransition);
    CHECK_THROWS_AS(dispatchAs<lims::InvoiceModel>("paula", lims::SubmitInvoice{.invoiceId = invoice.id}),
                    lims::Forbidden);
    CHECK_THROWS_AS(dispatchAs<lims::InvoiceModel>("bea", lims::SubmitInvoice{.invoiceId = lims::InvoiceId{9999}}),
                    lims::Forbidden);
    // A dispatch refuses an unnamed invoice before the handler runs, with the registry's own error type.
    CHECK_THROWS_AS(dispatchAs<lims::InvoiceModel>("bea", lims::SubmitInvoice{}), morph::model::ValidationError);
    lims::InvoiceModel invoices;
    ScopedPrincipal const bea{"bea"};
    CHECK_THROWS_AS(invoices.execute(lims::ApproveInvoice{}), lims::ValidationError);
    CHECK(read(invoice.id).submittedBy == "bea");
}

// Mutation: make `checkState` a no-op (a draft reaches the value rule and is refused as an invoice with no value in
// its company's currency, not as an IllegalTransition).
TEST_CASE("invoice approval: a draft is not approved, and it is an IllegalTransition for an approver",
          "[lims][billing][approval]") {
    lims::test::LabFixture const fixture;
    auto const invoice = lims::test::draftedInvoice();
    lims::InvoiceModel invoices;
    {
        ScopedPrincipal const carl{"carl"};
        CHECK_THROWS_AS(::morph::model::executeChecked(invoices, lims::ApproveInvoice{.invoiceId = invoice.id}),
                        lims::IllegalTransition);
    }
    CHECK_THROWS_AS(lims::test::dispatchAs<lims::InvoiceModel>("carl", lims::SubmitInvoice{.invoiceId = invoice.id}),
                    lims::Forbidden);
    CHECK(read(invoice.id).state == lims::InvoiceState::Draft);
}

// Mutation: make `checkState` a no-op (the creator re-approving an approved invoice is told they created it).
TEST_CASE("invoice approval: re-approving an approved invoice is an IllegalTransition, even for its creator",
          "[lims][billing][approval]") {
    lims::test::LabFixture const fixture;
    auto const invoice = lims::test::draftedInvoice();
    lims::test::submit(invoice);
    ScopedClockOverride const clock{lims::test::at(2026, 11, 11)};
    lims::InvoiceModel invoices;
    {
        ScopedPrincipal const paula{"paula"};
        CHECK(invoices.execute(lims::ApproveInvoice{.invoiceId = invoice.id}).state == lims::InvoiceState::Approved);
    }
    for (char const* who : {"paula", "bea"}) {
        ScopedPrincipal const caller{who};
        CHECK_THROWS_AS(::morph::model::executeChecked(invoices, lims::ApproveInvoice{.invoiceId = invoice.id}),
                        lims::IllegalTransition);
    }
}

// Mutation: drop the role check before `checkState` in `requireAuthorized` (an approver of another company is told the
// draft exists, where an unknown id is refused).
TEST_CASE("invoice approval: an approver of another company and an unknown id are refused identically",
          "[lims][billing][approval]") {
    lims::test::LabFixture const fixture;
    auto const invoice = lims::test::draftedInvoice();
    lims::InvoiceModel invoices;
    std::string foreign;
    std::string unknown;
    {
        ScopedPrincipal const carl{"carl"};
        // carl holds the approver role at the seeded company; make the invoice another company's.
        execute("UPDATE billing_invoices SET company_id = company_id + 1000, site_id = site_id + 1000");
        try {
            static_cast<void>(::morph::model::executeChecked(invoices, lims::ApproveInvoice{.invoiceId = invoice.id}));
            FAIL("approving another company's invoice was not refused");
        } catch (lims::Forbidden const& refusal) {
            foreign = refusal.what();
        }
        try {
            static_cast<void>(
                ::morph::model::executeChecked(invoices, lims::ApproveInvoice{.invoiceId = lims::InvoiceId{9999}}));
            FAIL("approving an unknown invoice was not refused");
        } catch (lims::Forbidden const& refusal) {
            unknown = refusal.what();
        }
    }
    CHECK(foreign == unknown);
    CHECK_THAT(foreign, !Catch::Matchers::ContainsSubstring("draft") && !Catch::Matchers::ContainsSubstring("value") &&
                            !Catch::Matchers::ContainsSubstring("created"));
}

// Mutation: drop the currency check from SubmitInvoice (a yen invoice is submitted with a gross no rate valued).
TEST_CASE(
    "invoice approval: an invoice in another currency than its company's is not submitted before a rate values it",
    "[lims][billing][approval]") {
    lims::test::LabFixture const fixture;
    auto const invoice = lims::test::draftedInvoice();
    execute("UPDATE billing_invoices SET currency = " + std::to_string(static_cast<int>(lims::CurrencyCode::JPY)));
    CHECK_THROWS_WITH(lims::test::dispatchAs<lims::InvoiceModel>("bea", lims::SubmitInvoice{.invoiceId = invoice.id}),
                      Catch::Matchers::ContainsSubstring("no rate values it in EUR"));
    CHECK(read(invoice.id).state == lims::InvoiceState::Draft);
}

// Mutation: drop the state condition from the conditional update (the clerk who lost the race overwrites the winner's
// submitter and no Conflict is raised).
TEST_CASE("invoice approval: two clerks submitting one invoice at once cannot both succeed",
          "[lims][billing][approval]") {
    lims::test::LabFixture const fixture;
    auto const invoice = lims::test::draftedInvoice();
    FaultHookGuard const guard;
    raceWith(
        "UPDATE billing_invoices SET state = 1, submitted_by = 'ann', gross_company_num = gross_num, "
        "gross_company_den = gross_den, deadline_at = 1, submitted_at = 1");
    ScopedClockOverride const clock{lims::test::at(2026, 11, 10)};
    CHECK_THROWS_WITH(lims::test::dispatchAs<lims::InvoiceModel>("bea", lims::SubmitInvoice{.invoiceId = invoice.id}),
                      Catch::Matchers::ContainsSubstring("was submitted or edited meanwhile"));
    lims::setFaultHook({});
    auto const after = read(invoice.id);
    CHECK(after.state == lims::InvoiceState::Submitted);
    CHECK(after.submittedBy == "ann");
}

// Mutation: drop the state condition from the conditional update (the loser overwrites the winner's approver and no
// Conflict is raised).
TEST_CASE("invoice approval: two approvers approving one invoice at once cannot both succeed",
          "[lims][billing][approval]") {
    lims::test::LabFixture const fixture;
    auto const invoice = lims::test::draftedInvoice();
    lims::test::submit(invoice);
    ScopedClockOverride const clock{lims::test::at(2026, 11, 11)};
    FaultHookGuard const guard;
    raceWith("UPDATE billing_invoices SET state = 2, approved_by = 'carl'");
    {
        ScopedPrincipal const paula{"paula"};
        CHECK_THROWS_WITH(lims::InvoiceModel{}.execute(lims::ApproveInvoice{.invoiceId = invoice.id}),
                          Catch::Matchers::ContainsSubstring("was approved meanwhile"));
    }
    lims::setFaultHook({});
    auto const after = read(invoice.id);
    CHECK(after.state == lims::InvoiceState::Approved);
    CHECK(after.approvedBy == "carl");
}

// Escalation, which the design listed as hypothesis 4's second instance: nothing is scheduled. Mutation: judge the
// scope at the site whatever the time (paula still approves after the deadline, and carl's pending list stays as it
// was).
TEST_CASE(
    "Hypothesis 4 (mis-framed for escalation): an approval past its deadline goes to a company-scoped approver, "
    "with nothing scheduled",
    "[lims][billing][approval][h4]") {
    lims::test::LabFixture const fixture;
    auto const invoice = lims::test::draftedInvoice();
    lims::test::submit(invoice);
    lims::InvoiceModel invoices;
    auto const pendingFor = [&invoices](std::string principal) {
        ScopedPrincipal const as{std::move(principal)};
        return invoices.execute(lims::ListPendingApprovals{}).invoices.size();
    };
    {
        ScopedClockOverride const before{lims::test::at(2026, 11, 12, 8)};
        CHECK(pendingFor("paula") == 1U);
        CHECK(pendingFor("carl") == 1U);  // a company role holds at the site
        CHECK(pendingFor("tom") == 0U);   // over his limit
        CHECK(pendingFor("bea") == 0U);   // she created it
        ScopedPrincipal const bea{"bea"};
        CHECK(invoices.execute(lims::GetInvoice{.invoiceId = invoice.id}).level == lims::ApprovalLevel::Site);
    }
    ScopedClockOverride const after{lims::test::at(2026, 11, 12, 10)};
    CHECK(pendingFor("paula") == 0U);
    CHECK(pendingFor("carl") == 1U);
    {
        ScopedPrincipal const paula{"paula"};
        CHECK_THROWS_WITH(::morph::model::executeChecked(invoices, lims::ApproveInvoice{.invoiceId = invoice.id}),
                          Catch::Matchers::ContainsSubstring("does not hold the invoice-approver role"));
    }
    ScopedPrincipal const carl{"carl"};
    CHECK(invoices.execute(lims::GetInvoice{.invoiceId = invoice.id}).level == lims::ApprovalLevel::Company);
    CHECK(invoices.execute(lims::ApproveInvoice{.invoiceId = invoice.id}).state == lims::InvoiceState::Approved);
}

// Hypothesis 5. The approval's state is in the invoice's row, which a second rig reads; state kept in the process
// would also pass, since the rigs share it. Mutation: drop `submitted_by` from the submission's update (the approval
// made over a second rig finds no submitter on the row).
TEST_CASE("Hypothesis 5: an approval lasts across days, principals and rigs as ordinary actions",
          "[lims][billing][approval][h5]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    lims::test::LabFixture const fixture;
    auto const invoice = lims::test::draftedInvoice();
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    {
        ScopedClockOverride const monday{lims::test::at(2026, 11, 10)};
        BackendRig rig{mode, 1, lims::test::limsAuthorizer(kSecret)};
        rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "bea"));
        auto clerk = rig.client<lims::InvoiceModel>(0);
        CHECK(awaitQt(clerk.execute(lims::SubmitInvoice{.invoiceId = invoice.id})).state ==
              lims::InvoiceState::Submitted);
    }
    // Another rig, as a restarted server would be; another principal, a day later, on another connection.
    ScopedClockOverride const tuesday{lims::test::at(2026, 11, 11, 15)};
    BackendRig rig{mode, 1, lims::test::limsAuthorizer(kSecret)};
    rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "paula"));
    auto approver = rig.client<lims::InvoiceModel>(0);
    auto const approved = awaitQt(approver.execute(lims::ApproveInvoice{.invoiceId = invoice.id}));
    CHECK(approved.state == lims::InvoiceState::Approved);
    CHECK(approved.submittedBy == "bea");
    CHECK(approved.approvedBy == "paula");
}

// Hypothesis 6, second instance: the edge sees (principal, model, action) and allows; the model sees the invoice.
// Mutation: remove `using guard` from ApproveInvoice and its in-transaction `requireAuthorized` (either alone leaves
// the refusal: bea's self-approval succeeds over every backend only with both gone).
TEST_CASE("Hypothesis 6: the edge lets an invoice's creator through to approve it, and the model refuses",
          "[lims][billing][approval][h6]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    lims::test::LabFixture const fixture;
    auto const invoice = lims::test::draftedInvoice();
    lims::test::submit(invoice);
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    auto const authorizer = lims::test::limsAuthorizer(kSecret);
    CHECK(authorizer->authorize(lims::test::signedAs(issuer, "bea"), "InvoiceModel", "ApproveInvoice"));
    CHECK(authorizer->authorize(lims::test::signedAs(issuer, "tom"), "InvoiceModel", "ApproveInvoice"));
    ScopedClockOverride const clock{lims::test::at(2026, 11, 11)};
    BackendRig rig{mode, 1, authorizer};
    rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "bea"));
    auto client = rig.client<lims::InvoiceModel>(0);
    CHECK_THROWS_WITH(awaitQt(client.execute(lims::ApproveInvoice{.invoiceId = invoice.id})),
                      Catch::Matchers::ContainsSubstring("created this invoice"));
    rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "tom"));
    CHECK_THROWS_WITH(awaitQt(client.execute(lims::ApproveInvoice{.invoiceId = invoice.id})),
                      Catch::Matchers::ContainsSubstring("value limit"));
}

// Mutation: drop the SubmitInvoice pair from Authorization<ApproveInvoice> (bea approves the invoice she submitted).
TEST_CASE("invoice approval: an invoice the billing run drafted still needs a second person to approve it",
          "[lims][billing][approval]") {
    lims::test::LabFixture const fixture;
    auto const lab = lims::test::approvalSetUp();
    lims::test::grantService(std::string{lims::app::kSchedulePrincipal});
    lims::InvoiceId id;
    {
        // What one schedule tick does for this company, called as its principal; the billing run's suite measures the
        // dispatch.
        ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
        ScopedPrincipal const schedule{std::string{lims::app::kSchedulePrincipal}};
        static_cast<void>(lims::BillingRunModel{}.execute(
            lims::RunBillingSchedule{.batch = lims::Count{Rational{50, DecimalPlaces{0}}}}));
        auto const drafted =
            lims::InvoiceModel{}.execute(lims::ListInvoices{.company = lims::test::seededCompany()}).invoices;
        REQUIRE(drafted.size() == 1U);
        CHECK(drafted[0].createdBy == lims::app::kSchedulePrincipal);
        CHECK(drafted[0].client == lab.client);
        id = drafted[0].id;
    }
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25, 10)};
    static_cast<void>(lims::test::dispatchAs<lims::InvoiceModel>("bea", lims::SubmitInvoice{.invoiceId = id}));
    lims::InvoiceModel invoices;
    {
        ScopedPrincipal const bea{"bea"};
        CHECK_THROWS_WITH(::morph::model::executeChecked(invoices, lims::ApproveInvoice{.invoiceId = id}),
                          Catch::Matchers::ContainsSubstring("submitted this invoice"));
    }
    ScopedPrincipal const carl{"carl"};
    auto const approved = invoices.execute(lims::ApproveInvoice{.invoiceId = id});
    CHECK(approved.state == lims::InvoiceState::Approved);
    CHECK(approved.submittedBy == "bea");
    CHECK(approved.approvedBy == "carl");
}

namespace {

/// @brief Sets @p principal's value limit at the seeded company to @p amount, as the supervisor.
void setLimit(std::string principal, Rational amount) {
    ScopedPrincipal const sam{"sam"};
    lims::OrgModel{}.execute(lims::SetValueLimit{
        .principal = std::move(principal), .company = lims::test::seededCompany(), .limit = lims::Amount{amount}});
}

}  // namespace

// Mutations: compare with >= (a limit equal to the gross is refused); read a missing limit as unlimited (dora
// approves); compare the deadline with > (paula still approves at the deadline instant).
TEST_CASE("invoice approval: the edges of the limit and of the deadline", "[lims][billing][approval]") {
    lims::test::LabFixture const fixture;
    auto const invoice = lims::test::draftedInvoice();
    lims::test::submit(invoice);  // the gross is 47.60, the deadline 09:00 on 12 November
    lims::InvoiceModel invoices;
    SECTION("a limit equal to the gross allows") {
        setLimit("paula", Rational{morph::math::Numerator{4760}, morph::math::Denominator{100}, DecimalPlaces{2}});
        ScopedClockOverride const clock{lims::test::at(2026, 11, 11)};
        ScopedPrincipal const paula{"paula"};
        CHECK(invoices.execute(lims::ApproveInvoice{.invoiceId = invoice.id}).state == lims::InvoiceState::Approved);
    }
    SECTION("a limit a cent below the gross refuses") {
        setLimit("paula", Rational{morph::math::Numerator{4759}, morph::math::Denominator{100}, DecimalPlaces{2}});
        ScopedClockOverride const clock{lims::test::at(2026, 11, 11)};
        ScopedPrincipal const paula{"paula"};
        CHECK_THROWS_WITH(::morph::model::executeChecked(invoices, lims::ApproveInvoice{.invoiceId = invoice.id}),
                          Catch::Matchers::ContainsSubstring("value limit"));
    }
    SECTION("an approver with the role and no limit is refused") {
        lims::test::grant("dora", lims::LimsRole::InvoiceApprover, lims::Scope{lims::test::seededSite()});
        ScopedClockOverride const clock{lims::test::at(2026, 11, 11)};
        ScopedPrincipal const dora{"dora"};
        CHECK_THROWS_WITH(::morph::model::executeChecked(invoices, lims::ApproveInvoice{.invoiceId = invoice.id}),
                          Catch::Matchers::ContainsSubstring("has no value limit"));
        CHECK(invoices.execute(lims::ListPendingApprovals{}).invoices.empty());
    }
    SECTION("at the deadline instant the approval is at the company") {
        ScopedClockOverride const clock{lims::test::at(2026, 11, 12)};
        {
            ScopedPrincipal const paula{"paula"};
            CHECK_THROWS_WITH(::morph::model::executeChecked(invoices, lims::ApproveInvoice{.invoiceId = invoice.id}),
                              Catch::Matchers::ContainsSubstring("does not hold the invoice-approver role"));
            CHECK(invoices.execute(lims::ListPendingApprovals{}).invoices.empty());
        }
        ScopedPrincipal const carl{"carl"};
        CHECK(invoices.execute(lims::GetInvoice{.invoiceId = invoice.id}).level == lims::ApprovalLevel::Company);
    }
}

// A company whose base currency has no decimals: the gross and the limits are whole yen, and the limit equal to the
// gross allows. Mutation: compare with >= (carl's limit equals the gross and is refused).
TEST_CASE("invoice approval: a company billing in yen is judged against whole-yen limits",
          "[lims][billing][approval]") {
    lims::test::LabFixture const fixture;
    auto const invoice = lims::test::draftedInvoice();
    execute("UPDATE org_companies SET base_currency = " + std::to_string(static_cast<int>(lims::CurrencyCode::JPY)));
    execute("UPDATE billing_invoices SET currency = " + std::to_string(static_cast<int>(lims::CurrencyCode::JPY)) +
            ", gross_num = 5000, gross_den = 1");
    setLimit("paula", Rational{4999, DecimalPlaces{0}});
    setLimit("carl", Rational{5000, DecimalPlaces{0}});
    lims::test::submit(invoice);
    ScopedClockOverride const clock{lims::test::at(2026, 11, 11)};
    lims::InvoiceModel invoices;
    {
        ScopedPrincipal const paula{"paula"};
        CHECK_THROWS_WITH(::morph::model::executeChecked(invoices, lims::ApproveInvoice{.invoiceId = invoice.id}),
                          Catch::Matchers::ContainsSubstring("value limit"));
    }
    ScopedPrincipal const carl{"carl"};
    CHECK(invoices.execute(lims::ApproveInvoice{.invoiceId = invoice.id}).state == lims::InvoiceState::Approved);
}

// The list is prefiltered by the companies the caller sees, and the verdict refuses the rest too; this pins the
// outcome, which no single mutation of either alone changes.
TEST_CASE("invoice approval: the pending list holds nothing of a company the caller cannot see",
          "[lims][billing][approval]") {
    lims::test::LabFixture const fixture;
    auto const invoice = lims::test::draftedInvoice();
    lims::test::submit(invoice);
    ScopedClockOverride const clock{lims::test::at(2026, 11, 11)};
    lims::InvoiceModel invoices;
    {
        ScopedPrincipal const carl{"carl"};
        CHECK(invoices.execute(lims::ListPendingApprovals{}).invoices.size() == 1U);
    }
    lims::CompanyId other;
    {
        ScopedPrincipal const sam{"sam"};
        other = lims::OrgModel{}
                    .execute(lims::CreateCompany{.name = "Elsewhere",
                                                 .baseCurrency = lims::CurrencyCode::EUR,
                                                 .fiscalYearStart = lims::Month::January})
                    .id;
    }
    execute("UPDATE billing_invoices SET company_id = " + std::to_string(*other));
    ScopedPrincipal const carl{"carl"};
    CHECK(invoices.execute(lims::ListPendingApprovals{}).invoices.empty());
}
