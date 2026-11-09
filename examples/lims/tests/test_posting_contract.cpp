// SPDX-License-Identifier: Apache-2.0
//
// The posting contract: what a poster hands the ledger, and what it gets back. DTOs only.

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/util/rational.hpp>
#include <string>

#include "lims/posting/posting.hpp"

namespace {

using morph::math::DecimalPlaces;
using morph::math::Rational;

}  // namespace

// Mutation: drop `side` from PostingLine's reflection by renaming it (the round trip loses the credit side).
TEST_CASE("posting contract: a request round-trips through JSON with its zero-decimal amounts", "[lims][posting]") {
    lims::PostingRequest request{
        .company = lims::CompanyId{2},
        .date = {},
        .sourceKey = "billing.invoice:7",
        .sourceReference = "invoice 7",
        .currency = lims::CurrencyCode::JPY,
        .rate = lims::Rate{Rational{1, DecimalPlaces{6}}},
        .lines = {lims::PostingLine{.accountCode = "1200",
                                    .side = lims::PostingSide::Debit,
                                    .amount = lims::Amount{Rational{1100, DecimalPlaces{0}}},
                                    .companyAmount = lims::Amount{Rational{1100, DecimalPlaces{0}}}},
                  lims::PostingLine{.accountCode = "4000",
                                    .side = lims::PostingSide::Credit,
                                    .amount = lims::Amount{Rational{1100, DecimalPlaces{0}}},
                                    .companyAmount = lims::Amount{Rational{1100, DecimalPlaces{0}}}}},
    };
    auto const json = glz::write_json(request);
    REQUIRE(json.has_value());
    lims::PostingRequest decoded;
    REQUIRE_FALSE(glz::read_json(decoded, *json));
    REQUIRE(decoded.lines.size() == 2U);
    CHECK(decoded.company == lims::CompanyId{2});
    CHECK(decoded.sourceKey == "billing.invoice:7");
    CHECK(decoded.currency == lims::CurrencyCode::JPY);
    CHECK(decoded.lines[1].side == lims::PostingSide::Credit);
    CHECK(decoded.lines[1].accountCode == "4000");
    CHECK(*decoded.lines[0].amount == Rational{1100, DecimalPlaces{0}});
}

TEST_CASE("posting contract: a result names the entry", "[lims][posting]") {
    lims::PostingResult const result{.entryId = lims::JournalEntryId{9}};
    auto const json = glz::write_json(result);
    REQUIRE(json.has_value());
    CHECK(*json == R"({"entryId":9})");
}
