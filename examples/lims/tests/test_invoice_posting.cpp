// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 1, first instance: posting an invoice is billing's state change and the ledger's entry. Measured two
// ways — the ledger's half dispatched as its own action (two commits in sequence), and the ledger's half sharing
// billing's transaction (one commit) — each under a failure injected between the halves, and each for what it keeps
// of the ledger's authorization, effects and journal.

#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <condition_variable>
#include <core/async/SyncRun.hpp>
#include <cstdint>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <morph/session/session_auth.hpp>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/billing/core/fault_hook.hpp"
#include "lims/billing/core/invoice_math.hpp"
#include "lims/billing/db/billing_store.hpp"
#include "lims/ledger/core/balance.hpp"
#include "testkit/backend_rig.hpp"

using lims::PostingMode;
using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::ServerConnection;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

struct FaultHookGuard {
    FaultHookGuard() = default;
    FaultHookGuard(FaultHookGuard const&) = delete;
    FaultHookGuard& operator=(FaultHookGuard const&) = delete;
    FaultHookGuard(FaultHookGuard&&) = delete;
    FaultHookGuard& operator=(FaultHookGuard&&) = delete;
    ~FaultHookGuard() { lims::setFaultHook({}); }
};

/// @brief An action log that keeps what it is given: whose actions were journaled.
class Recorder final : public morph::journal::IActionLog {
public:
    void append(morph::journal::LogEntry entry) override {
        std::scoped_lock const lock{_mutex};
        _entries.push_back(std::move(entry));
    }
    void flush() override {}
    [[nodiscard]] std::vector<morph::journal::LogEntry> entries(std::string_view entityKey = {}) const override {
        std::scoped_lock const lock{_mutex};
        std::vector<morph::journal::LogEntry> matching;
        for (auto const& entry : _entries) {
            if (entityKey.empty() || entry.entityKey == entityKey) {
                matching.push_back(entry);
            }
        }
        return matching;
    }
    /// @brief Whether @p modelType journaled @p actionType.
    [[nodiscard]] bool holds(std::string_view modelType, std::string_view actionType) const {
        return std::ranges::any_of(entries(), [&](morph::journal::LogEntry const& entry) {
            return entry.modelType == modelType && entry.actionType == actionType;
        });
    }

private:
    mutable std::mutex _mutex;
    std::vector<morph::journal::LogEntry> _entries;
};

/// @brief The seeded company's entries, as acc sees them.
[[nodiscard]] std::vector<lims::JournalEntryView> entries() {
    ScopedPrincipal const acc{"acc"};
    return lims::LedgerModel{}.execute(lims::ListEntries{.company = lims::test::seededCompany()}).entries;
}

/// @brief @p invoice as bea sees it now.
[[nodiscard]] lims::InvoiceView current(lims::InvoiceView const& invoice) {
    ScopedPrincipal const bea{"bea"};
    return lims::InvoiceModel{}.execute(lims::GetInvoice{.invoiceId = invoice.id});
}

/// @brief A session for @p principal, signed with the rig's secret.
[[nodiscard]] morph::session::Context as(std::string const& principal) {
    morph::session::TokenIssuer const issuer{std::string{lims::test::kServiceSecret}, morph::session::hmacSha256};
    return lims::test::signedAs(issuer, principal);
}

[[nodiscard]] Rational exact(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return Rational{Numerator{num}, Denominator{den}, DecimalPlaces{places}};
}

/// @brief Holds each caller until @p parties have arrived, or a second has passed.
class Rendezvous {
public:
    /// @brief Blocks until every party has arrived.
    /// @param parties How many callers to wait for.
    void arrive(int parties) {
        std::unique_lock lock{_mutex};
        ++_arrived;
        _arrivedChanged.notify_all();
        static_cast<void>(
            _arrivedChanged.wait_for(lock, std::chrono::seconds{1}, [&] { return _arrived >= parties; }));
    }

private:
    std::mutex _mutex;
    std::condition_variable _arrivedChanged;
    int _arrived = 0;
};

