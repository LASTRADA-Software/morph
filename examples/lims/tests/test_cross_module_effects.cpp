// SPDX-License-Identifier: Apache-2.0
//
// The cross-module effects question, cause by cause, on watching connections: (i) billing's PostInvoice writes the
// ledger's Ledger_Entry inside itself and cannot declare it; (ii) the relay's billing line reaches a watcher when its
// write is a dispatched action, and not when the same write runs outside any dispatch.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <glaze/glaze.hpp>
#include <morph/session/session_auth.hpp>
#include <string>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/app/event_wiring.hpp"
#include "lims/app/internal_client.hpp"
#include "lims/app/service_principals.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::ServerConnection;

namespace {

/// @brief A session for @p principal, signed with the test servers' secret.
[[nodiscard]] morph::session::Context as(std::string const& principal) {
    morph::session::TokenIssuer const issuer{std::string{lims::test::kServiceSecret}, morph::session::hmacSha256};
    return lims::test::signedAs(issuer, principal);
}

/// @brief One cent, in the seeded company's two-decimal currency.
[[nodiscard]] lims::Amount cent() { return lims::Amount{morph::math::Rational{1, morph::math::DecimalPlaces{2}}}; }

/// @brief A balanced one-cent draft entry for the seeded company.
[[nodiscard]] std::string draftEntryJson() {
    return glz::write_json(lims::CreateDraftEntry{
                               .company = lims::test::seededCompany(),
                               .date = morph::time::Timestamp{lims::test::at(2026, 11, 20)},
                               .memo = "control",
                               .currency = lims::CurrencyCode::EUR,
                               .rate = lims::Rate{morph::math::Rational{1, morph::math::DecimalPlaces{6}}},
                               .lines = {lims::PostingLine{.accountCode = "6900",
                                                           .side = lims::PostingSide::Debit,
                                                           .amount = cent(),
                                                           .companyAmount = cent()},
                                         lims::PostingLine{.accountCode = "2200",
                                                           .side = lims::PostingSide::Credit,
                                                           .amount = cent(),
                                                           .companyAmount = cent()}},
                           })
        .value_or(std::string{});
}

}  // namespace

// Cause (i). The control is the ledger's own action on the same connection, which it hears.
TEST_CASE(
    "cross-module effects (i): billing's write of a ledger entry inside its own action reaches no ledger "
    "watcher",
    "[lims][effects]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig posting{lims::PostingMode::SharedTransaction};
    auto const invoice = lims::test::approvedInvoice();
    auto const accountant = posting.rig().connect();
    REQUIRE(accountant->watch({"Ledger_Entry"}, as("acc")).kind == "ok");

    auto const posted = posting.post(invoice);
    REQUIRE(posted.state == lims::InvoiceState::Posted);
    bool const heard = ServerConnection::eventually([&] { return accountant->heardOf("Ledger_Entry"); },
                                                    std::chrono::milliseconds{1500});
    CHECK_FALSE(heard);

    // The control: the ledger's own action, dispatched on the same server, is heard on the same connection.
    auto const ledger = accountant->registerModel("LedgerModel", as("acc"));
    REQUIRE(accountant->execute(ledger, "LedgerModel", "CreateDraftEntry", draftEntryJson(), as("acc")).kind == "ok");
    CHECK(ServerConnection::eventually([&] { return accountant->heardOf("Ledger_Entry"); }));
}

// Cause (ii). Mutation: declare RecordBillableLine with no effect (the dispatched line is not heard either).
TEST_CASE(
    "cross-module effects (ii): the relay's billing line reaches a watcher as a dispatched action, and not "
    "when the same write runs outside one",
    "[lims][effects]") {
    lims::test::LabFixture const fixture;
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    lims::test::grantService(std::string{lims::app::kEventsPrincipal});
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(lims::test::kServiceSecret)};
    auto const watcher = rig.connect();
    REQUIRE(watcher->watch({"Billing_Line"}, as("bea")).kind == "ok");

    // Outside any dispatch: the consumer's body, run in process as the events principal, writes the line.
    {
        ScopedPrincipal const events{std::string{lims::app::kEventsPrincipal}};
        static_cast<void>(lims::InvoiceModel{}.execute(
            lims::RecordBillableLine{.eventId = "test:outside:1",
                                     .result = lims::ResultId{9001},
                                     .sample = lab.sample,
                                     .analysis = lab.nitrate,
                                     .client = lab.client,
                                     .site = lims::test::seededSite(),
                                     .publishedAt = morph::time::Timestamp{lims::test::at(2026, 11, 10)}}));
    }
    bool const heardOutside = ServerConnection::eventually([&] { return watcher->heardOf("Billing_Line"); },
                                                           std::chrono::milliseconds{1500});
    CHECK_FALSE(heardOutside);

    // Dispatched: the relay, through the events client on the same server.
    lims::app::InternalClient events{rig.server(), lims::test::kServiceSecret,
                                     std::string{lims::app::kEventsPrincipal}};
    REQUIRE(lims::app::relayLabEvents(events) == 1U);
    CHECK(ServerConnection::eventually([&] { return watcher->heardOf("Billing_Line"); }));
}
