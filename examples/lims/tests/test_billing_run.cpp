// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 4, first instance: the billing run starts on its day, advances one batch per scheduled tick so its
// progress is seen, and a run interrupted mid-way resumes on the next tick without drafting any invoice twice. The
// schedule dispatches through an internal client, as the server's does.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <map>
#include <morph/core/action_guard.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "lims/app/billing_scheduler.hpp"
#include "lims/app/internal_client.hpp"
#include "lims/app/service_principals.hpp"
#include "lims/billing/core/fault_hook.hpp"
#include "lims/billing/db/billing_entity.hpp"
#include "lims/billing/models/billing_run_model.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::Mode;
using morph::math::DecimalPlaces;
using morph::math::Rational;

namespace {

/// @brief Clears the fault hook when the case ends, however it ends.
struct FaultHookGuard {
    FaultHookGuard() = default;
    FaultHookGuard(FaultHookGuard const&) = delete;
    FaultHookGuard& operator=(FaultHookGuard const&) = delete;
    FaultHookGuard(FaultHookGuard&&) = delete;
    FaultHookGuard& operator=(FaultHookGuard&&) = delete;
    ~FaultHookGuard() { lims::setFaultHook({}); }
};

/// @brief Billing settings for @p company with billing day 25, set by @p supervisor.
void settingsFor(lims::CompanyId company, std::string const& supervisor = "sam") {
    if (supervisor == "sam") {
        lims::test::grant("sam", lims::LimsRole::Supervisor);
    }
    ScopedPrincipal const sam{supervisor};
    lims::PriceListModel{}.execute(lims::SetBillingSettings{
        .company = company,
        .receivableAccount = "1200",
        .revenueAccount = "4000",
        .taxAccount = "2200",
        .roundingAccount = "6900",
        .taxRate = lims::Rate{Rational{morph::math::Numerator{19}, morph::math::Denominator{100}, DecimalPlaces{6}}},
        .approvalHours = lims::Count{Rational{48, DecimalPlaces{0}}},
        .billingDay = lims::Count{Rational{25, DecimalPlaces{0}}},
    });
}

/// @brief Three clients, each with one published, billable result at the seeded site; billing settings with billing
///        day 25; the schedule principal holding the service role at the seeded company. The lines are relayed.
/// @return The three clients' samples, in the order they were made.
std::vector<lims::test::LabSample> threeClientsToBill() {
    settingsFor(lims::test::seededCompany());
    std::vector<lims::test::LabSample> labs;
    for (std::string const reference : {"A-1", "B-1", "C-1"}) {
        labs.push_back(lims::test::publishedSample(reference));
        if (labs.size() == 1U) {
            // The first call also makes the one price list, which prices every client.
            lims::test::billingFor(labs.back().client);
            continue;
        }
        ScopedPrincipal const bea{"bea"};
        lims::PriceListModel{}.execute(lims::RegisterBillingClient{
            .client = labs.back().client,
            .company = lims::test::seededCompany(),
            .name = "Client " + reference,
            .currency = lims::CurrencyCode::EUR,
        });
    }
    lims::test::relayLabEvents();
    lims::test::grantService(std::string{lims::app::kSchedulePrincipal});
    return labs;
}

/// @brief A server of the case's own and the schedule's client on it, as the server process holds them.
struct ScheduleServer {
    morph::ladder::testkit::BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(lims::test::kServiceSecret)};
    lims::app::InternalClient client{rig.server(), lims::test::kServiceSecret,
                                     std::string{lims::app::kSchedulePrincipal}};
};

/// @brief A batch of @p items.
[[nodiscard]] lims::Count batchOf(std::int64_t items) { return lims::Count{Rational{items, DecimalPlaces{0}}}; }

/// @brief A whole count.
[[nodiscard]] Rational whole(std::int64_t value) { return Rational{value, DecimalPlaces{0}}; }

/// @brief The seeded company's runs, as bea sees them.
[[nodiscard]] std::vector<lims::BillingRunView> runs() {
    ScopedPrincipal const bea{"bea"};
    return lims::BillingRunModel{}.execute(lims::ListBillingRuns{.company = lims::test::seededCompany()}).runs;
}