/// @brief Fails the posting at @p point for as long as it lives.
void failAt(std::string const& point) {
    lims::setFaultHook([point](std::string_view reached, Lightweight::SqlConnection&) {
        if (reached == point) {
            throw std::runtime_error{"server stopped while posting"};
        }
    });
}

}  // namespace

// Mutation: drop the rounding line from postingFor (the USD invoice's company values differ by a cent and the ledger
// refuses the entry).
TEST_CASE("invoice posting: the poster rounds each line half-up and books the difference to the rounding account",
          "[lims][billing][posting]") {
    lims::InvoiceToPost const usd{
        .company = lims::CompanyId{1},
        .companyCurrency = lims::CurrencyCode::EUR,
        .currency = lims::CurrencyCode::USD,
        .rate = exact(92, 100, 6),
        .date = morph::time::Timestamp{lims::test::at(2026, 11, 20)},
        .sourceKey = "billing.invoice:9",
        .sourceReference = "invoice 9",
        .net = exact(1005, 100, 2),
        .tax = exact(191, 100, 2),
        .gross = exact(1196, 100, 2),
        .receivableAccount = "1200",
        .revenueAccount = "4000",
        .taxAccount = "2200",
        .roundingAccount = "6900",
    };
    auto const request = lims::postingFor(usd);
    REQUIRE(request.lines.size() == 4U);
    // 11.96 × 0.92 = 11.0032 → 11.00; 10.05 × 0.92 = 9.246 → 9.25; 1.91 × 0.92 = 1.7572 → 1.76; 11.00 − 11.01 = −0.01.
    CHECK(*request.lines.at(0).companyAmount == exact(1100, 100, 2));
    CHECK(*request.lines.at(1).companyAmount == exact(925, 100, 2));
    CHECK(*request.lines.at(2).companyAmount == exact(176, 100, 2));
    CHECK(request.lines.at(3).accountCode == "6900");
    CHECK(request.lines.at(3).side == lims::PostingSide::Debit);
    CHECK((*request.lines.at(3).amount).isZero());
    CHECK(*request.lines.at(3).companyAmount == exact(1, 100, 2));
    CHECK_FALSE(
        lims::entryProblem(request.lines, lims::CurrencyCode::USD, lims::CurrencyCode::EUR, request.rate).has_value());

    auto same = usd;
    same.currency = lims::CurrencyCode::EUR;
    same.rate = exact(1, 1, 6);
    CHECK(lims::postingFor(same).lines.size() == 3U);
}

// Mutation: round the company values to the transaction currency's decimals (the yen values keep cents, and the
// entry is refused for them).
TEST_CASE("invoice posting: a EUR invoice posted into a JPY company rounds to whole yen and books the yen difference",
          "[lims][billing][posting]") {
    lims::InvoiceToPost const eur{
        .company = lims::CompanyId{2},
        .companyCurrency = lims::CurrencyCode::JPY,
        .currency = lims::CurrencyCode::EUR,
        .rate = exact(15001, 100, 6),
        .date = morph::time::Timestamp{lims::test::at(2026, 11, 20)},
        .sourceKey = "billing.invoice:11",
        .sourceReference = "invoice 11",
        .net = exact(1005, 100, 2),
        .tax = exact(191, 100, 2),
        .gross = exact(1196, 100, 2),
        .receivableAccount = "1200",
        .revenueAccount = "4000",
        .taxAccount = "2200",
        .roundingAccount = "6900",
    };
    auto const request = lims::postingFor(eur);
    REQUIRE(request.lines.size() == 4U);
    // 11.96 × 150.01 = 1794.1196 → 1794; 10.05 × 150.01 = 1507.6005 → 1508; 1.91 × 150.01 = 286.5191 → 287;
    // 1794 − 1795 = −1 yen, booked to the rounding account on the debit side, with no euro amount.
    CHECK(*request.lines.at(0).companyAmount == exact(1794, 1, 0));
    CHECK(*request.lines.at(1).companyAmount == exact(1508, 1, 0));
    CHECK(*request.lines.at(2).companyAmount == exact(287, 1, 0));
    CHECK(request.lines.at(3).accountCode == "6900");
    CHECK(request.lines.at(3).side == lims::PostingSide::Debit);
    CHECK((*request.lines.at(3).amount).isZero());
    CHECK(*request.lines.at(3).companyAmount == exact(1, 1, 0));
    CHECK_FALSE(
        lims::entryProblem(request.lines, lims::CurrencyCode::EUR, lims::CurrencyCode::JPY, request.rate).has_value());
}

