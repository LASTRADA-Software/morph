// SPDX-License-Identifier: Apache-2.0
//
// Dated exchange rates, the three reports, and hypothesis 8's first half: totals at ERP magnitude are exact, and a
// total that does not fit is refused rather than saturated.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "lims/ledger/db/ledger_entity.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/ledger/models/ledger_report_model.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/money.hpp"
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
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

constexpr std::string_view kSecret = "lims-ledger-reports-test-secret-at-least-32b";
constexpr std::int64_t kHuge = 5'000'000'000'000'000'000;

/// @brief A whole-unit amount.
/// @param count The units.
/// @return The amount.
[[nodiscard]] lims::Amount units(std::int64_t count) { return lims::Amount{Rational{count, DecimalPlaces{2}}}; }

/// @brief A rate of @p num / @p den.
/// @param num The numerator.
/// @param den The denominator.
/// @return The rate.
[[nodiscard]] lims::Rate rate(std::int64_t num, std::int64_t den) {
    return lims::Rate{Rational{Numerator{num}, Denominator{den}, DecimalPlaces{6}}};
}

/// @brief The rate one.
/// @return It.
[[nodiscard]] lims::Rate one() { return lims::Rate{Rational{1, DecimalPlaces{6}}}; }

/// @brief A November 2026 day at 09:00 UTC.
/// @param day The day.
/// @return The instant.
[[nodiscard]] morph::time::Timestamp november(unsigned day) {
    return morph::time::Timestamp{lims::test::at(2026, 11, day)};
}

/// @brief alice an accountant at the seeded company with accounts 1200, 1300, 4000 and 4100, November 2026 open.
/// @return November's period.
lims::PeriodView books() {
    lims::test::grant("alice", lims::LimsRole::Accountant);
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    for (auto const& [code, type] :
         {std::pair{"1200", lims::AccountType::Asset}, std::pair{"1300", lims::AccountType::Asset},
          std::pair{"4000", lims::AccountType::Revenue}, std::pair{"4100", lims::AccountType::Revenue}}) {
        ledger.execute(lims::CreateAccount{.company = lims::test::seededCompany(),
                                           .code = code,
                                           .name = std::string{"Account "} + code,
                                           .type = type});
    }
    return ledger.execute(lims::OpenPeriod{.company = lims::test::seededCompany(), .month = november(1)});
}

/// @brief Records @p amount between two accounts in EUR.
/// @param key The source key.
/// @param amount The amount.
/// @param day The day of November.
/// @param debit The debited account.
/// @param credit The credited account.
void post(std::string const& key, lims::Amount const& amount, unsigned day = 10, std::string debit = "1200",
          std::string credit = "4000") {
    static_cast<void>(
        lims::LedgerModel::record(lims::PostingRequest{.company = lims::test::seededCompany(),
                                                       .date = november(day),
                                                       .sourceKey = key,
                                                       .sourceReference = key,
                                                       .currency = lims::CurrencyCode::EUR,
                                                       .rate = one(),
                                                       .lines = {lims::PostingLine{.accountCode = std::move(debit),
                                                                                   .side = lims::PostingSide::Debit,
                                                                                   .amount = amount,
                                                                                   .companyAmount = amount},
                                                                 lims::PostingLine{.accountCode = std::move(credit),
                                                                                   .side = lims::PostingSide::Credit,
                                                                                   .amount = amount,
                                                                                   .companyAmount = amount}}}));
}

/// @brief The seeded company's trial balance as of 30 November.
/// @return It.
[[nodiscard]] lims::TrialBalanceView trial() {
    return lims::LedgerReportModel{}.execute(
        lims::TrialBalance{.company = lims::test::seededCompany(), .asOf = november(30)});
}

/// @brief The frozen balance row of account @p code in @p period.
/// @param mapper The open mapper.
/// @param period The period.
/// @param code The account's code.
/// @return The row.
[[nodiscard]] lims::db::PeriodBalanceRecord frozenRow(Lightweight::DataMapper& mapper, lims::PeriodId period,
                                                      std::string const& code) {
    for (auto const& account : mapper.Query<lims::db::AccountRecord>()
                                   .Where(::Lightweight::FieldNameOf<&lims::db::AccountRecord::code>, "=", code)
                                   .All()) {
        auto rows = mapper.Query<lims::db::PeriodBalanceRecord>()
                        .Where(::Lightweight::FieldNameOf<&lims::db::PeriodBalanceRecord::period>, "=", *period)
                        .Where(::Lightweight::FieldNameOf<&lims::db::PeriodBalanceRecord::account>, "=",
                               static_cast<std::int64_t>(account.id.Value()))
                        .All();
        if (!rows.empty()) {
            return rows.front();
        }
    }
    FAIL("no frozen row for account " << code);
    return {};
}

}  // namespace

// Mutation: pick the earliest valid rate instead of the latest (the 15 November posting converts at 1.05).
TEST_CASE("ledger rates: a posting uses the rate valid on its date", "[lims][ledger][rates]") {
    DbFixture fixture;
    books();
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    for (auto const& [day, value] : {std::pair{1U, rate(105, 100)}, std::pair{15U, rate(92, 100)}}) {
        ledger.execute(lims::SetExchangeRate{.company = lims::test::seededCompany(),
                                             .from = lims::CurrencyCode::USD,
                                             .to = lims::CurrencyCode::EUR,
                                             .validFrom = morph::time::Timestamp{lims::test::at(2026, 11, day, 0)},
                                             .rate = value});
    }
    auto const on = [](unsigned day) {
        return lims::LedgerModel::rateOn(lims::test::seededCompany(), lims::CurrencyCode::USD, lims::CurrencyCode::EUR,
                                         november(day));
    };
    CHECK(on(14) == Rational{Numerator{105}, Denominator{100}, DecimalPlaces{6}});
    CHECK(on(15) == Rational{Numerator{92}, Denominator{100}, DecimalPlaces{6}});
    CHECK(on(30) == Rational{Numerator{92}, Denominator{100}, DecimalPlaces{6}});
    CHECK_FALSE(lims::LedgerModel::rateOn(lims::test::seededCompany(), lims::CurrencyCode::USD,
                                          lims::CurrencyCode::EUR,
                                          morph::time::Timestamp{lims::test::at(2026, 10, 31)})
                    .has_value());

    // A USD posting on 20 November must state 0.92, and its company values are the poster's rounding.
    lims::PostingRequest usd{.company = lims::test::seededCompany(),
                             .date = november(20),
                             .sourceKey = "billing.invoice:1",
                             .sourceReference = "invoice 1",
                             .currency = lims::CurrencyCode::USD,
                             .rate = rate(105, 100),
                             .lines = {lims::PostingLine{.accountCode = "1200",
                                                         .side = lims::PostingSide::Debit,
                                                         .amount = units(100),
                                                         .companyAmount = units(92)},
                                       lims::PostingLine{.accountCode = "4000",
                                                         .side = lims::PostingSide::Credit,
                                                         .amount = units(100),
                                                         .companyAmount = units(92)}}};
    CHECK_THROWS_AS(lims::LedgerModel::record(usd), lims::ValidationError);
    usd.rate = rate(92, 100);
    CHECK(lims::LedgerModel::record(usd).entryId.hasValue());
    // On 10 November the same request is at the 1.05 rate, so 0.92 is no longer the dated one.
    usd.date = november(10);
    usd.sourceKey = "billing.invoice:2";
    CHECK_THROWS_AS(lims::LedgerModel::record(usd), lims::ValidationError);
}