/// @brief How many invoices each client has.
[[nodiscard]] std::map<std::int64_t, int> invoicesPerClient() {
    ScopedPrincipal const bea{"bea"};
    std::map<std::int64_t, int> counts;
    for (auto const& invoice :
         lims::InvoiceModel{}.execute(lims::ListInvoices{.company = lims::test::seededCompany()}).invoices) {
        ++counts[*invoice.client];
    }
    return counts;
}

/// @brief Each done item's invoice id by client id; empty for an item done with no invoice.
[[nodiscard]] std::map<std::int64_t, std::optional<std::int64_t>> doneItemInvoices() {
    Lightweight::DataMapper mapper;
    std::map<std::int64_t, std::optional<std::int64_t>> result;
    for (auto const& item : mapper.Query<lims::db::BillingRunItemRecord>().All()) {
        if (item.state.Value() == 1) {
            result[item.clientId.Value()] = item.invoiceId.Value();
        }
    }
    return result;
}

/// @brief The client each invoice belongs to, by invoice id.
[[nodiscard]] std::map<std::int64_t, std::int64_t> clientOfInvoice() {
    ScopedPrincipal const bea{"bea"};
    std::map<std::int64_t, std::int64_t> result;
    for (auto const& invoice :
         lims::InvoiceModel{}.execute(lims::ListInvoices{.company = lims::test::seededCompany()}).invoices) {
        result[*invoice.id] = *invoice.client;
    }
    return result;
}

/// @brief Runs @p sql on the fixture's database.
void execute(std::string const& sql) {
    Lightweight::SqlStatement statement;
    static_cast<void>(statement.ExecuteDirect(sql));
}

}  // namespace

// Mutation: let dueCompanies ignore an existing run for the month (the later tick starts a second run, refused with
// Conflict, which the tick reports).
TEST_CASE("Hypothesis 4: the billing run starts on its day of the month, once", "[lims][billing][run][h4]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(threeClientsToBill());
    ScheduleServer server;
    lims::app::BillingScheduler scheduler{server.client, batchOf(50)};
    {
        ScopedClockOverride const clock{lims::test::at(2026, 11, 24)};
        CHECK(scheduler.tick().runs.empty());
        CHECK(runs().empty());
    }
    {
        ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
        static_cast<void>(scheduler.tick());
        REQUIRE(runs().size() == 1U);
        CHECK(runs().front().label == "2026-11");
        CHECK(runs().front().state == lims::BillingRunState::Finished);
        CHECK(runs().front().startedBy == lims::app::kSchedulePrincipal);
    }
    ScopedClockOverride const later{lims::test::at(2026, 11, 25, 15)};
    static_cast<void>(scheduler.tick());
    CHECK(runs().size() == 1U);
    CHECK(invoicesPerClient().size() == 3U);
}

// Mutation: advance every running run to its end in one tick (the first tick answers 3 done, and the watcher hears
// one change instead of three).
TEST_CASE("billing run: each tick advances one batch, and a watching clerk sees the progress change",
          "[lims][billing][run]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(threeClientsToBill());
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    ScheduleServer server;
    morph::session::TokenIssuer const issuer{std::string{lims::test::kServiceSecret}, morph::session::hmacSha256};
    auto const watcher = server.rig.connect();
    REQUIRE(watcher->watch({"Billing_Run"}, lims::test::signedAs(issuer, "bea")).kind == "ok");
    lims::app::BillingScheduler scheduler{server.client, batchOf(1)};
    std::size_t heard = 0;
    for (std::int64_t done = 1; done <= 3; ++done) {
        CAPTURE(done);
        auto const ticked = scheduler.tick();
        REQUIRE(ticked.runs.size() == 1U);
        CHECK(*ticked.runs.front().done == whole(done));
        CHECK(*ticked.runs.front().total == whole(3));
        CHECK(morph::ladder::testkit::ServerConnection::eventually(
            [&] { return watcher->changes().size() > heard && watcher->heardOf("Billing_Run"); }));
        heard = watcher->changes().size();
    }
    REQUIRE(runs().size() == 1U);
    CHECK(runs().front().state == lims::BillingRunState::Finished);
}

