// SPDX-License-Identifier: Apache-2.0
//
// Journal entries: balanced per currency and in company currency, never rounded by the ledger, immutable once posted,
// corrected by reversal.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <morph/core/action_guard.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/table/plan_conformance.hpp>
#include <morph/table/query.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "lims/ledger/core/balance.hpp"
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
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

constexpr std::string_view kSecret = "lims-ledger-entries-test-secret-at-least-32b";

/// @brief @p minor minor units at @p places decimals.
/// @param minor The minor units.
/// @param places The currency's decimals.
/// @return The amount.
[[nodiscard]] lims::Amount money(std::int64_t minor, std::uint32_t places = 2) {
    std::int64_t const scale = places == 0U ? 1 : 100;
    return lims::Amount{Rational{Numerator{minor}, Denominator{scale}, DecimalPlaces{places}}};
}

/// @brief A line whose company amount equals its amount.
/// @param code The account.
/// @param side The side.
/// @param minor The amount in minor units.
/// @return The line.
[[nodiscard]] lims::PostingLine line(std::string code, lims::PostingSide side, std::int64_t minor) {
    return lims::PostingLine{
        .accountCode = std::move(code), .side = side, .amount = money(minor), .companyAmount = money(minor)};
}

/// @brief The rate one.
/// @return It.
[[nodiscard]] lims::Rate one() { return lims::Rate{Rational{1, DecimalPlaces{6}}}; }

/// @brief A rate of @p millionths millionths.
/// @param millionths The rate times 10^6.
/// @return It.
[[nodiscard]] lims::Rate rateOf(std::int64_t millionths) {
    return lims::Rate{Rational{Numerator{millionths}, Denominator{1000000}, DecimalPlaces{6}}};
}

/// @brief A line valued at @p company minor units of the company currency.
/// @param code The account.
/// @param side The side.
/// @param minor The amount in minor units of the transaction currency.
/// @param company The value in minor units of the company currency.
/// @param places The company currency's decimals.
/// @return The line.
[[nodiscard]] lims::PostingLine valued(std::string code, lims::PostingSide side, std::int64_t minor,
                                       std::int64_t company, std::uint32_t transactionPlaces = 2,
                                       std::uint32_t places = 2) {
    return lims::PostingLine{.accountCode = std::move(code),
                             .side = side,
                             .amount = money(minor, transactionPlaces),
                             .companyAmount = money(company, places)};
}

/// @brief The books every case starts from: alice an accountant at the seeded company (EUR), its chart and
///        November 2026 open.
/// @return November's period.
lims::PeriodView books() {
    lims::test::grant("alice", lims::LimsRole::Accountant);
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    for (auto const& [code, type] :
         std::vector<std::pair<std::string, lims::AccountType>>{{"1200", lims::AccountType::Asset},
                                                                {"2200", lims::AccountType::Liability},
                                                                {"4000", lims::AccountType::Revenue},
                                                                {"6900", lims::AccountType::Expense}}) {
        ledger.execute(lims::CreateAccount{
            .company = lims::test::seededCompany(), .code = code, .name = "Account " + code, .type = type});
    }
    return ledger.execute(lims::OpenPeriod{.company = lims::test::seededCompany(),
                                           .month = morph::time::Timestamp{lims::test::at(2026, 11, 1)}});
}

/// @brief A draft of a balanced 50.00 accrual on @p date.
/// @param ledger The model.
/// @param date The entry's date.
/// @return The draft.
lims::JournalEntryView accrual(lims::LedgerModel& ledger, morph::time::Timestamp date) {
    return ledger.execute(lims::CreateDraftEntry{
        .company = lims::test::seededCompany(),
        .date = date,
        .memo = "accrual",
        .currency = lims::CurrencyCode::EUR,
        .rate = one(),
        .lines = {line("6900", lims::PostingSide::Debit, 5000), line("2200", lims::PostingSide::Credit, 5000)}});
}

/// @brief Sets a period's state directly, past the actions that move it.
/// @param period The period.
/// @param state The state.
void forceState(lims::PeriodId period, lims::PeriodState state) {
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<lims::db::PeriodRecord>()
                    .Where(::Lightweight::FieldNameOf<&lims::db::PeriodRecord::id>, "=", *period)
                    .All();
    REQUIRE(rows.size() == 1U);
    rows.front().state = static_cast<int>(state);
    mapper.Update(rows.front());
}

/// @brief Runs @p sql on the fixture's database.
/// @param sql The statement.
void execute(std::string const& sql) {
    Lightweight::SqlStatement statement;
    (void)statement.ExecuteDirect(sql);
}

/// @brief A posted entry of four distinct USD lines valued in the EUR company at a rate none of whose line values is
///        the one rounding amount x rate gives for every line.
/// @param ledger The model.
/// @return The posted entry.
lims::JournalEntryView postedUsdEntry(lims::LedgerModel& ledger) {
    auto const draft =
        ledger.execute(lims::CreateDraftEntry{.company = lims::test::seededCompany(),
                                              .date = morph::time::Timestamp{lims::test::at(2026, 11, 12)},
                                              .memo = "usd",
                                              .currency = lims::CurrencyCode::USD,
                                              .rate = rateOf(921500),
                                              .lines = {valued("1200", lims::PostingSide::Debit, 1000, 921),
                                                        valued("4000", lims::PostingSide::Credit, 2000, 1843),
                                                        valued("6900", lims::PostingSide::Debit, 1250, 1152),
                                                        valued("2200", lims::PostingSide::Credit, 250, 230)}});
    return ledger.execute(lims::PostEntry{.entryId = draft.id});
}

}  // namespace

// Mutation: skip the company-currency sum in entryProblem (the 1-cent company difference passes).
TEST_CASE("ledger entries: an entry balances per transaction currency and in company currency, unrounded",
          "[lims][ledger]") {
    std::vector<lims::PostingLine> balanced{line("1200", lims::PostingSide::Debit, 1000),
                                            line("4000", lims::PostingSide::Credit, 1000)};
    CHECK_FALSE(lims::entryProblem(balanced, lims::CurrencyCode::EUR, lims::CurrencyCode::EUR, one()).has_value());

    std::vector<lims::PostingLine> oneLine{line("1200", lims::PostingSide::Debit, 1000)};
    CHECK(lims::entryProblem(oneLine, lims::CurrencyCode::EUR, lims::CurrencyCode::EUR, one()).has_value());

    std::vector<lims::PostingLine> unbalanced{line("1200", lims::PostingSide::Debit, 1000),
                                              line("4000", lims::PostingSide::Credit, 999)};
    CHECK(lims::entryProblem(unbalanced, lims::CurrencyCode::EUR, lims::CurrencyCode::EUR, one()).has_value());

    // USD lines in a EUR company: the USD amounts balance, the EUR values are one cent apart.
    std::vector<lims::PostingLine> companyOff{
        {.accountCode = "1200", .side = lims::PostingSide::Debit, .amount = money(1000), .companyAmount = money(921)},
        {.accountCode = "4000",
         .side = lims::PostingSide::Credit,
         .amount = money(1000),
         .companyAmount = money(920)}};
    auto const problem = lims::entryProblem(companyOff, lims::CurrencyCode::USD, lims::CurrencyCode::EUR, one());
    REQUIRE(problem.has_value());
    CHECK_THAT(*problem, Catch::Matchers::ContainsSubstring("company currency"));

    // An unrounded company value is refused, not rounded: the ledger never rounds.
    std::vector<lims::PostingLine> unrounded{
        {.accountCode = "1200",
         .side = lims::PostingSide::Debit,
         .amount = money(1000),
         .companyAmount = lims::Amount{Rational{Numerator{92051}, Denominator{10000}, DecimalPlaces{4}}}},
        {.accountCode = "4000",
         .side = lims::PostingSide::Credit,
         .amount = money(1000),
         .companyAmount = lims::Amount{Rational{Numerator{92051}, Denominator{10000}, DecimalPlaces{4}}}}};
    CHECK(lims::entryProblem(unrounded, lims::CurrencyCode::USD, lims::CurrencyCode::EUR, one()).has_value());

    // JPY has no minor unit.
    std::vector<lims::PostingLine> fractionalYen{
        {.accountCode = "1200", .side = lims::PostingSide::Debit, .amount = money(1050), .companyAmount = money(1050)},
        {.accountCode = "4000",
         .side = lims::PostingSide::Credit,
         .amount = money(1050),
         .companyAmount = money(1050)}};
    CHECK(lims::entryProblem(fractionalYen, lims::CurrencyCode::JPY, lims::CurrencyCode::JPY, one()).has_value());
}

// Mutation: drop the negative-amount check; drop the zero-total check; drop the same-currency equality check.
TEST_CASE("ledger entries: negative, empty and self-contradicting lines are refused", "[lims][ledger]") {
    std::vector<lims::PostingLine> negative{line("1200", lims::PostingSide::Debit, -1000),
                                            line("4000", lims::PostingSide::Credit, -1000)};
    CHECK(lims::entryProblem(negative, lims::CurrencyCode::EUR, lims::CurrencyCode::EUR, one()).has_value());

    std::vector<lims::PostingLine> zero{line("1200", lims::PostingSide::Debit, 0),
                                        line("4000", lims::PostingSide::Credit, 0)};
    CHECK(lims::entryProblem(zero, lims::CurrencyCode::EUR, lims::CurrencyCode::EUR, one()).has_value());

    std::vector<lims::PostingLine> blank{line("1200", lims::PostingSide::Debit, 1000),
                                         {.accountCode = "4000", .side = lims::PostingSide::Credit}};
    CHECK(lims::entryProblem(blank, lims::CurrencyCode::EUR, lims::CurrencyCode::EUR, one()).has_value());

    // Same currency, so the company value must equal the amount; both sides still balance on their own.
    std::vector<lims::PostingLine> differing{
        {.accountCode = "1200", .side = lims::PostingSide::Debit, .amount = money(1000), .companyAmount = money(900)},
        {.accountCode = "4000",
         .side = lims::PostingSide::Credit,
         .amount = money(1000),
         .companyAmount = money(900)}};
    CHECK(lims::entryProblem(differing, lims::CurrencyCode::EUR, lims::CurrencyCode::EUR, one()).has_value());

    // A line may carry zero in the transaction currency and a company value: an exchange-rounding line.
    std::vector<lims::PostingLine> rounding{
        {.accountCode = "1200", .side = lims::PostingSide::Debit, .amount = money(1000), .companyAmount = money(921)},
        {.accountCode = "4000", .side = lims::PostingSide::Credit, .amount = money(1000), .companyAmount = money(920)},
        {.accountCode = "6900", .side = lims::PostingSide::Credit, .amount = money(0), .companyAmount = money(1)}};
    // Both 10.00 USD lines sit exactly half a cent from 10.00 x 0.9205 = 9.205, on either side.
    lims::Rate const rate{Rational{Numerator{920500}, Denominator{1000000}, DecimalPlaces{6}}};
    CHECK_FALSE(lims::entryProblem(rounding, lims::CurrencyCode::USD, lims::CurrencyCode::EUR, rate).has_value());
}