// Mutations: drop the company filter from rateOn (the Osaka rate answers for the seeded company); drop the currency
// filters (the EUR/USD rate answers for USD/EUR); drop the engaged-argument guard.
TEST_CASE("ledger rates: a rate belongs to a company and a direction", "[lims][ledger][rates]") {
    DbFixture fixture;
    books();
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
        lims::OrgModel{}.execute(
            lims::AssignRole{.principal = "dave", .role = lims::LimsRole::Accountant, .company = osaka, .site = {}});
    }
    ScopedPrincipal const dave{"dave"};
    ledger.execute(lims::SetExchangeRate{.company = osaka,
                                         .from = lims::CurrencyCode::USD,
                                         .to = lims::CurrencyCode::JPY,
                                         .validFrom = morph::time::Timestamp{lims::test::at(2026, 11, 1, 0)},
                                         .rate = rate(150, 1)});
    CHECK(lims::LedgerModel::rateOn(osaka, lims::CurrencyCode::USD, lims::CurrencyCode::JPY, november(10)) ==
          Rational{Numerator{150}, Denominator{1}, DecimalPlaces{6}});
    CHECK_FALSE(lims::LedgerModel::rateOn(lims::test::seededCompany(), lims::CurrencyCode::USD,
                                          lims::CurrencyCode::JPY, november(10))
                    .has_value());
    CHECK_FALSE(
        lims::LedgerModel::rateOn(osaka, lims::CurrencyCode::JPY, lims::CurrencyCode::USD, november(10)).has_value());
    CHECK_FALSE(
        lims::LedgerModel::rateOn(osaka, lims::CurrencyCode::USD, lims::CurrencyCode::EUR, november(10)).has_value());
    CHECK_FALSE(
        lims::LedgerModel::rateOn(lims::CompanyId{}, lims::CurrencyCode::USD, lims::CurrencyCode::JPY, november(10))
            .has_value());
    CHECK_FALSE(
        lims::LedgerModel::rateOn(osaka, lims::CurrencyCode::USD, lims::CurrencyCode::JPY, morph::time::Timestamp{})
            .has_value());
}

// Mutations: delete validate() from SetExchangeRate; delete requireRoleSomewhere or requireAuthorized; make the
// company lookup answer for any id; drop the duplicate-start check (the second set succeeds, then the unique index
// refuses it with a database error instead of Conflict); drop the unique index; drop the visibility filter from
// ListExchangeRates.
TEST_CASE("ledger rates: entering one is an accountant's, at its company, and each start is entered once",
          "[lims][ledger][rates]") {
    DbFixture fixture;
    books();
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    lims::CompanyId osaka;
    {
        ScopedPrincipal const alice{"alice"};
        osaka = lims::OrgModel{}
                    .execute(lims::CreateCompany{.name = "Osaka branch",
                                                 .baseCurrency = lims::CurrencyCode::JPY,
                                                 .fiscalYearStart = lims::Month::April})
                    .id;
        lims::OrgModel{}.execute(
            lims::AssignRole{.principal = "dave", .role = lims::LimsRole::Accountant, .company = osaka, .site = {}});
    }
    auto const set = [](lims::CompanyId company, lims::Rate value, unsigned day = 1) {
        return lims::SetExchangeRate{.company = company,
                                     .from = lims::CurrencyCode::USD,
                                     .to = lims::CurrencyCode::EUR,
                                     .validFrom = morph::time::Timestamp{lims::test::at(2026, 11, day, 0)},
                                     .rate = value};
    };
    lims::LedgerModel ledger;
    {
        ScopedPrincipal const nobody{"nobody"};
        CHECK_THROWS_AS(::morph::model::executeChecked(ledger, set(lims::test::seededCompany(), rate(1, 1))),
                        lims::Forbidden);
        CHECK_THROWS_AS(::morph::model::executeChecked(ledger, set(lims::CompanyId{9999}, rate(1, 1))),
                        lims::Forbidden);
    }
    {
        ScopedPrincipal const dave{"dave"};
        CHECK_THROWS_AS(::morph::model::executeChecked(ledger, set(lims::test::seededCompany(), rate(1, 1))),
                        lims::Forbidden);
        CHECK_THROWS_AS(::morph::model::executeChecked(ledger, set(lims::CompanyId{9999}, rate(1, 1))),
                        lims::Forbidden);
    }
    ScopedPrincipal const alice{"alice"};
    SECTION("a malformed request is a ValidationError") {
        CHECK_THROWS_AS(ledger.execute(set(lims::CompanyId{}, rate(92, 100))), lims::ValidationError);
        CHECK_THROWS_AS(ledger.execute(set(lims::test::seededCompany(), lims::Rate{})), lims::ValidationError);
        CHECK_THROWS_AS(ledger.execute(set(lims::test::seededCompany(), rate(0, 1))), lims::ValidationError);
        CHECK_THROWS_AS(ledger.execute(set(lims::test::seededCompany(), rate(-92, 100))), lims::ValidationError);
        auto noStart = set(lims::test::seededCompany(), rate(92, 100));
        noStart.validFrom = morph::time::Timestamp{};
        CHECK_THROWS_AS(ledger.execute(noStart), lims::ValidationError);
        auto samePair = set(lims::test::seededCompany(), rate(1, 1));
        samePair.from = lims::CurrencyCode::EUR;
        CHECK_THROWS_AS(ledger.execute(samePair), lims::ValidationError);
        CHECK(ledger.execute(lims::ListExchangeRates{.company = lims::test::seededCompany()}).rates.empty());
    }
    SECTION("each company, pair and start is entered once") {
        auto const first = ledger.execute(set(lims::test::seededCompany(), rate(92, 100)));
        CHECK(first.id.hasValue());
        CHECK(first.setBy == "alice");
        CHECK(first.from == lims::CurrencyCode::USD);
        CHECK(*first.rate == Rational{Numerator{92}, Denominator{100}, DecimalPlaces{6}});
        CHECK_THROWS_AS(::morph::model::executeChecked(ledger, set(lims::test::seededCompany(), rate(93, 100))),
                        lims::Conflict);
        // Another start, another direction and another company are other rows.
        CHECK_NOTHROW(ledger.execute(set(lims::test::seededCompany(), rate(93, 100), 2)));
        auto reverse = set(lims::test::seededCompany(), rate(109, 100));
        reverse.from = lims::CurrencyCode::EUR;
        reverse.to = lims::CurrencyCode::USD;
        CHECK_NOTHROW(ledger.execute(reverse));
        {
            ScopedPrincipal const dave{"dave"};
            CHECK_NOTHROW(ledger.execute(set(osaka, rate(92, 100))));
        }
        auto const listed = ledger.execute(lims::ListExchangeRates{.company = lims::test::seededCompany()});
        REQUIRE(listed.rates.size() == 3U);
        CHECK(listed.rates[0].from == lims::CurrencyCode::EUR);
        CHECK(*listed.rates[1].rate == Rational{Numerator{92}, Denominator{100}, DecimalPlaces{6}});
        CHECK(*listed.rates[2].rate == Rational{Numerator{93}, Denominator{100}, DecimalPlaces{6}});
        // The unique index holds when the model is bypassed.
        Lightweight::DataMapper mapper;
        lims::db::ExchangeRateRecord duplicate;
        duplicate.companyId = *lims::test::seededCompany();
        duplicate.fromCurrency = static_cast<int>(lims::CurrencyCode::USD);
        duplicate.toCurrency = static_cast<int>(lims::CurrencyCode::EUR);
        duplicate.validFromMs = lims::millisOf(morph::time::Timestamp{lims::test::at(2026, 11, 1, 0)});
        CHECK_THROWS(mapper.Create(duplicate));
    }
    SECTION("a list shows only a company the caller can see") {
        {
            ScopedPrincipal const dave{"dave"};
            ledger.execute(set(osaka, rate(92, 100)));
            CHECK(ledger.execute(lims::ListExchangeRates{.company = osaka}).rates.size() == 1U);
            CHECK(ledger.execute(lims::ListExchangeRates{.company = lims::test::seededCompany()}).rates.empty());
        }
        ScopedPrincipal const stranger{"stranger"};
        CHECK(ledger.execute(lims::ListExchangeRates{.company = osaka}).rates.empty());
        CHECK_THROWS_AS(ledger.execute(lims::ListExchangeRates{}), lims::ValidationError);
    }
}

