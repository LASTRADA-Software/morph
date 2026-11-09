// SPDX-License-Identifier: Apache-2.0
//
// Billing settings, billing clients and price lists with effective dates and per-client prices.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>
#include <thread>

#include "lims/billing/db/billing_entity.hpp"
#include "lims/billing/models/price_list_model.hpp"
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

constexpr std::string_view kSecret = "lims-price-lists-test-secret-at-least-32b";

[[nodiscard]] lims::Amount euros(std::int64_t units) { return lims::Amount{Rational{units, DecimalPlaces{2}}}; }

[[nodiscard]] morph::time::Timestamp day(unsigned month, unsigned dayOfMonth) {
    return morph::time::Timestamp{lims::test::at(2026, month, dayOfMonth, 0)};
}

[[nodiscard]] lims::Rate taxRate(std::int64_t numerator, std::int64_t denominator) {
    return lims::Rate{Rational{Numerator{numerator}, Denominator{denominator}, DecimalPlaces{6}}};
}

[[nodiscard]] lims::Count count(std::int64_t value) { return lims::Count{Rational{value, DecimalPlaces{0}}}; }

[[nodiscard]] lims::SetBillingSettings settingsFor(lims::CompanyId company) {
    return lims::SetBillingSettings{.company = company,
                                    .receivableAccount = "1200",
                                    .revenueAccount = "4000",
                                    .taxAccount = "2200",
                                    .roundingAccount = "6900",
                                    .taxRate = taxRate(19, 100),
                                    .approvalHours = count(48),
                                    .billingDay = count(25)};
}

[[nodiscard]] lims::CreatePriceList listFor(lims::CompanyId company, unsigned month = 10) {
    return lims::CreatePriceList{
        .company = company, .name = "List", .currency = lims::CurrencyCode::EUR, .validFrom = day(month, 1)};
}

[[nodiscard]] lims::SetPrice priceIn(lims::PriceListId list, std::int64_t analysis, std::int64_t cents,
                                     lims::ClientId client = {}) {
    return lims::SetPrice{
        .priceList = list, .analysis = lims::AnalysisId{analysis}, .client = client, .amount = euros(cents)};
}

/// @brief A second company with a billing clerk, `dave`, and a supervisor, `sven`, who hold nothing at the seeded one.
/// @return Its id.
lims::CompanyId secondCompany() {
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    auto const osaka = org.execute(lims::CreateCompany{.name = "Osaka",
                                                       .baseCurrency = lims::CurrencyCode::JPY,
                                                       .fiscalYearStart = lims::Month::April})
                           .id;
    org.execute(
        lims::AssignRole{.principal = "dave", .role = lims::LimsRole::BillingClerk, .company = osaka, .site = {}});
    org.execute(
        lims::AssignRole{.principal = "sven", .role = lims::LimsRole::Supervisor, .company = osaka, .site = {}});
    return osaka;
}

}  // namespace