// Mutation: skip the series update in assignNumberAndPost (the second invoice is numbered 000001 too).
TEST_CASE("invoice posting: an approved invoice gets the next number of its company's fiscal year, and its entry",
          "[lims][billing][posting]") {
    auto const mode = GENERATE(PostingMode::Dispatched, PostingMode::SharedTransaction);
    CAPTURE(mode);
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig posting{mode};
    auto const first = lims::test::approvedInvoice("WW-1");
    auto const second = lims::test::approvedInvoice("WW-2");
    auto const posted = posting.post(first);
    CHECK(posted.state == lims::InvoiceState::Posted);
    CHECK(posted.number == "INV-2026-000001");
    CHECK(posted.postedBy == "bea");
    REQUIRE(entries().size() == 1U);
    CHECK(entries().at(0).id == posted.entry);
    CHECK(entries().at(0).sourceKey == "billing.invoice:" + std::to_string(*first.id));
    CHECK(posting.post(second).number == "INV-2026-000002");
    CHECK_THROWS_AS(posting.post(first), std::exception);
}

// Hypothesis 1, two commits in sequence. Mutation: assign the number before the ledger's half (the failure leaves
// INV-2026-000001 assigned to an invoice with no entry, and the retry's number check fails).
TEST_CASE(
    "Hypothesis 1: dispatched, a failure between the ledger's commit and billing's leaves an entry for an "
    "unposted invoice, and no hole",
    "[lims][billing][posting][h1]") {
    auto const point = GENERATE(std::string{"post.afterLedger"}, std::string{"post.beforeCommit"});
    CAPTURE(point);
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig posting{PostingMode::Dispatched};
    auto const invoice = lims::test::approvedInvoice();
    FaultHookGuard const guard;
    failAt(point);
    CHECK_THROWS_WITH(posting.post(invoice), Catch::Matchers::ContainsSubstring("server stopped"));

    // The partial state: the ledger holds the entry, billing does not hold the posting.
    REQUIRE(entries().size() == 1U);
    CHECK(entries().at(0).sourceKey == "billing.invoice:" + std::to_string(*invoice.id));
    CHECK(current(invoice).state == lims::InvoiceState::Approved);
    CHECK(current(invoice).number.empty());
    CHECK_FALSE(current(invoice).entry.hasValue());

    // No hole: the number was never assigned. A retry converges on the one entry.
    lims::setFaultHook({});
    auto const retried = posting.post(invoice);
    CHECK(retried.number == "INV-2026-000001");
    REQUIRE(entries().size() == 1U);
    CHECK(retried.entry == entries().at(0).id);
}

// Hypothesis 1, one shared transaction. Mutation: commit billing's mapper before the fault point (the entry survives
// the failure).
TEST_CASE("Hypothesis 1: in one shared transaction, a failure between the halves leaves no partial state and no hole",
          "[lims][billing][posting][h1]") {
    auto const point = GENERATE(std::string{"post.afterLedger"}, std::string{"post.beforeCommit"});
    CAPTURE(point);
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig posting{PostingMode::SharedTransaction};
    auto const invoice = lims::test::approvedInvoice();
    FaultHookGuard const guard;
    failAt(point);
    CHECK_THROWS_WITH(posting.post(invoice), Catch::Matchers::ContainsSubstring("server stopped"));
    CHECK(entries().empty());
    CHECK(current(invoice).state == lims::InvoiceState::Approved);
    CHECK(current(invoice).number.empty());

    lims::setFaultHook({});
    auto const retried = posting.post(invoice);
    CHECK(retried.number == "INV-2026-000001");
    REQUIRE(entries().size() == 1U);
    CHECK(retried.entry == entries().at(0).id);
}

