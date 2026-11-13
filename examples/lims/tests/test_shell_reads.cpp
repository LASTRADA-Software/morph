// SPDX-License-Identifier: Apache-2.0
//
// What the application shell reads from each module: the scopes the caller can choose between, a count of each
// module's open work and a search over each module's records. Every one is a read that answers only about the
// companies the caller can see.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "billing_test_support.hpp"
#include "inventory_test_support.hpp"
#include "lims/billing/db/billing_entity.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/inventory/models/procurement_model.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/lab/db/lims_entity.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

namespace {

/// @brief A whole number as a `Count`.
/// @param value The number.
/// @return The count.
[[nodiscard]] lims::Count countOf(std::int64_t value) {
    return lims::Count{morph::math::Rational{value, morph::math::DecimalPlaces{0}}};
}

/// @brief The titles of @p hits.
/// @param hits The hits.
/// @return Their titles, in order.
[[nodiscard]] std::vector<std::string> titlesOf(std::vector<lims::SearchHit> const& hits) {
    std::vector<std::string> titles;
    for (lims::SearchHit const& hit : hits) {
        titles.push_back(hit.title);
    }
    return titles;
}

/// @brief What `secondCompany` made.
struct Second {
    /// @brief The company, which keeps its books in yen.
    lims::CompanyId company;
    /// @brief Its site "Osaka lab".
    lims::SiteId lab;
    /// @brief Its site "Annex".
    lims::SiteId annex;
};

/// @brief `sam` supervises the seeded company and makes a second one with two sites; `sam` supervises that one too.
/// @return The second company and its sites.
[[nodiscard]] Second secondCompany() {
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    ScopedPrincipal const sam{"sam"};
    lims::OrgModel org;
    auto const company = org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});
    auto const lab = org.execute(lims::CreateSite{.companyId = company.id, .name = "Osaka lab"});
    auto const annex = org.execute(lims::CreateSite{.companyId = company.id, .name = "Annex"});
    return Second{.company = company.id, .lab = lab.id, .annex = annex.id};
}

/// @brief Gives @p principal @p role in the second company, as `sam`.
/// @param principal Who gets the role.
/// @param role The role.
/// @param second The company.
void grantAtSecond(std::string principal, lims::LimsRole role, Second const& second) {
    ScopedPrincipal const sam{"sam"};
    lims::OrgModel{}.execute(lims::AssignRole{
        .principal = std::move(principal), .role = role, .company = second.company, .site = lims::SiteId{}});
}

/// @brief Writes a sample row straight into the table.
/// @param mapper The mapper.
/// @param client The sample's client.
/// @param reference Its reference.
/// @param state Its state, as the enumerator's integer.
/// @param site Its site.
void insertSample(Lightweight::DataMapper& mapper, lims::db::ClientRecord const& client, std::string const& reference,
                  lims::SampleState state, lims::SiteId site) {
    lims::db::SampleRecord row;
    row.client = client;
    row.reference = reference;
    row.state = static_cast<int>(state);
    row.siteId = std::optional<std::int64_t>{*site};
    mapper.Create(row);
}

/// @brief Writes an invoice row straight into the table.
/// @param mapper The mapper.
/// @param company The company.
/// @param site The site.
/// @param client The client's id.
/// @param state The state.
/// @param number The number; empty for a draft.
void insertInvoice(Lightweight::DataMapper& mapper, lims::CompanyId company, lims::SiteId site, std::int64_t client,
                   lims::InvoiceState state, std::string const& number) {
    lims::db::InvoiceRecord row;
    row.companyId = *company;
    row.siteId = *site;
    row.clientId = client;
    row.state = static_cast<int>(state);
    row.number = number;
    mapper.Create(row);
}

}  // namespace

