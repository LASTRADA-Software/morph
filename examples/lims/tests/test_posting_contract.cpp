// SPDX-License-Identifier: Apache-2.0
//
// The posting contract: what a poster hands the ledger, and what it gets back. DTOs only.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <glaze/glaze.hpp>
#include <morph/util/rational.hpp>
#include <string>

#include "lims/org/core/model_support.hpp"
#include "lims/posting/posting.hpp"

namespace {

using morph::math::DecimalPlaces;
using morph::math::Rational;

}  // namespace

// Mutation: drop `side` from PostingLine's reflection by renaming it (the round trip loses the credit side).
TEST_CASE("posting contract: a request round-trips through JSON with its zero-decimal amounts", "[lims][posting]") {
    lims::PostingRequest request{
        .company = lims::CompanyId{2},
        .date = lims::timestampFromMillis(1767225600000),
        .sourceKey = "billing.invoice:7",
        .sourceReference = "invoice 7",
        .currency = lims::CurrencyCode::JPY,
        .rate = lims::Rate{Rational{157, DecimalPlaces{6}}},
        .lines = {lims::PostingLine{.accountCode = "1200",
                                    .side = lims::PostingSide::Debit,
                                    .amount = lims::Amount{Rational{1100, DecimalPlaces{0}}},
                                    .companyAmount = lims::Amount{Rational{7, DecimalPlaces{0}}}},
                  lims::PostingLine{.accountCode = "4000",
                                    .side = lims::PostingSide::Credit,
                                    .amount = lims::Amount{Rational{1200, DecimalPlaces{0}}},
                                    .companyAmount = lims::Amount{Rational{8, DecimalPlaces{0}}}}},
    };
    auto const json = glz::write_json(request);
    REQUIRE(json.has_value());
    // Each amount travels under its own name: a round trip alone would pass with the two names swapped on both sides.
    CHECK(json->find(R"("amount":{"num":1100,)") != std::string::npos);
    CHECK(json->find(R"("companyAmount":{"num":7,)") != std::string::npos);
    lims::PostingRequest decoded;
    REQUIRE_FALSE(glz::read_json(decoded, *json));
    REQUIRE(decoded.lines.size() == 2U);
    CHECK(decoded.company == lims::CompanyId{2});
    CHECK(decoded.date == request.date);
    CHECK(decoded.sourceKey == "billing.invoice:7");
    CHECK(decoded.sourceReference == "invoice 7");
    CHECK(decoded.currency == lims::CurrencyCode::JPY);
    CHECK(*decoded.rate == Rational{157, DecimalPlaces{6}});
    CHECK((*decoded.rate).getDecimalPlaces() == DecimalPlaces{6});
    for (std::size_t i = 0; i < 2U; ++i) {
        CAPTURE(i);
        CHECK(decoded.lines[i].accountCode == request.lines[i].accountCode);
        CHECK(decoded.lines[i].side == request.lines[i].side);
        CHECK(*decoded.lines[i].amount == *request.lines[i].amount);
        CHECK((*decoded.lines[i].amount).getDecimalPlaces() == DecimalPlaces{0});
        CHECK(*decoded.lines[i].companyAmount == *request.lines[i].companyAmount);
        CHECK((*decoded.lines[i].companyAmount).getDecimalPlaces() == DecimalPlaces{0});
    }
}

TEST_CASE("posting contract: a result names the entry", "[lims][posting]") {
    lims::PostingResult const result{.entryId = lims::JournalEntryId{9}, .date = {}};
    auto const json = glz::write_json(result);
    REQUIRE(json.has_value());
    CHECK_THAT(*json, Catch::Matchers::ContainsSubstring(R"("entryId":9)"));
}