// Mutation: fall back to an older list when the list in force has no price (the nitrate price of the October list
// answers in December).
TEST_CASE("price lists: the list in force on a date prices an analysis, a client's own price first",
          "[lims][billing][prices]") {
    DbFixture fixture;
    lims::test::grant("bea", lims::LimsRole::BillingClerk);
    ScopedPrincipal const bea{"bea"};
    lims::PriceListModel prices;
    lims::ClientId const waterworks{1};
    lims::ClientId const brewery{2};
    lims::AnalysisId const nitrate{1};
    lims::AnalysisId const turbidity{2};
    prices.execute(lims::RegisterBillingClient{.client = waterworks,
                                               .company = lims::test::seededCompany(),
                                               .name = "Waterworks",
                                               .currency = lims::CurrencyCode::EUR});
    prices.execute(lims::RegisterBillingClient{.client = brewery,
                                               .company = lims::test::seededCompany(),
                                               .name = "Brewery",
                                               .currency = lims::CurrencyCode::EUR});
    auto const october = prices.execute(lims::CreatePriceList{.company = lims::test::seededCompany(),
                                                              .name = "2026 Q4",
                                                              .currency = lims::CurrencyCode::EUR,
                                                              .validFrom = day(10, 1)});
    prices.execute(lims::SetPrice{.priceList = october.id, .analysis = nitrate, .client = {}, .amount = euros(40)});
    prices.execute(
        lims::SetPrice{.priceList = october.id, .analysis = nitrate, .client = brewery, .amount = euros(35)});
    auto const december = prices.execute(lims::CreatePriceList{.company = lims::test::seededCompany(),
                                                               .name = "December",
                                                               .currency = lims::CurrencyCode::EUR,
                                                               .validFrom = day(12, 1)});
    prices.execute(lims::SetPrice{.priceList = december.id, .analysis = turbidity, .client = {}, .amount = euros(20)});

    auto const quote = [](lims::ClientId client, lims::AnalysisId analysis, morph::time::Timestamp on) {
        return lims::PriceListModel::priceOn(lims::test::seededCompany(), client, analysis, on);
    };
    REQUIRE(quote(waterworks, nitrate, day(11, 10)).has_value());
    CHECK(*quote(waterworks, nitrate, day(11, 10))->amount == Rational{40, DecimalPlaces{2}});
    CHECK(quote(waterworks, nitrate, day(11, 10))->priceList == october.id);
    CHECK(*quote(brewery, nitrate, day(11, 10))->amount == Rational{35, DecimalPlaces{2}});
    CHECK_FALSE(quote(waterworks, nitrate, day(12, 2)).has_value());
    REQUIRE(quote(waterworks, turbidity, day(12, 2)).has_value());
    CHECK(*quote(waterworks, turbidity, day(12, 2))->amount == Rational{20, DecimalPlaces{2}});
    CHECK(quote(waterworks, turbidity, day(12, 2))->priceList == december.id);
    CHECK_FALSE(quote(waterworks, nitrate, day(9, 30)).has_value());
    CHECK_FALSE(quote(lims::ClientId{3}, nitrate, day(11, 10)).has_value());
    CHECK_FALSE(quote(waterworks, lims::AnalysisId{}, day(11, 10)).has_value());
    CHECK_FALSE(quote(waterworks, nitrate, morph::time::Timestamp{}).has_value());
    // The list starts at its first instant.
    CHECK(quote(waterworks, nitrate, day(10, 1)).has_value());

    // The same question as an action.
    auto const asked = prices.execute(lims::QuotePrice{
        .company = lims::test::seededCompany(), .client = brewery, .analysis = nitrate, .on = day(11, 10)});
    CHECK(*asked.amount == Rational{35, DecimalPlaces{2}});
    CHECK(asked.currency == lims::CurrencyCode::EUR);
    CHECK_THROWS_AS(
        prices.execute(lims::QuotePrice{
            .company = lims::test::seededCompany(), .client = waterworks, .analysis = nitrate, .on = day(12, 2)}),
        lims::NotFound);
    CHECK_THROWS_AS(prices.execute(lims::QuotePrice{
                        .company = lims::test::seededCompany(), .client = waterworks, .analysis = nitrate, .on = {}}),
                    lims::ValidationError);
}

// Mutation: price a client from the list in force of any currency (a USD client is quoted a EUR list's price).
TEST_CASE("price lists: a client is priced from the lists of its own currency and company",
          "[lims][billing][prices]") {
    DbFixture fixture;
    auto const osaka = secondCompany();
    lims::test::grant("bea", lims::LimsRole::BillingClerk);
    ScopedPrincipal const bea{"bea"};
    lims::PriceListModel prices;
    auto const eur = prices.execute(listFor(lims::test::seededCompany()));
    prices.execute(priceIn(eur.id, 1, 40));
    prices.execute(lims::RegisterBillingClient{.client = lims::ClientId{1},
                                               .company = lims::test::seededCompany(),
                                               .name = "US client",
                                               .currency = lims::CurrencyCode::USD});
    prices.execute(lims::RegisterBillingClient{.client = lims::ClientId{2},
                                               .company = lims::test::seededCompany(),
                                               .name = "EU client",
                                               .currency = lims::CurrencyCode::EUR});
    auto const quote = [](lims::CompanyId company, lims::ClientId client) {
        return lims::PriceListModel::priceOn(company, client, lims::AnalysisId{1}, day(11, 1));
    };
    CHECK_FALSE(quote(lims::test::seededCompany(), lims::ClientId{1}).has_value());
    CHECK(quote(lims::test::seededCompany(), lims::ClientId{2}).has_value());
    // The EU client is registered at the seeded company only, and the list is that company's.
    CHECK_FALSE(quote(osaka, lims::ClientId{2}).has_value());
    CHECK(lims::PriceListModel::billingClient(lims::ClientId{2}, lims::test::seededCompany()).has_value());
    CHECK_FALSE(lims::PriceListModel::billingClient(lims::ClientId{2}, osaka).has_value());
    CHECK_FALSE(lims::PriceListModel::billingClient(lims::ClientId{}, osaka).has_value());
}

