// SPDX-License-Identifier: Apache-2.0
//
// QuoteModel: exact Rational line items, per-line computed totals, and the
// hand-accumulated grand total (README build order §4, "Tryton semantics").

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <morph/util/rational.hpp>
#include <string>

#include "crm/core/errors.hpp"
#include "crm/models/account_model.hpp"
#include "crm/models/opportunity_model.hpp"
#include "crm/models/quote_model.hpp"
#include "crm_test_support.hpp"
#include "testkit/db_fixture.hpp"

using crm::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

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

Rational money(std::int64_t whole) { return Rational{whole, DecimalPlaces{2}}; }

Rational fraction(std::int64_t num, std::int64_t den, std::uint32_t dp) {
    return Rational{Numerator{num}, Denominator{den}, DecimalPlaces{dp}};
}

std::vector<crm::QuoteLine> twoLines() {
    return {
        crm::QuoteLine{.productName = "Widget", .quantity = money(3), .unitPrice = money(10), .discount = money(0)},
        crm::QuoteLine{.productName = "Gadget", .quantity = money(2), .unitPrice = money(25), .discount = money(5)},
    };
}
}  // namespace

TEST_CASE("CreateQuote recomputes each line's total exactly, ignoring a submitted value", "[crm][quote]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::QuoteModel quotes;

    const auto opportunityId = createDeal(accounts, opportunities);
    auto lines = twoLines();
    lines[0].total = money(999);  // a tampered/stale submitted total — must be ignored
    const auto created = quotes.execute(
        crm::CreateQuote{.opportunityId = opportunityId, .lines = lines, .taxRate = fraction(0, 100, 2)});

    REQUIRE(created.quote.lines.size() == 2);
    // Widget: 3 * 10 - 0 = 30
    CHECK(created.quote.lines[0].total == money(30));
    // Gadget: 2 * 25 - 5 = 45
    CHECK(created.quote.lines[1].total == money(45));
}

TEST_CASE("CreateQuote's grandTotal is the sum of every line's total, plus tax", "[crm][quote]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::QuoteModel quotes;

    const auto opportunityId = createDeal(accounts, opportunities);
    // subtotal = 30 + 45 = 75; 10% tax = 7.50; grand total = 82.50
    const auto created = quotes.execute(
        crm::CreateQuote{.opportunityId = opportunityId, .lines = twoLines(), .taxRate = fraction(10, 100, 2)});

    CHECK(created.quote.grandTotal == fraction(8250, 100, 2));
    CHECK(created.quote.status == crm::QuoteStatus::Draft);
}

TEST_CASE("CreateQuote with a zero-tax rate leaves the grand total equal to the subtotal", "[crm][quote]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::QuoteModel quotes;

    const auto opportunityId = createDeal(accounts, opportunities);
    const auto created = quotes.execute(
        crm::CreateQuote{.opportunityId = opportunityId, .lines = twoLines(), .taxRate = fraction(0, 100, 2)});
    CHECK(created.quote.grandTotal == money(75));
}

TEST_CASE("CreateQuote with an empty line list is rejected", "[crm][quote]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::QuoteModel quotes;

    const auto opportunityId = createDeal(accounts, opportunities);
    CHECK_THROWS_AS(
        quotes.execute(crm::CreateQuote{.opportunityId = opportunityId, .lines = {}, .taxRate = fraction(0, 100, 2)}),
        crm::ValidationError);
}

TEST_CASE("CreateQuote with a line missing a product name is rejected", "[crm][quote]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::QuoteModel quotes;

    const auto opportunityId = createDeal(accounts, opportunities);
    auto lines = twoLines();
    lines[0].productName.clear();
    CHECK_THROWS_AS(quotes.execute(crm::CreateQuote{
                        .opportunityId = opportunityId, .lines = lines, .taxRate = fraction(0, 100, 2)}),
                    crm::ValidationError);
}

TEST_CASE("CreateQuote with a non-positive quantity is rejected", "[crm][quote]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::QuoteModel quotes;

    const auto opportunityId = createDeal(accounts, opportunities);
    auto lines = twoLines();
    lines[0].quantity = money(0);
    CHECK_THROWS_AS(quotes.execute(crm::CreateQuote{
                        .opportunityId = opportunityId, .lines = lines, .taxRate = fraction(0, 100, 2)}),
                    crm::ValidationError);
}

TEST_CASE("CreateQuote naming a nonexistent opportunity is NotFound", "[crm][quote]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::QuoteModel quotes;

    CHECK_THROWS_AS(
        quotes.execute(crm::CreateQuote{
            .opportunityId = crm::OpportunityId{999}, .lines = twoLines(), .taxRate = fraction(0, 100, 2)}),
        crm::NotFound);
}