// What each way keeps of the ledger's own action: its authorization, its effect on a watching connection, its journal
// entry. The control is billing's own effect, which both keep. Mutation: declare RecordPosting with no effect (the
// dispatched case's Ledger_Entry check fails).
TEST_CASE("Hypothesis 1: what the shared transaction gives up of the ledger's action",
          "[lims][billing][posting][h1]") {
    auto const mode = GENERATE(PostingMode::Dispatched, PostingMode::SharedTransaction);
    CAPTURE(mode);
    bool const dispatched = mode == PostingMode::Dispatched;
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();

    // Authorization, first. With no role anywhere the edge's role table refuses before the ledger's rule can run; with
    // the role at another company only, the edge lets the posting through and the ledger's rule at this company
    // refuses it. The refusals cross the dispatch as messages, not types, so each is matched by what it says.
    for (auto const& [role, refusal] :
         {std::pair{lims::test::PostingRole::Withheld, std::string_view{"unauthorized"}},
          std::pair{lims::test::PostingRole::ElsewhereOnly, std::string_view{"does not hold the service role"}}}) {
        lims::test::PostingRig withheld{mode, role};
        auto const invoice = lims::test::approvedInvoice(role == lims::test::PostingRole::Withheld ? "WW-1" : "WW-3");
        if (dispatched) {
            CHECK_THROWS_WITH(withheld.post(invoice), Catch::Matchers::ContainsSubstring(std::string{refusal}));
            CHECK(entries().empty());
        } else {
            CHECK(withheld.post(invoice).state == lims::InvoiceState::Posted);
        }
    }

    // Effects and journal: an accountant watching the ledger, a clerk watching invoices, every action journaled.
    auto const journal = std::make_shared<Recorder>();
    morph::journal::ScopedActionLog const log{journal};
    lims::test::PostingRig posting{mode};
    auto const invoice = lims::test::approvedInvoice("WW-2");
    auto const accountant = posting.rig().connect();
    auto const clerk = posting.rig().connect();
    REQUIRE(accountant->watch({"Ledger_Entry"}, as("acc")).kind == "ok");
    REQUIRE(clerk->watch({"Billing_Invoice"}, as("bea")).kind == "ok");
    static_cast<void>(posting.post(invoice));
    CHECK(ServerConnection::eventually([&] { return clerk->heardOf("Billing_Invoice"); }));
    bool const ledgerHeard = ServerConnection::eventually([&] { return accountant->heardOf("Ledger_Entry"); },
                                                          std::chrono::milliseconds{1500});
    CHECK(ledgerHeard == dispatched);
    CHECK(journal->holds("InvoiceModel", "PostInvoice"));
    CHECK(journal->holds("LedgerModel", "RecordPosting") == dispatched);
}

// Mutation: number by the fiscal year of now() rather than of the entry's date (the retry on 2 January numbers
// INV-2027-000001).
TEST_CASE("invoice posting: a retry numbers the invoice in the fiscal year of the entry it converges on",
          "[lims][billing][posting]") {
    lims::test::LabFixture const fixture;
    lims::test::books();
    {
        ScopedPrincipal const acc{"acc"};
        lims::LedgerModel{}.execute(lims::OpenPeriod{
            .company = lims::test::seededCompany(),
            .month = morph::time::Timestamp{lims::test::at(2026, 12, 1)},
        });
    }
    lims::test::billingSettings();
    lims::test::PostingRig posting{PostingMode::Dispatched};
    lims::InvoiceView invoice;
    {
        ScopedClockOverride const newYearsEve{lims::test::at(2026, 12, 31)};
        invoice = lims::test::approvedInvoice();
        FaultHookGuard const guard;
        failAt("post.afterLedger");
        CHECK_THROWS(posting.post(invoice));
    }
    ScopedClockOverride const january{lims::test::at(2027, 1, 2)};
    auto const retried = posting.post(invoice);
    CHECK(retried.number == "INV-2026-000001");
    CHECK(entries().at(0).date == morph::time::Timestamp{lims::test::at(2026, 12, 31)});
}