// Mutation: skip the fitsCurrency check in SetPrice; delete both the lookup and the unique index of billing_clients
// (a second registration succeeds).
TEST_CASE("price lists: a JPY list holds whole yen, and a client is registered once per company",
          "[lims][billing][prices]") {
    DbFixture fixture;
    lims::test::grant("bea", lims::LimsRole::BillingClerk);
    ScopedPrincipal const bea{"bea"};
    lims::PriceListModel prices;
    auto const yen = prices.execute(lims::CreatePriceList{.company = lims::test::seededCompany(),
                                                          .name = "Yen",
                                                          .currency = lims::CurrencyCode::JPY,
                                                          .validFrom = day(1, 1)});
    CHECK_THROWS_AS(prices.execute(lims::SetPrice{
                        .priceList = yen.id,
                        .analysis = lims::AnalysisId{1},
                        .client = {},
                        .amount = lims::Amount{Rational{Numerator{25}, Denominator{2}, DecimalPlaces{1}}}}),
                    lims::ValidationError);
    CHECK(prices
              .execute(lims::SetPrice{.priceList = yen.id,
                                      .analysis = lims::AnalysisId{1},
                                      .client = {},
                                      .amount = lims::Amount{Rational{4500, DecimalPlaces{0}}}})
              .id.hasValue());
    // A EUR price of a fraction of a cent is refused too.
    auto const eur = prices.execute(listFor(lims::test::seededCompany()));
    CHECK_THROWS_AS(prices.execute(lims::SetPrice{
                        .priceList = eur.id,
                        .analysis = lims::AnalysisId{1},
                        .client = {},
                        .amount = lims::Amount{Rational{Numerator{1}, Denominator{3}, DecimalPlaces{2}}}}),
                    lims::ValidationError);
    prices.execute(lims::RegisterBillingClient{.client = lims::ClientId{1},
                                               .company = lims::test::seededCompany(),
                                               .name = "W",
                                               .currency = lims::CurrencyCode::JPY});
    CHECK_THROWS_AS(prices.execute(lims::RegisterBillingClient{.client = lims::ClientId{1},
                                                               .company = lims::test::seededCompany(),
                                                               .name = "W",
                                                               .currency = lims::CurrencyCode::EUR}),
                    lims::Conflict);
    auto const registered = lims::PriceListModel::billingClient(lims::ClientId{1}, lims::test::seededCompany());
    REQUIRE(registered.has_value());
    CHECK(registered->currency == lims::CurrencyCode::JPY);
    CHECK(registered->name == "W");
}