TEST_CASE("Creating a quote with no principal is refused", "[crm][quote][audit]") {
    DbFixture fixture;
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::QuoteModel quotes;
    crm::OpportunityId opportunityId;
    {
        const ScopedPrincipal alice{"alice"};
        opportunityId = createDeal(accounts, opportunities);
    }
    CHECK_THROWS_AS(quotes.execute(crm::CreateQuote{
                        .opportunityId = opportunityId, .lines = twoLines(), .taxRate = fraction(0, 100, 2)}),
                    crm::EmptyPrincipalError);
}

TEST_CASE("UpdateQuote replaces lines and recomputes the grand total", "[crm][quote]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::QuoteModel quotes;

    const auto opportunityId = createDeal(accounts, opportunities);
    const auto created = quotes.execute(
        crm::CreateQuote{.opportunityId = opportunityId, .lines = twoLines(), .taxRate = fraction(0, 100, 2)});

    std::vector<crm::QuoteLine> singleLine{
        crm::QuoteLine{.productName = "Widget", .quantity = money(1), .unitPrice = money(100), .discount = money(0)}};
    const auto updated = quotes.execute(crm::UpdateQuote{
        .quoteId = created.quote.id, .lines = singleLine, .taxRate = fraction(0, 100, 2), .expectedVersion = 1});
    REQUIRE(updated.quote.lines.size() == 1);
    CHECK(updated.quote.grandTotal == money(100));
    CHECK(updated.quote.version == 2);
}

TEST_CASE("UpdateQuote with a stale version is a Conflict", "[crm][quote]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::QuoteModel quotes;

    const auto opportunityId = createDeal(accounts, opportunities);
    const auto created = quotes.execute(
        crm::CreateQuote{.opportunityId = opportunityId, .lines = twoLines(), .taxRate = fraction(0, 100, 2)});

    CHECK_THROWS_AS(
        quotes.execute(crm::UpdateQuote{
            .quoteId = created.quote.id, .lines = twoLines(), .taxRate = fraction(0, 100, 2), .expectedVersion = 0}),
        crm::Conflict);
}

TEST_CASE("Editing a non-Draft quote's lines throws IllegalTransition", "[crm][quote]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::QuoteModel quotes;

    const auto opportunityId = createDeal(accounts, opportunities);
    const auto created = quotes.execute(
        crm::CreateQuote{.opportunityId = opportunityId, .lines = twoLines(), .taxRate = fraction(0, 100, 2)});
    quotes.execute(crm::SendQuote{.quoteId = created.quote.id});

    CHECK_THROWS_AS(
        quotes.execute(crm::UpdateQuote{
            .quoteId = created.quote.id, .lines = twoLines(), .taxRate = fraction(0, 100, 2), .expectedVersion = 2}),
        crm::IllegalTransition);
}

TEST_CASE("SendQuote transitions Draft to Sent", "[crm][quote]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::QuoteModel quotes;

    const auto opportunityId = createDeal(accounts, opportunities);
    const auto created = quotes.execute(
        crm::CreateQuote{.opportunityId = opportunityId, .lines = twoLines(), .taxRate = fraction(0, 100, 2)});
    const auto sent = quotes.execute(crm::SendQuote{.quoteId = created.quote.id});
    CHECK(sent.status == crm::QuoteStatus::Sent);
}

TEST_CASE("Sending an already-Sent quote throws IllegalTransition", "[crm][quote]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::QuoteModel quotes;

    const auto opportunityId = createDeal(accounts, opportunities);
    const auto created = quotes.execute(
        crm::CreateQuote{.opportunityId = opportunityId, .lines = twoLines(), .taxRate = fraction(0, 100, 2)});
    quotes.execute(crm::SendQuote{.quoteId = created.quote.id});
    CHECK_THROWS_AS(quotes.execute(crm::SendQuote{.quoteId = created.quote.id}), crm::IllegalTransition);
}

TEST_CASE("DecideQuote accepts or rejects a Sent quote", "[crm][quote]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::QuoteModel quotes;

    const auto opportunityId = createDeal(accounts, opportunities);
    const auto created = quotes.execute(
        crm::CreateQuote{.opportunityId = opportunityId, .lines = twoLines(), .taxRate = fraction(0, 100, 2)});
    quotes.execute(crm::SendQuote{.quoteId = created.quote.id});

    const auto accepted = quotes.execute(crm::DecideQuote{.quoteId = created.quote.id, .accepted = true});
    CHECK(accepted.status == crm::QuoteStatus::Accepted);
}