// Mutations: list every company whatever the caller holds (stranger lists both); count the sites of every company
// rather than the caller's (tech's second scope reads "JPY · 2 sites"); leave the currency out of the detail.
TEST_CASE("shell: the scopes are the companies the caller can see, with the sites of them the caller can see",
          "[lims][org][shell]") {
    DbFixture fixture;
    auto const second = secondCompany();
    lims::test::grant("tech", lims::LimsRole::Analyst);
    {
        ScopedPrincipal const sam{"sam"};
        lims::OrgModel{}.execute(lims::AssignRole{
            .principal = "tech", .role = lims::LimsRole::Analyst, .company = lims::CompanyId{}, .site = second.annex});
    }
    {
        ScopedPrincipal const tech{"tech"};
        auto const scopes = lims::OrgModel{}.execute(lims::ListMyScopes{}).scopes;
        REQUIRE(scopes.size() == 2);
        CHECK(scopes.at(0).company == lims::test::seededCompany());
        CHECK(scopes.at(0).label == "Main laboratory");
        CHECK(scopes.at(0).detail.t == "lims.shell.scope.site");
        CHECK(scopes.at(0).detail.args.currency == "EUR");
        CHECK(scopes.at(0).detail.args.count == "1");
        CHECK(scopes.at(0).currency == lims::CurrencyCode::EUR);
        CHECK(scopes.at(0).decimals == countOf(2));
        CHECK(scopes.at(1).company == second.company);
        CHECK(scopes.at(1).label == "Osaka branch");
        CHECK(scopes.at(1).detail.t == "lims.shell.scope.site");
        CHECK(scopes.at(1).detail.args.currency == "JPY");
        CHECK(scopes.at(1).detail.args.count == "1");
        CHECK(scopes.at(1).currency == lims::CurrencyCode::JPY);
        CHECK(scopes.at(1).decimals == countOf(0));
    }
    ScopedPrincipal const stranger{"stranger"};
    CHECK(lims::OrgModel{}.execute(lims::ListMyScopes{}).scopes.empty());
}

// Mutations: count the samples of every site the caller can see (the second company's count is wrong); answer the
// total as the number of hits (the total reads as the 8 hits).
TEST_CASE("shell: the lab counts the samples to verify and finds samples by reference, at one company",
          "[lims][lab][shell]") {
    DbFixture fixture;
    auto const second = secondCompany();
    lims::test::grant("tech", lims::LimsRole::Analyst);
    grantAtSecond("tech", lims::LimsRole::Analyst, second);
    lims::test::grant("visitor", lims::LimsRole::Analyst);
    {
        ScopedPrincipal const tech{"tech"};
        auto const client = lims::SampleModel{}.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
        Lightweight::DataMapper mapper;
        auto const clients = mapper.Query<lims::db::ClientRecord>().All();
        REQUIRE(!clients.empty());
        auto const& owner = clients.front();
        static_cast<void>(client);
        insertSample(mapper, owner, "WW-1", lims::SampleState::ToBeVerified, lims::test::seededSite());
        insertSample(mapper, owner, "WW-2", lims::SampleState::Registered, lims::test::seededSite());
        insertSample(mapper, owner, "WW-3", lims::SampleState::ToBeVerified, second.annex);
        for (int n = 1; n <= 10; ++n) {
            insertSample(mapper, owner, "BULK-" + std::to_string(n), lims::SampleState::Published,
                         lims::test::seededSite());
        }
    }
    lims::SampleDirectoryModel lab;
    {
        ScopedPrincipal const tech{"tech"};
        CHECK(lab.execute(lims::CountSamplesToVerify{.company = lims::test::seededCompany()}).count == countOf(1));
        CHECK(lab.execute(lims::CountSamplesToVerify{.company = second.company}).count == countOf(1));

        auto const found = lab.execute(lims::SearchSamples{.query = "ww", .company = lims::test::seededCompany()});
        CHECK(titlesOf(found.hits) == std::vector<std::string>{"WW-1", "WW-2"});
        CHECK(found.total == countOf(2));
        REQUIRE(found.hits.size() == 2);
        CHECK(found.hits.at(0).detail == "Main site");
        CHECK(found.hits.at(0).screen == "lims.lab.sample");
        CHECK(found.hits.at(0).params.at("sampleId") > 0);

        auto const osaka = lab.execute(lims::SearchSamples{.query = "WW", .company = second.company});
        CHECK(titlesOf(osaka.hits) == std::vector<std::string>{"WW-3"});
        CHECK(osaka.hits.front().detail == "Annex");

        auto const bulk = lab.execute(lims::SearchSamples{.query = "bulk", .company = lims::test::seededCompany()});
        CHECK(bulk.hits.size() == 8);
        CHECK(bulk.total == countOf(10));
        CHECK(lab.execute(lims::SearchSamples{.query = {}, .company = lims::test::seededCompany()}).hits.empty());
    }
    ScopedPrincipal const visitor{"visitor"};
    CHECK(lab.execute(lims::CountSamplesToVerify{.company = lims::test::seededCompany()}).count == countOf(1));
    CHECK(lab.execute(lims::CountSamplesToVerify{.company = second.company}).count == countOf(0));
    auto const hidden = lab.execute(lims::SearchSamples{.query = "WW", .company = second.company});
    CHECK(hidden.hits.empty());
    CHECK(hidden.total == countOf(0));
    CHECK_THROWS_AS(lab.execute(lims::CountSamplesToVerify{}), lims::ValidationError);
}