// Mutations: accept a billing day of 29; accept a tax rate above 1; accept a negative tax rate; accept a tax rate
// finer than six decimals; accept approval hours of 0 or 721; accept a fractional count; delete the whole-number
// check on the billing day.
TEST_CASE("price lists: a supervisor sets a company's billing settings, within their ranges",
          "[lims][billing][prices]") {
    DbFixture fixture;
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    ScopedPrincipal const sam{"sam"};
    lims::PriceListModel prices;
    CHECK_FALSE(lims::PriceListModel::settingsOf(lims::test::seededCompany()).has_value());
    CHECK_THROWS_AS(prices.execute(lims::GetBillingSettings{.company = lims::test::seededCompany()}), lims::NotFound);

    lims::SetBillingSettings settings = settingsFor(lims::test::seededCompany());
    auto const saved = prices.execute(settings);
    CHECK(saved.revenueAccount == "4000");
    auto const stored = lims::PriceListModel::settingsOf(lims::test::seededCompany());
    REQUIRE(stored.has_value());
    CHECK(stored->company == lims::test::seededCompany());
    CHECK(stored->receivableAccount == "1200");
    CHECK(stored->revenueAccount == "4000");
    CHECK(stored->taxAccount == "2200");
    CHECK(stored->roundingAccount == "6900");
    CHECK(*stored->taxRate == Rational{Numerator{19}, Denominator{100}, DecimalPlaces{6}});
    CHECK(*stored->approvalHours == Rational{48, DecimalPlaces{0}});
    CHECK(*stored->billingDay == Rational{25, DecimalPlaces{0}});

    auto const refused = [&](auto edit) {
        lims::SetBillingSettings bad = settingsFor(lims::test::seededCompany());
        edit(bad);
        CHECK_THROWS_AS(prices.execute(bad), lims::ValidationError);
    };
    refused([](auto& s) { s.billingDay = count(29); });
    refused([](auto& s) { s.billingDay = count(0); });
    refused([](auto& s) { s.billingDay = lims::Count{Rational{Numerator{5}, Denominator{2}, DecimalPlaces{1}}}; });
    refused([](auto& s) { s.taxRate = lims::Rate{Rational{2, DecimalPlaces{6}}}; });
    refused([](auto& s) { s.taxRate = taxRate(1000001, 1000000); });
    refused([](auto& s) { s.taxRate = taxRate(-1, 100); });
    refused([](auto& s) { s.taxRate = taxRate(1, 3); });
    refused([](auto& s) { s.approvalHours = count(0); });
    refused([](auto& s) { s.approvalHours = count(721); });
    refused([](auto& s) { s.receivableAccount = ""; });
    refused([](auto& s) { s.revenueAccount = "4000 "; });
    refused([](auto& s) { s.taxAccount = "tax"; });
    refused([](auto& s) { s.roundingAccount = "12345678901234567"; });
    refused([](auto& s) { s.taxRate = lims::Rate{}; });
    refused([](auto& s) { s.approvalHours = lims::Count{}; });
    refused([](auto& s) { s.billingDay = lims::Count{}; });
    refused([](auto& s) { s.company = lims::CompanyId{}; });
    // The edges are accepted.
    settings.billingDay = count(28);
    settings.approvalHours = count(720);
    settings.taxRate = taxRate(1, 1);
    CHECK_NOTHROW(prices.execute(settings));
    settings.billingDay = count(1);
    settings.approvalHours = count(1);
    settings.taxRate = taxRate(0, 1);
    CHECK_NOTHROW(prices.execute(settings));
    // Settings replace the earlier ones: one row per company.
    settings.revenueAccount = "4100";
    prices.execute(settings);
    CHECK(prices.execute(lims::GetBillingSettings{.company = lims::test::seededCompany()}).revenueAccount == "4100");
    Lightweight::DataMapper mapper;
    CHECK(mapper.Query<lims::db::BillingSettingsRecord>().All().size() == 1U);
}

