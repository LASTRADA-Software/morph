// SPDX-License-Identifier: Apache-2.0
//
// The ledger's chart of accounts and its monthly periods, per company.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <morph/core/action_guard.hpp>
#include <morph/session/session_auth.hpp>
#include <string>
#include <string_view>

#include "lims/ledger/db/ledger_entity.hpp"
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
        CHECK_THROWS_AS(account(ledger, "ab12", lims::AccountType::Asset), lims::ValidationError);
        CHECK_THROWS_AS(account(ledger, "1200a", lims::AccountType::Asset), lims::ValidationError);
        auto const listed = ledger.execute(lims::ListAccounts{.company = lims::test::seededCompany()});
        REQUIRE(listed.accounts.size() == 5U);
        CHECK(listed.accounts.front().code == "1200");
        CHECK(listed.accounts.back().type == lims::AccountType::Expense);
        lims::OrgModel{}.execute(
            lims::AssignRole{.principal = "dave", .role = lims::LimsRole::Accountant, .company = osaka, .site = {}});
    }
    ScopedPrincipal const dave{"dave"};
    CHECK(ledger.execute(lims::ListAccounts{.company = lims::test::seededCompany()}).accounts.empty());
    CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::CreateAccount{.company = lims::test::seededCompany(),
                                                                               .code = "1300",
                                                                               .name = "Account 1300",
                                                                               .type = lims::AccountType::Asset}),
                    lims::Forbidden);
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
    CHECK_THROWS_AS(::morph::model::executeChecked(
                        ledger, lims::OpenPeriod{.company = lims::test::seededCompany(),
                                                 .month = morph::time::Timestamp{lims::test::at(2026, 11, 2)}}),
                    lims::Conflict);
    auto const closing = ledger.execute(lims::BeginClosePeriod{.periodId = opened.id});
    CHECK(closing.state == lims::PeriodState::Closing);
    CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::BeginClosePeriod{.periodId = opened.id}),
                    lims::IllegalTransition);
    CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::BeginClosePeriod{.periodId = lims::PeriodId{999}}),
                    lims::Forbidden);
    CHECK(ledger.execute(lims::ListPeriods{.company = lims::test::seededCompany()}).periods.size() == 1U);
}

// Mutation: remove the unique index from the migration (both direct duplicate inserts succeed).
TEST_CASE("ledger chart: the database refuses a duplicate code or month on its own", "[lims][ledger]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Accountant);
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    account(ledger, "1200", lims::AccountType::Asset);
    auto const period = ledger.execute(lims::OpenPeriod{.company = lims::test::seededCompany(),
                                                        .month = morph::time::Timestamp{lims::test::at(2026, 11, 3)}});

    Lightweight::DataMapper mapper;
    lims::db::AccountRecord duplicateAccount;
    duplicateAccount.companyId = *lims::test::seededCompany();
    duplicateAccount.code = Lightweight::SqlAnsiString<16>{"1200"};
    duplicateAccount.name = Lightweight::SqlAnsiString<128>{"Again"};
    CHECK_THROWS(mapper.Create(duplicateAccount));

    lims::db::PeriodRecord duplicatePeriod;
    duplicatePeriod.companyId = *lims::test::seededCompany();
    duplicatePeriod.startMs = lims::millisOf(period.start);
    CHECK_THROWS(mapper.Create(duplicatePeriod));

    // The model still answers with the rung's typed error, ahead of the index.
    CHECK_THROWS_AS(account(ledger, "1200", lims::AccountType::Asset), lims::Conflict);
    CHECK_THROWS_AS(::morph::model::executeChecked(
                        ledger, lims::OpenPeriod{.company = lims::test::seededCompany(),
                                                 .month = morph::time::Timestamp{lims::test::at(2026, 11, 20)}}),
                    lims::Conflict);
}