// Mutation: let PostEntry update a posted entry (the second post succeeds).
TEST_CASE("ledger entries: a draft is posted once and is then immutable; a correction is a reversal naming it",
          "[lims][ledger]") {
    DbFixture fixture;
    auto const november = books();
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const draft = ledger.execute(lims::CreateDraftEntry{
        .company = lims::test::seededCompany(),
        .date = morph::time::Timestamp{lims::test::at(2026, 11, 12)},
        .memo = "accrual",
        .currency = lims::CurrencyCode::EUR,
        .rate = one(),
        .lines = {line("6900", lims::PostingSide::Debit, 5000), line("2200", lims::PostingSide::Credit, 5000)}});
    CHECK(draft.state == lims::EntryState::Draft);
    CHECK(draft.period == november.id);
    auto const posted = ledger.execute(lims::PostEntry{.entryId = draft.id});
    CHECK(posted.state == lims::EntryState::Posted);
    CHECK(posted.postedBy == "alice");
    CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::PostEntry{.entryId = draft.id}),
                    lims::IllegalTransition);

    auto const reversal = ledger.execute(
        lims::ReverseEntry{.entryId = draft.id, .date = morph::time::Timestamp{lims::test::at(2026, 11, 20)}});
    CHECK(reversal.state == lims::EntryState::Posted);
    CHECK(reversal.reverses == draft.id);
    REQUIRE(reversal.lines.size() == 2U);
    CHECK(reversal.lines[0].side == lims::PostingSide::Credit);
    CHECK(ledger.execute(lims::GetEntry{.entryId = draft.id}).reversedBy == reversal.id);
    CHECK_THROWS_AS(::morph::model::executeChecked(
                        ledger, lims::ReverseEntry{.entryId = draft.id,
                                                   .date = morph::time::Timestamp{lims::test::at(2026, 11, 21)}}),
                    lims::Conflict);

    // The original is not edited: its lines and state are as posted; only reversedBy is set.
    auto const original = ledger.execute(lims::GetEntry{.entryId = draft.id});
    CHECK(original.state == lims::EntryState::Posted);
    CHECK(original.lines.size() == 2U);
    CHECK(original.lines[0].side == lims::PostingSide::Debit);
    CHECK(original.memo == "accrual");
    CHECK(ledger.execute(lims::ListEntries{.company = lims::test::seededCompany()}).entries.size() == 2U);
}

// Mutation: let ReverseEntry take a draft (the reversal of a draft succeeds).
TEST_CASE("ledger entries: only a posted entry is reversed, and a reversal is itself reversible once",
          "[lims][ledger]") {
    DbFixture fixture;
    books();
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const draft = accrual(ledger, morph::time::Timestamp{lims::test::at(2026, 11, 12)});
    CHECK_THROWS_AS(::morph::model::executeChecked(
                        ledger, lims::ReverseEntry{.entryId = draft.id,
                                                   .date = morph::time::Timestamp{lims::test::at(2026, 11, 13)}}),
                    lims::IllegalTransition);
    ledger.execute(lims::PostEntry{.entryId = draft.id});
    auto const reversal = ledger.execute(
        lims::ReverseEntry{.entryId = draft.id, .date = morph::time::Timestamp{lims::test::at(2026, 11, 13)}});
    // The reversal names the original and is reversed in its turn, which restores the original's effect.
    auto const again = ledger.execute(
        lims::ReverseEntry{.entryId = reversal.id, .date = morph::time::Timestamp{lims::test::at(2026, 11, 14)}});
    CHECK(again.reverses == reversal.id);
    CHECK(again.lines[0].side == lims::PostingSide::Debit);
    CHECK(ledger.execute(lims::GetEntry{.entryId = reversal.id}).reversedBy == again.id);
}

// Mutation: store the rate or the company amounts rounded; read the lines in id order instead of position order.
TEST_CASE("ledger entries: a foreign-currency entry keeps its rate and amounts exactly, in line order",
          "[lims][ledger]") {
    DbFixture fixture;
    books();
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    lims::Rate const rate{Rational{Numerator{920000}, Denominator{1000000}, DecimalPlaces{6}}};
    auto const draft =
        ledger.execute(lims::CreateDraftEntry{.company = lims::test::seededCompany(),
                                              .date = morph::time::Timestamp{lims::test::at(2026, 11, 3)},
                                              .memo = "usd sale",
                                              .currency = lims::CurrencyCode::USD,
                                              .rate = rate,
                                              .lines = {{.accountCode = "4000",
                                                         .side = lims::PostingSide::Credit,
                                                         .amount = money(1000),
                                                         .companyAmount = money(920)},
                                                        {.accountCode = "1200",
                                                         .side = lims::PostingSide::Debit,
                                                         .amount = money(1000),
                                                         .companyAmount = money(920)}}});
    auto const read = ledger.execute(lims::GetEntry{.entryId = draft.id});
    CHECK(read.currency == lims::CurrencyCode::USD);
    CHECK(*read.rate == *rate);
    REQUIRE(read.lines.size() == 2U);
    CHECK(read.lines[0].accountCode == "4000");
    CHECK(read.lines[1].accountCode == "1200");
    CHECK(*read.lines[0].companyAmount == *money(920));
    CHECK(*read.lines[1].amount == *money(1000));
    CHECK(read.createdBy == "alice");
    CHECK(read.postedBy.empty());
}

// Mutations: delete requireBalanced from CreateDraftEntry; delete requireRate; delete the period lookup's NotFound;
// look accounts up without the company filter; let requireRecordable pass a closing period for a new draft; let it
// pass a closed one for a post; let ReverseEntry skip requireRecordable.
TEST_CASE("ledger entries: what the ledger refuses", "[lims][ledger]") {
    DbFixture fixture;
    auto const november = books();
    lims::CompanyId osaka;
    {
        lims::test::grant("alice", lims::LimsRole::Supervisor);
        ScopedPrincipal const alice{"alice"};
        osaka = lims::OrgModel{}
                    .execute(lims::CreateCompany{.name = "Osaka branch",
                                                 .baseCurrency = lims::CurrencyCode::JPY,
                                                 .fiscalYearStart = lims::Month::April})
                    .id;
        lims::OrgModel{}.execute(
            lims::AssignRole{.principal = "dave", .role = lims::LimsRole::Accountant, .company = osaka, .site = {}});
    }
    // An account of another company: the seeded company has none called 9999.
    {
        ScopedPrincipal const dave{"dave"};
        lims::LedgerModel{}.execute(lims::CreateAccount{
            .company = osaka, .code = "9999", .name = "Osaka only", .type = lims::AccountType::Asset});
    }
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const date = morph::time::Timestamp{lims::test::at(2026, 11, 12)};
    auto const make = [&](std::vector<lims::PostingLine> lines, lims::CurrencyCode currency = lims::CurrencyCode::EUR,
                          lims::Rate rate = one()) {
        return ledger.execute(lims::CreateDraftEntry{.company = lims::test::seededCompany(),
                                                     .date = date,
                                                     .memo = "m",
                                                     .currency = currency,
                                                     .rate = rate,
                                                     .lines = std::move(lines)});
    };

    SECTION("unbalanced per transaction currency, and in the company currency") {
        CHECK_THROWS_AS(
            make({line("1200", lims::PostingSide::Debit, 1000), line("4000", lims::PostingSide::Credit, 999)}),
            lims::ValidationError);
        CHECK_THROWS_AS(
            make({{.accountCode = "1200",
                   .side = lims::PostingSide::Debit,
                   .amount = money(1000),
                   .companyAmount = money(921)},
                  {.accountCode = "4000",
                   .side = lims::PostingSide::Credit,
                   .amount = money(1000),
                   .companyAmount = money(920)}},
                 lims::CurrencyCode::USD, lims::Rate{Rational{Numerator{92}, Denominator{100}, DecimalPlaces{6}}}),
            lims::ValidationError);
        CHECK(ledger.execute(lims::ListEntries{.company = lims::test::seededCompany()}).entries.empty());
    }
    SECTION("the rate must be positive, and one in the company's own currency") {
        auto const lines = std::vector<lims::PostingLine>{line("1200", lims::PostingSide::Debit, 1000),
                                                          line("4000", lims::PostingSide::Credit, 1000)};
        CHECK_THROWS_AS(make(lines, lims::CurrencyCode::EUR, lims::Rate{Rational{0, DecimalPlaces{6}}}),
                        lims::ValidationError);
        CHECK_THROWS_AS(make(lines, lims::CurrencyCode::EUR, lims::Rate{Rational{2, DecimalPlaces{6}}}),
                        lims::ValidationError);
    }
    SECTION("an account of another company, and a month with no period") {
        CHECK_THROWS_AS(
            make({line("9999", lims::PostingSide::Debit, 1000), line("4000", lims::PostingSide::Credit, 1000)}),
            lims::NotFound);
        CHECK_THROWS_AS(::morph::model::executeChecked(
                            ledger, lims::CreateDraftEntry{.company = lims::test::seededCompany(),
                                                           .date = morph::time::Timestamp{lims::test::at(2027, 1, 5)},
                                                           .memo = "m",
                                                           .currency = lims::CurrencyCode::EUR,
                                                           .rate = one(),
                                                           .lines = {line("1200", lims::PostingSide::Debit, 1000),
                                                                     line("4000", lims::PostingSide::Credit, 1000)}}),
                        lims::NotFound);
    }
    SECTION("a closing period takes no new draft but posts and reverses what is there") {
        auto const draft = accrual(ledger, date);
        auto const second = accrual(ledger, date);
        ledger.execute(lims::PostEntry{.entryId = draft.id});
        ledger.execute(lims::BeginClosePeriod{.periodId = november.id});
        CHECK_THROWS_AS(accrual(ledger, date), lims::Conflict);
        CHECK(ledger.execute(lims::PostEntry{.entryId = second.id}).state == lims::EntryState::Posted);
        CHECK(ledger.execute(lims::ReverseEntry{.entryId = draft.id, .date = date}).reverses == draft.id);
    }
    SECTION("a closed period takes nothing") {
        auto const draft = accrual(ledger, date);
        auto const posted = accrual(ledger, date);
        ledger.execute(lims::PostEntry{.entryId = posted.id});
        forceState(november.id, lims::PeriodState::Closed);
        CHECK_THROWS_AS(accrual(ledger, date), lims::Conflict);
        CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::PostEntry{.entryId = draft.id}), lims::Conflict);
        CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::ReverseEntry{.entryId = posted.id, .date = date}),
                        lims::Conflict);
        // The refused reversal left the original unreversed and wrote nothing.
        CHECK_FALSE(ledger.execute(lims::GetEntry{.entryId = posted.id}).reversedBy.hasValue());
        CHECK(ledger.execute(lims::ListEntries{.company = lims::test::seededCompany()}).entries.size() == 2U);
    }
}