// Mutation: rethrow the SqlException from LedgerModel::enterRate instead of answering Conflict.
// The winner is another connection (a thread) that enters the same start between the loser's lookup and its insert,
// so the loser's insert meets the unique index and its rolled-back transaction is followed by a lookup that finds the
// winner's committed row.
TEST_CASE("ledger rates: a start entered by a concurrent call between the lookup and the insert is a Conflict",
          "[lims][ledger][rates]") {
    DbFixture fixture;
    books();
    ScopedPrincipal const alice{"alice"};
    lims::SetExchangeRate const request{.company = lims::test::seededCompany(),
                                        .from = lims::CurrencyCode::USD,
                                        .to = lims::CurrencyCode::EUR,
                                        .validFrom = morph::time::Timestamp{lims::test::at(2026, 11, 1, 0)},
                                        .rate = rate(92, 100)};
    bool winnerEntered = false;
    auto const winner = [&] {
        std::thread other{[&] {
            static_cast<void>(lims::LedgerModel::enterRate(lims::SetExchangeRate{.company = request.company,
                                                                                 .from = request.from,
                                                                                 .to = request.to,
                                                                                 .validFrom = request.validFrom,
                                                                                 .rate = rate(93, 100)},
                                                           "winner", {}));
            winnerEntered = true;
        }};
        other.join();
    };
    CHECK_THROWS_AS(static_cast<void>(lims::LedgerModel::enterRate(request, "alice", winner)), lims::Conflict);
    REQUIRE(winnerEntered);
    auto const listed = lims::LedgerModel{}.execute(lims::ListExchangeRates{.company = lims::test::seededCompany()});
    REQUIRE(listed.rates.size() == 1U);
    CHECK(listed.rates.front().setBy == "winner");
    CHECK(*listed.rates.front().rate == Rational{Numerator{93}, Denominator{100}, DecimalPlaces{6}});
}

// Mutation: include drafts in TrialBalance (the draft's 7 units appear in the totals).
TEST_CASE("ledger reports: trial balance, account balance and account ledger agree, over posted entries only",
          "[lims][ledger][reports]") {
    DbFixture fixture;
    books();
    post("a", units(100), 3);
    post("b", units(250), 12);
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel{}.execute(lims::CreateDraftEntry{.company = lims::test::seededCompany(),
                                                       .date = november(13),
                                                       .memo = "draft",
                                                       .currency = lims::CurrencyCode::EUR,
                                                       .rate = one(),
                                                       .lines = {lims::PostingLine{.accountCode = "1200",
                                                                                   .side = lims::PostingSide::Debit,
                                                                                   .amount = units(7),
                                                                                   .companyAmount = units(7)},
                                                                 lims::PostingLine{.accountCode = "4000",
                                                                                   .side = lims::PostingSide::Credit,
                                                                                   .amount = units(7),
                                                                                   .companyAmount = units(7)}}});
    lims::LedgerReportModel reports;
    auto const view = trial();
    CHECK(*view.totalDebit == Rational{350, DecimalPlaces{2}});
    CHECK(*view.totalCredit == *view.totalDebit);
    REQUIRE(view.rows.size() == 2U);
    CHECK(view.rows[0].accountCode == "1200");
    CHECK(view.rows[0].accountName == "Account 1200");
    CHECK(view.rows[0].type == lims::AccountType::Asset);
    CHECK(*view.rows[0].debit == Rational{350, DecimalPlaces{2}});
    CHECK(*view.rows[0].credit == Rational{0, DecimalPlaces{2}});
    CHECK(view.rows[1].accountCode == "4000");
    CHECK(*view.rows[1].credit == Rational{350, DecimalPlaces{2}});

    // As of the 5th only the first posting counts, on both ends of the range.
    auto const early =
        reports.execute(lims::TrialBalance{.company = lims::test::seededCompany(), .asOf = november(5)});
    CHECK(*early.totalDebit == Rational{100, DecimalPlaces{2}});
    auto const asOfFifth = reports.execute(
        lims::AccountBalance{.company = lims::test::seededCompany(), .accountCode = "1200", .asOf = november(5)});
    CHECK(*asOfFifth.debit == Rational{100, DecimalPlaces{2}});
    CHECK(*asOfFifth.credit == Rational{0, DecimalPlaces{2}});
    CHECK(*asOfFifth.balance == Rational{100, DecimalPlaces{2}});
    // The instant itself is counted.
    auto const onTheDay = reports.execute(
        lims::AccountBalance{.company = lims::test::seededCompany(), .accountCode = "1200", .asOf = november(12)});
    CHECK(*onTheDay.balance == Rational{350, DecimalPlaces{2}});

    auto const ledgerLines =
        reports.execute(lims::AccountLedger{.company = lims::test::seededCompany(),
                                            .accountCode = "4000",
                                            .from = morph::time::Timestamp{lims::test::at(2026, 11, 1, 0)},
                                            .to = november(30)});
    REQUIRE(ledgerLines.lines.size() == 2U);
    CHECK(ledgerLines.lines[0].sourceReference == "a");
    CHECK(ledgerLines.lines[0].date == november(3));
    CHECK(ledgerLines.lines[1].side == lims::PostingSide::Credit);
    CHECK(*ledgerLines.lines[0].balance == Rational{-100, DecimalPlaces{2}});
    CHECK(*ledgerLines.lines[1].balance == Rational{-350, DecimalPlaces{2}});
    CHECK(*ledgerLines.lines[1].companyAmount == Rational{250, DecimalPlaces{2}});

    // A range that starts later begins from what was posted before it.
    auto const later = reports.execute(lims::AccountLedger{
        .company = lims::test::seededCompany(), .accountCode = "1200", .from = november(10), .to = november(30)});
    REQUIRE(later.lines.size() == 1U);
    CHECK(*later.lines[0].balance == Rational{350, DecimalPlaces{2}});
    CHECK(later.lines[0].entryId.hasValue());
}

// Mutations: count a foreign-currency line at its transaction amount; leave the reversal's flipped side out of the
// totals.
TEST_CASE("ledger reports: a foreign line counts at its company value, and a reversal nets an entry out",
          "[lims][ledger][reports]") {
    DbFixture fixture;
    books();
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    ledger.execute(lims::SetExchangeRate{.company = lims::test::seededCompany(),
                                         .from = lims::CurrencyCode::USD,
                                         .to = lims::CurrencyCode::EUR,
                                         .validFrom = morph::time::Timestamp{lims::test::at(2026, 11, 1, 0)},
                                         .rate = rate(92, 100)});
    auto const posted =
        lims::LedgerModel::record(lims::PostingRequest{.company = lims::test::seededCompany(),
                                                       .date = november(10),
                                                       .sourceKey = "usd",
                                                       .sourceReference = "usd",
                                                       .currency = lims::CurrencyCode::USD,
                                                       .rate = rate(92, 100),
                                                       .lines = {lims::PostingLine{.accountCode = "1200",
                                                                                   .side = lims::PostingSide::Debit,
                                                                                   .amount = units(100),
                                                                                   .companyAmount = units(92)},
                                                                 lims::PostingLine{.accountCode = "4000",
                                                                                   .side = lims::PostingSide::Credit,
                                                                                   .amount = units(100),
                                                                                   .companyAmount = units(92)}}});
    CHECK(*trial().totalDebit == Rational{92, DecimalPlaces{2}});
    ledger.execute(lims::ReverseEntry{.entryId = posted.entryId, .date = november(11)});
    auto const after = trial();
    CHECK(*after.totalDebit == Rational{184, DecimalPlaces{2}});
    CHECK(*after.totalCredit == Rational{184, DecimalPlaces{2}});
    auto const balance = lims::LedgerReportModel{}.execute(
        lims::AccountBalance{.company = lims::test::seededCompany(), .accountCode = "1200", .asOf = november(30)});
    CHECK(*balance.balance == Rational{0, DecimalPlaces{2}});
}