// Mutations: delete validate() from each action; delete requireRoleSomewhere or requireAuthorized from each; make the
// company lookup answer for any id; scope SetPrice by any role instead of the list's company; swap the role of an
// action (a clerk sets settings, a supervisor registers clients).
TEST_CASE("price lists: each write is a role's, at the company, and a missing thing is a NotFound after the role",
          "[lims][billing][authorization]") {
    DbFixture fixture;
    auto const osaka = secondCompany();
    lims::test::grant("bea", lims::LimsRole::BillingClerk);
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    lims::PriceListModel prices;
    lims::PriceListId list;
    {
        ScopedPrincipal const bea{"bea"};
        list = prices.execute(listFor(lims::test::seededCompany())).id;
    }
    {
        ScopedPrincipal const nobody{"nobody"};
        CHECK_THROWS_AS(prices.execute(settingsFor(lims::test::seededCompany())), lims::Forbidden);
        CHECK_THROWS_AS(prices.execute(settingsFor(lims::CompanyId{9999})), lims::Forbidden);
        CHECK_THROWS_AS(prices.execute(lims::RegisterBillingClient{
                            .client = lims::ClientId{1}, .company = lims::test::seededCompany(), .name = "C"}),
                        lims::Forbidden);
        CHECK_THROWS_AS(prices.execute(listFor(lims::test::seededCompany())), lims::Forbidden);
        CHECK_THROWS_AS(prices.execute(priceIn(list, 1, 10)), lims::Forbidden);
        CHECK_THROWS_AS(prices.execute(priceIn(lims::PriceListId{9999}, 1, 10)), lims::Forbidden);
    }
    {
        // A role of the wrong kind is no role: a clerk does not set settings, a supervisor does not price.
        ScopedPrincipal const bea{"bea"};
        CHECK_THROWS_AS(prices.execute(settingsFor(lims::test::seededCompany())), lims::Forbidden);
        ScopedPrincipal const sam{"sam"};
        CHECK_THROWS_AS(prices.execute(lims::RegisterBillingClient{
                            .client = lims::ClientId{1}, .company = lims::test::seededCompany(), .name = "C"}),
                        lims::Forbidden);
        CHECK_THROWS_AS(prices.execute(listFor(lims::test::seededCompany())), lims::Forbidden);
        CHECK_THROWS_AS(prices.execute(priceIn(list, 1, 10)), lims::Forbidden);
    }
    {
        // A role at another company is a role somewhere, so what does not exist is a NotFound; for the company it
        // does exist at, it is Forbidden.
        ScopedPrincipal const dave{"dave"};
        CHECK_THROWS_AS(prices.execute(listFor(lims::CompanyId{9999})), lims::NotFound);
        CHECK_THROWS_AS(prices.execute(lims::RegisterBillingClient{
                            .client = lims::ClientId{1}, .company = lims::CompanyId{9999}, .name = "C"}),
                        lims::NotFound);
        CHECK_THROWS_AS(prices.execute(priceIn(lims::PriceListId{9999}, 1, 10)), lims::NotFound);
        CHECK_THROWS_AS(prices.execute(listFor(lims::test::seededCompany())), lims::Forbidden);
        CHECK_THROWS_AS(prices.execute(lims::RegisterBillingClient{
                            .client = lims::ClientId{1}, .company = lims::test::seededCompany(), .name = "C"}),
                        lims::Forbidden);
        CHECK_THROWS_AS(prices.execute(priceIn(list, 1, 10)), lims::Forbidden);
        CHECK_NOTHROW(prices.execute(listFor(osaka)));
    }
    {
        ScopedPrincipal const sven{"sven"};
        CHECK_THROWS_AS(prices.execute(settingsFor(lims::test::seededCompany())), lims::Forbidden);
        CHECK_THROWS_AS(prices.execute(settingsFor(lims::CompanyId{9999})), lims::NotFound);
        CHECK_NOTHROW(prices.execute(settingsFor(osaka)));
    }
    ScopedPrincipal const sam{"sam"};
    CHECK_NOTHROW(prices.execute(settingsFor(lims::test::seededCompany())));
    ScopedPrincipal const bea{"bea"};
    SECTION("a malformed request is a ValidationError, before anything is looked up") {
        auto client = lims::RegisterBillingClient{
            .client = lims::ClientId{1}, .company = lims::test::seededCompany(), .name = "C"};
        client.client = lims::ClientId{};
        CHECK_THROWS_AS(prices.execute(client), lims::ValidationError);
        client.client = lims::ClientId{1};
        client.company = lims::CompanyId{};
        CHECK_THROWS_AS(prices.execute(client), lims::ValidationError);
        client.company = lims::test::seededCompany();
        client.name = "";
        CHECK_THROWS_AS(prices.execute(client), lims::ValidationError);
        client.name = std::string(129, 'x');
        CHECK_THROWS_AS(prices.execute(client), lims::ValidationError);
        client.name = std::string(128, 'x');
        CHECK_NOTHROW(prices.execute(client));

        auto bad = listFor(lims::test::seededCompany());
        bad.company = lims::CompanyId{};
        CHECK_THROWS_AS(prices.execute(bad), lims::ValidationError);
        bad = listFor(lims::test::seededCompany());
        bad.validFrom = morph::time::Timestamp{};
        CHECK_THROWS_AS(prices.execute(bad), lims::ValidationError);
        bad = listFor(lims::test::seededCompany());
        bad.name = "";
        CHECK_THROWS_AS(prices.execute(bad), lims::ValidationError);
        bad.name = std::string(129, 'x');
        CHECK_THROWS_AS(prices.execute(bad), lims::ValidationError);

        CHECK_THROWS_AS(prices.execute(priceIn(lims::PriceListId{}, 1, 10)), lims::ValidationError);
        CHECK_THROWS_AS(
            prices.execute(lims::SetPrice{.priceList = list, .analysis = {}, .client = {}, .amount = euros(1)}),
            lims::ValidationError);
        CHECK_THROWS_AS(prices.execute(lims::SetPrice{
                            .priceList = list, .analysis = lims::AnalysisId{1}, .client = {}, .amount = {}}),
                        lims::ValidationError);
        CHECK_THROWS_AS(prices.execute(priceIn(list, 1, -1)), lims::ValidationError);
        CHECK_THROWS_AS(prices.execute(lims::ListPriceLists{}), lims::ValidationError);
        CHECK_THROWS_AS(prices.execute(lims::GetBillingSettings{}), lims::ValidationError);
        CHECK_THROWS_AS(prices.execute(lims::QuotePrice{}), lims::ValidationError);
        CHECK(prices.execute(lims::ListPriceLists{.company = lims::test::seededCompany()}).priceLists.size() == 1U);
    }
    SECTION("a price is set once per list, analysis and client, and may be zero") {
        auto const first = prices.execute(priceIn(list, 1, 4000));
        CHECK(first.id.hasValue());
        CHECK_FALSE(first.client.hasValue());
        CHECK(first.analysis == lims::AnalysisId{1});
        CHECK(*first.amount == Rational{4000, DecimalPlaces{2}});
        CHECK_THROWS_AS(prices.execute(priceIn(list, 1, 4100)), lims::Conflict);
        // Another client, another analysis and a free price are other rows.
        CHECK_NOTHROW(prices.execute(priceIn(list, 1, 3500, lims::ClientId{2})));
        CHECK_THROWS_AS(prices.execute(priceIn(list, 1, 3600, lims::ClientId{2})), lims::Conflict);
        CHECK_NOTHROW(prices.execute(priceIn(list, 2, 0)));
        auto const shown = prices.execute(lims::ListPriceLists{.company = lims::test::seededCompany()});
        REQUIRE(shown.priceLists.size() == 1U);
        REQUIRE(shown.priceLists.front().prices.size() == 3U);
        CHECK(shown.priceLists.front().prices[0].analysis == lims::AnalysisId{1});
        CHECK_FALSE(shown.priceLists.front().prices[0].client.hasValue());
        CHECK(shown.priceLists.front().prices[1].client == lims::ClientId{2});
        CHECK(shown.priceLists.front().prices[2].analysis == lims::AnalysisId{2});
    }
    SECTION("a company has one list per currency and start") {
        CHECK_THROWS_AS(prices.execute(listFor(lims::test::seededCompany())), lims::Conflict);
        auto other = listFor(lims::test::seededCompany());
        other.currency = lims::CurrencyCode::USD;
        CHECK_NOTHROW(prices.execute(other));
        CHECK_NOTHROW(prices.execute(listFor(lims::test::seededCompany(), 11)));
    }
}