// Mutation: delete validate() from CreateDraftEntry, PostEntry, ReverseEntry, GetEntry or ListEntries.
TEST_CASE("ledger entries: malformed requests are ValidationErrors", "[lims][ledger]") {
    DbFixture fixture;
    books();
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const date = morph::time::Timestamp{lims::test::at(2026, 11, 12)};
    auto const lines = std::vector<lims::PostingLine>{line("1200", lims::PostingSide::Debit, 1000),
                                                      line("4000", lims::PostingSide::Credit, 1000)};
    CHECK_THROWS_AS(ledger.execute(lims::CreateDraftEntry{.company = {}, .date = date, .rate = one(), .lines = lines}),
                    lims::ValidationError);
    CHECK_THROWS_AS(ledger.execute(lims::CreateDraftEntry{
                        .company = lims::test::seededCompany(), .date = {}, .rate = one(), .lines = lines}),
                    lims::ValidationError);
    CHECK_THROWS_AS(ledger.execute(lims::CreateDraftEntry{
                        .company = lims::test::seededCompany(), .date = date, .rate = {}, .lines = lines}),
                    lims::ValidationError);
    CHECK_THROWS_AS(
        ledger.execute(lims::CreateDraftEntry{
            .company = lims::test::seededCompany(), .date = date, .rate = one(), .lines = {lines.front()}}),
        lims::ValidationError);
    CHECK_THROWS_AS(ledger.execute(lims::CreateDraftEntry{.company = lims::test::seededCompany(),
                                                          .date = date,
                                                          .memo = std::string(256, 'x'),
                                                          .rate = one(),
                                                          .lines = lines}),
                    lims::ValidationError);
    CHECK_THROWS_AS(ledger.execute(lims::PostEntry{}), lims::ValidationError);
    CHECK_THROWS_AS(ledger.execute(lims::ReverseEntry{.entryId = lims::JournalEntryId{1}, .date = {}}),
                    lims::ValidationError);
    CHECK_THROWS_AS(ledger.execute(lims::ReverseEntry{.entryId = {}, .date = date}), lims::ValidationError);
    CHECK_THROWS_AS(ledger.execute(lims::GetEntry{}), lims::ValidationError);
    CHECK_THROWS_AS(ledger.execute(lims::ListEntries{}), lims::ValidationError);
}

// Mutations: delete requireRoleSomewhere from PostEntry or ReverseEntry (the stranger learns the id is unknown);
// delete requireAuthorized from either; make entryCompany answer the seeded company for every entry; drop the
// visibility filter from GetEntry or ListEntries.
TEST_CASE("ledger entries: roles gate entries per company, and a stranger learns nothing", "[lims][ledger]") {
    DbFixture fixture;
    books();
    lims::CompanyId osaka;
    lims::JournalEntryId draft;
    lims::JournalEntryId posted;
    auto const date = morph::time::Timestamp{lims::test::at(2026, 11, 12)};
    {
        lims::test::grant("alice", lims::LimsRole::Supervisor);
        ScopedPrincipal const alice{"alice"};
        osaka = lims::OrgModel{}
                    .execute(lims::CreateCompany{.name = "Osaka branch",
                                                 .baseCurrency = lims::CurrencyCode::JPY,
                                                 .fiscalYearStart = lims::Month::April})
                    .id;
        lims::OrgModel{}.execute(
            lims::AssignRole{.principal = "dave", .role = lims::LimsRole::Accountant, .company = osaka, .site = {}});
        lims::LedgerModel ledger;
        draft = accrual(ledger, date).id;
        posted = accrual(ledger, date).id;
        ledger.execute(lims::PostEntry{.entryId = posted});
    }
    lims::LedgerModel ledger;
    lims::JournalEntryId osakaDraft;
    {
        ScopedPrincipal const dave{"dave"};
        for (std::string const code : {"1200", "4000"}) {
            ledger.execute(lims::CreateAccount{
                .company = osaka, .code = code, .name = "Account " + code, .type = lims::AccountType::Asset});
        }
        ledger.execute(lims::OpenPeriod{.company = osaka, .month = date});
        osakaDraft = ledger
                         .execute(lims::CreateDraftEntry{.company = osaka,
                                                         .date = date,
                                                         .memo = "yen",
                                                         .currency = lims::CurrencyCode::JPY,
                                                         .rate = one(),
                                                         .lines = {{.accountCode = "1200",
                                                                    .side = lims::PostingSide::Debit,
                                                                    .amount = money(1000, 0),
                                                                    .companyAmount = money(1000, 0)},
                                                                   {.accountCode = "4000",
                                                                    .side = lims::PostingSide::Credit,
                                                                    .amount = money(1000, 0),
                                                                    .companyAmount = money(1000, 0)}}})
                         .id;
    }
    {
        // No role anywhere: Forbidden before any lookup, so ids that exist and ids that do not look alike.
        ScopedPrincipal const nobody{"nobody"};
        for (auto const id : {draft, lims::JournalEntryId{9999}}) {
            CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::PostEntry{.entryId = id}), lims::Forbidden);
            CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::ReverseEntry{.entryId = id, .date = date}),
                            lims::Forbidden);
        }
        CHECK_THROWS_AS(::morph::model::executeChecked(
                            ledger, lims::CreateDraftEntry{.company = lims::CompanyId{9999},
                                                           .date = date,
                                                           .rate = one(),
                                                           .lines = {line("1200", lims::PostingSide::Debit, 1),
                                                                     line("4000", lims::PostingSide::Credit, 1)}}),
                        lims::Forbidden);
        CHECK_THROWS_AS(ledger.execute(lims::GetEntry{.entryId = posted}), lims::NotFound);
        CHECK(ledger.execute(lims::ListEntries{.company = lims::test::seededCompany()}).entries.empty());
    }
    {
        // An accountant at Osaka only: a role, but not at this entry's company.
        ScopedPrincipal const dave{"dave"};
        CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::PostEntry{.entryId = draft}), lims::Forbidden);
        CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::ReverseEntry{.entryId = posted, .date = date}),
                        lims::Forbidden);
        CHECK_THROWS_AS(::morph::model::executeChecked(
                            ledger, lims::CreateDraftEntry{.company = lims::test::seededCompany(),
                                                           .date = date,
                                                           .rate = one(),
                                                           .lines = {line("1200", lims::PostingSide::Debit, 1),
                                                                     line("4000", lims::PostingSide::Credit, 1)}}),
                        lims::Forbidden);
        CHECK_THROWS_AS(ledger.execute(lims::GetEntry{.entryId = posted}), lims::NotFound);
        CHECK(ledger.execute(lims::ListEntries{.company = lims::test::seededCompany()}).entries.empty());
        // His own company's entries are his to see.
        CHECK(ledger.execute(lims::ListEntries{.company = osaka}).entries.size() == 1U);
        CHECK(ledger.execute(lims::GetEntry{.entryId = osakaDraft}).currency == lims::CurrencyCode::JPY);
    }
    {
        lims::test::grant("erin", lims::LimsRole::Accountant);
        ScopedPrincipal const erin{"erin"};
        CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::PostEntry{.entryId = lims::JournalEntryId{9999}}),
                        lims::Forbidden);
        CHECK_THROWS_AS(::morph::model::executeChecked(
                            ledger, lims::ReverseEntry{.entryId = lims::JournalEntryId{9999}, .date = date}),
                        lims::Forbidden);
        // An entry that exists at another company: a role, but not there.
        CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::PostEntry{.entryId = osakaDraft}),
                        lims::Forbidden);
        CHECK_THROWS_AS(
            ::morph::model::executeChecked(ledger, lims::ReverseEntry{.entryId = osakaDraft, .date = date}),
            lims::Forbidden);
        CHECK_THROWS_AS(ledger.execute(lims::GetEntry{.entryId = osakaDraft}), lims::NotFound);
        CHECK(ledger.execute(lims::GetEntry{.entryId = draft}).state == lims::EntryState::Draft);
        CHECK(ledger.execute(lims::ListEntries{.company = lims::test::seededCompany()}).entries.size() == 2U);
        CHECK(ledger.execute(lims::PostEntry{.entryId = draft}).state == lims::EntryState::Posted);
    }
}

// Mutation: drop the unique index on (entry_id, position) from the migration.
TEST_CASE("ledger entries: the database refuses two lines at one position of an entry", "[lims][ledger]") {
    DbFixture fixture;
    books();
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const draft = accrual(ledger, morph::time::Timestamp{lims::test::at(2026, 11, 12)});
    Lightweight::DataMapper mapper;
    lims::db::EntryLineRecord duplicate;
    duplicate.entry = *draft.id;
    duplicate.position = 0;
    duplicate.account = 1;
    CHECK_THROWS(mapper.Create(duplicate));
}

TEST_CASE("LedgerModel over the full backend-mode matrix: a draft, its post and its reversal",
          "[lims][ledger][matrix]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    books();
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    BackendRig rig{mode, 1, lims::test::limsAuthorizer(kSecret)};
    rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "alice"));
    auto ledger = rig.client<lims::LedgerModel>(0);
    auto const draft = awaitQt(ledger.execute(lims::CreateDraftEntry{
        .company = lims::test::seededCompany(),
        .date = morph::time::Timestamp{lims::test::at(2026, 11, 12)},
        .memo = "accrual",
        .currency = lims::CurrencyCode::EUR,
        .rate = one(),
        .lines = {line("6900", lims::PostingSide::Debit, 5000), line("2200", lims::PostingSide::Credit, 5000)}}));
    CHECK(draft.state == lims::EntryState::Draft);
    CHECK(awaitQt(ledger.execute(lims::PostEntry{.entryId = draft.id})).postedBy == "alice");
    auto const reversal = awaitQt(ledger.execute(
        lims::ReverseEntry{.entryId = draft.id, .date = morph::time::Timestamp{lims::test::at(2026, 11, 20)}}));
    CHECK(reversal.reverses == draft.id);
    CHECK(awaitQt(ledger.execute(lims::GetEntry{.entryId = draft.id})).reversedBy == reversal.id);
    auto const listed = awaitQt(ledger.execute(lims::ListEntries{.company = lims::test::seededCompany()}));
    REQUIRE(listed.entries.size() == 2U);
    CHECK(listed.entries[1].lines[0].side == lims::PostingSide::Credit);
}