// Mutations: count the invoices of every company; count the caller's pending approvals
// at every company (carl's count at the second company is wrong); answer the total as the number of hits (the total
// reads as the 8 hits).
TEST_CASE(
    "shell: billing counts the invoices in work and the approvals waiting, and finds invoices by number or client",
    "[lims][invoice][shell]") {
    DbFixture fixture;
    auto const invoice = lims::test::draftedInvoice();
    lims::test::submit(invoice);
    auto const second = secondCompany();
    grantAtSecond("bea", lims::LimsRole::BillingClerk, second);
    grantAtSecond("carl", lims::LimsRole::InvoiceApprover, second);
    {
        Lightweight::DataMapper mapper;
        insertInvoice(mapper, lims::test::seededCompany(), lims::test::seededSite(), 1, lims::InvoiceState::Approved,
                      {});
        insertInvoice(mapper, second.company, second.lab, 1, lims::InvoiceState::Draft, {});
        for (int n = 1; n <= 10; ++n) {
            insertInvoice(mapper, lims::test::seededCompany(), lims::test::seededSite(), 1, lims::InvoiceState::Posted,
                          "INV-2026-" + std::to_string(1000 + n));
        }
    }
    lims::InvoiceModel invoices;
    ScopedPrincipal const clerk{"bea"};
    // One draft became a submitted invoice; the other seeded rows are approved or posted.
    CHECK(invoices.execute(lims::CountOpenInvoiceWork{.company = lims::test::seededCompany()}).count == countOf(1));
    CHECK(invoices.execute(lims::CountOpenInvoiceWork{.company = second.company}).count == countOf(1));

    auto const byClient =
        invoices.execute(lims::SearchInvoices{.query = "water", .company = lims::test::seededCompany()});
    REQUIRE(!byClient.hits.empty());
    CHECK(byClient.hits.front().screen == "lims.billing.invoice");
    CHECK(byClient.hits.front().params.at("invoiceId") > 0);
    auto const bulk =
        invoices.execute(lims::SearchInvoices{.query = "inv-2026", .company = lims::test::seededCompany()});
    CHECK(bulk.hits.size() == 8);
    CHECK(bulk.total == countOf(10));
    CHECK(bulk.hits.front().title == "INV-2026-1001");
    CHECK(invoices.execute(lims::SearchInvoices{.query = "inv-2026", .company = second.company}).hits.empty());

    ScopedPrincipal const carl{"carl"};
    CHECK(invoices.execute(lims::CountPendingApprovals{.company = lims::test::seededCompany()}).count == countOf(1));
    CHECK(invoices.execute(lims::CountPendingApprovals{.company = second.company}).count == countOf(0));
    CHECK_THROWS_AS(invoices.execute(lims::CountOpenInvoiceWork{}), lims::ValidationError);
}