// Mutation: drop the visibility filter from ListPriceLists, ListBillingClients or GetBillingSettings.
TEST_CASE("price lists: a list or a read shows only a company the caller can see", "[lims][billing][prices]") {
    DbFixture fixture;
    auto const osaka = secondCompany();
    lims::test::grant("bea", lims::LimsRole::BillingClerk);
    lims::PriceListModel prices;
    {
        ScopedPrincipal const bea{"bea"};
        auto const list = prices.execute(listFor(lims::test::seededCompany()));
        prices.execute(priceIn(list.id, 1, 4000));
        prices.execute(lims::RegisterBillingClient{
            .client = lims::ClientId{1}, .company = lims::test::seededCompany(), .name = "Waterworks"});
    }
    {
        ScopedPrincipal const sven{"sven"};
        prices.execute(settingsFor(osaka));
    }
    {
        ScopedPrincipal const dave{"dave"};
        prices.execute(lims::RegisterBillingClient{.client = lims::ClientId{1},
                                                   .company = osaka,
                                                   .name = "Waterworks Japan",
                                                   .currency = lims::CurrencyCode::JPY});
        prices.execute(
            lims::RegisterBillingClient{.client = lims::ClientId{0 + 7}, .company = osaka, .name = "Another"});
        CHECK(prices.execute(lims::ListPriceLists{.company = lims::test::seededCompany()}).priceLists.empty());
        auto const clients = prices.execute(lims::ListBillingClients{}).clients;
        REQUIRE(clients.size() == 2U);
        CHECK(clients[0].company == osaka);
        CHECK(clients[0].client == lims::ClientId{1});
        CHECK(clients[0].currency == lims::CurrencyCode::JPY);
        CHECK(clients[1].client == lims::ClientId{7});
        CHECK_THROWS_AS(prices.execute(lims::GetBillingSettings{.company = lims::test::seededCompany()}),
                        lims::NotFound);
        CHECK_NOTHROW(prices.execute(lims::GetBillingSettings{.company = osaka}));
        CHECK_THROWS_AS(prices.execute(lims::QuotePrice{.company = lims::test::seededCompany(),
                                                        .client = lims::ClientId{1},
                                                        .analysis = lims::AnalysisId{1},
                                                        .on = day(11, 1)}),
                        lims::NotFound);
    }
    ScopedPrincipal const bea{"bea"};
    auto const shown = prices.execute(lims::ListPriceLists{.company = lims::test::seededCompany()}).priceLists;
    REQUIRE(shown.size() == 1U);
    CHECK(shown.front().prices.size() == 1U);
    CHECK(prices.execute(lims::ListPriceLists{.company = osaka}).priceLists.empty());
    CHECK(prices.execute(lims::ListBillingClients{}).clients.size() == 1U);
    CHECK_THROWS_AS(prices.execute(lims::GetBillingSettings{.company = osaka}), lims::NotFound);
    ScopedPrincipal const stranger{"stranger"};
    CHECK(prices.execute(lims::ListPriceLists{.company = lims::test::seededCompany()}).priceLists.empty());
    CHECK(prices.execute(lims::ListBillingClients{}).clients.empty());
}