// A retry numbers the invoice against the entry the failed attempt recorded, without building the posting again. The
// ledger refuses a rebuilt posting whose accounts or rate differ from the recorded one, so a retry that rebuilt it
// would be refused for good after either change. Mutation: build the posting before looking for the entry (the
// retry is refused as a different posting).
TEST_CASE("invoice posting: a retry converges on the recorded entry after the billing settings or the rates changed",
          "[lims][billing][posting][h1]") {
    enum class Change : std::uint8_t { ReceivableAccount, BackDatedRate };
    auto const change = GENERATE(Change::ReceivableAccount, Change::BackDatedRate);
    CAPTURE(change);
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    auto const rateFrom = [](unsigned day, std::int64_t hundredths) {
        ScopedPrincipal const acc{"acc"};
        lims::LedgerModel{}.execute(lims::SetExchangeRate{
            .company = lims::test::seededCompany(),
            .from = lims::CurrencyCode::USD,
            .to = lims::CurrencyCode::EUR,
            .validFrom = morph::time::Timestamp{lims::test::at(2026, 11, day)},
            .rate = lims::Rate{exact(hundredths, 100, 6)},
        });
    };
    rateFrom(1, 92);
    lims::test::billingSettings();
    lims::test::PostingRig posting{PostingMode::Dispatched};
    auto const invoice = lims::test::approvedInvoice(
        "WW-1", change == Change::BackDatedRate ? lims::CurrencyCode::USD : lims::CurrencyCode::EUR);
    {
        FaultHookGuard const guard;
        failAt("post.afterLedger");
        CHECK_THROWS_WITH(posting.post(invoice), Catch::Matchers::ContainsSubstring("server stopped"));
    }
    REQUIRE(entries().size() == 1U);
    auto const recorded = entries().at(0);

    if (change == Change::ReceivableAccount) {
        {
            ScopedPrincipal const acc{"acc"};
            lims::LedgerModel{}.execute(lims::CreateAccount{.company = lims::test::seededCompany(),
                                                            .code = "1210",
                                                            .name = "Account 1210",
                                                            .type = lims::AccountType::Asset});
        }
        ScopedPrincipal const sam{"sam"};
        auto settings = lims::PriceListModel::settingsOf(lims::test::seededCompany()).value();
        lims::PriceListModel{}.execute(lims::SetBillingSettings{
            .company = lims::test::seededCompany(),
            .receivableAccount = "1210",
            .revenueAccount = settings.revenueAccount,
            .taxAccount = settings.taxAccount,
            .roundingAccount = settings.roundingAccount,
            .taxRate = settings.taxRate,
            .approvalHours = settings.approvalHours,
            .billingDay = settings.billingDay,
        });
    } else {
        rateFrom(10, 95);
    }

    auto const retried = posting.post(invoice);
    CHECK(retried.state == lims::InvoiceState::Posted);
    CHECK(retried.number == "INV-2026-000001");
    CHECK(retried.entry == recorded.id);
    REQUIRE(entries().size() == 1U);
    CHECK(entries().at(0).lines.size() == recorded.lines.size());
    CHECK(entries().at(0).lines.at(0).accountCode == "1200");
}

// A missing grant is not a missing rate: the caller that cannot see the company is refused, not told there is none.
// Mutation: answer an empty rate for an unseen company (the submission fails as "no rate values it").
TEST_CASE("invoice posting: a rate asked by a caller that cannot see the company is refused, not answered empty",
          "[lims][billing][posting]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig const posting{PostingMode::Dispatched, lims::test::PostingRole::ElsewhereOnly};
    CHECK_THROWS_WITH(lims::test::approvedInvoice("WW-1", lims::CurrencyCode::USD),
                      Catch::Matchers::ContainsSubstring("GetRateOn: the caller may not see company"));
}