// Mutations: skip the rate check in entryProblem; compare against the unrounded product only; let the zero-amount
// exemption cover every line; treat a product that overflows as acceptable.
TEST_CASE("ledger entries: a foreign value follows from its amount and the rate, to half a minor unit",
          "[lims][ledger]") {
    auto const check = [](std::vector<lims::PostingLine> const& lines, lims::CurrencyCode transaction,
                          lims::CurrencyCode company,
                          lims::Rate const& rate) { return lims::entryProblem(lines, transaction, company, rate); };
    using lims::CurrencyCode;
    using lims::PostingSide;
    SECTION("USD 10.00 valued at EUR 0.01 is refused; the rate's value is accepted") {
        auto const problem =
            check({valued("1200", PostingSide::Debit, 1000, 1), valued("4000", PostingSide::Credit, 1000, 1)},
                  CurrencyCode::USD, CurrencyCode::EUR, rateOf(920000));
        REQUIRE(problem.has_value());
        CHECK_THAT(*problem, Catch::Matchers::ContainsSubstring("at the entry's rate"));
        CHECK_FALSE(
            check({valued("1200", PostingSide::Debit, 1000, 920), valued("4000", PostingSide::Credit, 1000, 920)},
                  CurrencyCode::USD, CurrencyCode::EUR, rateOf(920000))
                .has_value());
        // One cent off is more than half a cent.
        CHECK(check({valued("1200", PostingSide::Debit, 1000, 921), valued("4000", PostingSide::Credit, 1000, 921)},
                    CurrencyCode::USD, CurrencyCode::EUR, rateOf(920000))
                  .has_value());
    }
    SECTION("a zero-amount line is the rounding line and is not checked against the rate") {
        CHECK_FALSE(check({valued("1200", PostingSide::Debit, 1000, 921),
                           valued("4000", PostingSide::Credit, 1000, 920), valued("6900", PostingSide::Credit, 0, 1)},
                          CurrencyCode::USD, CurrencyCode::EUR, rateOf(920500))
                        .has_value());
    }
    SECTION("JPY in the EUR company: whole yen, euro cents") {
        // 1000 JPY x 0.0062 = 6.20 EUR.
        CHECK_FALSE(check({valued("1200", PostingSide::Debit, 1000, 620, 0, 2),
                           valued("4000", PostingSide::Credit, 1000, 620, 0, 2)},
                          CurrencyCode::JPY, CurrencyCode::EUR, rateOf(6200))
                        .has_value());
        CHECK(check({valued("1200", PostingSide::Debit, 1000, 700, 0, 2),
                     valued("4000", PostingSide::Credit, 1000, 700, 0, 2)},
                    CurrencyCode::JPY, CurrencyCode::EUR, rateOf(6200))
                  .has_value());
    }
    SECTION("EUR in the JPY company: euro cents, whole yen") {
        // 10.00 EUR x 160 = 1600 JPY.
        CHECK_FALSE(check({valued("1200", PostingSide::Debit, 1000, 1600, 2, 0),
                           valued("4000", PostingSide::Credit, 1000, 1600, 2, 0)},
                          CurrencyCode::EUR, CurrencyCode::JPY, rateOf(160000000))
                        .has_value());
        CHECK(check({valued("1200", PostingSide::Debit, 1000, 1601, 2, 0),
                     valued("4000", PostingSide::Credit, 1000, 1601, 2, 0)},
                    CurrencyCode::EUR, CurrencyCode::JPY, rateOf(160000000))
                  .has_value());
    }
    SECTION("a product that does not fit is refused, not saturated") {
        auto const huge =
            lims::Amount{Rational{Numerator{4'000'000'000'000'000'000}, Denominator{1}, DecimalPlaces{2}}};
        auto const problem =
            check({{.accountCode = "1200", .side = PostingSide::Debit, .amount = huge, .companyAmount = money(100)},
                   {.accountCode = "4000", .side = PostingSide::Credit, .amount = huge, .companyAmount = money(100)}},
                  CurrencyCode::JPY, CurrencyCode::EUR, lims::Rate{Rational{3, DecimalPlaces{6}}});
        REQUIRE(problem.has_value());
        CHECK_THAT(*problem, Catch::Matchers::ContainsSubstring("too large to total exactly"));
    }
}

// Mutations: mirror only the first line's side; swap amount and companyAmount while mirroring; rebuild the company
// value from amount x rate.
TEST_CASE("ledger entries: a reversal mirrors every line exactly, with the side flipped", "[lims][ledger]") {
    DbFixture fixture;
    books();
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const original = postedUsdEntry(ledger);
    auto const reversal = ledger.execute(
        lims::ReverseEntry{.entryId = original.id, .date = morph::time::Timestamp{lims::test::at(2026, 11, 20)}});
    REQUIRE(reversal.lines.size() == original.lines.size());
    REQUIRE(reversal.lines.size() == 4U);
    CHECK(reversal.currency == lims::CurrencyCode::USD);
    CHECK(*reversal.rate == *original.rate);
    for (std::size_t index = 0; index < original.lines.size(); ++index) {
        CAPTURE(index);
        auto const& before = original.lines[index];
        auto const& after = reversal.lines[index];
        CHECK(after.accountCode == before.accountCode);
        CHECK(after.side != before.side);
        CHECK(*after.amount == *before.amount);
        CHECK(*after.companyAmount == *before.companyAmount);
    }
    CHECK(*reversal.lines[0].companyAmount == *money(921));
    CHECK(*reversal.lines[2].companyAmount == *money(1152));
}

// Mutations: remove the SqlTransaction from ReverseEntry; drop the guard on the reversed mark; drop the guard on
// the post.
TEST_CASE("ledger entries: a failed reversal leaves nothing behind, and a lost race is a Conflict", "[lims][ledger]") {
    DbFixture fixture;
    books();
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const original = postedUsdEntry(ledger);
    auto const date = morph::time::Timestamp{lims::test::at(2026, 11, 20)};
    auto const untouched = [&] {
        CHECK(ledger.execute(lims::ListEntries{.company = lims::test::seededCompany()}).entries.size() == 1U);
        CHECK_FALSE(ledger.execute(lims::GetEntry{.entryId = original.id}).reversedBy.hasValue());
    };

    SECTION("the reversal is written, then marking the original fails") {
        execute(
            "CREATE TRIGGER fail_mark BEFORE UPDATE OF reversed_by_id ON ledger_entries "
            "BEGIN SELECT RAISE(ABORT, 'injected: the reversed mark never landed'); END");
        CHECK_THROWS(::morph::model::executeChecked(ledger, lims::ReverseEntry{.entryId = original.id, .date = date}));
        execute("DROP TRIGGER fail_mark");
        untouched();
    }
    SECTION("another reversal marks the original between the read and the mark") {
        execute(
            "CREATE TRIGGER other_reversal AFTER INSERT ON ledger_entries WHEN NEW.reverses_id IS NOT NULL "
            "BEGIN UPDATE ledger_entries SET reversed_by_id = 999 WHERE id = NEW.reverses_id; END");
        CHECK_THROWS_AS(
            ::morph::model::executeChecked(ledger, lims::ReverseEntry{.entryId = original.id, .date = date}),
            lims::Conflict);
        execute("DROP TRIGGER other_reversal");
        untouched();
    }
    SECTION("a post that matches no draft row is a Conflict and posts nothing") {
        auto const draft = accrual(ledger, date);
        execute(
            "CREATE TRIGGER already_posted BEFORE UPDATE ON ledger_entries "
            "BEGIN SELECT RAISE(IGNORE); END");
        CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::PostEntry{.entryId = draft.id}), lims::Conflict);
        execute("DROP TRIGGER already_posted");
        CHECK(ledger.execute(lims::GetEntry{.entryId = draft.id}).state == lims::EntryState::Draft);
    }
}

// Mutations: map an unknown side to Debit; map a missing account to an empty code.
TEST_CASE("ledger entries: a stored line with an unknown side or a missing account is refused on read",
          "[lims][ledger]") {
    DbFixture fixture;
    books();
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const draft = accrual(ledger, morph::time::Timestamp{lims::test::at(2026, 11, 12)});
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<lims::db::EntryLineRecord>()
                    .Where(::Lightweight::FieldNameOf<&lims::db::EntryLineRecord::entry>, "=", *draft.id)
                    .All();
    REQUIRE(rows.size() == 2U);
    auto original = rows.front();
    SECTION("a side that is neither debit nor credit") {
        rows.front().side = 7;
        mapper.Update(rows.front());
        CHECK_THROWS_AS(ledger.execute(lims::GetEntry{.entryId = draft.id}), lims::ValidationError);
    }
    SECTION("an account that does not exist") {
        rows.front().account = 424242;
        mapper.Update(rows.front());
        CHECK_THROWS_AS(ledger.execute(lims::GetEntry{.entryId = draft.id}), lims::ValidationError);
    }
    mapper.Update(original);
    CHECK(ledger.execute(lims::GetEntry{.entryId = draft.id}).lines.size() == 2U);
}

TEST_CASE("ledger entries: SQLite foreign-key enforcement on the lims connection", "[lims][ledger]") {
    DbFixture fixture;
    Lightweight::SqlStatement statement;
    auto const enforced = statement.ExecuteDirectScalar<int>("PRAGMA foreign_keys");
    REQUIRE(enforced.has_value());
    CAPTURE(*enforced);
    CHECK(*enforced == 0);
}

namespace {

/// @brief A posting of @p minor from receivables to revenue on 10 November 2026.
/// @param key The source key.
/// @param minor The amount.
/// @return The request.
[[nodiscard]] lims::PostingRequest posting(std::string key, std::int64_t minor) {
    return lims::PostingRequest{
        .company = lims::test::seededCompany(),
        .date = morph::time::Timestamp{lims::test::at(2026, 11, 10)},
        .sourceKey = key,
        .sourceReference = key,
        .currency = lims::CurrencyCode::EUR,
        .rate = one(),
        .lines = {line("1200", lims::PostingSide::Debit, minor), line("4000", lims::PostingSide::Credit, minor)}};
}

/// @brief The entries of the seeded company.
/// @return How many there are.
[[nodiscard]] std::size_t entryCount() {
    ScopedPrincipal const alice{"alice"};
    return lims::LedgerModel{}.execute(lims::ListEntries{.company = lims::test::seededCompany()}).entries.size();
}

/// @brief The frozen totals of @p period, by account code.
/// @param period The period.
/// @return Each account's debit and credit total.
[[nodiscard]] std::map<std::string, std::pair<Rational, Rational>> frozen(lims::PeriodId period) {
    Lightweight::DataMapper mapper;
    std::map<std::string, std::pair<Rational, Rational>> result;
    for (auto const& row : mapper.Query<lims::db::PeriodBalanceRecord>()
                               .Where(::Lightweight::FieldNameOf<&lims::db::PeriodBalanceRecord::period>, "=", *period)
                               .All()) {
        auto const accounts = mapper.Query<lims::db::AccountRecord>()
                                  .Where(::Lightweight::FieldNameOf<&lims::db::AccountRecord::id>, "=",
                                         static_cast<std::uint64_t>(row.account.Value()))
                                  .All();
        REQUIRE(accounts.size() == 1U);
        result.emplace(
            std::string{accounts.front().code.Value().ToStringView()},
            std::pair{
                Rational{Numerator{row.debitNum.Value()}, Denominator{row.debitDen.Value()}, DecimalPlaces{2}},
                Rational{Numerator{row.creditNum.Value()}, Denominator{row.creditDen.Value()}, DecimalPlaces{2}}});
    }
    return result;
}

/// @brief @p minor minor units as a two-decimal value.
/// @param minor The minor units.
/// @return The value.
[[nodiscard]] Rational cents(std::int64_t minor) {
    return Rational{Numerator{minor}, Denominator{100}, DecimalPlaces{2}};
}

/// @brief The rows of the posting-source table.
/// @return How many there are.
[[nodiscard]] std::size_t sourceRows() {
    Lightweight::DataMapper mapper;
    return mapper.Query<lims::db::PostingSourceRecord>().All().size();
}

/// @brief The rows of the entry-line table.
/// @return How many there are.
[[nodiscard]] std::size_t lineRows() {
    Lightweight::DataMapper mapper;
    return mapper.Query<lims::db::EntryLineRecord>().All().size();
}

}  // namespace