// Mutation: leave out the existing-run check in startRun (the second start fails on the unique index instead of with
// Conflict).
TEST_CASE("billing run: a clerk starts and advances a run by hand, item by item", "[lims][billing][run]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(threeClientsToBill());
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    ScopedPrincipal const bea{"bea"};
    lims::BillingRunModel model;
    auto run = model.execute(lims::StartBillingRun{.company = lims::test::seededCompany()});
    CHECK(*run.total == whole(3));
    CHECK(*run.done == whole(0));
    run = model.execute(lims::AdvanceBillingRun{.runId = run.id, .batch = batchOf(1)});
    CHECK(*run.done == whole(1));
    CHECK(run.state == lims::BillingRunState::Running);
    run = model.execute(lims::AdvanceBillingRun{.runId = run.id, .batch = batchOf(5)});
    CHECK(run.state == lims::BillingRunState::Finished);
    CHECK_THROWS_AS(
        ::morph::model::executeChecked(model, lims::StartBillingRun{.company = lims::test::seededCompany()}),
        lims::Conflict);
    CHECK(runs().size() == 1U);
}

// Mutation: drop requireServicePrincipal from RunBillingSchedule (bea's tick starts the run).
TEST_CASE("billing run: only the server's own client runs the schedule", "[lims][billing][run]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(threeClientsToBill());
    lims::test::grantService("bea");
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    ScopedPrincipal const bea{"bea"};
    CHECK_THROWS_WITH(lims::BillingRunModel{}.execute(lims::RunBillingSchedule{.batch = batchOf(50)}),
                      Catch::Matchers::ContainsSubstring("only the server's own clients"));
    CHECK(runs().empty());
}

// Mutation: make serves() answer true for every company (the tick also starts Osaka's run, whose service role the
// schedule principal does not hold).
TEST_CASE("billing run: the schedule acts only for companies where its principal holds the service role",
          "[lims][billing][run]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(threeClientsToBill());
    lims::CompanyId osaka;
    {
        ScopedPrincipal const sam{"sam"};
        osaka = lims::OrgModel{}
                    .execute(lims::CreateCompany{
                        .name = "Osaka branch",
                        .baseCurrency = lims::CurrencyCode::JPY,
                        .fiscalYearStart = lims::Month::April,
                    })
                    .id;
        lims::OrgModel{}.execute(
            lims::AssignRole{.principal = "dave", .role = lims::LimsRole::Supervisor, .company = osaka, .site = {}});
    }
    settingsFor(osaka, "dave");
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    ScheduleServer server;
    lims::app::BillingScheduler scheduler{server.client, batchOf(50)};
    auto const osakaRuns = [&] {
        ScopedPrincipal const dave{"dave"};
        return lims::BillingRunModel{}.execute(lims::ListBillingRuns{.company = osaka}).runs;
    };
    auto const ticked = scheduler.tick();
    REQUIRE(ticked.runs.size() == 1U);
    CHECK(ticked.runs.front().company == lims::test::seededCompany());
    CHECK(osakaRuns().empty());
    {
        ScopedPrincipal const sam{"sam"};
        lims::OrgModel{}.execute(lims::AssignRole{
            .principal = std::string{lims::app::kSchedulePrincipal},
            .role = lims::LimsRole::Service,
            .company = osaka,
            .site = {},
        });
    }
    static_cast<void>(scheduler.tick());
    // Osaka has nothing to bill, so its run is finished as it starts.
    REQUIRE(osakaRuns().size() == 1U);
    CHECK(osakaRuns().front().state == lims::BillingRunState::Finished);
}