// Mutation: value a foreign-currency invoice at one in SubmitInvoice (carl's limit is judged against 47.60, not 43.79,
// and the entry's company values are refused).
TEST_CASE("invoice posting: a USD invoice is valued and posted at the ledger's dated rate",
          "[lims][billing][posting]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    {
        ScopedPrincipal const acc{"acc"};
        lims::LedgerModel{}.execute(lims::SetExchangeRate{
            .company = lims::test::seededCompany(),
            .from = lims::CurrencyCode::USD,
            .to = lims::CurrencyCode::EUR,
            .validFrom = morph::time::Timestamp{lims::test::at(2026, 11, 1)},
            .rate = lims::Rate{exact(92, 100, 6)},
        });
    }
    lims::test::billingSettings();
    lims::test::PostingRig posting{PostingMode::Dispatched};
    auto const invoice = lims::test::approvedInvoice("WW-1", lims::CurrencyCode::USD);
    auto const posted = posting.post(invoice);
    REQUIRE(entries().size() == 1U);
    auto const entry = entries().at(0);
    CHECK(entry.id == posted.entry);
    CHECK(entry.currency == lims::CurrencyCode::USD);
    // 47.60 × 0.92 = 43.792 → 43.79; 40.00 × 0.92 = 36.80; 7.60 × 0.92 = 6.992 → 6.99; no rounding line.
    REQUIRE(entry.lines.size() == 3U);
    CHECK(*entry.lines.at(0).companyAmount == exact(4379, 100, 2));
    // The submission valued the gross at the same rate: this is what the approver's limit was judged against.
    auto const row = lims::findInvoiceRecord(invoice.id).value_or(lims::db::InvoiceRecord{});
    CHECK(Rational{Numerator{row.grossCompanyNum.Value().value_or(0)},
                   Denominator{row.grossCompanyDen.Value().value_or(1)}, DecimalPlaces{2}} == exact(4379, 100, 2));
}

// Mutation: skip the empty-rate check in SubmitInvoice (the invoice is valued at nothing and submitted).
TEST_CASE("invoice posting: a foreign-currency invoice is not submitted while the ledger has no rate for it",
          "[lims][billing][posting]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig const posting{PostingMode::Dispatched};
    CHECK_THROWS_WITH(lims::test::approvedInvoice("WW-1", lims::CurrencyCode::USD),
                      Catch::Matchers::ContainsSubstring("no rate values it in EUR"));
}

TEST_CASE("invoice posting: a posting the ledger refuses changes nothing in billing and leaves no hole",
          "[lims][billing][posting]") {
    auto const mode = GENERATE(PostingMode::Dispatched, PostingMode::SharedTransaction);
    CAPTURE(mode);
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig posting{mode};
    auto const invoice = lims::test::approvedInvoice();
    {
        ScopedPrincipal const acc{"acc"};
        lims::LedgerModel ledger;
        auto const november = ledger.execute(lims::ListPeriods{.company = lims::test::seededCompany()}).periods.at(0);
        ledger.execute(lims::BeginClosePeriod{.periodId = november.id});
        ledger.execute(lims::ClosePeriod{.periodId = november.id});
    }
    CHECK_THROWS_WITH(posting.post(invoice), Catch::Matchers::ContainsSubstring("closed"));
    CHECK(entries().empty());
    {
        ScopedPrincipal const acc{"acc"};
        lims::LedgerModel{}.execute(lims::OpenPeriod{
            .company = lims::test::seededCompany(),
            .month = morph::time::Timestamp{lims::test::at(2026, 12, 1)},
        });
    }
    ScopedClockOverride const december{lims::test::at(2026, 12, 2)};
    CHECK(posting.post(invoice).number == "INV-2026-000001");
}