TEST_CASE("DecideQuote on a Draft quote throws IllegalTransition", "[crm][quote]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::QuoteModel quotes;

    const auto opportunityId = createDeal(accounts, opportunities);
    const auto created = quotes.execute(
        crm::CreateQuote{.opportunityId = opportunityId, .lines = twoLines(), .taxRate = fraction(0, 100, 2)});
    CHECK_THROWS_AS(quotes.execute(crm::DecideQuote{.quoteId = created.quote.id, .accepted = true}),
                    crm::IllegalTransition);
}

TEST_CASE("ListQuotes filters by opportunity when engaged", "[crm][quote]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::QuoteModel quotes;

    const auto dealA = createDeal(accounts, opportunities);
    const auto dealB = createDeal(accounts, opportunities);
    quotes.execute(crm::CreateQuote{.opportunityId = dealA, .lines = twoLines(), .taxRate = fraction(0, 100, 2)});
    quotes.execute(crm::CreateQuote{.opportunityId = dealB, .lines = twoLines(), .taxRate = fraction(0, 100, 2)});

    const auto forA = quotes.execute(crm::ListQuotes{.opportunityId = dealA});
    REQUIRE(forA.quotes.size() == 1);

    const auto all = quotes.execute(crm::ListQuotes{});
    CHECK(all.quotes.size() == 2);
}

TEST_CASE("QuoteModel journals its edits against the attached identity", "[crm][quote][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::QuoteModel quotes;
    quotes.attachActionLog(log, std::string{"quotes"});

    const auto opportunityId = createDeal(accounts, opportunities);
    const auto created = quotes.execute(
        crm::CreateQuote{.opportunityId = opportunityId, .lines = twoLines(), .taxRate = fraction(0, 100, 2)});
    quotes.execute(crm::SendQuote{.quoteId = created.quote.id});

    const auto entries = log->entries("quotes");
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].actionType == "CreateQuote");
    CHECK(entries[1].actionType == "SendQuote");
    for (const auto& entry : entries) {
        CHECK(entry.modelType == "QuoteModel");
        CHECK(entry.principal == "alice");
        CHECK(entry.outcome == morph::journal::Outcome::Succeeded);
    }
}

// ── GetQuote: registered on the wire, driven by nothing (morph#412) ──────
//
// `BRIDGE_REGISTER_ACTION(crm::QuoteModel, crm::GetQuote, "GetQuote", ...)`
// puts this action on the rung's wire surface, and the per-miss audit of
// crm's uncovered lines (README, "What the uncovered lines in crm's models
// are") found
// its whole body — every line of it — executed by no test, no presenter and
// no scenario. A read action that is never read back is where a wrong answer
// hides longest: `ListQuotes` exercises the same `toView`/`fetchLines` pair
// in aggregate, so a `GetQuote` that fetched the wrong row, or that answered
// an unknown id with a default-constructed `QuoteView` instead of refusing,
// would look identical to a working one from anywhere else in the suite.

TEST_CASE("GetQuote answers with that quote, its lines and its computed total", "[crm][quote]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::QuoteModel quotes;

    const auto dealA = createDeal(accounts, opportunities);
    const auto dealB = createDeal(accounts, opportunities);
    const auto wanted =
        quotes.execute(crm::CreateQuote{.opportunityId = dealA, .lines = twoLines(), .taxRate = fraction(0, 100, 2)});
    // A second quote exists, on a different opportunity, so "returns a quote"
    // and "returns *this* quote" are distinguishable.
    quotes.execute(crm::CreateQuote{.opportunityId = dealB, .lines = twoLines(), .taxRate = fraction(0, 100, 2)});

    const auto fetched = quotes.execute(crm::GetQuote{.quoteId = wanted.quote.id});
    CHECK(fetched.id == wanted.quote.id);
    CHECK(fetched.opportunityId == dealA);
    CHECK(fetched.status == crm::QuoteStatus::Draft);
    CHECK(fetched.lines.size() == wanted.quote.lines.size());
    CHECK(fetched.grandTotal == wanted.quote.grandTotal);
    CHECK(fetched.version == wanted.quote.version);
}

TEST_CASE("GetQuote refuses an id that names no quote rather than answering an empty one", "[crm][quote]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel accounts;
    crm::OpportunityModel opportunities;
    crm::QuoteModel quotes;

    const auto opportunityId = createDeal(accounts, opportunities);
    const auto created = quotes.execute(
        crm::CreateQuote{.opportunityId = opportunityId, .lines = twoLines(), .taxRate = fraction(0, 100, 2)});

    CHECK_THROWS_AS(quotes.execute(crm::GetQuote{.quoteId = crm::QuoteId{*created.quote.id + 1}}), crm::NotFound);
    // An unset id is a client error, not a missing row: the two are different
    // refusals and a caller that cannot tell them apart cannot retry correctly.
    CHECK_THROWS_AS(quotes.execute(crm::GetQuote{}), crm::ValidationError);
}
