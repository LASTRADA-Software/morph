// SPDX-License-Identifier: Apache-2.0
//
// Draft invoices and their totals, and the invoice half of hypothesis 8's second instance: the currency is the
// record's, not the type's.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <limits>
#include <morph/forms/forms.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "billing_test_support.hpp"
#include "lims/app/event_wiring.hpp"
#include "lims/billing/core/invoice_math.hpp"
#include "lims/billing/db/billing_entity.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/org/core/calendar.hpp"
#include "lims/org/models/org_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::Mode;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

[[nodiscard]] Rational exact(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return Rational{Numerator{num}, Denominator{den}, DecimalPlaces{places}};
}

/// @brief Settings for the seeded company with @p taxRate, set by sam.
void settings(Rational taxRate) {
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    ScopedPrincipal const sam{"sam"};
    lims::PriceListModel{}.execute(lims::SetBillingSettings{
        .company = lims::test::seededCompany(),
        .receivableAccount = "1200",
        .revenueAccount = "4000",
        .taxAccount = "2200",
        .roundingAccount = "6900",
        .taxRate = lims::Rate{taxRate},
        .approvalHours = lims::Count{Rational{48, DecimalPlaces{0}}},
        .billingDay = lims::Count{Rational{25, DecimalPlaces{0}}},
    });
}

/// @brief A published sample of a billed client, with its one line relayed into billing.
[[nodiscard]] lims::test::LabSample billedSample(Rational taxRate = exact(19, 100, 6)) {
    settings(taxRate);
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    lims::test::relayLabEvents();
    return lab;
}

/// @brief Writes an unbilled line straight through the ORM, below the model: what no action makes.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters) -- result id and amount, in the order a reader writes them
void insertLine(lims::test::LabSample const& lab, lims::SiteId site, std::int64_t resultId, std::int64_t num,
                lims::CurrencyCode currency, lims::ClientId client = {}) {
    Lightweight::DataMapper mapper;
    lims::db::BillableLineRecord line;
    line.companyId = *lims::test::seededCompany();
    line.siteId = *site;
    line.clientId = client.hasValue() ? *client : *lab.client;
    line.sampleId = *lab.sample;
    line.resultId = resultId;
    line.analysisId = *lab.nitrate;
    line.publishedAt = lims::millisOf(morph::time::Timestamp{lims::test::at(2026, 11, 10)});
    line.state = static_cast<int>(lims::BillableLineState::Unbilled);
    line.amountNum = num;
    line.amountDen = 1;
    line.currency = static_cast<int>(currency);
    line.eventId = Lightweight::SqlAnsiString<128>{"test:line:" + std::to_string(resultId)};
    mapper.Create(line);
}

[[nodiscard]] lims::CreateInvoice invoiceOf(lims::test::LabSample const& lab) {
    return lims::CreateInvoice{
        .company = lims::test::seededCompany(),
        .site = lims::test::seededSite(),
        .client = lab.client,
    };
}

/// @brief The lines billing holds for @p lab's client, as bea sees them.
[[nodiscard]] std::vector<lims::BillableLineView> linesOf(lims::test::LabSample const& lab) {
    ScopedPrincipal const bea{"bea"};
    return lims::InvoiceModel{}
        .execute(lims::ListBillableLines{.company = {}, .client = lab.client, .sample = {}})
        .lines;
}

}  // namespace

// Mutation: compute tax per line and sum it (three 0.05 lines at 50 % tax 0.09, not 0.08).
TEST_CASE("invoices: tax is the flat rate on the net total, rounded half-up once", "[lims][billing][invoices]") {
    auto const eur =
        lims::invoiceTotals(std::vector<Rational>{exact(1005, 100, 2)}, exact(19, 100, 6), lims::CurrencyCode::EUR);
    CHECK(eur.net == exact(1005, 100, 2));
    CHECK(eur.tax == exact(191, 100, 2));
    CHECK(eur.gross == exact(1196, 100, 2));

    auto const yen =
        lims::invoiceTotals(std::vector<Rational>{exact(1005, 1, 0)}, exact(10, 100, 6), lims::CurrencyCode::JPY);
    CHECK(yen.tax == exact(101, 1, 0));
    CHECK(yen.gross == exact(1106, 1, 0));

    auto const once = lims::invoiceTotals(std::vector<Rational>{exact(5, 100, 2), exact(5, 100, 2), exact(5, 100, 2)},
                                          exact(1, 2, 6), lims::CurrencyCode::EUR);
    CHECK(once.tax == exact(8, 100, 2));
}

