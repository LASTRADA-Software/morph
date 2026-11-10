// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 3, first instance: a draft invoice is leased to the clerk editing it; a second clerk is told who holds
// it; the lease lapses after a timeout and when the holder's connection closes; a release reaches a second clerk's
// connection, and the lapse is measured for the same.

#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <glaze/glaze.hpp>
#include <morph/session/session_auth.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/billing/core/fault_hook.hpp"
#include "lims/billing/models/invoice_edit_model.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "testkit/backend_rig.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::ServerConnection;

namespace {

constexpr std::string_view kSecret = "lims-invoice-lease-test-secret-at-least-32";

/// @brief Clears the fault hook when the case ends, however it ends.
struct FaultHookGuard {
    FaultHookGuard() = default;
    FaultHookGuard(FaultHookGuard const&) = delete;
    FaultHookGuard& operator=(FaultHookGuard const&) = delete;
    FaultHookGuard(FaultHookGuard&&) = delete;
    FaultHookGuard& operator=(FaultHookGuard&&) = delete;
    ~FaultHookGuard() { lims::setFaultHook({}); }
};

/// @brief A draft invoice over one line at 40.00 with no tax, and two billing clerks, alice and bob, at the seeded
///        company.
/// @return The invoice.
lims::InvoiceView draft() {
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    {
        ScopedPrincipal const sam{"sam"};
        lims::PriceListModel{}.execute(lims::SetBillingSettings{
            .company = lims::test::seededCompany(),
            .receivableAccount = "1200",
            .revenueAccount = "4000",
            .taxAccount = "2200",
            .roundingAccount = "6900",
            .taxRate = lims::Rate{morph::math::Rational{0, morph::math::DecimalPlaces{6}}},
            .approvalHours = lims::Count{morph::math::Rational{48, morph::math::DecimalPlaces{0}}},
            .billingDay = lims::Count{morph::math::Rational{25, morph::math::DecimalPlaces{0}}}});
    }
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    lims::test::relayLabEvents();
    lims::test::grant("alice", lims::LimsRole::BillingClerk);
    lims::test::grant("bob", lims::LimsRole::BillingClerk);
    ScopedPrincipal const alice{"alice"};
    return lims::InvoiceModel{}.execute(lims::CreateInvoice{
        .company = lims::test::seededCompany(), .site = lims::test::seededSite(), .client = lab.client});
}

/// @brief Who holds @p invoice's lease, as bob sees it.
/// @param invoice The invoice.
/// @return The holder; empty when it is free.
[[nodiscard]] std::string holderOf(lims::InvoiceId invoice) {
    ScopedPrincipal const bob{"bob"};
    return lims::InvoiceModel{}.execute(lims::GetInvoiceLease{.invoiceId = invoice}).holder;
}

/// @brief Who a session says @p principal is, signed with the case's secret.
/// @param principal The principal.
/// @return The session.
[[nodiscard]] morph::session::Context as(std::string const& principal) {
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    return lims::test::signedAs(issuer, principal);
}

/// @brief Runs @p sql on a connection of its own, committed, when the next lease is about to be written, as a
///        concurrent editor that committed first would have.
/// @param sql The statement.
void raceWith(std::string sql) {
    lims::setFaultHook([sql = std::move(sql)](std::string_view point, Lightweight::SqlConnection&) {
        if (point == "invoice.beforeLeaseWrite") {
            Lightweight::SqlConnection other;
            Lightweight::SqlStatement statement{other};
            (void)statement.ExecuteDirect(sql);
        }
    });
}

}  // namespace