// Hypothesis 4. Mutation: draft an item outside the transaction that marks it done (the interrupted item's invoice
// stays committed while the item stays pending, so two clients have an invoice after the failure instead of one).
// Mutation: commit the draft before marking the item done, in a second transaction (the failure at
// run.beforeItemDone leaves the invoice committed and the item pending; the resumed item is then done with no
// invoice, which the invoice-id assertion catches). Mutation: let advanceRun walk every item instead of the pending
// ones (the resume makes three draft attempts instead of two).
TEST_CASE("Hypothesis 4: a run interrupted mid-way resumes and drafts each client's invoice once",
          "[lims][billing][run][h4]") {
    auto const interruptedAt = GENERATE(std::string_view{"run.afterInvoice"}, std::string_view{"run.beforeItemDone"});
    CAPTURE(interruptedAt);
    lims::test::LabFixture const fixture;
    auto const labs = threeClientsToBill();
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    FaultHookGuard const guard;
    int drafted = 0;
    lims::setFaultHook([&drafted, interruptedAt](std::string_view point, Lightweight::SqlConnection&) {
        if (point == interruptedAt && ++drafted == 2) {
            throw std::runtime_error{"server stopped during the billing run"};
        }
    });
    {
        ScheduleServer server;
        lims::app::BillingScheduler scheduler{server.client, batchOf(50)};
        CHECK_THROWS_WITH(scheduler.tick(), Catch::Matchers::ContainsSubstring("server stopped"));
    }
    REQUIRE(runs().size() == 1U);
    CHECK(runs().front().state == lims::BillingRunState::Running);
    CHECK(*runs().front().done == whole(1));
    CHECK(invoicesPerClient().size() == 1U);

    // The restart: a new server and scheduler tick, and the run resumes where it stood. The hook now only counts
    // the items the resume drafts: the done item is not walked again.
    int attempts = 0;
    lims::setFaultHook([&attempts](std::string_view point, Lightweight::SqlConnection&) {
        if (point == "run.beforeItemDone") {
            ++attempts;
        }
    });
    ScheduleServer restarted;
    lims::app::BillingScheduler scheduler{restarted.client, batchOf(50)};
    ScopedClockOverride const later{lims::test::at(2026, 11, 25, 10)};
    static_cast<void>(scheduler.tick());
    CHECK(attempts == 2);
    REQUIRE(runs().size() == 1U);
    CHECK(runs().front().state == lims::BillingRunState::Finished);
    CHECK(*runs().front().done == whole(3));
    auto const perClient = invoicesPerClient();
    CHECK(perClient.size() == 3U);
    for (auto const& [client, count] : perClient) {
        CAPTURE(client);
        CHECK(count == 1);
    }
    // The cursor's own work: every done item names the invoice of its own client.
    auto const items = doneItemInvoices();
    auto const owners = clientOfInvoice();
    REQUIRE(items.size() == 3U);
    for (auto const& lab : labs) {
        CAPTURE(*lab.client);
        REQUIRE(items.contains(*lab.client));
        auto const invoice = items.at(*lab.client);
        REQUIRE(invoice.has_value());
        std::int64_t const invoiceId = invoice.has_value() ? *invoice : 0;
        REQUIRE(owners.contains(invoiceId));
        CHECK(owners.at(invoiceId) == *lab.client);
    }
}

// Mutation: catch only std::bad_alloc in draftItem's refusal handler, so a domain refusal escapes (the first tick
// throws at the poison item and the case fails there).
TEST_CASE("billing run: an item the domain refuses fails alone, is shown on the run, and holds nothing else up",
          "[lims][billing][run]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(threeClientsToBill());
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    lims::CompanyId osaka;
    {
        ScopedPrincipal const sam{"sam"};
        osaka = lims::OrgModel{}.execute(lims::CreateCompany{.name = "Osaka branch"}).id;
        lims::OrgModel{}.execute(lims::AssignRole{
            .principal = std::string{lims::app::kSchedulePrincipal},
            .role = lims::LimsRole::Service,
            .company = osaka,
            .site = {},
        });
    }
    FaultHookGuard const guard;
    int drafted = 0;
    lims::setFaultHook([&drafted](std::string_view point, Lightweight::SqlConnection&) {
        if (point == "run.afterInvoice" && ++drafted == 1) {
            throw lims::ValidationError{"client 1 bills in another currency"};
        }
    });
    ScheduleServer server;
    morph::session::TokenIssuer const issuer{std::string{lims::test::kServiceSecret}, morph::session::hmacSha256};
    auto const watcher = server.rig.connect();
    REQUIRE(watcher->watch({"Billing_Run"}, lims::test::signedAs(issuer, "bea")).kind == "ok");
    lims::app::BillingScheduler scheduler{server.client, batchOf(1)};

    // The first tick starts the seeded company's run and meets the poison item.
    auto const first = scheduler.tick();
    REQUIRE(first.runs.size() == 1U);
    CHECK(*first.runs.front().failed == whole(1));
    CHECK(*first.runs.front().done == whole(0));
    CHECK(first.runs.front().state == lims::BillingRunState::Running);
    CHECK_THAT(first.runs.front().lastError, Catch::Matchers::ContainsSubstring("another currency"));
    CHECK(morph::ladder::testkit::ServerConnection::eventually([&] { return watcher->heardOf("Billing_Run"); }));

    // Osaka has an unfinished run of its own, with no item left to draft.
    execute("INSERT INTO billing_runs (company_id, label, state, started_at, started_by) VALUES (" +
            std::to_string(*osaka) + ", '2026-10', 0, 0, 'system:billing-schedule')");

    // Osaka's run finishes in the tick after the poison item, while the seeded run is still drafting.
    auto const second = scheduler.tick();
    REQUIRE(second.runs.size() == 2U);
    lims::BillingRunView seeded;
    lims::BillingRunView other;
    for (auto const& run : second.runs) {
        (run.company == osaka ? other : seeded) = run;
    }
    CHECK(seeded.state == lims::BillingRunState::Running);
    auto const third = scheduler.tick();
    REQUIRE(third.runs.size() == 1U);
    seeded = third.runs.front();
    CHECK(other.state == lims::BillingRunState::Finished);
    CHECK(seeded.state == lims::BillingRunState::FinishedWithFailures);
    CHECK(*seeded.total == whole(3));
    CHECK(*seeded.done == whole(2));
    CHECK(*seeded.failed == whole(1));
    CHECK_THAT(seeded.lastError, Catch::Matchers::ContainsSubstring("another currency"));
    CHECK(invoicesPerClient().size() == 2U);

    // The failure stays visible to a clerk reading the run afterwards, and the tick no longer advances it.
    REQUIRE(runs().size() == 1U);
    CHECK(runs().front().state == lims::BillingRunState::FinishedWithFailures);
    CHECK(*runs().front().failed == whole(1));
    CHECK(scheduler.tick().runs.empty());
}