// Mutation: drop the replay-ledger lookup from record (the retried posting makes a second entry).
TEST_CASE("ledger entries: a posting is recorded once per source key, and a retry answers the first entry",
          "[lims][ledger]") {
    DbFixture fixture;
    books();
    auto const first = lims::LedgerModel::record(posting("billing.invoice:1", 11900));
    auto const retried = lims::LedgerModel::record(posting("billing.invoice:1", 11900));
    CHECK(retried.entryId == first.entryId);
    CHECK(entryCount() == 1U);
    ScopedPrincipal const alice{"alice"};
    auto const entry = lims::LedgerModel{}.execute(lims::GetEntry{.entryId = first.entryId});
    CHECK(entry.state == lims::EntryState::Posted);
    CHECK(entry.sourceKey == "billing.invoice:1");
    CHECK(entry.postedBy == "posting");
    // A different key is a different entry.
    CHECK(lims::LedgerModel::record(posting("billing.invoice:2", 100)).entryId != first.entryId);
    CHECK(entryCount() == 2U);
}

// Mutation: key the replay ledger's scope by a constant instead of the company (the other company's posting under
// the same key answers the first company's entry).
TEST_CASE("ledger entries: a source key is recorded once per company", "[lims][ledger]") {
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
    auto const date = morph::time::Timestamp{lims::test::at(2026, 11, 10)};
    {
        ScopedPrincipal const dave{"dave"};
        lims::LedgerModel ledger;
        for (std::string const code : {"1200", "4000"}) {
            ledger.execute(lims::CreateAccount{
                .company = osaka, .code = code, .name = "Account " + code, .type = lims::AccountType::Asset});
        }
        ledger.execute(lims::OpenPeriod{.company = osaka, .month = date});
    }
    auto const mine = lims::LedgerModel::record(posting("shared-key", 100));
    lims::PostingRequest yen{.company = osaka,
                             .date = date,
                             .sourceKey = "shared-key",
                             .sourceReference = "shared-key",
                             .currency = lims::CurrencyCode::JPY,
                             .rate = one(),
                             .lines = {{.accountCode = "1200",
                                        .side = lims::PostingSide::Debit,
                                        .amount = money(500, 0),
                                        .companyAmount = money(500, 0)},
                                       {.accountCode = "4000",
                                        .side = lims::PostingSide::Credit,
                                        .amount = money(500, 0),
                                        .companyAmount = money(500, 0)}}};
    auto const theirs = lims::LedgerModel::record(yen);
    CHECK(theirs.entryId != mine.entryId);
    CHECK(lims::LedgerModel::record(yen).entryId == theirs.entryId);
    CHECK(entryCount() == 1U);
}

// Mutation: drop the unique index on (scope, source_key) from the migration.
TEST_CASE("ledger entries: the database refuses a second row for one source key", "[lims][ledger]") {
    DbFixture fixture;
    books();
    (void)lims::LedgerModel::record(posting("billing.invoice:1", 100));
    Lightweight::DataMapper mapper;
    lims::db::PostingSourceRecord duplicate;
    duplicate.scope = Lightweight::SqlAnsiString<32>{std::to_string(*lims::test::seededCompany())};
    duplicate.sourceKey = Lightweight::SqlAnsiString<128>{"billing.invoice:1"};
    duplicate.entry = Lightweight::SqlAnsiString<32>{"99"};
    CHECK_THROWS(mapper.Create(duplicate));
}

// Mutation: drop the comparison from answerKnown (a different body under a recorded key answers the first entry).
TEST_CASE("ledger entries: a recorded key with a different body is a Conflict, an identical repeat is not",
          "[lims][ledger]") {
    DbFixture fixture;
    books();
    auto const base = posting("k", 100);
    auto const first = lims::LedgerModel::record(base);
    REQUIRE(entryCount() == 1U);
    auto const sources = sourceRows();
    auto const lines = lineRows();

    SECTION("an identical repeat answers the first entry and adds no row to either table") {
        CHECK(lims::LedgerModel::record(base).entryId == first.entryId);
        CHECK(sourceRows() == sources);
        CHECK(lineRows() == lines);
    }
    SECTION("an identical repeat with its amounts written at other decimals is still identical") {
        auto repeat = base;
        for (auto& each : repeat.lines) {
            each.amount = money(100, 4);
            each.companyAmount = money(100, 4);
        }
        CHECK(lims::LedgerModel::record(repeat).entryId == first.entryId);
    }
    SECTION("another amount") { CHECK_THROWS_AS(lims::LedgerModel::record(posting("k", 200)), lims::Conflict); }
    SECTION("another source reference") {
        auto changed = base;
        changed.sourceReference = "other";
        CHECK_THROWS_AS(lims::LedgerModel::record(changed), lims::Conflict);
    }
    SECTION("the sides exchanged between the lines") {
        auto changed = base;
        changed.lines = {line("1200", lims::PostingSide::Credit, 100), line("4000", lims::PostingSide::Debit, 100)};
        CHECK_THROWS_AS(lims::LedgerModel::record(changed), lims::Conflict);
    }
    SECTION("another account on one line") {
        auto changed = base;
        changed.lines = {line("1200", lims::PostingSide::Debit, 100), line("6900", lims::PostingSide::Credit, 100)};
        CHECK_THROWS_AS(lims::LedgerModel::record(changed), lims::Conflict);
    }
    SECTION("another date") {
        auto changed = base;
        changed.date = morph::time::Timestamp{lims::test::at(2026, 11, 11)};
        CHECK_THROWS_AS(lims::LedgerModel::record(changed), lims::Conflict);
    }
    SECTION("the same lines in another order") {
        auto changed = base;
        std::swap(changed.lines.front(), changed.lines.back());
        CHECK_THROWS_AS(lims::LedgerModel::record(changed), lims::Conflict);
    }
    CHECK(entryCount() == 1U);
    CHECK(sourceRows() == sources);
    CHECK(lineRows() == lines);
}

// Mutation: move the key lookup after the period check (a repeat into a since-closed period is refused).
TEST_CASE("ledger entries: an identical repeat answers the first entry after its period has closed",
          "[lims][ledger]") {
    DbFixture fixture;
    auto const november = books();
    auto const first = lims::LedgerModel::record(posting("k", 100));
    forceState(november.id, lims::PeriodState::Closed);
    CHECK(lims::LedgerModel::record(posting("k", 100)).entryId == first.entryId);
    CHECK_THROWS_AS(lims::LedgerModel::record(posting("k", 200)), lims::Conflict);
}

// Mutation: drop the catch of the unique-key failure from record (the loser surfaces a database exception).
TEST_CASE("ledger entries: simultaneous first calls with one key record one entry and both answer it",
          "[lims][ledger]") {
    DbFixture fixture;
    books();
    constexpr int kRounds = 20;
    std::atomic<int> failures{0};
    for (int round = 0; round < kRounds; ++round) {
        auto const request = posting("race:" + std::to_string(round), 100);
        std::atomic<bool> go{false};
        std::int64_t answers[2] = {0, 0};
        auto const call = [&](int slot) {
            while (!go.load()) {
                std::this_thread::yield();
            }
            try {
                answers[slot] = *lims::LedgerModel::record(request).entryId;
            } catch (...) {
                ++failures;
            }
        };
        std::thread a{call, 0};
        std::thread b{call, 1};
        go = true;
        a.join();
        b.join();
        CHECK(answers[0] != 0);
        CHECK(answers[0] == answers[1]);
    }
    CHECK(failures.load() == 0);
    CHECK(entryCount() == static_cast<std::size_t>(kRounds));
    CHECK(sourceRows() == static_cast<std::size_t>(kRounds));
}

// Mutation: replace the OutOfRange throw in ClosePeriod's freeze with `continue`.
TEST_CASE("ledger entries: a total too large to freeze is OutOfRange and leaves the period closing",
          "[lims][ledger]") {
    DbFixture fixture;
    auto const november = books();
    (void)lims::LedgerModel::record(posting("billing.invoice:1", 100));
    (void)lims::LedgerModel::record(posting("billing.invoice:2", 100));
    {
        Lightweight::DataMapper mapper;
        for (auto& row : mapper.Query<lims::db::EntryLineRecord>().All()) {
            row.companyNum = std::numeric_limits<std::int64_t>::max();
            row.companyDen = 1;
            mapper.Update(row);
        }
    }
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    ledger.execute(lims::BeginClosePeriod{.periodId = november.id});
    CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::ClosePeriod{.periodId = november.id}),
                    lims::OutOfRange);
    Lightweight::DataMapper mapper;
    CHECK(mapper.Query<lims::db::PeriodBalanceRecord>().All().empty());
    auto const periods = mapper.Query<lims::db::PeriodRecord>()
                             .Where(::Lightweight::FieldNameOf<&lims::db::PeriodRecord::id>, "=", *november.id)
                             .All();
    REQUIRE(periods.size() == 1U);
    CHECK(periods.front().state.Value() == static_cast<int>(lims::PeriodState::Closing));
}