// Mutation: read the series' next number outside the posting's transaction (two concurrent posts take the same
// number, or one fails on the database lock).
TEST_CASE("invoice posting: two clerks posting at once get consecutive numbers",
          "[lims][billing][posting][socket-only]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig posting{PostingMode::Dispatched};
    auto const first = lims::test::approvedInvoice("WW-1");
    auto const second = lims::test::approvedInvoice("WW-2");
    lims::test::grant("bob", lims::LimsRole::BillingClerk);
    posting.rig().bridge(0).setDefaultSession(as("bea"));
    posting.rig().bridge(1).setDefaultSession(as("bob"));
    auto bea = posting.rig().client<lims::InvoiceModel>(0);
    auto bob = posting.rig().client<lims::InvoiceModel>(1);
    // Both postings are held between the ledger's half and billing's until both have arrived, so their numbering
    // transactions overlap instead of running one after the other.
    Rendezvous rendezvous;
    FaultHookGuard const guard;
    lims::setFaultHook([&rendezvous](std::string_view point, Lightweight::SqlConnection&) {
        if (point == "post.afterLedger") {
            rendezvous.arrive(2);
        }
    });
    auto beaPosting = bea.execute(lims::PostInvoice{.invoiceId = first.id});
    auto bobPosting = bob.execute(lims::PostInvoice{.invoiceId = second.id});
    std::set<std::string> const numbers{
        morph::ladder::testkit::awaitQt(std::move(beaPosting)).number,
        morph::ladder::testkit::awaitQt(std::move(bobPosting)).number,
    };
    CHECK(numbers == std::set<std::string>{"INV-2026-000001", "INV-2026-000002"});
}

// Mutation: delete validate() from PostInvoice's handler.
TEST_CASE("invoice posting: the post handler refuses an unnamed invoice on its own", "[lims][billing][posting]") {
    lims::test::LabFixture const fixture;
    ScopedPrincipal const bea{"bea"};
    lims::InvoiceModel invoices;
    CHECK_THROWS_AS(core::async::syncRun(invoices.execute(lims::PostInvoice{})), lims::ValidationError);
}

// Mutation: drop the port completeness check from PostInvoice (an unwired server dereferences an empty function).
TEST_CASE("invoice posting: an approved invoice is not posted while no ledger is wired", "[lims][billing][posting]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    auto const invoice = lims::test::approvedInvoice();
    CHECK_THROWS_WITH(lims::test::dispatchAs<lims::InvoiceModel>("bea", lims::PostInvoice{.invoiceId = invoice.id}),
                      Catch::Matchers::ContainsSubstring("no ledger is wired"));
    CHECK(lims::findInvoiceRecord(invoice.id).value_or(lims::db::InvoiceRecord{}).state.Value() ==
          static_cast<int>(lims::InvoiceState::Approved));
}

// Mutation: post a foreign-currency invoice at rate one when the ledger has no rate on the posting date (the entry
// values 47.60 USD as 47.60 EUR).
TEST_CASE("invoice posting: a foreign-currency invoice is not posted on a date the ledger has no rate for",
          "[lims][billing][posting]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    {
        ScopedPrincipal const acc{"acc"};
        lims::LedgerModel{}.execute(lims::SetExchangeRate{
            .company = lims::test::seededCompany(),
            .from = lims::CurrencyCode::USD,
            .to = lims::CurrencyCode::EUR,
            .validFrom = morph::time::Timestamp{lims::test::at(2026, 11, 1)},
            .rate = lims::Rate{exact(92, 100, 6)},
        });
    }
    lims::test::billingSettings();
    lims::test::PostingRig posting{PostingMode::Dispatched};
    auto const invoice = lims::test::approvedInvoice("WW-1", lims::CurrencyCode::USD);
    // The rate is valid from 1 November; on 20 October none is.
    ScopedClockOverride const october{lims::test::at(2026, 10, 20)};
    CHECK_THROWS_WITH(posting.post(invoice),
                      Catch::Matchers::ContainsSubstring("no USD/EUR rate on the posting date"));
    CHECK(entries().empty());
    ScopedClockOverride const november{lims::test::at(2026, 11, 20)};
    CHECK(posting.post(invoice).state == lims::InvoiceState::Posted);
}

// Mutation: delete the settings lookup of PostInvoice (the entry is built with no accounts).
TEST_CASE("invoice posting: an approved invoice is not posted in a company with no billing settings",
          "[lims][billing][posting]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig posting{PostingMode::Dispatched};
    auto const invoice = lims::test::approvedInvoice();
    {
        Lightweight::SqlStatement statement;
        (void)statement.ExecuteDirect("DELETE FROM billing_settings");
    }
    CHECK_THROWS_WITH(posting.post(invoice), Catch::Matchers::ContainsSubstring("has no billing settings"));
    CHECK(entries().empty());
}