// Mutations: ignore the frozen totals (the tampered figure is not reported); treat a period as frozen whose end is
// after the report's date (the 15 November figure reports the tampered total); skip the lines of a period that is
// not wholly inside the range.
TEST_CASE("ledger reports: a closed period inside the range reports its frozen totals", "[lims][ledger][reports]") {
    DbFixture fixture;
    auto const novemberPeriod = books();
    post("nov", units(100), 10);
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const december = ledger.execute(lims::OpenPeriod{
        .company = lims::test::seededCompany(), .month = morph::time::Timestamp{lims::test::at(2026, 12, 1)}});
    static_cast<void>(december);
    static_cast<void>(ledger.execute(lims::SetExchangeRate{.company = lims::test::seededCompany(),
                                                           .from = lims::CurrencyCode::USD,
                                                           .to = lims::CurrencyCode::EUR,
                                                           .validFrom = november(1),
                                                           .rate = rate(92, 100)}));
    static_cast<void>(
        lims::LedgerModel::record(lims::PostingRequest{.company = lims::test::seededCompany(),
                                                       .date = morph::time::Timestamp{lims::test::at(2026, 12, 5)},
                                                       .sourceKey = "dec",
                                                       .sourceReference = "dec",
                                                       .currency = lims::CurrencyCode::EUR,
                                                       .rate = one(),
                                                       .lines = {lims::PostingLine{.accountCode = "1200",
                                                                                   .side = lims::PostingSide::Debit,
                                                                                   .amount = units(10),
                                                                                   .companyAmount = units(10)},
                                                                 lims::PostingLine{.accountCode = "4000",
                                                                                   .side = lims::PostingSide::Credit,
                                                                                   .amount = units(10),
                                                                                   .companyAmount = units(10)}}}));
    ledger.execute(lims::BeginClosePeriod{.periodId = novemberPeriod.id});
    ledger.execute(lims::ClosePeriod{.periodId = novemberPeriod.id});

    // Before any tampering the frozen and the computed figures agree.
    lims::LedgerReportModel reports;
    auto const balanceOn = [&](unsigned month, unsigned day) {
        return reports.execute(lims::AccountBalance{.company = lims::test::seededCompany(),
                                                    .accountCode = "1200",
                                                    .asOf = morph::time::Timestamp{lims::test::at(2026, month, day)}});
    };
    CHECK(*balanceOn(12, 31).balance == Rational{110, DecimalPlaces{2}});

    // The report reads the frozen row: a figure changed there shows in a range that holds November whole, and not in
    // one that stops inside it (30 November at 09:00).
    {
        Lightweight::DataMapper mapper;
        auto row = frozenRow(mapper, novemberPeriod.id, "1200");
        row.debitNum = 5000;
        row.debitDen = 1;
        mapper.Update(row);
    }
    CHECK(*balanceOn(12, 31).balance == Rational{5010, DecimalPlaces{2}});
    CHECK(*balanceOn(12, 1).balance == Rational{5000, DecimalPlaces{2}});
    CHECK(*balanceOn(11, 30).balance == Rational{100, DecimalPlaces{2}});
    CHECK(*balanceOn(11, 15).balance == Rational{100, DecimalPlaces{2}});
    CHECK(*reports
               .execute(lims::TrialBalance{.company = lims::test::seededCompany(),
                                           .asOf = morph::time::Timestamp{lims::test::at(2026, 12, 1)}})
               .totalDebit == Rational{5000, DecimalPlaces{2}});
    // An account ledger starting in December opens with November's frozen balance.
    auto const decLines =
        reports.execute(lims::AccountLedger{.company = lims::test::seededCompany(),
                                            .accountCode = "1200",
                                            .from = morph::time::Timestamp{lims::test::at(2026, 12, 1, 0)},
                                            .to = morph::time::Timestamp{lims::test::at(2026, 12, 31)}});
    REQUIRE(decLines.lines.size() == 1U);
    CHECK(*decLines.lines[0].balance == Rational{5010, DecimalPlaces{2}});
    // The frozen figure is one side of one account: the credit side stays what was computed.
    CHECK(*reports
               .execute(lims::AccountBalance{.company = lims::test::seededCompany(),
                                             .accountCode = "4000",
                                             .asOf = morph::time::Timestamp{lims::test::at(2026, 12, 31)}})
               .credit == Rational{110, DecimalPlaces{2}});
}

// Mutation: format a report's amounts at a fixed two decimals instead of the company currency's.
TEST_CASE("ledger reports: a yen company reports whole yen", "[lims][ledger][reports]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    lims::CompanyId osaka;
    {
        ScopedPrincipal const alice{"alice"};
        osaka = lims::OrgModel{}
                    .execute(lims::CreateCompany{.name = "Osaka branch",
                                                 .baseCurrency = lims::CurrencyCode::JPY,
                                                 .fiscalYearStart = lims::Month::April})
                    .id;
        lims::OrgModel{}.execute(
            lims::AssignRole{.principal = "dave", .role = lims::LimsRole::Accountant, .company = osaka, .site = {}});
    }
    ScopedPrincipal const dave{"dave"};
    lims::LedgerModel ledger;
    ledger.execute(
        lims::CreateAccount{.company = osaka, .code = "1200", .name = "Cash", .type = lims::AccountType::Asset});
    ledger.execute(
        lims::CreateAccount{.company = osaka, .code = "4000", .name = "Sales", .type = lims::AccountType::Revenue});
    ledger.execute(lims::OpenPeriod{.company = osaka, .month = november(1)});
    lims::Amount const yen{Rational{Numerator{1500}, Denominator{1}, DecimalPlaces{0}}};
    static_cast<void>(lims::LedgerModel::record(lims::PostingRequest{
        .company = osaka,
        .date = november(10),
        .sourceKey = "yen",
        .sourceReference = "yen",
        .currency = lims::CurrencyCode::JPY,
        .rate = one(),
        .lines = {
            lims::PostingLine{
                .accountCode = "1200", .side = lims::PostingSide::Debit, .amount = yen, .companyAmount = yen},
            lims::PostingLine{
                .accountCode = "4000", .side = lims::PostingSide::Credit, .amount = yen, .companyAmount = yen}}}));
    lims::LedgerReportModel reports;
    auto const view = reports.execute(lims::TrialBalance{.company = osaka, .asOf = november(30)});
    CHECK(*view.totalDebit == Rational{1500, DecimalPlaces{0}});
    CHECK((*view.totalDebit).getDecimalPlaces() == DecimalPlaces{0});
    REQUIRE(view.rows.size() == 2U);
    CHECK((*view.rows[0].debit).getDecimalPlaces() == DecimalPlaces{0});
    auto const balance =
        reports.execute(lims::AccountBalance{.company = osaka, .accountCode = "1200", .asOf = november(30)});
    CHECK((*balance.balance).getDecimalPlaces() == DecimalPlaces{0});
}