// Mutations: drop requireBalanced, requireRate or the malformed-request check from record; drop the
// requireRecordable call from record; let accountIds ignore the company.
TEST_CASE("ledger entries: a posting that is malformed, unbalanced or unplaceable is refused and writes nothing",
          "[lims][ledger]") {
    DbFixture fixture;
    books();
    CHECK_THROWS_AS(lims::LedgerModel::record(posting("billing.invoice:1", 0)), lims::ValidationError);
    auto unbalanced = posting("billing.invoice:2", 100);
    unbalanced.lines[1] = line("4000", lims::PostingSide::Credit, 99);
    CHECK_THROWS_AS(lims::LedgerModel::record(unbalanced), lims::ValidationError);
    auto badRate = posting("billing.invoice:3", 100);
    badRate.rate = rateOf(1100000);
    CHECK_THROWS_AS(lims::LedgerModel::record(badRate), lims::ValidationError);
    auto noKey = posting("", 100);
    CHECK_THROWS_AS(lims::LedgerModel::record(noKey), lims::ValidationError);
    auto noCompany = posting("billing.invoice:4", 100);
    noCompany.company = lims::CompanyId{};
    CHECK_THROWS_AS(lims::LedgerModel::record(noCompany), lims::ValidationError);
    auto unknownCompany = posting("billing.invoice:5", 100);
    unknownCompany.company = lims::CompanyId{9999};
    CHECK_THROWS_AS(lims::LedgerModel::record(unknownCompany), lims::NotFound);
    auto unknown = posting("billing.invoice:6", 100);
    unknown.lines[0].accountCode = "9999";
    CHECK_THROWS_AS(lims::LedgerModel::record(unknown), lims::NotFound);
    auto noPeriod = posting("billing.invoice:7", 100);
    noPeriod.date = morph::time::Timestamp{lims::test::at(2026, 12, 1)};
    CHECK_THROWS_AS(lims::LedgerModel::record(noPeriod), lims::NotFound);
    // A foreign-currency posting needs a ledger rate for its date; the ledger holds none for the pair.
    lims::PostingRequest usd = posting("billing.invoice:8", 100);
    usd.currency = lims::CurrencyCode::USD;
    usd.rate = rateOf(920000);
    usd.lines = {valued("1200", lims::PostingSide::Debit, 100, 92),
                 valued("4000", lims::PostingSide::Credit, 100, 92)};
    CHECK_THROWS_AS(lims::LedgerModel::record(usd), lims::ValidationError);
    CHECK(entryCount() == 0U);
    // None of the refusals recorded its key: the same key records once the request is right.
    CHECK_NOTHROW((void)lims::LedgerModel::record(posting("billing.invoice:7", 100)));
    CHECK(entryCount() == 1U);
}

// Mutation: skip the draft check in ClosePeriod (the period closes with a draft in it).
TEST_CASE("ledger entries: a period closes only without drafts, and nothing is recorded in it afterwards",
          "[lims][ledger]") {
    DbFixture fixture;
    auto const november = books();
    (void)lims::LedgerModel::record(posting("billing.invoice:1", 1000));
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const draft = accrual(ledger, morph::time::Timestamp{lims::test::at(2026, 11, 12)});
    // Not closing yet: ClosePeriod is refused even with no drafts to wait for.
    CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::ClosePeriod{.periodId = november.id}),
                    lims::IllegalTransition);
    ledger.execute(lims::BeginClosePeriod{.periodId = november.id});
    // Closing: no new drafts, postings still recorded.
    CHECK_THROWS_AS(accrual(ledger, morph::time::Timestamp{lims::test::at(2026, 11, 13)}), lims::Conflict);
    (void)lims::LedgerModel::record(posting("billing.invoice:2", 2000));
    CHECK_THROWS_WITH(::morph::model::executeChecked(ledger, lims::ClosePeriod{.periodId = november.id}),
                      Catch::Matchers::ContainsSubstring("draft"));
    CHECK(frozen(november.id).empty());
    ledger.execute(lims::PostEntry{.entryId = draft.id});
    auto const closed = ledger.execute(lims::ClosePeriod{.periodId = november.id});
    CHECK(closed.state == lims::PeriodState::Closed);
    CHECK(closed.id == november.id);
    CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::ClosePeriod{.periodId = november.id}),
                    lims::IllegalTransition);
    // Closed: nothing new enters it.
    CHECK_THROWS_AS(lims::LedgerModel::record(posting("billing.invoice:3", 3000)), lims::Conflict);
    CHECK_THROWS_AS(accrual(ledger, morph::time::Timestamp{lims::test::at(2026, 11, 14)}), lims::Conflict);
    CHECK_THROWS_AS(::morph::model::executeChecked(
                        ledger, lims::ReverseEntry{.entryId = draft.id,
                                                   .date = morph::time::Timestamp{lims::test::at(2026, 11, 30)}}),
                    lims::Conflict);
    // A retried posting from before the close still answers its entry.
    CHECK_NOTHROW((void)lims::LedgerModel::record(posting("billing.invoice:2", 2000)));
    CHECK(entryCount() == 3U);
}

// Mutations: skip the freeze loop in ClosePeriod (no rows); sum debits and credits into one side; drop the Closed
// refusal from requireRecordable (the December reversal below would still pass, the November one would not).
TEST_CASE("ledger entries: closing freezes each account's totals, and later entries do not change them",
          "[lims][ledger]") {
    DbFixture fixture;
    auto const november = books();
    (void)lims::LedgerModel::record(posting("billing.invoice:1", 1000));
    (void)lims::LedgerModel::record(posting("billing.invoice:2", 2000));
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const accrued = accrual(ledger, morph::time::Timestamp{lims::test::at(2026, 11, 12)});
    ledger.execute(lims::PostEntry{.entryId = accrued.id});
    auto const december = ledger.execute(lims::OpenPeriod{
        .company = lims::test::seededCompany(), .month = morph::time::Timestamp{lims::test::at(2026, 12, 1)}});
    ledger.execute(lims::BeginClosePeriod{.periodId = november.id});
    ledger.execute(lims::ClosePeriod{.periodId = november.id});

    auto const balances = frozen(november.id);
    REQUIRE(balances.size() == 4U);
    CHECK(balances.at("1200").first == cents(3000));
    CHECK(balances.at("1200").second == cents(0));
    CHECK(balances.at("4000").first == cents(0));
    CHECK(balances.at("4000").second == cents(3000));
    CHECK(balances.at("6900").first == cents(5000));
    CHECK(balances.at("2200").second == cents(5000));

    // December takes a reversal of a November entry; November's totals stay as they were.
    auto const reversal = ledger.execute(
        lims::ReverseEntry{.entryId = accrued.id, .date = morph::time::Timestamp{lims::test::at(2026, 12, 2)}});
    CHECK(reversal.period == december.id);
    auto inDecember = posting("billing.invoice:3", 700);
    inDecember.date = morph::time::Timestamp{lims::test::at(2026, 12, 3)};
    (void)lims::LedgerModel::record(inDecember);
    auto const after = frozen(november.id);
    REQUIRE(after.size() == 4U);
    for (auto const& [code, totals] : balances) {
        CAPTURE(code);
        CHECK(after.at(code).first == totals.first);
        CHECK(after.at(code).second == totals.second);
    }
    CHECK(frozen(december.id).empty());
}

// Mutation: drop the unique index on (period_id, account_id) from the migration.
TEST_CASE("ledger entries: the database refuses two frozen rows for one account of a period", "[lims][ledger]") {
    DbFixture fixture;
    auto const november = books();
    (void)lims::LedgerModel::record(posting("billing.invoice:1", 1000));
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    ledger.execute(lims::BeginClosePeriod{.periodId = november.id});
    ledger.execute(lims::ClosePeriod{.periodId = november.id});
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<lims::db::PeriodBalanceRecord>().All();
    REQUIRE_FALSE(rows.empty());
    lims::db::PeriodBalanceRecord duplicate;
    duplicate.period = rows.front().period.Value();
    duplicate.account = rows.front().account.Value();
    CHECK_THROWS(mapper.Create(duplicate));
}

// Mutation: sum a foreign line at its transaction amount instead of its company amount.
TEST_CASE("ledger entries: frozen totals are in company currency", "[lims][ledger]") {
    DbFixture fixture;
    auto const november = books();
    ScopedPrincipal const alice{"alice"};
    lims::LedgerModel ledger;
    auto const draft =
        ledger.execute(lims::CreateDraftEntry{.company = lims::test::seededCompany(),
                                              .date = morph::time::Timestamp{lims::test::at(2026, 11, 12)},
                                              .memo = "usd",
                                              .currency = lims::CurrencyCode::USD,
                                              .rate = rateOf(920000),
                                              .lines = {valued("1200", lims::PostingSide::Debit, 1000, 920),
                                                        valued("4000", lims::PostingSide::Credit, 1000, 920)}});
    ledger.execute(lims::PostEntry{.entryId = draft.id});
    ledger.execute(lims::BeginClosePeriod{.periodId = november.id});
    ledger.execute(lims::ClosePeriod{.periodId = november.id});
    auto const balances = frozen(november.id);
    REQUIRE(balances.size() == 2U);
    CHECK(balances.at("1200").first == cents(920));
    CHECK(balances.at("4000").second == cents(920));
}

// Mutations: delete requireRoleSomewhere or requireAuthorized from ClosePeriod; make periodCompany answer the
// seeded company for every period; delete validate() from ClosePeriod.
TEST_CASE("ledger entries: closing a period is an accountant's, at its company", "[lims][ledger]") {
    DbFixture fixture;
    auto const november = books();
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
        lims::LedgerModel{}.execute(lims::BeginClosePeriod{.periodId = november.id});
    }
    lims::LedgerModel ledger;
    {
        ScopedPrincipal const nobody{"nobody"};
        for (auto const id : {november.id, lims::PeriodId{9999}}) {
            CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::ClosePeriod{.periodId = id}),
                            lims::Forbidden);
        }
    }
    {
        ScopedPrincipal const dave{"dave"};
        CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::ClosePeriod{.periodId = november.id}),
                        lims::Forbidden);
        CHECK_THROWS_AS(::morph::model::executeChecked(ledger, lims::ClosePeriod{.periodId = lims::PeriodId{9999}}),
                        lims::Forbidden);
        CHECK_THROWS_AS(ledger.execute(lims::ClosePeriod{}), lims::ValidationError);
        auto const osakaPeriod = ledger.execute(
            lims::OpenPeriod{.company = osaka, .month = morph::time::Timestamp{lims::test::at(2026, 11, 1)}});
        ledger.execute(lims::BeginClosePeriod{.periodId = osakaPeriod.id});
        CHECK(ledger.execute(lims::ClosePeriod{.periodId = osakaPeriod.id}).state == lims::PeriodState::Closed);
    }
    {
        ScopedPrincipal const alice{"alice"};
        CHECK(frozen(november.id).empty());
    }
}

TEST_CASE("LedgerModel over the full backend-mode matrix: a period is begun and closed", "[lims][ledger][matrix]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto const november = books();
    (void)lims::LedgerModel::record(posting("billing.invoice:1", 1000));
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    BackendRig rig{mode, 1, lims::test::limsAuthorizer(kSecret)};
    rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "alice"));
    auto ledger = rig.client<lims::LedgerModel>(0);
    CHECK(awaitQt(ledger.execute(lims::BeginClosePeriod{.periodId = november.id})).state ==
          lims::PeriodState::Closing);
    auto const closed = awaitQt(ledger.execute(lims::ClosePeriod{.periodId = november.id}));
    CHECK(closed.state == lims::PeriodState::Closed);
    CHECK(closed.id == november.id);
    CHECK(frozen(november.id).size() == 2U);
}