// Mutation: leave out the unique-violation mapping in startRun (the start fails with the database's own error
// instead of Conflict).
TEST_CASE("billing run: a start that loses a race on the unique index is a Conflict", "[lims][billing][run]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(threeClientsToBill());
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    // Another start's row appears between this one's lookup and its write. SQLite would make the other writer wait, so
    // the row is written on this start's own connection, and rolls back with the refused start.
    FaultHookGuard const guard;
    lims::setFaultHook([](std::string_view point, Lightweight::SqlConnection& connection) {
        if (point == "run.beforeStartWrite") {
            lims::setFaultHook({});
            Lightweight::SqlStatement statement{connection};
            static_cast<void>(statement.ExecuteDirect(
                "INSERT INTO billing_runs (company_id, label, state, started_at, started_by) VALUES (" +
                std::to_string(*lims::test::seededCompany()) + ", '2026-11', 0, 0, 'racer')"));
        }
    });
    ScopedPrincipal const bea{"bea"};
    CHECK_THROWS_AS(lims::BillingRunModel{}.execute(lims::StartBillingRun{.company = lims::test::seededCompany()}),
                    lims::Conflict);
    CHECK(runs().empty());
}

// Mutation: leave the run of an empty start Running (the run reads Running, 0 of 0).
TEST_CASE("billing run: a run with nothing to draft is finished when it starts", "[lims][billing][run]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(threeClientsToBill());
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    lims::CompanyId osaka;
    {
        ScopedPrincipal const sam{"sam"};
        osaka = lims::OrgModel{}.execute(lims::CreateCompany{.name = "Osaka branch"}).id;
        lims::OrgModel{}.execute(
            lims::AssignRole{.principal = "dave", .role = lims::LimsRole::Supervisor, .company = osaka, .site = {}});
        lims::OrgModel{}.execute(
            lims::AssignRole{.principal = "dave", .role = lims::LimsRole::BillingClerk, .company = osaka, .site = {}});
    }
    settingsFor(osaka, "dave");
    ScopedPrincipal const dave{"dave"};
    auto const run = lims::BillingRunModel{}.execute(lims::StartBillingRun{.company = osaka});
    CHECK(*run.total == whole(0));
    CHECK(run.state == lims::BillingRunState::Finished);
}