// Mutations: delete validate() from a report query; answer a company the caller cannot see; drop the NotFound for an
// unknown account; drop the range-order check from AccountLedger.
TEST_CASE("ledger reports: a query is checked, a company the caller cannot see is empty, an account must exist",
          "[lims][ledger][reports]") {
    DbFixture fixture;
    books();
    post("a", units(100));
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    lims::CompanyId osaka;
    {
        ScopedPrincipal const alice{"alice"};
        osaka = lims::OrgModel{}
                    .execute(lims::CreateCompany{.name = "Osaka branch",
                                                 .baseCurrency = lims::CurrencyCode::JPY,
                                                 .fiscalYearStart = lims::Month::April})
                    .id;
        lims::OrgModel{}.execute(
            lims::AssignRole{.principal = "dave", .role = lims::LimsRole::Accountant, .company = osaka, .site = {}});
    }
    lims::LedgerReportModel reports;
    SECTION("a malformed query") {
        ScopedPrincipal const alice{"alice"};
        CHECK_THROWS_AS(reports.execute(lims::TrialBalance{.company = {}, .asOf = november(30)}),
                        lims::ValidationError);
        CHECK_THROWS_AS(reports.execute(lims::TrialBalance{.company = lims::test::seededCompany(), .asOf = {}}),
                        lims::ValidationError);
        CHECK_THROWS_AS(reports.execute(lims::AccountBalance{
                            .company = lims::test::seededCompany(), .accountCode = "", .asOf = november(30)}),
                        lims::ValidationError);
        CHECK_THROWS_AS(
            reports.execute(lims::AccountBalance{.company = {}, .accountCode = "1200", .asOf = november(30)}),
            lims::ValidationError);
        CHECK_THROWS_AS(reports.execute(lims::AccountBalance{
                            .company = lims::test::seededCompany(), .accountCode = "1200", .asOf = {}}),
                        lims::ValidationError);
        CHECK_THROWS_AS(
            reports.execute(lims::AccountLedger{
                .company = lims::test::seededCompany(), .accountCode = "", .from = november(1), .to = november(30)}),
            lims::ValidationError);
        CHECK_THROWS_AS(
            reports.execute(lims::AccountLedger{
                .company = lims::test::seededCompany(), .accountCode = "1200", .from = {}, .to = november(30)}),
            lims::ValidationError);
        CHECK_THROWS_AS(reports.execute(lims::AccountLedger{.company = lims::test::seededCompany(),
                                                            .accountCode = "1200",
                                                            .from = november(30),
                                                            .to = november(1)}),
                        lims::ValidationError);
    }
    SECTION("an unknown account") {
        ScopedPrincipal const alice{"alice"};
        CHECK_THROWS_AS(reports.execute(lims::AccountBalance{
                            .company = lims::test::seededCompany(), .accountCode = "9999", .asOf = november(30)}),
                        lims::NotFound);
        CHECK_THROWS_AS(reports.execute(lims::AccountLedger{.company = lims::test::seededCompany(),
                                                            .accountCode = "9999",
                                                            .from = november(1),
                                                            .to = november(30)}),
                        lims::NotFound);
    }
    SECTION("a company the caller cannot see is empty, whether or not it exists") {
        ScopedPrincipal const dave{"dave"};
        for (auto const company : {lims::test::seededCompany(), lims::CompanyId{9999}}) {
            CAPTURE(*company);
            auto const view = reports.execute(lims::TrialBalance{.company = company, .asOf = november(30)});
            CHECK(view.rows.empty());
            CHECK_FALSE(view.totalDebit.hasValue());
            CHECK_FALSE(
                reports.execute(lims::AccountBalance{.company = company, .accountCode = "1200", .asOf = november(30)})
                    .balance.hasValue());
            CHECK(reports
                      .execute(lims::AccountLedger{
                          .company = company, .accountCode = "1200", .from = november(1), .to = november(30)})
                      .lines.empty());
        }
        // Their own company has no such account, and says so.
        CHECK_THROWS_AS(
            reports.execute(lims::AccountBalance{.company = osaka, .accountCode = "1200", .asOf = november(30)}),
            lims::NotFound);
    }
}

// Hypothesis 8, first half. The summation every report uses, at the magnitude the design names: 10^7 lines of up to
// 10^9 minor units (a total of at most 10^16 minor units, inside int64), and an over-range total.
// Mutation: make ExactSum::add use operator+ (the over-range case reports a clamped total and add() returns true).
TEST_CASE(
    "Stress: Hypothesis 8: ten million lines of up to 10^9 minor units total exactly, and an over-range total is "
    "refused",
    "[lims][ledger][h8][stress]") {
    constexpr std::int64_t kLines = 10'000'000;
    constexpr std::uint64_t kMaxMinor = 1'000'000'000;
    lims::ExactSum sum;
    std::int64_t expectedMinor = 0;
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    bool allAdded = true;
    auto const started = std::chrono::steady_clock::now();
    for (std::int64_t i = 0; i < kLines; ++i) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        auto const minor = static_cast<std::int64_t>((state >> 33U) % (kMaxMinor + 1U));
        expectedMinor += minor;
        allAdded = sum.add(Rational{Numerator{minor}, Denominator{100}, DecimalPlaces{2}}) && allAdded;
    }
    auto const elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    WARN("summed " << kLines << " lines exactly in " << elapsed.count() << " ms; total " << expectedMinor
                   << " minor units");
    REQUIRE(allAdded);
    CHECK(sum.total() == Rational{Numerator{expectedMinor}, Denominator{100}, DecimalPlaces{2}});

    // The control: operator+ saturates and reports success; ExactSum refuses and keeps its total.
    Rational const big{kHuge, DecimalPlaces{0}};
    Rational const clamped = big + big;
    WARN("operator+ of two " << kHuge << " answers " << clamped.numerator << "/" << clamped.denominator);
    CHECK(clamped.numerator == std::numeric_limits<std::int64_t>::max());
    lims::ExactSum over;
    CHECK(over.add(big));
    CHECK_FALSE(over.add(big));
    CHECK(over.total() == big);
}