// Mutation: skip the holder check in LeaseInvoice (bob takes alice's live lease).
TEST_CASE("invoice lease: a second clerk is told who holds the draft, and takes it once it expires",
          "[lims][billing][lease]") {
    lims::test::LabFixture const fixture;
    auto const invoice = draft();
    ScopedClockOverride const nine{lims::test::at(2026, 11, 10, 9)};
    lims::InvoiceEditModel aliceEditor;
    lims::InvoiceEditModel bobEditor;
    {
        ScopedPrincipal const alice{"alice"};
        auto const lease = aliceEditor.execute(lims::LeaseInvoice{.invoiceId = invoice.id});
        CHECK(lease.holder == "alice");
        aliceEditor.execute(lims::SetInvoiceReference{.reference = "PO 4711"});
    }
    {
        ScopedPrincipal const bob{"bob"};
        CHECK_THROWS_WITH(bobEditor.execute(lims::LeaseInvoice{.invoiceId = invoice.id}),
                          Catch::Matchers::ContainsSubstring("being edited by 'alice'"));
        CHECK_THROWS_AS(bobEditor.execute(lims::SetInvoiceReference{.reference = "mine"}), lims::Conflict);
    }
    CHECK(holderOf(invoice.id) == "alice");
    ScopedClockOverride const later{lims::test::at(2026, 11, 10, 10)};
    ScopedPrincipal const bob{"bob"};
    CHECK(bobEditor.execute(lims::LeaseInvoice{.invoiceId = invoice.id}).holder == "bob");
    CHECK(bobEditor.execute(lims::SetInvoiceReference{.reference = "PO 4712"}).reference == "PO 4712");
    // Alice's instance still names a lease, but it is bob's now: her edit is refused, not applied.
    ScopedPrincipal const alice{"alice"};
    CHECK_THROWS_AS(aliceEditor.execute(lims::SetInvoiceReference{.reference = "late"}), lims::Conflict);
}

// Mutation: leave `expires_at` out of requireOwnLease's renewal (an edit no longer extends the lease).
TEST_CASE("invoice lease: each edit renews the lease for fifteen minutes", "[lims][billing][lease]") {
    lims::test::LabFixture const fixture;
    auto const invoice = draft();
    lims::InvoiceEditModel editor;
    ScopedPrincipal const alice{"alice"};
    {
        ScopedClockOverride const nine{lims::test::at(2026, 11, 10, 9)};
        editor.execute(lims::LeaseInvoice{.invoiceId = invoice.id});
    }
    {
        // Ten minutes in, the edit renews the lease to 09:25.
        ScopedClockOverride const tenPast{morph::time::DateTime{std::chrono::year{2026}, std::chrono::month{11},
                                                                std::chrono::day{10}, std::chrono::hours{9},
                                                                std::chrono::minutes{10}, std::chrono::seconds{0}}};
        editor.execute(lims::SetInvoiceReference{.reference = "PO 1"});
    }
    {
        ScopedClockOverride const twentyPast{morph::time::DateTime{std::chrono::year{2026}, std::chrono::month{11},
                                                                   std::chrono::day{10}, std::chrono::hours{9},
                                                                   std::chrono::minutes{20}, std::chrono::seconds{0}}};
        CHECK(editor.execute(lims::SetInvoiceReference{.reference = "PO 2"}).reference == "PO 2");
    }
    ScopedClockOverride const late{lims::test::at(2026, 11, 10, 10)};
    CHECK_THROWS_AS(editor.execute(lims::SetInvoiceReference{.reference = "PO 3"}), lims::Conflict);
    ScopedPrincipal const bob{"bob"};
    CHECK(holderOf(invoice.id).empty());
}

// Mutation: skip requireOwnLease in RemoveInvoiceLine (bob removes a line from alice's invoice).
TEST_CASE("invoice lease: removing a line needs the lease, returns the line and recomputes the totals",
          "[lims][billing][lease]") {
    lims::test::LabFixture const fixture;
    auto const invoice = draft();
    REQUIRE(invoice.lines.size() == 1);
    auto const line = invoice.lines.front().id;
    lims::InvoiceEditModel aliceEditor;
    lims::InvoiceEditModel bobEditor;
    {
        ScopedPrincipal const alice{"alice"};
        aliceEditor.execute(lims::LeaseInvoice{.invoiceId = invoice.id});
    }
    {
        ScopedPrincipal const bob{"bob"};
        CHECK_THROWS_AS(bobEditor.execute(lims::RemoveInvoiceLine{.line = line}), lims::Conflict);
    }
    ScopedPrincipal const alice{"alice"};
    auto const edited = aliceEditor.execute(lims::RemoveInvoiceLine{.line = line});
    CHECK(edited.lines.empty());
    CHECK(edited.net.hasValue());
    CHECK((*edited.net).numerator == 0);
    CHECK((*edited.gross).numerator == 0);
    auto const lines = lims::InvoiceModel{}.execute(lims::ListBillableLines{});
    REQUIRE(lines.lines.size() == 1);
    CHECK(lines.lines.front().state == lims::BillableLineState::Unbilled);
    CHECK_FALSE(lines.lines.front().invoice.hasValue());
    CHECK_THROWS_AS(aliceEditor.execute(lims::RemoveInvoiceLine{.line = line}), lims::NotFound);
}

