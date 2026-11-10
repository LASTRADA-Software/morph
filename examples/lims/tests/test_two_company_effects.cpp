// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 7, first instance: two companies' clerks watch invoices on one server; a company-B invoice is created.
// Does the company-A connection hear of it?

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "billing_test_support.hpp"
#include "lims/app/service_principals.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/org/models/org_model.hpp"
#include "testkit/backend_rig.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::ServerConnection;
using morph::math::DecimalPlaces;
using morph::math::Rational;

namespace {

constexpr std::string_view kSecret = "lims-two-company-effects-secret-at-least-32";

/// @brief A session for @p principal, signed with the case's secret.
/// @param principal The principal.
/// @return The session.
[[nodiscard]] morph::session::Context as(std::string const& principal) {
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    return lims::test::signedAs(issuer, principal);
}

/// @brief A second company with one site, billing settings, a JPY billing client 1 and one unbilled line at its site,
///        set up by sam and bea; olga is a billing clerk there.
/// @return The company and its site.
std::pair<lims::CompanyId, lims::SiteId> osaka() {
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    lims::CompanyId company;
    lims::SiteId site;
    {
        ScopedPrincipal const sam{"sam"};
        lims::OrgModel org;
        company = org.execute(lims::CreateCompany{.name = "Osaka branch",
                                                  .baseCurrency = lims::CurrencyCode::JPY,
                                                  .fiscalYearStart = lims::Month::April})
                      .id;
        site = org.execute(lims::CreateSite{.companyId = company, .name = "Osaka lab"}).id;
        org.execute(lims::AssignRole{
            .principal = "olga", .role = lims::LimsRole::BillingClerk, .company = company, .site = {}});
        org.execute(lims::AssignRole{
            .principal = "bea", .role = lims::LimsRole::BillingClerk, .company = company, .site = {}});
        org.execute(lims::AssignRole{.principal = std::string{lims::app::kEventsPrincipal},
                                     .role = lims::LimsRole::Service,
                                     .company = company,
                                     .site = {}});
        lims::PriceListModel prices;
        prices.execute(lims::SetBillingSettings{.company = company,
                                                .receivableAccount = "1200",
                                                .revenueAccount = "4000",
                                                .taxAccount = "2200",
                                                .roundingAccount = "6900",
                                                .taxRate = lims::Rate{Rational{0, DecimalPlaces{6}}},
                                                .approvalHours = lims::Count{Rational{48, DecimalPlaces{0}}},
                                                .billingDay = lims::Count{Rational{25, DecimalPlaces{0}}}});
    }
    {
        ScopedPrincipal const bea{"bea"};
        lims::PriceListModel prices;
        prices.execute(lims::RegisterBillingClient{.client = lims::ClientId{1},
                                                   .company = company,
                                                   .name = "Osaka water",
                                                   .currency = lims::CurrencyCode::JPY});
        auto const list =
            prices.execute(lims::CreatePriceList{.company = company,
                                                 .name = "Yen",
                                                 .currency = lims::CurrencyCode::JPY,
                                                 .validFrom = morph::time::Timestamp{lims::test::at(2026, 1, 1, 0)}});
        prices.execute(lims::SetPrice{.priceList = list.id,
                                      .analysis = lims::AnalysisId{1},
                                      .client = {},
                                      .amount = lims::Amount{Rational{4000, DecimalPlaces{0}}}});
    }
    // The unbilled line company B invoices is written as the relay writes one (the events principal holds the service
    // role at company B, which sam as its supervisor gave it), called in process: this case measures the invoice's
    // fan-out, not the relay's.
    ScopedPrincipal const events{std::string{lims::app::kEventsPrincipal}};
    static_cast<void>(lims::InvoiceModel{}.execute(
        lims::RecordBillableLine{.eventId = "test:osaka:1",
                                 .result = lims::ResultId{901},
                                 .sample = lims::SampleId{901},
                                 .analysis = lims::AnalysisId{1},
                                 .client = lims::ClientId{1},
                                 .site = site,
                                 .publishedAt = morph::time::Timestamp{lims::test::at(2026, 11, 10)}}));
    return {company, site};
}

/// @brief The keys of every `Billing_Invoice` change @p connection was sent, each as the JSON it arrived as, and
/// whether
///        any such change named the record as a whole.
struct InvoiceChangesHeard {
    std::vector<std::string> keys;
    bool asWhole = false;
};

/// @param connection A connection that watches `Billing_Invoice`.
/// @return What its `Billing_Invoice` changes carried.
[[nodiscard]] InvoiceChangesHeard invoiceChangesHeard(ServerConnection const& connection) {
    InvoiceChangesHeard heard;
    for (auto const& sent : connection.changes()) {
        for (auto const& change : sent.records) {
            if (change.record != "Billing_Invoice") {
                continue;
            }
            if (!change.keys) {
                heard.asWhole = true;
                continue;
            }
            for (auto const& key : *change.keys) {
                heard.keys.emplace_back(key.str);
            }
        }
    }
    return heard;
}

}  // namespace