// The trial balance of a real ledger: kEntries entries of kPairs debit/credit pairs each, posted through the ledger
// and totalled by the report. Entries carry many lines because every posting is its own durable transaction and so
// costs milliseconds whatever its size, while the report's cost is in loading lines: a count of lines reachable in
// minutes needs few entries. The report loads each posted entry's lines with its own query. Mutation: make
// TrialBalance skip every other entry (the totals no longer equal the posted sum).
TEST_CASE("Stress: Hypothesis 10: a trial balance over posted entries totals exactly", "[lims][ledger][h10][stress]") {
    constexpr std::int64_t kEntries = 1'000;
    constexpr std::int64_t kPairs = 100;
    constexpr std::uint64_t kMaxMinor = 1'000'000'000;
    DbFixture fixture;
    books();
    ScopedPrincipal const alice{"alice"};
    std::int64_t expectedMinor = 0;
    std::uint64_t state = 0x9E3779B97F4A7C15ULL;
    auto const postingStarted = std::chrono::steady_clock::now();
    for (std::int64_t i = 0; i < kEntries; ++i) {
        lims::PostingRequest request{.company = lims::test::seededCompany(),
                                     .date = november(static_cast<unsigned>(1 + (i % 28))),
                                     .sourceKey = "stress:" + std::to_string(i),
                                     .sourceReference = "stress",
                                     .currency = lims::CurrencyCode::EUR,
                                     .rate = one(),
                                     .lines = {}};
        for (std::int64_t pair = 0; pair < kPairs; ++pair) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            auto const minor = static_cast<std::int64_t>((state >> 33U) % (kMaxMinor + 1U));
            expectedMinor += minor;
            lims::Amount const amount{Rational{Numerator{minor}, Denominator{100}, DecimalPlaces{2}}};
            request.lines.push_back(lims::PostingLine{.accountCode = pair % 2 == 0 ? "1200" : "1300",
                                                      .side = lims::PostingSide::Debit,
                                                      .amount = amount,
                                                      .companyAmount = amount});
            request.lines.push_back(lims::PostingLine{.accountCode = pair % 3 == 0 ? "4000" : "4100",
                                                      .side = lims::PostingSide::Credit,
                                                      .amount = amount,
                                                      .companyAmount = amount});
        }
        static_cast<void>(lims::LedgerModel::record(request));
    }
    auto const reportStarted = std::chrono::steady_clock::now();
    auto const view = trial();
    auto const finished = std::chrono::steady_clock::now();
    WARN("posted " << kEntries << " entries (" << 2 * kEntries * kPairs << " lines) in "
                   << std::chrono::duration_cast<std::chrono::milliseconds>(reportStarted - postingStarted).count()
                   << " ms; trial balance in "
                   << std::chrono::duration_cast<std::chrono::milliseconds>(finished - reportStarted).count()
                   << " ms; total " << expectedMinor << " minor units");
    auto const expected = Rational{Numerator{expectedMinor}, Denominator{100}, DecimalPlaces{2}};
    CHECK(*view.totalDebit == expected);
    CHECK(*view.totalCredit == expected);
    CHECK(view.rows.size() == 4U);
}

// Mutation: let TrialBalance clamp instead of throwing (it answers a total of INT64_MAX).
TEST_CASE("Hypothesis 8: a trial balance whose total does not fit is refused, not saturated", "[lims][ledger][h8]") {
    DbFixture fixture;
    books();
    ScopedPrincipal const alice{"alice"};
    SECTION("one account's total does not fit") {
        post("huge-1", lims::Amount{Rational{kHuge, DecimalPlaces{2}}});
        post("huge-2", lims::Amount{Rational{kHuge, DecimalPlaces{2}}});
        CHECK_THROWS_AS(trial(), lims::OutOfRange);
        CHECK_THROWS_AS(lims::LedgerReportModel{}.execute(lims::AccountBalance{
                            .company = lims::test::seededCompany(), .accountCode = "1200", .asOf = november(30)}),
                        lims::OutOfRange);
    }
    SECTION("every account fits and the company's total does not") {
        post("huge-1", lims::Amount{Rational{kHuge, DecimalPlaces{2}}}, 10, "1200", "4000");
        post("huge-2", lims::Amount{Rational{kHuge, DecimalPlaces{2}}}, 10, "1300", "4100");
        CHECK_THROWS_AS(trial(), lims::OutOfRange);
        // Each account alone reports exactly.
        CHECK(*lims::LedgerReportModel{}
                   .execute(lims::AccountBalance{
                       .company = lims::test::seededCompany(), .accountCode = "1300", .asOf = november(30)})
                   .balance == Rational{kHuge, DecimalPlaces{2}});
    }
    SECTION("an amount that fits reports exactly") {
        post("big", lims::Amount{Rational{kHuge, DecimalPlaces{2}}});
        auto const view = trial();
        CHECK(*view.totalDebit == Rational{kHuge, DecimalPlaces{2}});
        CHECK(*view.totalCredit == Rational{kHuge, DecimalPlaces{2}});
    }
}

// Mutations: let the running balance of AccountLedger clamp; let AccountBalance's debit-less-credit clamp.
TEST_CASE("Hypothesis 8: a running balance or a balance that does not fit is refused", "[lims][ledger][h8]") {
    DbFixture fixture;
    auto const novemberPeriod = books();
    ScopedPrincipal const alice{"alice"};
    lims::LedgerReportModel reports;
    SECTION("a running balance past int64") {
        // 5e18 on the 3rd and again on the 12th: the range starts on the 10th with 5e18 already, and its one line
        // would take the balance to 1e19.
        post("huge-1", lims::Amount{Rational{kHuge, DecimalPlaces{2}}}, 3);
        post("huge-2", lims::Amount{Rational{kHuge, DecimalPlaces{2}}}, 12);
        CHECK_THROWS_AS(reports.execute(lims::AccountLedger{.company = lims::test::seededCompany(),
                                                            .accountCode = "1200",
                                                            .from = november(10),
                                                            .to = november(30)}),
                        lims::OutOfRange);
        // The first posting alone is shown exactly.
        auto const first = reports.execute(lims::AccountLedger{
            .company = lims::test::seededCompany(), .accountCode = "1200", .from = november(1), .to = november(5)});
        REQUIRE(first.lines.size() == 1U);
        CHECK(*first.lines[0].balance == Rational{kHuge, DecimalPlaces{2}});
    }
    SECTION("a frozen debit and credit whose difference does not fit") {
        post("one", units(1));
        lims::LedgerModel ledger;
        ledger.execute(lims::BeginClosePeriod{.periodId = novemberPeriod.id});
        ledger.execute(lims::ClosePeriod{.periodId = novemberPeriod.id});
        Lightweight::DataMapper mapper;
        auto row = frozenRow(mapper, novemberPeriod.id, "1200");
        row.debitNum = std::numeric_limits<std::int64_t>::max();
        row.debitDen = 1;
        row.creditNum = -std::numeric_limits<std::int64_t>::max();
        row.creditDen = 1;
        mapper.Update(row);
        CHECK_THROWS_AS(
            reports.execute(lims::AccountBalance{.company = lims::test::seededCompany(),
                                                 .accountCode = "1200",
                                                 .asOf = morph::time::Timestamp{lims::test::at(2026, 12, 31)}}),
            lims::OutOfRange);
    }
}

TEST_CASE("LedgerModel and LedgerReportModel over the full backend-mode matrix: a rate is set and reported on",
          "[lims][ledger][matrix]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    books();
    post("a", units(100));
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    BackendRig rig{mode, 1, lims::test::limsAuthorizer(kSecret)};
    rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "alice"));
    auto ledger = rig.client<lims::LedgerModel>(0);
    auto const set = awaitQt(ledger.execute(lims::SetExchangeRate{.company = lims::test::seededCompany(),
                                                                  .from = lims::CurrencyCode::USD,
                                                                  .to = lims::CurrencyCode::EUR,
                                                                  .validFrom = november(1),
                                                                  .rate = rate(92, 100)}));
    CHECK(set.setBy == "alice");
    CHECK(awaitQt(ledger.execute(lims::ListExchangeRates{.company = lims::test::seededCompany()})).rates.size() == 1U);
    auto reports = rig.client<lims::LedgerReportModel>(0);
    auto const view =
        awaitQt(reports.execute(lims::TrialBalance{.company = lims::test::seededCompany(), .asOf = november(30)}));
    CHECK(*view.totalDebit == Rational{100, DecimalPlaces{2}});
    CHECK(view.rows.size() == 2U);
    CHECK(*awaitQt(reports.execute(lims::AccountBalance{
                       .company = lims::test::seededCompany(), .accountCode = "4000", .asOf = november(30)}))
               .credit == Rational{100, DecimalPlaces{2}});
    CHECK(awaitQt(reports.execute(lims::AccountLedger{.company = lims::test::seededCompany(),
                                                      .accountCode = "1200",
                                                      .from = november(1),
                                                      .to = november(30)}))
              .lines.size() == 1U);
}