// Mutation: drop the `state = pending` condition from the item's update (the second section's trigger no longer
// refuses the claim, and the invoice commits).
TEST_CASE("billing run: an item another draft took stays pending, and the next advance judges it",
          "[lims][billing][run]") {
    lims::test::LabFixture const fixture;
    auto const labs = threeClientsToBill();
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    std::int64_t const last = *labs.back().client;
    ScheduleServer server;
    lims::app::BillingScheduler scheduler{server.client, batchOf(50)};
    SECTION("its lines were invoiced between the read and the claim") {
        execute("CREATE TRIGGER other_draft AFTER INSERT ON billing_invoices WHEN NEW.client_id = " +
                std::to_string(last) +
                " BEGIN UPDATE billing_lines SET state = 2, invoice_id = 999 WHERE client_id = " +
                std::to_string(last) + " AND state = 0; END");
    }
    SECTION("the item itself was finished by another advance") {
        execute("CREATE TRIGGER other_advance AFTER INSERT ON billing_invoices WHEN NEW.client_id = " +
                std::to_string(last) +
                " BEGIN UPDATE billing_run_items SET state = 1 WHERE client_id = " + std::to_string(last) + "; END");
    }
    auto const ticked = scheduler.tick();
    REQUIRE(ticked.runs.size() == 1U);
    CHECK(*ticked.runs.front().done == whole(2));
    CHECK(ticked.runs.front().state == lims::BillingRunState::Running);
    CHECK(invoicesPerClient().size() == 2U);
    CHECK_FALSE(invoicesPerClient().contains(last));

    execute("DROP TRIGGER IF EXISTS other_draft");
    execute("DROP TRIGGER IF EXISTS other_advance");
    auto const resumed = scheduler.tick();
    REQUIRE(resumed.runs.size() == 1U);
    CHECK(resumed.runs.front().state == lims::BillingRunState::Finished);
    CHECK(*resumed.runs.front().done == whole(3));
    CHECK(invoicesPerClient().size() == 3U);
}

// Mutation: leave the item pending when draftInvoice answers no invoice (the run never finishes, 2 of 3).
TEST_CASE("billing run: an item whose lines were invoiced since the run started is done without an invoice",
          "[lims][billing][run]") {
    lims::test::LabFixture const fixture;
    auto const labs = threeClientsToBill();
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    ScopedPrincipal const bea{"bea"};
    lims::BillingRunModel model;
    auto run = model.execute(lims::StartBillingRun{.company = lims::test::seededCompany()});
    execute("UPDATE billing_lines SET state = 2, invoice_id = 999 WHERE client_id = " +
            std::to_string(*labs.front().client));
    run = model.execute(lims::AdvanceBillingRun{.runId = run.id, .batch = batchOf(50)});
    CHECK(run.state == lims::BillingRunState::Finished);
    CHECK(*run.done == whole(3));
    CHECK(invoicesPerClient().size() == 2U);
}

// Mutation: accept a batch of zero (the zero-batch advance returns a run with no progress instead of a refusal).
TEST_CASE("billing run: a batch is a whole number of at least one, and a run must exist and be the caller's",
          "[lims][billing][run]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(threeClientsToBill());
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    ScopedPrincipal const bea{"bea"};
    lims::BillingRunModel model;
    auto const run = model.execute(lims::StartBillingRun{.company = lims::test::seededCompany()});
    CHECK_THROWS_AS(model.execute(lims::AdvanceBillingRun{.runId = run.id, .batch = batchOf(0)}),
                    lims::ValidationError);
    CHECK_THROWS_AS(model.execute(lims::AdvanceBillingRun{.runId = run.id, .batch = batchOf(-3)}),
                    lims::ValidationError);
    CHECK_THROWS_AS(
        model.execute(lims::AdvanceBillingRun{
            .runId = run.id,
            .batch = lims::Count{Rational{morph::math::Numerator{3}, morph::math::Denominator{2}, DecimalPlaces{0}}}}),
        lims::ValidationError);
    CHECK_THROWS_AS(::morph::model::executeChecked(
                        model, lims::AdvanceBillingRun{.runId = lims::BillingRunId{9999}, .batch = batchOf(1)}),
                    lims::Forbidden);
    CHECK_THROWS_AS(model.execute(lims::StartBillingRun{}), lims::ValidationError);
    REQUIRE(runs().size() == 1U);
    CHECK(*runs().front().done == whole(0));
}