// Mutation: order the lists by id instead of start.
TEST_CASE("price lists: a company's lists are shown by start", "[lims][billing][prices]") {
    DbFixture fixture;
    lims::test::grant("bea", lims::LimsRole::BillingClerk);
    ScopedPrincipal const bea{"bea"};
    lims::PriceListModel prices;
    auto const december = prices.execute(listFor(lims::test::seededCompany(), 12));
    auto const october = prices.execute(listFor(lims::test::seededCompany(), 10));
    auto const shown = prices.execute(lims::ListPriceLists{.company = lims::test::seededCompany()}).priceLists;
    REQUIRE(shown.size() == 2U);
    CHECK(shown[0].id == october.id);
    CHECK(shown[1].id == december.id);
}

// Mutations: rethrow the SqlException from storeClient, storePriceList or storePrice instead of answering Conflict;
// from storeSettings, drop the retry as a replacement.
// The winner is another connection (a thread) that stores the same key between the loser's lookup and its insert, so
// the loser's insert meets the unique index and its rolled-back transaction is followed by a lookup that finds the
// winner's committed row.
TEST_CASE("price lists: a key stored by a concurrent call between the lookup and the insert is a Conflict",
          "[lims][billing][prices]") {
    DbFixture fixture;
    auto const company = lims::test::seededCompany();
    auto const inThread = [](auto fn) {
        std::thread other{std::move(fn)};
        other.join();
    };
    SECTION("a billing client") {
        lims::RegisterBillingClient const loser{
            .client = lims::ClientId{1}, .company = company, .name = "Loser", .currency = lims::CurrencyCode::EUR};
        auto winner = [&] {
            inThread([&] {
                static_cast<void>(
                    lims::PriceListModel::storeClient(lims::RegisterBillingClient{.client = loser.client,
                                                                                  .company = company,
                                                                                  .name = "Winner",
                                                                                  .currency = lims::CurrencyCode::USD},
                                                      {}));
            });
        };
        CHECK_THROWS_AS(static_cast<void>(lims::PriceListModel::storeClient(loser, winner)), lims::Conflict);
        auto const kept = lims::PriceListModel::billingClient(loser.client, company);
        REQUIRE(kept.has_value());
        CHECK(kept->name == "Winner");
    }
    SECTION("a price list") {
        auto const loser = listFor(company);
        auto winner = [&] {
            inThread([&] {
                auto other = listFor(company);
                other.name = "Winner";
                static_cast<void>(lims::PriceListModel::storePriceList(other, {}));
            });
        };
        CHECK_THROWS_AS(static_cast<void>(lims::PriceListModel::storePriceList(loser, winner)), lims::Conflict);
        lims::test::grant("bea", lims::LimsRole::BillingClerk);
        ScopedPrincipal const bea{"bea"};
        auto const shown = lims::PriceListModel{}.execute(lims::ListPriceLists{.company = company}).priceLists;
        REQUIRE(shown.size() == 1U);
        CHECK(shown.front().name == "Winner");
    }
    SECTION("a price") {
        lims::test::grant("bea", lims::LimsRole::BillingClerk);
        lims::PriceListId list;
        {
            ScopedPrincipal const bea{"bea"};
            list = lims::PriceListModel{}.execute(listFor(company)).id;
        }
        auto winner = [&] {
            inThread([&] { static_cast<void>(lims::PriceListModel::storePrice(priceIn(list, 1, 9900), {})); });
        };
        CHECK_THROWS_AS(static_cast<void>(lims::PriceListModel::storePrice(priceIn(list, 1, 4000), winner)),
                        lims::Conflict);
        ScopedPrincipal const bea{"bea"};
        auto const shown = lims::PriceListModel{}.execute(lims::ListPriceLists{.company = company}).priceLists;
        REQUIRE(shown.size() == 1U);
        REQUIRE(shown.front().prices.size() == 1U);
        CHECK(*shown.front().prices.front().amount == Rational{9900, DecimalPlaces{2}});
    }
    SECTION("settings: the loser replaces the winner's") {
        auto winner = [&] {
            inThread([&] {
                auto other = settingsFor(company);
                other.revenueAccount = "9999";
                static_cast<void>(lims::PriceListModel::storeSettings(other, {}));
            });
        };
        auto const stored = lims::PriceListModel::storeSettings(settingsFor(company), winner);
        CHECK(stored.revenueAccount == "4000");
        auto const read = lims::PriceListModel::settingsOf(company);
        REQUIRE(read.has_value());
        CHECK(read->revenueAccount == "4000");
        Lightweight::DataMapper mapper;
        CHECK(mapper.Query<lims::db::BillingSettingsRecord>().All().size() == 1U);
    }
}

