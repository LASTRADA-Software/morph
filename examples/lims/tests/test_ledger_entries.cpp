// SPDX-License-Identifier: Apache-2.0
//
// Journal entries: balanced per currency and in company currency, never rounded by the ledger, immutable once posted,
// corrected by reversal.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>
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

/// @brief Sets November 2026's state directly: there is no action that closes a period yet.
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
    CHECK_THROWS_AS(ledger.execute(lims::PostEntry{.entryId = draft.id}), lims::IllegalTransition);

    auto const reversal = ledger.execute(
        lims::ReverseEntry{.entryId = draft.id, .date = morph::time::Timestamp{lims::test::at(2026, 11, 20)}});
    CHECK(reversal.state == lims::EntryState::Posted);
    CHECK(reversal.reverses == draft.id);
    REQUIRE(reversal.lines.size() == 2U);
    CHECK(reversal.lines[0].side == lims::PostingSide::Credit);
    CHECK(ledger.execute(lims::GetEntry{.entryId = draft.id}).reversedBy == reversal.id);
    CHECK_THROWS_AS(ledger.execute(lims::ReverseEntry{.entryId = draft.id,
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
    CHECK_THROWS_AS(ledger.execute(lims::ReverseEntry{.entryId = draft.id,
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
        CHECK_THROWS_AS(
            ledger.execute(lims::CreateDraftEntry{.company = lims::test::seededCompany(),
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
        CHECK_THROWS_AS(ledger.execute(lims::PostEntry{.entryId = draft.id}), lims::Conflict);
        CHECK_THROWS_AS(ledger.execute(lims::ReverseEntry{.entryId = posted.id, .date = date}), lims::Conflict);
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
            CHECK_THROWS_AS(ledger.execute(lims::PostEntry{.entryId = id}), lims::Forbidden);
            CHECK_THROWS_AS(ledger.execute(lims::ReverseEntry{.entryId = id, .date = date}), lims::Forbidden);
        }
        CHECK_THROWS_AS(ledger.execute(lims::CreateDraftEntry{.company = lims::CompanyId{9999},
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
        CHECK_THROWS_AS(ledger.execute(lims::PostEntry{.entryId = draft}), lims::Forbidden);
        CHECK_THROWS_AS(ledger.execute(lims::ReverseEntry{.entryId = posted, .date = date}), lims::Forbidden);
        CHECK_THROWS_AS(ledger.execute(lims::CreateDraftEntry{.company = lims::test::seededCompany(),
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
        CHECK_THROWS_AS(ledger.execute(lims::PostEntry{.entryId = lims::JournalEntryId{9999}}), lims::NotFound);
        CHECK_THROWS_AS(ledger.execute(lims::ReverseEntry{.entryId = lims::JournalEntryId{9999}, .date = date}),
                        lims::NotFound);
        // An entry that exists at another company: a role, but not there.
        CHECK_THROWS_AS(ledger.execute(lims::PostEntry{.entryId = osakaDraft}), lims::Forbidden);
        CHECK_THROWS_AS(ledger.execute(lims::ReverseEntry{.entryId = osakaDraft, .date = date}), lims::Forbidden);
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
        CHECK_THROWS(ledger.execute(lims::ReverseEntry{.entryId = original.id, .date = date}));
        execute("DROP TRIGGER fail_mark");
        untouched();
    }
    SECTION("another reversal marks the original between the read and the mark") {
        execute(
            "CREATE TRIGGER other_reversal AFTER INSERT ON ledger_entries WHEN NEW.reverses_id IS NOT NULL "
            "BEGIN UPDATE ledger_entries SET reversed_by_id = 999 WHERE id = NEW.reverses_id; END");
        CHECK_THROWS_AS(ledger.execute(lims::ReverseEntry{.entryId = original.id, .date = date}), lims::Conflict);
        execute("DROP TRIGGER other_reversal");
        untouched();
    }
    SECTION("a post that matches no draft row is a Conflict and posts nothing") {
        auto const draft = accrual(ledger, date);
        execute(
            "CREATE TRIGGER already_posted BEFORE UPDATE ON ledger_entries "
            "BEGIN SELECT RAISE(IGNORE); END");
        CHECK_THROWS_AS(ledger.execute(lims::PostEntry{.entryId = draft.id}), lims::Conflict);
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