TEST_CASE("invoice lease: a reference with a quote is stored as typed, one over 128 bytes is refused",
          "[lims][billing][lease]") {
    lims::test::LabFixture const fixture;
    auto const invoice = draft();
    lims::InvoiceEditModel editor;
    ScopedPrincipal const alice{"alice"};
    editor.execute(lims::LeaseInvoice{.invoiceId = invoice.id});
    CHECK(editor.execute(lims::SetInvoiceReference{.reference = "O'Neil's PO"}).reference == "O'Neil's PO");
    CHECK_THROWS_AS(editor.execute(lims::SetInvoiceReference{.reference = std::string(129, 'x')}),
                    lims::ValidationError);
    CHECK_THROWS_AS(editor.execute(lims::LeaseInvoice{}), lims::ValidationError);
}

// Mutation: remove the draft check from LeaseInvoice (a submitted invoice is leased for editing).
TEST_CASE("invoice lease: only a draft is leased", "[lims][billing][lease]") {
    lims::test::LabFixture const fixture;
    auto const invoice = draft();
    lims::InvoiceEditModel editor;
    {
        ScopedPrincipal const alice{"alice"};
        editor.execute(lims::LeaseInvoice{.invoiceId = invoice.id});
    }
    lims::test::submit(invoice);
    ScopedPrincipal const alice{"alice"};
    CHECK_THROWS_AS(editor.execute(lims::SetInvoiceReference{.reference = "late"}), lims::IllegalTransition);
    CHECK_THROWS_AS(editor.execute(lims::LeaseInvoice{.invoiceId = invoice.id}), lims::IllegalTransition);
}

// Mutation: make the takeover update unconditional (drop its token and expiry conditions), or let the UNIQUE
// violation of the first write escape as a raw SQL error.
TEST_CASE("invoice lease: two editors taking one lease at once give one success and one Conflict",
          "[lims][billing][lease]") {
    lims::test::LabFixture const fixture;
    auto const invoice = draft();
    FaultHookGuard const guard;
    lims::InvoiceEditModel bobEditor;
    ScopedPrincipal const bob{"bob"};

    SECTION("a free invoice: the other editor's row appears first") {
        raceWith("INSERT INTO billing_invoice_leases (invoice_id, holder, token, expires_at) VALUES (" +
                 std::to_string(*invoice.id) + ", 'carl', 'carls-token', 4102444800000)");
        CHECK_THROWS_WITH(bobEditor.execute(lims::LeaseInvoice{.invoiceId = invoice.id}),
                          Catch::Matchers::ContainsSubstring("leased to another editor meanwhile"));
        lims::setFaultHook({});
        CHECK(holderOf(invoice.id) == "carl");
    }
    SECTION("a lapsed lease: the other editor takes it first") {
        {
            Lightweight::SqlStatement statement;
            (void)statement.ExecuteDirect(
                "INSERT INTO billing_invoice_leases (invoice_id, holder, token, expires_at) VALUES (" +
                std::to_string(*invoice.id) + ", 'alice', 'alices-token', 1000)");
        }
        raceWith(
            "UPDATE billing_invoice_leases SET holder = 'carl', token = 'carls-token', expires_at = 4102444800000");
        CHECK_THROWS_WITH(bobEditor.execute(lims::LeaseInvoice{.invoiceId = invoice.id}),
                          Catch::Matchers::ContainsSubstring("leased to another editor meanwhile"));
        lims::setFaultHook({});
        CHECK(holderOf(invoice.id) == "carl");
    }
}