// Mutation: drop the unique index of any table in the migration.
TEST_CASE("price lists: the unique indexes hold when the models are bypassed", "[lims][billing][prices]") {
    DbFixture fixture;
    Lightweight::DataMapper mapper;
    lims::db::BillingSettingsRecord settings;
    settings.companyId = *lims::test::seededCompany();
    mapper.Create(settings);
    lims::db::BillingSettingsRecord settingsAgain;
    settingsAgain.companyId = *lims::test::seededCompany();
    CHECK_THROWS(mapper.Create(settingsAgain));

    lims::db::BillingClientRecord client;
    client.clientId = 1;
    client.companyId = *lims::test::seededCompany();
    mapper.Create(client);
    lims::db::BillingClientRecord clientAgain;
    clientAgain.clientId = 1;
    clientAgain.companyId = *lims::test::seededCompany();
    CHECK_THROWS(mapper.Create(clientAgain));

    lims::db::PriceListRecord list;
    list.companyId = *lims::test::seededCompany();
    list.validFromMs = 1000;
    mapper.Create(list);
    lims::db::PriceListRecord listAgain;
    listAgain.companyId = *lims::test::seededCompany();
    listAgain.validFromMs = 1000;
    CHECK_THROWS(mapper.Create(listAgain));

    lims::db::PriceRecord price;
    price.priceList = static_cast<std::int64_t>(list.id.Value());
    price.analysisId = 1;
    mapper.Create(price);
    lims::db::PriceRecord priceAgain;
    priceAgain.priceList = static_cast<std::int64_t>(list.id.Value());
    priceAgain.analysisId = 1;
    CHECK_THROWS(mapper.Create(priceAgain));
}

TEST_CASE("PriceListModel over the full backend-mode matrix: a price list and a quote", "[lims][billing][matrix]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    lims::test::grant("bea", lims::LimsRole::BillingClerk);
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    BackendRig rig{mode, 1, lims::test::limsAuthorizer(kSecret)};
    rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "bea"));
    auto prices = rig.client<lims::PriceListModel>(0);
    awaitQt(prices.execute(lims::RegisterBillingClient{
        .client = lims::ClientId{1}, .company = lims::test::seededCompany(), .name = "Waterworks"}));
    auto const list = awaitQt(prices.execute(listFor(lims::test::seededCompany())));
    awaitQt(prices.execute(priceIn(list.id, 1, 4000)));
    auto const quote = awaitQt(prices.execute(lims::QuotePrice{.company = lims::test::seededCompany(),
                                                               .client = lims::ClientId{1},
                                                               .analysis = lims::AnalysisId{1},
                                                               .on = day(11, 1)}));
    CHECK(*quote.amount == Rational{4000, DecimalPlaces{2}});
    CHECK(quote.priceList == list.id);
    auto const shown =
        awaitQt(prices.execute(lims::ListPriceLists{.company = lims::test::seededCompany()})).priceLists;
    REQUIRE(shown.size() == 1U);
    CHECK(shown.front().prices.size() == 1U);
    CHECK(awaitQt(prices.execute(lims::ListBillingClients{})).clients.size() == 1U);
}