// Mutation: skip the refusal when the checked add reports overflow (the over-range sum is no longer refused).
TEST_CASE("invoices: a total that does not fit is refused, never saturated", "[lims][billing][invoices]") {
    constexpr std::int64_t kBig = std::numeric_limits<std::int64_t>::max() - 1;
    CHECK_THROWS_AS(lims::invoiceTotals(std::vector<Rational>{exact(kBig, 1, 2), exact(kBig, 1, 2)}, exact(19, 100, 6),
                                        lims::CurrencyCode::EUR),
                    lims::OutOfRange);
    // The net fits but the tax on it does not.
    CHECK_THROWS_AS(
        lims::invoiceTotals(std::vector<Rational>{exact(kBig, 1, 2)}, exact(19, 100, 6), lims::CurrencyCode::EUR),
        lims::OutOfRange);
    // The net and the tax fit, their sum does not.
    CHECK_THROWS_AS(
        lims::invoiceTotals(std::vector<Rational>{exact(kBig, 1, 2)}, exact(1, 1, 6), lims::CurrencyCode::EUR),
        lims::OutOfRange);
}

TEST_CASE("invoices: an invoice whose lines overflow is refused and leaves its lines unbilled",
          "[lims][billing][invoices]") {
    lims::test::LabFixture const fixture;
    auto const lab = billedSample();
    constexpr std::int64_t kHalf = std::numeric_limits<std::int64_t>::max() / 2;
    insertLine(lab, lims::test::seededSite(), 9001, kHalf, lims::CurrencyCode::EUR);
    insertLine(lab, lims::test::seededSite(), 9002, kHalf, lims::CurrencyCode::EUR);
    ScopedPrincipal const bea{"bea"};
    lims::InvoiceModel invoices;
    CHECK_THROWS_AS(invoices.execute(invoiceOf(lab)), lims::OutOfRange);
    for (auto const& line : linesOf(lab)) {
        CHECK(line.state == lims::BillableLineState::Unbilled);
    }
    CHECK(invoices.execute(lims::ListInvoices{.company = lims::test::seededCompany()}).invoices.empty());
}

// Mutation: let CreateInvoice take lines of every site (the field-lab line joins the seeded site's invoice).
TEST_CASE("invoices: a draft takes only the named site's lines", "[lims][billing][invoices]") {
    lims::test::LabFixture const fixture;
    auto const lab = billedSample();
    lims::SiteId fieldLab;
    {
        lims::test::grant("sam", lims::LimsRole::Supervisor);
        ScopedPrincipal const sam{"sam"};
        fieldLab =
            lims::OrgModel{}.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field"}).id;
    }
    insertLine(lab, fieldLab, 9003, 1000, lims::CurrencyCode::EUR);
    ScopedPrincipal const bea{"bea"};
    auto const draft = lims::InvoiceModel{}.execute(invoiceOf(lab));
    REQUIRE(draft.lines.size() == 1U);
    CHECK(draft.site == lims::test::seededSite());
    CHECK(*draft.net == exact(40, 1, 2));
    auto const lines = linesOf(lab);
    REQUIRE(lines.size() == 2U);
    auto const fieldLine = lines.at(0).site == fieldLab ? lines.at(0) : lines.at(1);
    CHECK(fieldLine.state == lims::BillableLineState::Unbilled);
    // The other site's lines make a draft of their own.
    auto const second = lims::InvoiceModel{}.execute(lims::CreateInvoice{
        .company = lims::test::seededCompany(),
        .site = fieldLab,
        .client = lab.client,
    });
    REQUIRE(second.lines.size() == 1U);
    CHECK(second.site == fieldLab);
}