// Mutation: declare ReleaseInvoice with no effect (bob's connection hears nothing when alice lets go).
TEST_CASE("invoice lease: a release reaches a second clerk's connection", "[lims][billing][lease]") {
    lims::test::LabFixture const fixture;
    auto const invoice = draft();
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(kSecret)};
    auto const lease = glz::write_json(lims::LeaseInvoice{.invoiceId = invoice.id}).value_or(std::string{});
    auto const alice = rig.connect();
    auto const bob = rig.connect();
    auto const aliceEditor = alice->registerModel("InvoiceEditModel", as("alice"));
    REQUIRE(alice->execute(aliceEditor, "InvoiceEditModel", "LeaseInvoice", lease, as("alice")).kind == "ok");
    REQUIRE(bob->watch({"Billing_Lease"}, as("bob")).kind == "ok");
    auto const before = bob->changes().size();

    REQUIRE(alice->execute(aliceEditor, "InvoiceEditModel", "ReleaseInvoice", "{}", as("alice")).kind == "ok");
    CHECK(
        ServerConnection::eventually([&] { return bob->changes().size() > before && bob->heardOf("Billing_Lease"); }));
    CHECK(holderOf(invoice.id).empty());
}

// Hypothesis 3. Mutation: remove the release from ~InvoiceEditModel (bob's lease is refused until the timeout).
TEST_CASE("invoice lease: an editor that goes away releases its lease", "[lims][billing][lease]") {
    lims::test::LabFixture const fixture;
    auto const invoice = draft();
    {
        ScopedPrincipal const alice{"alice"};
        lims::InvoiceEditModel editor;
        editor.execute(lims::LeaseInvoice{.invoiceId = invoice.id});
        CHECK(holderOf(invoice.id) == "alice");
    }
    CHECK(holderOf(invoice.id).empty());
}

// Hypothesis 3. Mutation: remove the release from ~InvoiceEditModel (bob's lease is refused until the timeout).
TEST_CASE("Hypothesis 3: the lease lapses when the holder's connection closes", "[lims][billing][lease][h3]") {
    lims::test::LabFixture const fixture;
    auto const invoice = draft();
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(kSecret)};
    auto const lease = glz::write_json(lims::LeaseInvoice{.invoiceId = invoice.id}).value_or(std::string{});
    auto const alice = rig.connect();
    auto const aliceEditor = alice->registerModel("InvoiceEditModel", as("alice"));
    REQUIRE(alice->execute(aliceEditor, "InvoiceEditModel", "LeaseInvoice", lease, as("alice")).kind == "ok");

    auto const bob = rig.connect();
    REQUIRE(bob->watch({"Billing_Lease"}, as("bob")).kind == "ok");
    auto const bobEditor = bob->registerModel("InvoiceEditModel", as("bob"));
    auto const refused = bob->execute(bobEditor, "InvoiceEditModel", "LeaseInvoice", lease, as("bob"));
    CHECK(refused.kind == "err");
    CHECK_THAT(refused.message, Catch::Matchers::ContainsSubstring("being edited by 'alice'"));
    auto const heardBefore = bob->changes().size();

    alice->close();
    // What bob's connection is told of the lapse itself, before bob acts: the lapse runs no action, so the expected
    // outcome is nothing. If this CHECK_FALSE fails, the lapse was told: record what carried it in decision 37.
    bool const toldOfLapse = ServerConnection::eventually([&] { return bob->changes().size() > heardBefore; },
                                                          std::chrono::milliseconds{1000});
    CHECK_FALSE(toldOfLapse);

    bool const lapsed = ServerConnection::eventually(
        [&] { return bob->execute(bobEditor, "InvoiceEditModel", "LeaseInvoice", lease, as("bob")).kind == "ok"; });
    CHECK(lapsed);
    CHECK(holderOf(invoice.id) == "bob");
}