// Mutation: delete the company lookup from PostInvoice (an invoice whose company row is gone is posted with the
// wrong decimals).
TEST_CASE("invoice posting: an approved invoice is not posted when its company row is gone",
          "[lims][billing][posting]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig posting{PostingMode::Dispatched};
    auto const invoice = lims::test::approvedInvoice();
    lims::test::runSql("DELETE FROM org_companies");
    CHECK_THROWS_WITH(posting.post(invoice), Catch::Matchers::ContainsSubstring("does not exist"));
    CHECK(entries().empty());
}

// Mutation: let a busy database answer the posting as a success, or surface as the raw database error; the message
// says whether the ledger's entry is already recorded.
TEST_CASE("invoice posting: a busy database is a Conflict that says what was kept, another failure surfaces as itself",
          "[lims][billing][posting]") {
    auto const mode = GENERATE(PostingMode::Dispatched, PostingMode::SharedTransaction);
    bool const dispatched = mode == PostingMode::Dispatched;
    CAPTURE(mode);
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig posting{mode};
    auto const invoice = lims::test::approvedInvoice();
    SECTION("busy") {
        lims::test::FailingStatements const busy{"billing_number_series", "INSERT",
                                                 std::string{lims::test::kBusyMessage}};
        CHECK_THROWS_WITH(posting.post(invoice), Catch::Matchers::ContainsSubstring(
                                                     dispatched ? "has its entry recorded" : "was not posted"));
        CHECK(current(invoice).state == lims::InvoiceState::Approved);
    }
    SECTION("another failure") {
        lims::test::FailingStatements const failing{"billing_number_series", "INSERT", "injected: disk full"};
        CHECK_THROWS_WITH(posting.post(invoice), Catch::Matchers::ContainsSubstring("injected: disk full"));
        CHECK(current(invoice).state == lims::InvoiceState::Approved);
    }
    CHECK(entries().size() == (dispatched ? 1U : 0U));
}

// Mutation: drop the "exactly one row changed" check from the posting update (an invoice posted meanwhile is numbered
// twice) or from the number series' advance (a series that moved on meanwhile hands out a number twice).
TEST_CASE("invoice posting: a posting whose update changes no row is a Conflict", "[lims][billing][posting]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig posting{PostingMode::SharedTransaction};
    auto const first = lims::test::approvedInvoice("WW-1");
    auto const second = lims::test::approvedInvoice("WW-2");
    SECTION("the invoice was posted meanwhile") {
        lims::test::SkippedStatements const skipped{"billing_invoices", "UPDATE", "NEW.state = 3"};
        CHECK_THROWS_WITH(posting.post(first), Catch::Matchers::ContainsSubstring("was posted meanwhile"));
        CHECK(current(first).state == lims::InvoiceState::Approved);
    }
    SECTION("the number series moved on meanwhile") {
        CHECK(posting.post(first).number == "INV-2026-000001");
        lims::test::SkippedStatements const skipped{"billing_number_series", "UPDATE"};
        CHECK_THROWS_WITH(posting.post(second), Catch::Matchers::ContainsSubstring("moved on meanwhile"));
        CHECK(current(second).state == lims::InvoiceState::Approved);
    }
}

// Mutation: drop the invoice lookup after the posting update (an invoice removed by the update's own transaction is
// numbered from nothing).
TEST_CASE("invoice posting: an invoice removed after the update is refused and nothing is kept",
          "[lims][billing][posting]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{lims::test::at(2026, 11, 20)};
    lims::test::books();
    lims::test::billingSettings();
    lims::test::PostingRig posting{PostingMode::SharedTransaction};
    auto const invoice = lims::test::approvedInvoice();
    {
        lims::test::ScopedTrigger const vanishing{"AFTER UPDATE ON billing_invoices WHEN NEW.state = 3",
                                                  "DELETE FROM billing_invoices WHERE id = NEW.id;"};
        CHECK_THROWS_WITH(posting.post(invoice), Catch::Matchers::ContainsSubstring("does not exist"));
    }
    CHECK(entries().empty());
    CHECK(current(invoice).state == lims::InvoiceState::Approved);
}