// Mutation: let CreateInvoice take lines of every client (a second client's line joins the first client's invoice).
TEST_CASE("invoices: a draft collects one client's unbilled lines at one site, in the client's currency",
          "[lims][billing][invoices]") {
    lims::test::LabFixture const fixture;
    auto const lab = billedSample();
    insertLine(lab, lims::test::seededSite(), 9004, 500, lims::CurrencyCode::EUR, lims::ClientId{77});
    ScopedPrincipal const bea{"bea"};
    lims::InvoiceModel invoices;
    auto const draft = invoices.execute(invoiceOf(lab));
    CHECK(draft.state == lims::InvoiceState::Draft);
    CHECK(draft.currency == lims::CurrencyCode::EUR);
    REQUIRE(draft.lines.size() == 1U);
    CHECK(draft.lines.front().state == lims::BillableLineState::Invoiced);
    CHECK(draft.lines.front().invoice == draft.id);
    CHECK(*draft.net == exact(40, 1, 2));
    CHECK(*draft.tax == exact(76, 10, 2));
    CHECK(*draft.gross == exact(476, 10, 2));
    CHECK(draft.createdBy == "bea");
    CHECK(draft.number.empty());
    CHECK(draft.level == lims::ApprovalLevel::Site);
    // Another client's line at the same site stays unbilled.
    auto const others =
        invoices.execute(lims::ListBillableLines{.company = {}, .client = lims::ClientId{77}, .sample = {}});
    REQUIRE(others.lines.size() == 1U);
    CHECK(others.lines.front().state == lims::BillableLineState::Unbilled);
    // The lines are spent: a second draft finds none.
    CHECK_THROWS_AS(invoices.execute(invoiceOf(lab)), lims::Conflict);
    // What the draft answers is what a later read answers.
    auto const read = invoices.execute(lims::GetInvoice{.invoiceId = draft.id});
    CHECK(read.id == draft.id);
    CHECK(*read.gross == *draft.gross);
    REQUIRE(read.lines.size() == 1U);
    auto const listed = invoices.execute(lims::ListInvoices{.company = lims::test::seededCompany()}).invoices;
    REQUIRE(listed.size() == 1U);
    CHECK(listed.front().id == draft.id);
}

TEST_CASE("invoices: a company without billing settings cannot invoice", "[lims][billing][invoices]") {
    lims::test::LabFixture const fixture;
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    lims::test::relayLabEvents();
    ScopedPrincipal const bea{"bea"};
    CHECK_THROWS_WITH(lims::InvoiceModel{}.execute(invoiceOf(lab)),
                      Catch::Matchers::ContainsSubstring("billing settings"));
}

TEST_CASE("invoices: a client not billed at the company cannot be invoiced there", "[lims][billing][invoices]") {
    lims::test::LabFixture const fixture;
    settings(exact(19, 100, 6));
    lims::test::grant("bea", lims::LimsRole::BillingClerk);
    ScopedPrincipal const bea{"bea"};
    CHECK_THROWS_AS(
        lims::InvoiceModel{}.execute(lims::CreateInvoice{
            .company = lims::test::seededCompany(), .site = lims::test::seededSite(), .client = lims::ClientId{77}}),
        lims::NotFound);
}

TEST_CASE("invoices: only a billing clerk at the company drafts, and only for a site of that company",
          "[lims][billing][invoices][authorization]") {
    lims::test::LabFixture const fixture;
    auto const lab = billedSample();
    lims::test::grant("carol", lims::LimsRole::Verifier);
    {
        ScopedPrincipal const carol{"carol"};
        CHECK_THROWS_AS(lims::InvoiceModel{}.execute(invoiceOf(lab)), lims::Forbidden);
    }
    lims::CompanyId osaka;
    lims::SiteId osakaLab;
    {
        ScopedPrincipal const sam{"sam"};
        lims::OrgModel org;
        osaka = org.execute(lims::CreateCompany{
                                .name = "Osaka branch",
                                .baseCurrency = lims::CurrencyCode::JPY,
                                .fiscalYearStart = lims::Month::April,
                            })
                    .id;
        osakaLab = org.execute(lims::CreateSite{.companyId = osaka, .name = "Osaka lab"}).id;
        // sam created the company, so sam alone supervises it and appoints its clerks.
        org.execute(lims::AssignRole{
            .principal = "olga",
            .role = lims::LimsRole::BillingClerk,
            .company = osaka,
            .site = {},
        });
    }
    ScopedPrincipal const bea{"bea"};
    CHECK_THROWS_WITH(lims::InvoiceModel{}.execute(lims::CreateInvoice{
                          .company = lims::test::seededCompany(),
                          .site = osakaLab,
                          .client = lab.client,
                      }),
                      Catch::Matchers::ContainsSubstring("is not a site of company"));
    // A clerk of Osaka only holds a billing role, so the edge passes her, and the model refuses her at the seeded
    // company.
    ScopedPrincipal const olga{"olga"};
    CHECK_THROWS_AS(lims::InvoiceModel{}.execute(invoiceOf(lab)), lims::Forbidden);
}