// Hypothesis 7 (decision 40). The leak CHECK asserts a received event, not an absence: the only execute on the server
// is the company-B CreateInvoice, so company A's connection has nothing else to have heard. The control (olga's own
// connection hears) fails if the watch or the fan-out does nothing.
TEST_CASE("Hypothesis 7: a company-B invoice change reaches a company-A connection watching invoices",
          "[lims][billing][h7]") {
    lims::test::LabFixture const fixture;
    auto const [companyB, siteB] = osaka();
    lims::test::grant("carol", lims::LimsRole::BillingClerk);  // company A, the seeded company
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(kSecret)};
    auto const carol = rig.connect();
    auto const olga = rig.connect();
    auto const olgaModel = olga->registerModel("InvoiceModel", as("olga"));
    REQUIRE(carol->watch({"Billing_Invoice"}, as("carol")).kind == "ok");
    REQUIRE(olga->watch({"Billing_Invoice"}, as("olga")).kind == "ok");

    auto const body =
        glz::write_json(lims::CreateInvoice{.company = companyB, .site = siteB, .client = lims::ClientId{1}})
            .value_or(std::string{});
    auto const created = olga->execute(olgaModel, "InvoiceModel", "CreateInvoice", body, as("olga"));
    REQUIRE(created.kind == "ok");

    // The control: the writer's own connection hears of it, so the watch and the fan-out work.
    REQUIRE(ServerConnection::eventually([&] { return olga->heardOf("Billing_Invoice"); }));
    bool const leaked = ServerConnection::eventually([&] { return carol->heardOf("Billing_Invoice"); },
                                                     std::chrono::milliseconds{1000});
    for (auto const& sent : carol->changes()) {
        for (auto const& change : sent.records) {
            UNSCOPED_INFO("sent to company A: " << change.record << (change.keys ? " with keys" : " as a whole"));
        }
    }
    CHECK(leaked);
}

// Hypothesis 7, an action that declares `updates<>`: approving a company-B invoice. The invoice is created and
// submitted before the server starts (no fan-out reaches its watchers), so the approval is the only change the server
// fans out. Mutation: declare `changes<InvoiceView>` instead of `updates<InvoiceView>` on ApproveInvoice; company A is
// then sent the record as a whole and the key CHECKs fail.
TEST_CASE("Hypothesis 7: a company-B invoice approval sends a company-A watcher the invoice's id",
          "[lims][billing][h7]") {
    lims::test::LabFixture const fixture;
    auto const [companyB, siteB] = osaka();
    lims::test::grant("carol", lims::LimsRole::BillingClerk);  // company A, the seeded company
    lims::InvoiceId invoiceB;
    {
        ScopedPrincipal const sam{"sam"};
        lims::OrgModel org;
        org.execute(lims::AssignRole{
            .principal = "ozzy", .role = lims::LimsRole::InvoiceApprover, .company = companyB, .site = {}});
        org.execute(lims::SetValueLimit{
            .principal = "ozzy", .company = companyB, .limit = lims::Amount{Rational{100000, DecimalPlaces{0}}}});
    }
    {
        ScopedPrincipal const olga{"olga"};
        lims::InvoiceModel invoices;
        invoiceB =
            invoices.execute(lims::CreateInvoice{.company = companyB, .site = siteB, .client = lims::ClientId{1}}).id;
    }
    // SubmitInvoice is a Task handler: a direct execute only builds it, so the suite dispatches it.
    static_cast<void>(lims::test::dispatchAs<lims::InvoiceModel>("olga", lims::SubmitInvoice{.invoiceId = invoiceB}));
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(kSecret)};
    auto const carol = rig.connect();
    auto const olga = rig.connect();
    auto const ozzy = rig.connect();
    auto const ozzyModel = ozzy->registerModel("InvoiceModel", as("ozzy"));
    REQUIRE(carol->watch({"Billing_Invoice"}, as("carol")).kind == "ok");
    REQUIRE(olga->watch({"Billing_Invoice"}, as("olga")).kind == "ok");

    auto const body = glz::write_json(lims::ApproveInvoice{.invoiceId = invoiceB}).value_or(std::string{});
    REQUIRE(ozzy->execute(ozzyModel, "InvoiceModel", "ApproveInvoice", body, as("ozzy")).kind == "ok");

    // The control: company B's own clerk is sent the invoice's key, so the approval was fanned out with keys.
    REQUIRE(ServerConnection::eventually([&] { return olga->heardOf("Billing_Invoice"); }));
    auto const own = invoiceChangesHeard(*olga);
    REQUIRE(own.keys.size() == 1);
    CHECK(own.keys.front() == std::to_string(*invoiceB));

    bool const heard = ServerConnection::eventually([&] { return carol->heardOf("Billing_Invoice"); },
                                                    std::chrono::milliseconds{1000});
    auto const leaked = invoiceChangesHeard(*carol);
    for (auto const& key : leaked.keys) {
        UNSCOPED_INFO("sent to company A: Billing_Invoice with key " << key);
    }
    UNSCOPED_INFO("sent to company A: as a whole " << leaked.asWhole << ", heard " << heard);
    REQUIRE(heard);
    REQUIRE(leaked.keys.size() == 1);
    CHECK(leaked.keys.front() == std::to_string(*invoiceB));
    CHECK_FALSE(leaked.asWhole);
}