// Mutation: let a company without billing settings start a run (the NotFound check fails).
TEST_CASE("billing run: a company without settings cannot start one, and only a clerk there may start or advance",
          "[lims][billing][run][authorization]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(threeClientsToBill());
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    lims::CompanyId osaka;
    {
        ScopedPrincipal const sam{"sam"};
        osaka = lims::OrgModel{}.execute(lims::CreateCompany{.name = "Osaka branch"}).id;
        lims::OrgModel{}.execute(
            lims::AssignRole{.principal = "dave", .role = lims::LimsRole::BillingClerk, .company = osaka, .site = {}});
    }
    {
        ScopedPrincipal const dave{"dave"};
        lims::BillingRunModel daves;
        CHECK_THROWS_AS(::morph::model::executeChecked(daves, lims::StartBillingRun{.company = osaka}),
                        lims::NotFound);
        CHECK_THROWS_AS(
            ::morph::model::executeChecked(daves, lims::StartBillingRun{.company = lims::test::seededCompany()}),
            lims::Forbidden);
    }
    lims::BillingRunId seeded;
    {
        ScopedPrincipal const bea{"bea"};
        seeded = lims::BillingRunModel{}.execute(lims::StartBillingRun{.company = lims::test::seededCompany()}).id;
    }
    ScopedPrincipal const dave{"dave"};
    lims::BillingRunModel daves;
    CHECK_THROWS_AS(
        ::morph::model::executeChecked(daves, lims::AdvanceBillingRun{.runId = seeded, .batch = batchOf(1)}),
        lims::Forbidden);
    CHECK(lims::BillingRunModel{}.execute(lims::ListBillingRuns{.company = lims::test::seededCompany()}).runs.empty());
}

// Mutation: make ListBillingRuns answer no runs (every case that lists runs fails; one dereferences the empty list
// and crashes).
TEST_CASE("BillingRunModel over the full backend-mode matrix: a run is started, advanced, ticked and listed",
          "[lims][billing][matrix]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    lims::test::LabFixture const fixture;
    static_cast<void>(threeClientsToBill());
    ScopedClockOverride const clock{lims::test::at(2026, 11, 25)};
    morph::session::TokenIssuer const issuer{std::string{lims::test::kServiceSecret}, morph::session::hmacSha256};
    morph::ladder::testkit::BackendRig rig{mode, 1, lims::test::limsAuthorizer(lims::test::kServiceSecret)};
    auto client = rig.client<lims::BillingRunModel>(0);
    lims::BillingRunId runId;
    SECTION("a clerk starts, advances and lists") {
        rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "bea"));
        auto const started = awaitQt(client.execute(lims::StartBillingRun{.company = lims::test::seededCompany()}));
        CHECK(*started.total == whole(3));
        CHECK(*started.done == whole(0));
        runId = started.id;
        auto const partway = awaitQt(client.execute(lims::AdvanceBillingRun{.runId = runId, .batch = batchOf(2)}));
        CHECK(*partway.done == whole(2));
        CHECK(partway.state == lims::BillingRunState::Running);
    }
    SECTION("the schedule's tick starts the run and a clerk finishes it") {
        rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, std::string{lims::app::kSchedulePrincipal}));
        auto const ticked = awaitQt(client.execute(lims::RunBillingSchedule{.batch = batchOf(2)})).runs;
        REQUIRE(ticked.size() == 1U);
        CHECK(*ticked.front().done == whole(2));
        runId = ticked.front().id;
        rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "bea"));
        auto const finished = awaitQt(client.execute(lims::AdvanceBillingRun{.runId = runId, .batch = batchOf(5)}));
        CHECK(finished.state == lims::BillingRunState::Finished);
        CHECK(*finished.done == whole(3));
    }
    rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "bea"));
    auto const listed = awaitQt(client.execute(lims::ListBillingRuns{.company = lims::test::seededCompany()})).runs;
    REQUIRE(listed.size() == 1U);
    CHECK(listed.front().id == runId);
    CHECK(listed.front().label == "2026-11");
}

// Mutation: delete validate() from AdvanceBillingRun, RunBillingSchedule or ListBillingRuns.
TEST_CASE("billing run: a request naming no run, batch or company is refused by the handler", "[lims][billing][run]") {
    lims::test::LabFixture const fixture;
    ScopedPrincipal const bea{"bea"};
    lims::BillingRunModel model;
    CHECK_THROWS_AS(model.execute(lims::AdvanceBillingRun{.runId = {}, .batch = batchOf(1)}), lims::ValidationError);
    CHECK_THROWS_AS(model.execute(lims::RunBillingSchedule{}), lims::ValidationError);
    CHECK_THROWS_AS(model.execute(lims::ListBillingRuns{}), lims::ValidationError);
}