namespace {

/// @brief One journal entry as the page tests write it: straight into the tables, so every cell can be made to
///        differ, be empty or hold text the collator folds.
struct RawEntry {
    std::int64_t dateOffsetMs = 0;
    lims::EntryState state = lims::EntryState::Posted;
    std::string sourceKey;
    std::string sourceReference;
    std::string memo;
    lims::CurrencyCode currency = lims::CurrencyCode::EUR;
    std::optional<std::int64_t> reverses;
    std::optional<std::int64_t> reversedBy;
    std::string createdBy = "alice";
    std::string postedBy = "alice";
};

/// @brief The journal the page tests page through: duplicate sort keys, empty cells in every nullable column,
///        accents, upper and lower case, a non-Latin-1 capital, characters a statement could misread, and instants
///        within one second, across a second and before the epoch.
/// @return The entries, in id order.
[[nodiscard]] std::vector<RawEntry> journalRows() {
    using lims::EntryState;
    std::vector<RawEntry> rows;
    auto const add = [&](RawEntry entry) { rows.push_back(std::move(entry)); };
    add({.dateOffsetMs = 0, .sourceKey = "a", .sourceReference = "Ref A", .memo = "Rent"});
    add({.dateOffsetMs = 250, .sourceKey = "A", .sourceReference = "ref a", .memo = "rent"});
    add({.dateOffsetMs = 999, .sourceKey = "", .sourceReference = "", .memo = "", .postedBy = ""});
    add({.dateOffsetMs = 1000, .state = EntryState::Draft, .sourceKey = "", .memo = "Rent", .postedBy = ""});
    add({.dateOffsetMs = 1001,
         .sourceKey = "\xC3\x89"
                      "clair",
         .sourceReference = "x",
         .memo = "\xC3\x89"
                 "CLAIR"});
    add({.dateOffsetMs = 86'400'000,
         .sourceKey = "\xC3\xA9"
                      "clair",
         .sourceReference = "X",
         .memo = "\xC3\xA9"
                 "clair"});
    add({.dateOffsetMs = 86'400'000,
         .sourceKey = "\xCE\xA9"
                      "mega",
         .sourceReference = "\xCF\x89"
                            "mega",
         .memo = "omega"});
    add({.dateOffsetMs = -2'000'000'000'123, .sourceKey = "a?b", .sourceReference = "{fn now()}", .memo = "it's"});
    add({.dateOffsetMs = 172'800'000,
         .sourceKey = "a_b",
         .sourceReference = "a%b",
         .memo = "O'Brien",
         .createdBy = "bob"});
    add({.dateOffsetMs = 172'800'500, .sourceKey = "Zeta", .memo = "z", .currency = lims::CurrencyCode::USD});
    add({.dateOffsetMs = 172'800'999, .sourceKey = "zeta", .memo = "Z", .currency = lims::CurrencyCode::USD});
    add({.dateOffsetMs = 259'200'000,
         .sourceKey = "\xC3\x9C"
                      "nder",
         .memo = "\xC3\xBC"
                 "nder",
         .currency = lims::CurrencyCode::GBP});
    add({.dateOffsetMs = 259'200'000,
         .sourceKey = "\xC3\x97"
                      "x",
         .sourceReference = "\xC3\xB7",
         .memo = "\xC3\x97"});
    add({.dateOffsetMs = 345'600'000, .sourceKey = "r1", .memo = "reversal of 1", .reverses = 1});
    add({.dateOffsetMs = 345'600'001, .sourceKey = "r5", .memo = "reversal of 5", .reverses = 5});
    add({.dateOffsetMs = 432'000'000, .sourceKey = "k15", .memo = "Rent"});
    add({.dateOffsetMs = 432'000'000, .sourceKey = "k16", .memo = "Rent"});
    add({.dateOffsetMs = 518'400'000, .state = EntryState::Draft, .sourceKey = "d", .memo = "draft", .postedBy = ""});
    add({.dateOffsetMs = 604'800'000, .sourceKey = "k19", .memo = "Rent"});
    add({.dateOffsetMs = 691'200'000, .sourceKey = "k20", .memo = "m20"});
    add({.dateOffsetMs = 777'600'000, .sourceKey = "k21", .memo = "m21"});
    add({.dateOffsetMs = 864'000'000, .sourceKey = "k22", .memo = "m22"});
    add({.dateOffsetMs = 950'400'000, .sourceKey = "k23", .memo = "m23"});
    add({.dateOffsetMs = 1'036'800'000, .sourceKey = "k24", .memo = "m24"});
    add({.dateOffsetMs = 1'100'000'000, .sourceKey = "x'; DROP TABLE ledger_entries; --", .memo = "back\\slash"});
    add({.dateOffsetMs = 1'200'000'000,
         .sourceKey = "100%_done",
         .sourceReference = "\"quoted\"",
         .memo = "C:\\temp\\%"});
    rows[0].reversedBy = 14;
    rows[4].reversedBy = 15;
    return rows;
}

/// @brief Writes @p rows as the seeded company's journal, two lines each, and returns the ids.
/// @param period The period every entry is in.
/// @param rows The entries.
void writeJournal(lims::PeriodId period, std::vector<RawEntry> const& rows) {
    Lightweight::DataMapper mapper;
    std::int64_t asset = 0;
    std::int64_t revenue = 0;
    for (auto const& account : mapper.Query<lims::db::AccountRecord>().All()) {
        (account.code.Value().ToStringView() == "1200" ? asset : revenue) =
            static_cast<std::int64_t>(account.id.Value());
    }
    std::int64_t const base = lims::millisOf(morph::time::Timestamp{lims::test::at(2026, 11, 10)});
    std::int64_t index = 0;
    for (auto const& raw : rows) {
        lims::db::EntryRecord entry;
        entry.companyId = *lims::test::seededCompany();
        // Three periods, so the period column is filtered to a part of the journal.
        entry.period = *period + (index % 3 == 0 ? 100 : 0);
        entry.dateMs = base + raw.dateOffsetMs;
        entry.state = static_cast<int>(raw.state);
        entry.sourceKey = raw.sourceKey;
        entry.sourceReference = raw.sourceReference;
        entry.memo = raw.memo;
        entry.currency = static_cast<int>(raw.currency);
        entry.rateNum = raw.currency == lims::CurrencyCode::EUR ? 1 : 92;
        entry.rateDen = raw.currency == lims::CurrencyCode::EUR ? 1 : 100;
        entry.reversesId = raw.reverses;
        entry.reversedById = raw.reversedBy;
        entry.createdBy = raw.createdBy;
        entry.postedBy = raw.postedBy;
        mapper.Create(entry);
        auto const entryId = static_cast<std::int64_t>(entry.id.Value());
        for (int position = 0; position < 2; ++position) {
            lims::db::EntryLineRecord row;
            row.entry = entryId;
            row.position = position;
            row.account = position == 0 ? asset : revenue;
            row.side = static_cast<int>(position == 0 ? lims::PostingSide::Debit : lims::PostingSide::Credit);
            row.amountNum = 1000 + index;
            row.amountDen = 100;
            row.companyNum = 1000 + index;
            row.companyDen = 100;
            mapper.Create(row);
        }
        ++index;
    }
}

/// @brief The date-time queries the standard corpus cannot make: its operands are the cells' own text, and a date-time
///        cell's text is not a date the parser reads. The operands are the whole seconds of the rows' own dates, so
///        every equality, bound and tie falls on a boundary, and a few instants that match no row.
/// @param entries The journal.
/// @return The queries.
[[nodiscard]] std::vector<morph::table::TableQuery> dateQueries(std::vector<lims::JournalEntryView> const& entries) {
    using morph::table::ColumnFilter;
    using morph::table::FilterEntry;
    using morph::table::SortDirection;
    using morph::table::SortKey;
    using morph::table::TableQuery;
    std::vector<TableQuery> queries;
    auto const filtered = [&](std::string const& column, FilterEntry entry, bool asExclude) {
        TableQuery query;
        ColumnFilter filter;
        (asExclude ? filter.exclude : filter.include).push_back(std::move(entry));
        query.filters.columns.emplace(column, std::move(filter));
        query.sort = {SortKey{.column = "date", .dir = SortDirection::Descending}};
        queries.push_back(std::move(query));
    };
    std::vector<std::string> instants{"2026-11-10T09:00:00.500Z", "2026-11-11", "1969-12-08T00:00:00Z"};
    for (auto const& entry : entries) {
        auto const second = lims::test::isoSecond(entry.date);
        if (std::ranges::find(instants, second) == instants.end()) {
            instants.push_back(second);
        }
    }
    for (bool const asExclude : {false, true}) {
        for (std::string const& instant : instants) {
            filtered("date", FilterEntry{.eq = instant}, asExclude);
            filtered("date", FilterEntry{.ne = instant}, asExclude);
            filtered("date", FilterEntry{.lt = instant}, asExclude);
            filtered("date", FilterEntry{.le = instant}, asExclude);
            filtered("date", FilterEntry{.gt = instant}, asExclude);
            filtered("date", FilterEntry{.ge = instant}, asExclude);
            filtered("date", FilterEntry{.between = std::array<std::string, 2>{instant, "2026-11-12"}}, asExclude);
        }
    }
    TableQuery ties;
    ties.sort = {SortKey{.column = "date", .dir = SortDirection::Ascending}};
    queries.push_back(ties);
    ties.sort = {SortKey{.column = "memo", .dir = SortDirection::Descending},
                 SortKey{.column = "date", .dir = SortDirection::Ascending}};
    queries.push_back(ties);
    return queries;
}

/// @brief Quick-search group queries over the journal's text columns that the database can compare. The standard
///        corpus's group spans every text column, `lines` among them, and is refused whole.
/// @return The queries.
[[nodiscard]] std::vector<morph::table::TableQuery> groupQueries() {
    using morph::table::FilterEntry;
    using morph::table::GroupFilter;
    using morph::table::TableQuery;
    std::vector<TableQuery> queries;
    std::vector<std::string> const columns{"sourceKey", "sourceReference", "memo",    "state",
                                           "currency",  "createdBy",       "postedBy"};
    auto const grouped = [&](morph::table::Combine combine, std::vector<FilterEntry> entries) {
        TableQuery query;
        query.filters.groups.push_back(
            GroupFilter{.columns = columns, .combine = combine, .include = std::move(entries)});
        queries.push_back(std::move(query));
    };
    grouped(morph::table::Combine::Any, {FilterEntry{.contains = "a"}});
    grouped(morph::table::Combine::Any, {FilterEntry{.contains = "E"}});
    grouped(morph::table::Combine::Any, {FilterEntry{.startsWith = "r"}, FilterEntry{.eq = "usd"}});
    grouped(morph::table::Combine::All, {FilterEntry{.contains = "e"}, FilterEntry{.contains = "r"}});
    grouped(morph::table::Combine::Any, {FilterEntry{.endsWith = "%"}, FilterEntry{.contains = "'"}});
    return queries;
}

/// @brief The seeded company's whole journal as the engine's source rows.
/// @return The entries by id.
[[nodiscard]] std::shared_ptr<std::vector<lims::JournalEntryView> const> journalRowsFromLedger() {
    auto const listed = lims::LedgerModel{}.execute(lims::ListEntries{.company = lims::test::seededCompany()});
    return std::make_shared<std::vector<lims::JournalEntryView> const>(listed.entries);
}

}  // namespace

// The page the database returns is the page the table engine returns over the same rows, for every sort and filter the
// standard corpus derives from the rows and for the date queries it cannot derive, with the total cap lowered so the
// above-cap rule runs on a small journal.
// Mutations: drop the plan's filters from the statement (translate returns "1" for the filter); order empty cells
// first; render an exclude as NOT (cell condition) without COALESCE; round an integer operand; drop the Latin-1 fold;
// order by the transaction-time seconds without the floor.
TEST_CASE(
    "ledger: a page of entries from the database equals the engine's page over the same rows, for every sort and "
    "filter the journal offers",
    "[lims][ledger][pages]") {
    DbFixture fixture;
    auto const period = books();
    writeJournal(period.id, journalRows());
    ScopedPrincipal const alice{"alice"};
    auto const rows = journalRowsFromLedger();
    REQUIRE(rows->size() == journalRows().size());
    morph::table::RowsSource<lims::JournalEntryView> const source{
        rows, [](lims::JournalEntryView const& entry) { return morph::table::RowId{*entry.id}; }};

    auto corpus = morph::table::standardCorpus(source);
    auto const standardQueries = corpus.size();
    for (auto&& query : dateQueries(*rows)) {
        corpus.push_back(std::move(query));
    }
    CHECK(standardQueries > 1000U);
    for (auto&& query : groupQueries()) {
        corpus.push_back(std::move(query));
    }

    morph::table::ConformanceOptions options;
    options.limits.totalCap = 10;
    options.translatable = lims::LedgerModel::canTranslateEntryColumn;
    auto const company = lims::test::seededCompany();
    auto const model =
        [&](morph::table::QueryPlan const& plan) -> std::expected<morph::table::ModelPage, morph::table::TableError> {
        auto const page = lims::LedgerModel::pageEntries(company, plan);
        morph::table::ModelPage out;
        for (auto const& row : page.rows) {
            out.rows.emplace_back(*row.id);
        }
        out.total = page.total;
        out.totalKind = page.totalKind;
        out.offset = page.offset;
        return out;
    };
    auto const failures = morph::table::conformanceFailures(source, model, corpus, options);
    for (std::size_t i = 0; i < failures.size() && i < 10; ++i) {
        WARN(failures[i]);
    }
    CHECK(failures.empty());
}

// The control for the case above: the journal and the corpus compare what the helper is said to compare. Every column
// the database can translate is filtered, somewhere in the corpus, by a query the plan accepts and whose view is
// neither empty nor the whole journal, except the company, which every entry shares. The standard quick-search group
// spans `lines` and is refused as Unavailable, so the group queries of the case above are the ones that are compared.
TEST_CASE("ledger: the entry page refuses the columns the database cannot compare, and nothing else",
          "[lims][ledger][pages]") {
    DbFixture fixture;
    auto const period = books();
    writeJournal(period.id, journalRows());
    ScopedPrincipal const alice{"alice"};
    auto const rows = journalRowsFromLedger();
    morph::table::RowsSource<lims::JournalEntryView> const source{
        rows, [](lims::JournalEntryView const& entry) { return morph::table::RowId{*entry.id}; }};
    std::vector<std::string> refused;
    for (auto const& column : source.columns()) {
        if (!lims::LedgerModel::canTranslateEntryColumn(column.id, morph::table::PlanUse::Sort)) {
            refused.push_back(column.id);
        }
    }
    CHECK(refused == std::vector<std::string>{"rate", "lines"});

    auto corpus = morph::table::standardCorpus(source);
    for (auto&& query : groupQueries()) {
        corpus.push_back(std::move(query));
    }
    std::map<std::string, bool> partial;
    std::size_t refusedGroups = 0;
    std::size_t translatedGroups = 0;
    auto const rowCount = static_cast<std::int64_t>(rows->size());
    for (auto const& query : corpus) {
        auto const planned =
            morph::table::planQuery(query, source.columns(), {}, {}, {}, lims::LedgerModel::canTranslateEntryColumn);
        if (!query.filters.groups.empty()) {
            if (planned) {
                ++translatedGroups;
            } else {
                CHECK(planned.error().code == morph::table::TableErrorCode::Unavailable);
                CHECK((planned.error().column == "lines" || planned.error().column == "rate"));
                ++refusedGroups;
            }
            continue;
        }
        if (!planned || query.filters.columns.size() != 1U) {
            continue;
        }
        auto counting = query;
        counting.page = morph::table::PageRequest{.offset = 0, .limit = 0};
        auto const counted = morph::table::apply(source, counting);
        REQUIRE(counted.has_value());
        if (counted->total && *counted->total > 0 && *counted->total < rowCount) {
            partial[query.filters.columns.begin()->first] = true;
        }
    }
    for (auto const& column : source.columns()) {
        if (!lims::LedgerModel::canTranslateEntryColumn(column.id, morph::table::PlanUse::Filter) ||
            column.id == "company") {
            continue;
        }
        INFO("column " << column.id);
        CHECK(partial[column.id]);
    }
    for (auto const& entry : *rows) {
        CHECK(entry.company == lims::test::seededCompany());
    }
    CHECK(refusedGroups > 0U);
    CHECK(translatedGroups >= 5U);

    lims::LedgerModel ledger;
    for (std::string const column : {"lines", "rate"}) {
        CAPTURE(column);
        lims::ListEntriesPage sorted{.company = lims::test::seededCompany(), .query = {}};
        sorted.query.sort = {morph::table::SortKey{.column = column, .dir = morph::table::SortDirection::Ascending}};
        CHECK_THROWS_AS(ledger.execute(sorted), lims::ValidationError);
    }
    lims::ListEntriesPage unknown{.company = lims::test::seededCompany(), .query = {}};
    unknown.query.sort = {morph::table::SortKey{.column = "nope", .dir = morph::table::SortDirection::Ascending}};
    CHECK_THROWS_AS(ledger.execute(unknown), lims::ValidationError);
}

// Operands shaped like injection and like pattern syntax are data: quotes, a statement terminator, a comment
// marker, `%`, `_`, backslashes and a driver escape. They are bound, so the page equals the engine's and the table is
// untouched; a NUL byte, which SQLite's text functions stop at, is refused in words.
// Mutation: write a text operand into the statement unescaped.
TEST_CASE("ledger: text operands that look like SQL or like patterns are only data", "[lims][ledger][pages]") {
    DbFixture fixture;
    auto const period = books();
    writeJournal(period.id, journalRows());
    ScopedPrincipal const alice{"alice"};
    auto const rows = journalRowsFromLedger();
    morph::table::RowsSource<lims::JournalEntryView> const source{
        rows, [](lims::JournalEntryView const& entry) { return morph::table::RowId{*entry.id}; }};
    using morph::table::ColumnFilter;
    using morph::table::FilterEntry;
    using morph::table::TableQuery;
    std::vector<TableQuery> corpus;
    for (std::string const operand :
         {"'", "''", "x'; DROP TABLE ledger_entries; --", "' OR '1'='1", "%", "_", "100%", "100%_done", "\\",
          "back\\slash", "C:\\temp\\%", "\"quoted\"", "{fn now()}", "a?b", "?", "--", "/*",
          "\xC3\x89"
          "clair"}) {
        for (std::string const column : {"memo", "sourceKey", "sourceReference"}) {
            for (bool const caseSensitive : {false, true}) {
                for (int op = 0; op < 5; ++op) {
                    FilterEntry entry;
                    entry.caseSensitive = caseSensitive;
                    switch (op) {
                        case 0:
                            entry.eq = operand;
                            break;
                        case 1:
                            entry.ne = operand;
                            break;
                        case 2:
                            entry.contains = operand;
                            break;
                        case 3:
                            entry.startsWith = operand;
                            break;
                        default:
                            entry.endsWith = operand;
                            break;
                    }
                    TableQuery query;
                    ColumnFilter filter;
                    filter.include.push_back(entry);
                    query.filters.columns.emplace(column, std::move(filter));
                    corpus.push_back(std::move(query));
                }
            }
        }
    }
    morph::table::ConformanceOptions options;
    options.translatable = lims::LedgerModel::canTranslateEntryColumn;
    auto const company = lims::test::seededCompany();
    auto const model =
        [&](morph::table::QueryPlan const& plan) -> std::expected<morph::table::ModelPage, morph::table::TableError> {
        auto const page = lims::LedgerModel::pageEntries(company, plan);
        morph::table::ModelPage out;
        for (auto const& row : page.rows) {
            out.rows.emplace_back(*row.id);
        }
        out.total = page.total;
        out.totalKind = page.totalKind;
        out.offset = page.offset;
        return out;
    };
    auto const failures = morph::table::conformanceFailures(source, model, corpus, options);
    for (std::size_t i = 0; i < failures.size() && i < 10; ++i) {
        WARN(failures[i]);
    }
    CHECK(failures.empty());
    CHECK(lims::LedgerModel{}.execute(lims::ListEntries{.company = company}).entries.size() == rows->size());

    lims::ListEntriesPage withNul{.company = company, .query = {}};
    withNul.query.filters.columns["memo"].include.push_back(FilterEntry{.contains = std::string{"a\0b", 3}});
    try {
        static_cast<void>(lims::LedgerModel{}.execute(withNul));
        FAIL("a NUL operand was answered");
    } catch (lims::ValidationError const& error) {
        CHECK(std::string_view{error.what()}.contains("NUL"));
    }
}

// Mutation: skip the visibility check (dave, who sees no company, gets the journal).
TEST_CASE("ledger: a page of entries is the caller's, with an exact total from the database",
          "[lims][ledger][pages]") {
    DbFixture fixture;
    auto const period = books();
    writeJournal(period.id, journalRows());
    lims::LedgerModel ledger;
    {
        ScopedPrincipal const alice{"alice"};
        lims::ListEntriesPage request{.company = lims::test::seededCompany(), .query = {}};
        request.query.page = morph::table::PageRequest{.offset = 20, .limit = 3};
        auto const page = ledger.execute(request);
        CHECK(page.total == static_cast<std::int64_t>(journalRows().size()));
        CHECK(page.totalKind == morph::table::TotalKind::Exact);
        CHECK(page.offset == 20);
        REQUIRE(page.rows.size() == 3U);
        CHECK(page.rows.front().id ==
              lims::LedgerModel{}.execute(lims::ListEntries{.company = lims::test::seededCompany()}).entries[20].id);
        CHECK(page.rows.front().lines.size() == 2U);
        request.query.page.limit = 0;
        auto const counted = ledger.execute(request);
        CHECK(counted.rows.empty());
        CHECK(counted.total == page.total);
    }
    {
        ScopedPrincipal const dave{"dave"};
        auto const page = ledger.execute(lims::ListEntriesPage{.company = lims::test::seededCompany(), .query = {}});
        CHECK(page.rows.empty());
        CHECK(page.total == 0);
    }
    CHECK_THROWS_AS(ledger.execute(lims::ListEntriesPage{}), lims::ValidationError);
}