// Mutations, each marked at the check it breaks: delete `requireAuthorized(action)` from OpenPeriod or from
// BeginClosePeriod; make periodCompany return the seeded company for every period.
TEST_CASE("ledger chart: roles gate periods per company, and a stranger learns nothing", "[lims][ledger]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Accountant);
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    lims::LedgerModel ledger;
    lims::CompanyId osaka;
    lims::PeriodView seededPeriod;
    lims::PeriodView osakaPeriod;
    {
        ScopedPrincipal const alice{"alice"};
        osaka = lims::OrgModel{}
                    .execute(lims::CreateCompany{.name = "Osaka branch",
                                                 .baseCurrency = lims::CurrencyCode::JPY,
                                                 .fiscalYearStart = lims::Month::April})
                    .id;
        seededPeriod = ledger.execute(lims::OpenPeriod{.company = lims::test::seededCompany(),
                                                       .month = morph::time::Timestamp{lims::test::at(2026, 10, 1)}});
        lims::OrgModel{}.execute(
            lims::AssignRole{.principal = "dave", .role = lims::LimsRole::Accountant, .company = osaka, .site = {}});
        lims::OrgModel{}.execute(lims::AssignRole{.principal = "erin",
                                                  .role = lims::LimsRole::Accountant,
                                                  .company = lims::test::seededCompany(),
                                                  .site = {}});
    }
    auto const month = morph::time::Timestamp{lims::test::at(2026, 12, 1)};
    {
        ScopedPrincipal const dave{"dave"};
        osakaPeriod = ledger.execute(
            lims::OpenPeriod{.company = osaka, .month = morph::time::Timestamp{lims::test::at(2026, 10, 1)}});
        // Mutation: delete requireAuthorized(action) from OpenPeriod.
        CHECK_THROWS_AS(::morph::model::executeChecked(
                            ledger, lims::OpenPeriod{.company = lims::test::seededCompany(), .month = month}),
                        lims::Forbidden);
        // Mutation: delete requireAuthorized(action) from BeginClosePeriod, or make periodCompany answer the
        // seeded company for every period.
        CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::BeginClosePeriod{.periodId = seededPeriod.id}),
                        lims::Forbidden);
        CHECK(ledger.execute(lims::BeginClosePeriod{.periodId = osakaPeriod.id}).state == lims::PeriodState::Closing);
        // Only visible companies' periods are listed.
        CHECK(ledger.execute(lims::ListPeriods{.company = lims::test::seededCompany()}).periods.empty());
        CHECK(ledger.execute(lims::ListPeriods{.company = osaka}).periods.size() == 1U);
    }
    {
        // Mutation: make periodCompany return the seeded company for every period (erin may then close Osaka's).
        ScopedPrincipal const erin{"erin"};
        CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::BeginClosePeriod{.periodId = osakaPeriod.id}),
                        lims::Forbidden);
        CHECK(ledger.execute(lims::ListPeriods{.company = osaka}).periods.empty());
        CHECK(ledger.execute(lims::ListPeriods{.company = lims::test::seededCompany()}).periods.size() == 1U);
        // A role at the right company reveals which ids exist.
        CHECK_THROWS_AS(
            ::morph::model::executeChecked(ledger, lims::BeginClosePeriod{.periodId = lims::PeriodId{9999}}),
            lims::Forbidden);
        CHECK_THROWS_AS(
            ::morph::model::executeChecked(ledger, lims::OpenPeriod{.company = lims::CompanyId{9999}, .month = month}),
            lims::Forbidden);
        CHECK_THROWS_AS(::morph::model::executeChecked(
                            ledger,
                            lims::CreateAccount{
                                .company = osaka, .code = "1300", .name = "Cash", .type = lims::AccountType::Asset}),
                        lims::Forbidden);
    }
}

TEST_CASE("ledger chart: a principal with no role is Forbidden, not told what exists", "[lims][ledger]") {
    DbFixture fixture;
    lims::LedgerModel ledger;
    ScopedPrincipal const nobody{"nobody"};
    CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::BeginClosePeriod{.periodId = lims::PeriodId{9999}}),
                    lims::Forbidden);
    CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::CreateAccount{.company = lims::CompanyId{9999},
                                                                               .code = "1200",
                                                                               .name = "Receivables",
                                                                               .type = lims::AccountType::Asset}),
                    lims::Forbidden);
    CHECK_THROWS_AS(::morph::model::executeChecked(
                        ledger, lims::OpenPeriod{.company = lims::CompanyId{9999},
                                                 .month = morph::time::Timestamp{lims::test::at(2026, 12, 1)}}),
                    lims::Forbidden);
}

// Mutation: delete the validate() check from OpenPeriod or BeginClosePeriod.
TEST_CASE("ledger chart: malformed period requests are ValidationErrors", "[lims][ledger]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Accountant);
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    CHECK_THROWS_AS(ledger.execute(lims::OpenPeriod{.company = lims::CompanyId{},
                                                    .month = morph::time::Timestamp{lims::test::at(2026, 12, 1)}}),
                    lims::ValidationError);
    CHECK_THROWS_AS(ledger.execute(lims::OpenPeriod{.company = lims::test::seededCompany(), .month = {}}),
                    lims::ValidationError);
    CHECK_THROWS_AS(ledger.execute(lims::BeginClosePeriod{.periodId = lims::PeriodId{}}), lims::ValidationError);
    CHECK_THROWS_AS(ledger.execute(lims::ListPeriods{.company = lims::CompanyId{}}), lims::ValidationError);
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
    auto const closing = awaitQt(ledger.execute(lims::BeginClosePeriod{.periodId = period.id}));
    CHECK(closing.state == lims::PeriodState::Closing);
    auto const periods = awaitQt(ledger.execute(lims::ListPeriods{.company = lims::test::seededCompany()})).periods;
    REQUIRE(periods.size() == 1U);
    CHECK(periods.front().state == lims::PeriodState::Closing);
}