TEST_CASE("invoices: an invoice of a company the caller cannot see is not found and not listed",
          "[lims][billing][invoices][authorization]") {
    lims::test::LabFixture const fixture;
    auto const lab = billedSample();
    lims::InvoiceId invoice;
    {
        ScopedPrincipal const bea{"bea"};
        invoice = lims::InvoiceModel{}.execute(invoiceOf(lab)).id;
    }
    {
        ScopedPrincipal const sam{"sam"};
        lims::OrgModel org;
        auto const osaka = org.execute(lims::CreateCompany{
                                           .name = "Osaka branch",
                                           .baseCurrency = lims::CurrencyCode::JPY,
                                           .fiscalYearStart = lims::Month::April,
                                       })
                               .id;
        org.execute(lims::AssignRole{
            .principal = "dave",
            .role = lims::LimsRole::Supervisor,
            .company = osaka,
            .site = {},
        });
    }
    ScopedPrincipal const dave{"dave"};
    lims::InvoiceModel invoices;
    CHECK_THROWS_AS(invoices.execute(lims::GetInvoice{.invoiceId = invoice}), lims::NotFound);
    CHECK(invoices.execute(lims::ListInvoices{.company = lims::test::seededCompany()}).invoices.empty());
}

// Hypothesis 8, the invoice half of instance 2: the currency is known only from the record, so the model keeps it.
// Mutation: drop the currency check from draftInvoice (the yen line joins the euro invoice; the refusal check fails).
TEST_CASE("Hypothesis 8: an amount's currency is a field the type does not see, so the model keeps currencies apart",
          "[lims][billing][h8]") {
    lims::test::LabFixture const fixture;
    auto const lab = billedSample();
    // A second unbilled line of the same client at the same site, priced in yen: what a price list in another
    // currency would leave behind. Written below the model, since no action makes one.
    insertLine(lab, lims::test::seededSite(), 9001, 4000, lims::CurrencyCode::JPY);
    // Both amounts are `lims::Amount`, the one money unit, so nothing in the type refuses 40 euros beside 4000 yen;
    // the model does.
    {
        ScopedPrincipal const bea{"bea"};
        CHECK_THROWS_WITH(lims::InvoiceModel{}.execute(invoiceOf(lab)),
                          Catch::Matchers::ContainsSubstring("is in JPY"));
    }
    for (auto const& line : linesOf(lab)) {
        CHECK(line.state == lims::BillableLineState::Unbilled);
    }
    // The schema a JPY price list's price form is served with advertises two decimals, because the declared
    // precision is the type's; the model refuses fractional yen (test_price_lists.cpp) where the form allows it.
    auto const& schema = morph::forms::schemaJson<lims::SetPrice>();
    CHECK_THAT(schema, Catch::Matchers::ContainsSubstring("\"x-decimalPlaces\":2"));
}

// Mutation: make ListInvoices answer no invoices (the listed.size() check fails on every backend).
TEST_CASE("InvoiceModel over the full backend-mode matrix: an invoice is drafted, read and listed",
          "[lims][billing][matrix]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    lims::test::LabFixture const fixture;
    auto const lab = billedSample();
    morph::session::TokenIssuer const issuer{std::string{lims::test::kServiceSecret}, morph::session::hmacSha256};
    morph::ladder::testkit::BackendRig rig{mode, 1, lims::test::limsAuthorizer(lims::test::kServiceSecret)};
    rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "bea"));
    auto invoices = rig.client<lims::InvoiceModel>(0);
    auto const draft = awaitQt(invoices.execute(invoiceOf(lab)));
    CHECK(draft.state == lims::InvoiceState::Draft);
    CHECK(*draft.gross == exact(476, 10, 2));
    REQUIRE(draft.lines.size() == 1U);
    auto const read = awaitQt(invoices.execute(lims::GetInvoice{.invoiceId = draft.id}));
    CHECK(read.id == draft.id);
    CHECK(read.lines.size() == 1U);
    auto const listed = awaitQt(invoices.execute(lims::ListInvoices{.company = lims::test::seededCompany()})).invoices;
    REQUIRE(listed.size() == 1U);
    CHECK(listed.front().id == draft.id);
}