// Mutations: count the periods of every company; answer the total as the number of hits
// (the total reads as the 8 hits).
TEST_CASE("shell: the ledger counts the periods being closed and finds accounts by code or name",
          "[lims][ledger][shell]") {
    DbFixture fixture;
    auto const second = secondCompany();
    lims::test::grant("acc", lims::LimsRole::Accountant);
    grantAtSecond("acc", lims::LimsRole::Accountant, second);
    ScopedPrincipal const acc{"acc"};
    lims::LedgerModel ledger;
    auto const nov = ledger.execute(lims::OpenPeriod{.company = lims::test::seededCompany(),
                                                     .month = morph::time::Timestamp{lims::test::at(2026, 11, 1)}});
    ledger.execute(lims::OpenPeriod{.company = lims::test::seededCompany(),
                                    .month = morph::time::Timestamp{lims::test::at(2026, 12, 1)}});
    ledger.execute(lims::BeginClosePeriod{.periodId = nov.id});
    auto const osaka = ledger.execute(
        lims::OpenPeriod{.company = second.company, .month = morph::time::Timestamp{lims::test::at(2026, 11, 1)}});
    ledger.execute(lims::BeginClosePeriod{.periodId = osaka.id});
    CHECK(ledger.execute(lims::CountClosingPeriods{.company = lims::test::seededCompany()}).count == countOf(1));
    CHECK(ledger.execute(lims::CountClosingPeriods{.company = second.company}).count == countOf(1));

    ledger.execute(lims::CreateAccount{.company = lims::test::seededCompany(),
                                       .code = "1200",
                                       .name = "Receivables",
                                       .type = lims::AccountType::Asset});
    ledger.execute(lims::CreateAccount{
        .company = second.company, .code = "9000", .name = "Receivables Japan", .type = lims::AccountType::Asset});
    for (int n = 1; n <= 10; ++n) {
        ledger.execute(lims::CreateAccount{.company = lims::test::seededCompany(),
                                           .code = "70" + std::to_string(10 + n),
                                           .name = "Bulk " + std::to_string(n),
                                           .type = lims::AccountType::Expense});
    }
    auto const byName = ledger.execute(lims::SearchAccounts{.query = "recei", .company = lims::test::seededCompany()});
    CHECK(titlesOf(byName.hits) == std::vector<std::string>{"1200"});
    CHECK(byName.hits.front().detail == "Receivables");
    CHECK(byName.hits.front().screen == "lims.ledger.accounts");
    auto const byCode = ledger.execute(lims::SearchAccounts{.query = "12", .company = lims::test::seededCompany()});
    CHECK(titlesOf(byCode.hits) == std::vector<std::string>{"1200"});
    auto const bulk = ledger.execute(lims::SearchAccounts{.query = "70", .company = lims::test::seededCompany()});
    CHECK(bulk.hits.size() == 8);
    CHECK(bulk.total == countOf(10));
    CHECK(titlesOf(ledger.execute(lims::SearchAccounts{.query = "9000", .company = second.company}).hits) ==
          std::vector<std::string>{"9000"});
    CHECK(ledger.execute(lims::SearchAccounts{.query = "9000", .company = lims::test::seededCompany()}).hits.empty());
    CHECK_THROWS_AS(ledger.execute(lims::CountClosingPeriods{}), lims::ValidationError);
}

// Mutations: count the caller's pending requisitions at every company (anna's count at the second company is wrong);
// answer the total as the number of hits (the total reads as the 8 hits).
TEST_CASE("shell: inventory counts the requisitions waiting for the caller and finds lots by item code or lot number",
          "[lims][requisition][inventory][shell]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    {
        ScopedPrincipal const rita{"rita"};
        static_cast<void>(lims::ProcurementModel{}.execute(lims::SubmitRequisition{.requisitionId = setUp.draft.id}));
    }
    auto const second = secondCompany();
    grantAtSecond("anna", lims::LimsRole::PurchaseApprover, second);
    grantAtSecond("kim", lims::LimsRole::StockKeeper, second);
    {
        ScopedPrincipal const anna{"anna"};
        morph::ladder::ScopedClockOverride const clock{lims::test::at(2026, 11, 11)};
        lims::ProcurementModel procurement;
        CHECK(procurement.execute(lims::CountPendingRequisitions{.company = lims::test::seededCompany()}).count ==
              countOf(1));
        CHECK(procurement.execute(lims::CountPendingRequisitions{.company = second.company}).count == countOf(0));
        CHECK_THROWS_AS(procurement.execute(lims::CountPendingRequisitions{}), lims::ValidationError);
    }
    for (int n = 1; n <= 10; ++n) {
        static_cast<void>(lims::test::openingLot(setUp.stock.reagent, setUp.stock.coldRoom,
                                                 "LOT-" + std::to_string(100 + n), lims::test::at(2027, 1, 1),
                                                 lims::test::litres(1000), lims::StockUnit::litre));
    }
    ScopedPrincipal const kim{"kim"};
    lims::StockModel stock;
    auto const byItem = stock.execute(lims::SearchStock{.query = "no3", .company = lims::test::seededCompany()});
    CHECK(byItem.hits.size() == 8);
    CHECK(byItem.total == countOf(10));
    CHECK(byItem.hits.front().title == "NO3-R");
    CHECK(byItem.hits.front().screen == "lims.inventory.stock");
    CHECK(byItem.hits.front().params.at("itemId") == *setUp.stock.reagent);
    auto const byLot = stock.execute(lims::SearchStock{.query = "lot-105", .company = lims::test::seededCompany()});
    CHECK(titlesOf(byLot.hits) == std::vector<std::string>{"NO3-R"});
    CHECK(byLot.hits.front().detail == "LOT-105");
    CHECK(stock.execute(lims::SearchStock{.query = "no3", .company = second.company}).hits.empty());
    CHECK(stock.execute(lims::SearchStock{.query = "zzz", .company = lims::test::seededCompany()}).total ==
          countOf(0));
}