namespace {

/// @brief Records a balanced two-line entry of @p transactionMinor in @p currency, valued at @p companyMinor in EUR.
/// @param key The source key.
/// @param date The entry's date.
/// @param debit The debited account.
/// @param credit The credited account.
/// @param currency The transaction currency; the rate is 0.92 unless it is EUR.
/// @param transactionMinor The amount in hundredths of the transaction currency.
/// @param companyMinor The value in hundredths of EUR.
void postValued(std::string const& key, morph::time::Timestamp date, std::string debit, std::string credit,
                lims::CurrencyCode currency, std::int64_t transactionMinor, std::int64_t companyMinor) {
    auto const amount = [](std::int64_t minor) {
        return lims::Amount{Rational{Numerator{minor}, Denominator{100}, DecimalPlaces{2}}};
    };
    static_cast<void>(lims::LedgerModel::record(
        lims::PostingRequest{.company = lims::test::seededCompany(),
                             .date = date,
                             .sourceKey = key,
                             .sourceReference = key,
                             .currency = currency,
                             .rate = currency == lims::CurrencyCode::EUR ? one() : rate(92, 100),
                             .lines = {lims::PostingLine{.accountCode = std::move(debit),
                                                         .side = lims::PostingSide::Debit,
                                                         .amount = amount(transactionMinor),
                                                         .companyAmount = amount(companyMinor)},
                                       lims::PostingLine{.accountCode = std::move(credit),
                                                         .side = lims::PostingSide::Credit,
                                                         .amount = amount(transactionMinor),
                                                         .companyAmount = amount(companyMinor)}}}));
}

/// @brief What the posted lines of the seeded company up to @p throughMs add up to, per account code, read through
///        the ledger's own entry list and summed here.
/// @param throughMs The last instant counted.
/// @return Each account's debit and credit total.
[[nodiscard]] std::map<std::string, std::pair<lims::ExactSum, lims::ExactSum>> sumOverLines(std::int64_t throughMs) {
    std::map<std::string, std::pair<lims::ExactSum, lims::ExactSum>> sums;
    for (auto const& entry :
         lims::LedgerModel{}.execute(lims::ListEntries{.company = lims::test::seededCompany()}).entries) {
        if (entry.state != lims::EntryState::Posted || lims::millisOf(entry.date) > throughMs) {
            continue;
        }
        for (auto const& line : entry.lines) {
            auto& [debit, credit] = sums[line.accountCode];
            REQUIRE((line.side == lims::PostingSide::Debit ? debit : credit).add(*line.companyAmount));
        }
    }
    return sums;
}

}  // namespace

// The trial balance is summed in the database, per account, side and denominator: a figure that differs between the
// transaction and the company currency, amounts of every denominator a two-decimal currency stores, a draft that is
// not counted and a closed period whose frozen totals stand in for its lines all give the sum over the lines.
// Mutations: sum the transaction amount (amount_num, amount_den) instead of the company amount; leave out the state
// filter (the draft is counted); drop the entry-date bound; drop the skip of frozen periods (a closed period counts
// twice).
TEST_CASE(
    "ledger: a trial balance summed in the database equals the sum over the lines, and an overflowing sum is "
    "OutOfRange",
    "[lims][ledger][pages]") {
    DbFixture fixture;
    auto const novemberPeriod = books();
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const december = ledger.execute(lims::OpenPeriod{
        .company = lims::test::seededCompany(), .month = morph::time::Timestamp{lims::test::at(2026, 12, 1)}});
    static_cast<void>(december);
    static_cast<void>(ledger.execute(lims::SetExchangeRate{.company = lims::test::seededCompany(),
                                                           .from = lims::CurrencyCode::USD,
                                                           .to = lims::CurrencyCode::EUR,
                                                           .validFrom = november(1),
                                                           .rate = rate(92, 100)}));
    struct Figure {
        std::int64_t transaction;
        std::int64_t company;
    };
    // Hundredths; the denominators 1, 2, 4, 5, 10, 20, 25, 50 and 100 once each, and a figure with two digits.
    constexpr std::array<Figure, 10> figures{Figure{10000, 9200}, Figure{50, 46},    Figure{25, 23}, Figure{20, 18},
                                             Figure{10, 9},       Figure{5, 5},      Figure{4, 4},   Figure{2, 2},
                                             Figure{1, 1},        Figure{1234, 1135}};
    int index = 0;
    for (auto const& figure : figures) {
        postValued("nov-" + std::to_string(index), november(2 + static_cast<unsigned>(index)),
                   index % 2 == 0 ? "1200" : "1300", index % 3 == 0 ? "4000" : "4100", lims::CurrencyCode::USD,
                   figure.transaction, figure.company);
        ++index;
    }
    postValued("nov-eur", november(20), "1200", "4000", lims::CurrencyCode::EUR, 777, 777);
    auto const check = [&](morph::time::Timestamp asOf) {
        auto const expected = sumOverLines(lims::millisOf(asOf));
        auto const view = lims::LedgerReportModel{}.execute(
            lims::TrialBalance{.company = lims::test::seededCompany(), .asOf = asOf});
        lims::ExactSum totalDebit;
        lims::ExactSum totalCredit;
        REQUIRE(view.rows.size() == expected.size());
        for (auto const& row : view.rows) {
            CAPTURE(row.accountCode);
            auto const found = expected.find(row.accountCode);
            REQUIRE(found != expected.end());
            CHECK(*row.debit == found->second.first.total());
            CHECK(*row.credit == found->second.second.total());
            REQUIRE(totalDebit.add(found->second.first.total()));
            REQUIRE(totalCredit.add(found->second.second.total()));
        }
        CHECK(*view.totalDebit == totalDebit.total());
        CHECK(*view.totalCredit == totalCredit.total());
        CHECK(*view.totalDebit == *view.totalCredit);
    };
    SECTION("open books, with a draft that is not counted") {
        // A draft is not in the balances.
        ledger.execute(lims::CreateDraftEntry{.company = lims::test::seededCompany(),
                                              .date = november(21),
                                              .memo = "draft",
                                              .currency = lims::CurrencyCode::EUR,
                                              .rate = one(),
                                              .lines = {lims::PostingLine{.accountCode = "1200",
                                                                          .side = lims::PostingSide::Debit,
                                                                          .amount = units(7),
                                                                          .companyAmount = units(7)},
                                                        lims::PostingLine{.accountCode = "4000",
                                                                          .side = lims::PostingSide::Credit,
                                                                          .amount = units(7),
                                                                          .companyAmount = units(7)}}});
        check(november(30));
        // A bound inside the month leaves out the later entries.
        check(november(5));
        CHECK(sumOverLines(lims::millisOf(november(5))).at("1200").first.total() !=
              sumOverLines(lims::millisOf(november(30))).at("1200").first.total());
    }
    SECTION("a closed period stands in for its lines") {
        ledger.execute(lims::BeginClosePeriod{.periodId = novemberPeriod.id});
        ledger.execute(lims::ClosePeriod{.periodId = novemberPeriod.id});
        postValued("dec", morph::time::Timestamp{lims::test::at(2026, 12, 5)}, "1300", "4100", lims::CurrencyCode::USD,
                   333, 306);
        check(morph::time::Timestamp{lims::test::at(2026, 12, 31)});
        check(november(30));
    }
}

