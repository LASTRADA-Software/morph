// SPDX-License-Identifier: Apache-2.0
//
// The ledger's chart of accounts and its monthly periods, per company.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <morph/session/session_auth.hpp>
#include <string>
#include <string_view>

#include "lims/ledger/models/ledger_model.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;

namespace {

constexpr std::string_view kSecret = "lims-ledger-chart-test-secret-at-least-32b";

/// @brief Creates account @p code of @p type at the seeded company, as the current principal.
/// @param ledger The model.
/// @param code The code.
/// @param type The type.
/// @return The account.
lims::AccountView account(lims::LedgerModel& ledger, std::string code, lims::AccountType type) {
    return ledger.execute(lims::CreateAccount{
        .company = lims::test::seededCompany(), .code = code, .name = "Account " + code, .type = type});
}

}  // namespace

TEST_CASE("calendar: months, labels and fiscal years are UTC calendar facts", "[lims][ledger]") {
    auto const feb = morph::time::Timestamp{lims::test::at(2027, 2, 10)};
    CHECK(lims::monthLabel(lims::calendarMonthOf(feb)) == "2027-02");
    CHECK(lims::dayOfMonth(feb) == 10U);
    CHECK(lims::fiscalYearOf(feb, lims::Month::April) == 2026);
    CHECK(lims::fiscalYearOf(feb, lims::Month::January) == 2027);
    CHECK(lims::fiscalYearOf(morph::time::Timestamp{lims::test::at(2026, 4, 1, 0)}, lims::Month::April) == 2026);
    CHECK(lims::monthStartMillis(lims::calendarMonthOf(feb)) ==
          lims::millisOf(morph::time::Timestamp{lims::test::at(2027, 2, 1, 0)}));
}

// Mutation: drop the per-company filter from ListAccounts (dave, an accountant at Osaka only, sees the seeded
// company's accounts).
TEST_CASE("ledger chart: an accountant keeps accounts per company, each code once", "[lims][ledger]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Accountant);
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    lims::LedgerModel ledger;
    lims::CompanyId osaka;
    {
        ScopedPrincipal const alice{"alice"};
        osaka = lims::OrgModel{}
                    .execute(lims::CreateCompany{.name = "Osaka branch",
                                                 .baseCurrency = lims::CurrencyCode::JPY,
                                                 .fiscalYearStart = lims::Month::April})
                    .id;
        account(ledger, "1200", lims::AccountType::Asset);
        account(ledger, "2100", lims::AccountType::Liability);
        account(ledger, "3000", lims::AccountType::Equity);
        account(ledger, "4000", lims::AccountType::Revenue);
        account(ledger, "6000", lims::AccountType::Expense);
        CHECK_THROWS_AS(account(ledger, "1200", lims::AccountType::Asset), lims::Conflict);
        CHECK_THROWS_AS(account(ledger, "", lims::AccountType::Asset), lims::ValidationError);
        auto const listed = ledger.execute(lims::ListAccounts{.company = lims::test::seededCompany()});
        REQUIRE(listed.accounts.size() == 5U);
        CHECK(listed.accounts.front().code == "1200");
        CHECK(listed.accounts.back().type == lims::AccountType::Expense);
        lims::OrgModel{}.execute(
            lims::AssignRole{.principal = "dave", .role = lims::LimsRole::Accountant, .company = osaka, .site = {}});
    }
    ScopedPrincipal const dave{"dave"};
    CHECK(ledger.execute(lims::ListAccounts{.company = lims::test::seededCompany()}).accounts.empty());
    CHECK_THROWS_AS(account(ledger, "1300", lims::AccountType::Asset), lims::Forbidden);
}

// Mutation: let BeginClosePeriod accept a Closing period (the second begin succeeds).
TEST_CASE("ledger chart: a month is opened once and moves from open to closing", "[lims][ledger]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Accountant);
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const opened = ledger.execute(lims::OpenPeriod{
        .company = lims::test::seededCompany(), .month = morph::time::Timestamp{lims::test::at(2026, 11, 17)}});
    CHECK(opened.label == "2026-11");
    CHECK(opened.state == lims::PeriodState::Open);
    CHECK(opened.start == morph::time::Timestamp{lims::test::at(2026, 11, 1, 0)});
    CHECK_THROWS_AS(ledger.execute(lims::OpenPeriod{.company = lims::test::seededCompany(),
                                                    .month = morph::time::Timestamp{lims::test::at(2026, 11, 2)}}),
                    lims::Conflict);
    auto const closing = ledger.execute(lims::BeginClosePeriod{.periodId = opened.id});
    CHECK(closing.state == lims::PeriodState::Closing);
    CHECK_THROWS_AS(ledger.execute(lims::BeginClosePeriod{.periodId = opened.id}), lims::IllegalTransition);
    CHECK_THROWS_AS(ledger.execute(lims::BeginClosePeriod{.periodId = lims::PeriodId{999}}), lims::NotFound);
    CHECK(ledger.execute(lims::ListPeriods{.company = lims::test::seededCompany()}).periods.size() == 1U);
}

TEST_CASE("LedgerModel over the full backend-mode matrix: an account and a period", "[lims][ledger][matrix]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Accountant);
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    BackendRig rig{mode, 1, lims::test::limsAuthorizer(kSecret)};
    rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "alice"));
    auto ledger = rig.client<lims::LedgerModel>(0);
    auto const created = awaitQt(ledger.execute(lims::CreateAccount{.company = lims::test::seededCompany(),
                                                                    .code = "1200",
                                                                    .name = "Receivables",
                                                                    .type = lims::AccountType::Asset}));
    CHECK(created.code == "1200");
    auto const period = awaitQt(ledger.execute(lims::OpenPeriod{
        .company = lims::test::seededCompany(), .month = morph::time::Timestamp{lims::test::at(2026, 11, 1)}}));
    CHECK(period.label == "2026-11");
    CHECK(awaitQt(ledger.execute(lims::ListAccounts{.company = lims::test::seededCompany()})).accounts.size() == 1U);
}