// The database refuses an integer sum that does not fit, where wrapping would answer a plausible wrong total. SQLite's
// own behaviour is measured here, not assumed, and the ledger reports it as OutOfRange.
// Mutation: sum as a real (SUM(CAST(company_num AS REAL))), which answers a rounded total instead of refusing.
TEST_CASE("ledger: SQLite's SUM raises on integer overflow, and the trial balance reports it as OutOfRange",
          "[lims][ledger][pages]") {
    DbFixture fixture;
    books();
    ScopedPrincipal const alice{"alice"};
    post("huge-1", lims::Amount{Rational{kHuge, DecimalPlaces{2}}}, 10, "1200", "4000");
    post("huge-2", lims::Amount{Rational{kHuge, DecimalPlaces{2}}}, 11, "1200", "4000");
    Lightweight::DataMapper mapper;
    Lightweight::SqlStatement statement{mapper.Connection()};
    // Four lines of 5 * 10^18: the integer sum is 2 * 10^19, past int64.
    CHECK_THROWS_AS(statement.ExecuteDirectScalar<std::int64_t>("SELECT SUM(company_num) FROM ledger_entry_lines"),
                    Lightweight::SqlException);
    try {
        static_cast<void>(
            statement.ExecuteDirectScalar<std::int64_t>("SELECT SUM(company_num) FROM ledger_entry_lines"));
    } catch (Lightweight::SqlException const& error) {
        WARN("SQLite said: " << error.info().message);
        CHECK(error.info().message.find("integer overflow") != std::string::npos);
    }
    CHECK_THROWS_AS(trial(), lims::OutOfRange);
}

// Mutation: delete validate() from GetRateOn.
TEST_CASE("ledger rates: a rate question naming no company or no date is refused", "[lims][ledger][reports]") {
    DbFixture fixture;
    books();
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    CHECK_THROWS_AS(ledger.execute(lims::GetRateOn{.query = {.company = {},
                                                             .from = lims::CurrencyCode::USD,
                                                             .to = lims::CurrencyCode::EUR,
                                                             .on = november(1)}}),
                    lims::ValidationError);
    CHECK_THROWS_AS(ledger.execute(lims::GetRateOn{.query = {.company = lims::test::seededCompany(),
                                                             .from = lims::CurrencyCode::USD,
                                                             .to = lims::CurrencyCode::EUR,
                                                             .on = {}}}),
                    lims::ValidationError);
}

// Mutations: drop the year carry from the end of a December period (its end falls in the January of the year before,
// so the period counts as frozen on every date and the 15 December figure reports the tampered total); drop the
// freeze of a period ending in January.
TEST_CASE("ledger reports: a closed December period ends in January of the next year", "[lims][ledger][reports]") {
    DbFixture fixture;
    books();
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const december = ledger.execute(lims::OpenPeriod{
        .company = lims::test::seededCompany(), .month = morph::time::Timestamp{lims::test::at(2026, 12, 1)}});
    static_cast<void>(
        lims::LedgerModel::record(lims::PostingRequest{.company = lims::test::seededCompany(),
                                                       .date = morph::time::Timestamp{lims::test::at(2026, 12, 5)},
                                                       .sourceKey = "dec-entry",
                                                       .sourceReference = "dec-entry",
                                                       .currency = lims::CurrencyCode::EUR,
                                                       .rate = one(),
                                                       .lines = {lims::PostingLine{.accountCode = "1200",
                                                                                   .side = lims::PostingSide::Debit,
                                                                                   .amount = units(10),
                                                                                   .companyAmount = units(10)},
                                                                 lims::PostingLine{.accountCode = "4000",
                                                                                   .side = lims::PostingSide::Credit,
                                                                                   .amount = units(10),
                                                                                   .companyAmount = units(10)}}}));
    ledger.execute(lims::BeginClosePeriod{.periodId = december.id});
    ledger.execute(lims::ClosePeriod{.periodId = december.id});
    {
        Lightweight::DataMapper mapper;
        auto row = frozenRow(mapper, december.id, "1200");
        row.debitNum = 5000;
        row.debitDen = 1;
        mapper.Update(row);
    }
    lims::LedgerReportModel reports;
    auto const balanceOn = [&](int year, unsigned month, unsigned day) {
        return *reports
                    .execute(lims::AccountBalance{.company = lims::test::seededCompany(),
                                                  .accountCode = "1200",
                                                  .asOf = morph::time::Timestamp{lims::test::at(year, month, day)}})
                    .balance;
    };
    // Inside December the period's own lines count, not its frozen row.
    CHECK(balanceOn(2026, 12, 15) == Rational{10, DecimalPlaces{2}});
    // After the period's end the frozen figure replaces the period's lines.
    CHECK(balanceOn(2027, 1, 5) == Rational{5000, DecimalPlaces{2}});
}

// Mutation: skip only the first closed period's lines in the statement (the second period's lines are counted on top
// of its frozen totals).
TEST_CASE("ledger reports: two closed periods are each reported from their frozen totals", "[lims][ledger][reports]") {
    DbFixture fixture;
    auto const november = books();
    post("nov", units(100), 10);
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const december = ledger.execute(lims::OpenPeriod{
        .company = lims::test::seededCompany(), .month = morph::time::Timestamp{lims::test::at(2026, 12, 1)}});
    static_cast<void>(
        lims::LedgerModel::record(lims::PostingRequest{.company = lims::test::seededCompany(),
                                                       .date = morph::time::Timestamp{lims::test::at(2026, 12, 5)},
                                                       .sourceKey = "dec",
                                                       .sourceReference = "dec",
                                                       .currency = lims::CurrencyCode::EUR,
                                                       .rate = one(),
                                                       .lines = {lims::PostingLine{.accountCode = "1200",
                                                                                   .side = lims::PostingSide::Debit,
                                                                                   .amount = units(10),
                                                                                   .companyAmount = units(10)},
                                                                 lims::PostingLine{.accountCode = "4000",
                                                                                   .side = lims::PostingSide::Credit,
                                                                                   .amount = units(10),
                                                                                   .companyAmount = units(10)}}}));
    for (auto const& period : {november, december}) {
        ledger.execute(lims::BeginClosePeriod{.periodId = period.id});
        ledger.execute(lims::ClosePeriod{.periodId = period.id});
    }
    {
        Lightweight::DataMapper mapper;
        for (auto const& [period, debit] : {std::pair{november.id, 5000}, std::pair{december.id, 7000}}) {
            auto row = frozenRow(mapper, period, "1200");
            row.debitNum = debit;
            row.debitDen = 1;
            mapper.Update(row);
        }
    }
    auto const report = lims::LedgerReportModel{}.execute(lims::TrialBalance{
        .company = lims::test::seededCompany(), .asOf = morph::time::Timestamp{lims::test::at(2027, 1, 5)}});
    CHECK(*report.totalDebit == Rational{12000, DecimalPlaces{2}});
    CHECK(*report.totalCredit == Rational{110, DecimalPlaces{2}});
}

// Mutation: read a stored line side that is neither debit nor credit as a credit (a corrupted row changes a report's
// totals instead of being refused).
TEST_CASE("ledger reports: a stored line with a side out of range is refused by every report",
          "[lims][ledger][reports]") {
    DbFixture fixture;
    books();
    post("a", units(100));
    {
        Lightweight::DataMapper mapper;
        auto lines = mapper.Query<lims::db::EntryLineRecord>().All();
        REQUIRE_FALSE(lines.empty());
        lines.front().side = 7;
        mapper.Update(lines.front());
    }
    ScopedPrincipal const alice{"alice"};
    lims::LedgerReportModel reports;
    CHECK_THROWS_AS(reports.execute(lims::TrialBalance{.company = lims::test::seededCompany(), .asOf = november(30)}),
                    lims::ValidationError);
    CHECK_THROWS_AS(reports.execute(lims::AccountLedger{.company = lims::test::seededCompany(),
                                                        .accountCode = "1200",
                                                        .from = morph::time::Timestamp{lims::test::at(2026, 11, 1, 0)},
                                                        .to = november(30)}),
                    lims::ValidationError);
}
