// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 2, first instance: lab's AnalysisPublished reaches billing's consumer through lab's outbox, morph's
// OutboxRelay and an IReplayLedger in billing, and bills each published result once, across redelivery and restart.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <memory>
#include <morph/core/action_guard.hpp>
#include <morph/core/effects.hpp>
#include <morph/journal/outbox.hpp>
#include <morph/session/session_auth.hpp>
#include <span>
#include <stdexcept>
#include <string>

#include "billing_test_support.hpp"
#include "lims/app/event_wiring.hpp"
#include "lims/app/internal_client.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/lab/db/lims_entity.hpp"
#include "lims/lab/events/lab_events.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::Mode;

namespace {

/// @brief The lines billing holds for @p sample, as bea sees them.
/// @param sample The sample.
/// @return The lines.
[[nodiscard]] std::vector<lims::BillableLineView> linesOf(lims::SampleId sample) {
    ScopedPrincipal const bea{"bea"};
    return lims::InvoiceModel{}.execute(lims::ListBillableLines{.company = {}, .client = {}, .sample = sample}).lines;
}

/// @brief Writes an outbox row naming @p eventId straight through the ORM.
/// @param eventId The row's event id.
void insertOutboxRow(std::string const& eventId) {
    Lightweight::DataMapper mapper;
    lims::db::LabOutboxRecord row;
    row.eventId = Lightweight::SqlAnsiString<128>{eventId};
    row.eventType = Lightweight::SqlAnsiString<64>{std::string{lims::kAnalysisPublished}};
    row.entityKey = Lightweight::SqlAnsiString<64>{"999"};
    row.payload = Lightweight::SqlDynamicAnsiString<4096>{"{\"result\":999}"};
    row.principal = Lightweight::SqlAnsiString<64>{"alice"};
    mapper.Create(row);
}

}  // namespace

// Mutation: drop the writeAnalysisPublished call from writeState (no row is written; the size check fails).
TEST_CASE("domain events: publishing a sample writes one outbox row per result, with a stable event id",
          "[lims][events]") {
    lims::test::LabFixture const fixture;
    auto const ready = lims::test::readyToPublish();
    CHECK(lims::drainLabOutbox().empty());
    {
        ScopedPrincipal const alice{"alice"};
        lims::SampleModel bench;
        bench.execute(lims::OpenSample{.sampleId = ready.sample});
        auto const published = bench.execute(lims::PublishSample{});
        auto const rows = lims::drainLabOutbox();
        REQUIRE(rows.size() == 1U);
        CHECK(rows.at(0).actionType == lims::kAnalysisPublished);
        CHECK(rows.at(0).idempotencyKey == "lab.AnalysisPublished:" + std::to_string(*ready.results.at(0)) + ":" +
                                               std::to_string(*published.version));
        CHECK(rows.at(0).principal == "alice");
    }
    // Draining is not consuming: the row stays until it is marked relayed.
    CHECK(lims::drainLabOutbox().size() == 1U);
}

TEST_CASE("domain events: a refused publish writes no outbox row", "[lims][events]") {
    lims::test::LabFixture const fixture;
    auto const lab = lims::test::readyToPublish();
    ScopedPrincipal const alice{"alice"};
    lims::SampleModel bench;
    bench.execute(lims::OpenSample{.sampleId = lab.sample});
    bench.execute(lims::ReturnForRework{.reason = "re-run the nitrate"});
    CHECK_THROWS_AS(::morph::model::executeChecked(bench, lims::PublishSample{}), lims::IllegalTransition);
    CHECK(lims::drainLabOutbox().empty());
}

// Hypothesis 2. Mutation: make lineOfEvent always empty (the redelivery is refused at the unique index and the relay
// throws). Removing the lookup and the unique index together makes the redelivery write a second line.
TEST_CASE("Hypothesis 2: an event bills once, across a relay that dies before marking it and a restart",
          "[lims][events][h2]") {
    lims::test::LabFixture const fixture;
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);

    // A relay that delivers and then dies before it marks the batch: what a crash between the two looks like.
    {
        lims::test::grantService(std::string{lims::app::kEventsPrincipal});
        morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, 0,
                                               lims::test::limsAuthorizer(lims::test::kServiceSecret)};
        lims::app::InternalClient events{rig.server(), lims::test::kServiceSecret,
                                         std::string{lims::app::kEventsPrincipal}};
        morph::journal::OutboxRelay dying;
        dying.drainOutbox = [] { return lims::drainLabOutbox(); };
        dying.markRelayed = [](std::span<morph::journal::LogEntry const> /*rows*/) {
            throw std::runtime_error{"process killed before marking the batch"};
        };
        dying.sink = std::make_shared<lims::app::BillingEventSink>(events);
        CHECK_THROWS_WITH(dying.relay(), Catch::Matchers::ContainsSubstring("process killed"));
    }
    auto const afterCrash = linesOf(lab.sample);
    REQUIRE(afterCrash.size() == 1U);
    CHECK(afterCrash.at(0).state == lims::BillableLineState::Unbilled);
    CHECK(*afterCrash.at(0).amount == morph::math::Rational{40, morph::math::DecimalPlaces{2}});
    CHECK(lims::drainLabOutbox().size() == 1U);

    // The restart: a fresh relay over the same database redelivers the event, and billing recognises it.
    CHECK(lims::test::relayLabEvents() == 1U);
    auto const afterRestart = linesOf(lab.sample);
    REQUIRE(afterRestart.size() == 1U);
    CHECK(afterRestart.at(0).id == afterCrash.at(0).id);
    CHECK(lims::drainLabOutbox().empty());
    CHECK(lims::test::relayLabEvents() == 0U);
}

TEST_CASE("domain events: a line with no price in force is recorded unpriced, not refused", "[lims][events]") {
    lims::test::LabFixture const fixture;
    auto const lab = lims::test::publishedSample();
    CHECK(lims::test::relayLabEvents() == 1U);
    lims::test::grant("bea", lims::LimsRole::BillingClerk);
    auto const lines = linesOf(lab.sample);
    REQUIRE(lines.size() == 1U);
    CHECK(lines.at(0).state == lims::BillableLineState::Unpriced);
    CHECK_FALSE(lines.at(0).amount.hasValue());
}

// What a consumer that refuses an event does to the events after it.
TEST_CASE("Hypothesis 2: an event the consumer refuses holds back every event after it", "[lims][events][h2]") {
    lims::test::LabFixture const fixture;
    insertOutboxRow("lab.AnalysisPublished:999:1");
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    CHECK_THROWS_WITH(lims::test::relayLabEvents(), Catch::Matchers::ContainsSubstring("action failed validation"));
    CHECK(lims::drainLabOutbox().size() == 2U);
    CHECK(linesOf(lab.sample).empty());
}

// Mutation: drop requireServicePrincipal from RecordBillableLine (bea's line is recorded).
TEST_CASE("domain events: a billable line is recorded only by the server's own client", "[lims][events]") {
    lims::test::LabFixture const fixture;
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    lims::test::grantService("bea");  // even holding the role, a person is not the event relay
    morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, 1,
                                           lims::test::limsAuthorizer(lims::test::kServiceSecret)};
    morph::session::TokenIssuer const issuer{std::string{lims::test::kServiceSecret}, morph::session::hmacSha256};
    rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, "bea"));
    auto invoices = rig.client<lims::InvoiceModel>(0);
    CHECK_THROWS_WITH(morph::ladder::testkit::awaitQt(invoices.execute(lims::RecordBillableLine{
                          .eventId = "lab.AnalysisPublished:1:9",
                          .result = lims::ResultId{1},
                          .sample = lab.sample,
                          .analysis = lab.nitrate,
                          .client = lab.client,
                          .site = lims::test::seededSite(),
                          .publishedAt = morph::time::Timestamp{lims::test::at(2026, 11, 10)}})),
                      Catch::Matchers::ContainsSubstring("only the server's own clients"));
    CHECK(linesOf(lab.sample).empty());
}

// Mutation: drop the catch in InvoiceModel::storeLine (the loser of the race surfaces the index's refusal).
TEST_CASE("domain events: two deliveries of one event that race past the lookup bill once", "[lims][events]") {
    lims::test::LabFixture const fixture;
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    lims::test::grantService(std::string{lims::app::kEventsPrincipal});
    lims::RecordBillableLine const event{
        .eventId = "lab.AnalysisPublished:1:9",
        .result = lims::ResultId{1},
        .sample = lab.sample,
        .analysis = lab.nitrate,
        .client = lab.client,
        .site = lims::test::seededSite(),
        .publishedAt = morph::time::Timestamp{lims::test::at(2026, 11, 10)},
    };
    lims::BillableLineView winner;
    lims::BillableLineView loser;
    {
        ScopedPrincipal const events{std::string{lims::app::kEventsPrincipal}};
        loser = lims::InvoiceModel::storeLine(event, lims::test::seededCompany(), [&] {
            winner = lims::InvoiceModel::storeLine(event, lims::test::seededCompany(), {});
        });
    }
    CHECK(loser.id == winner.id);
    CHECK(linesOf(lab.sample).size() == 1U);
}

// Mutation: let InternalClient accept any principal (alice's authority, not the server's, would deliver events).
TEST_CASE("domain events: the relay acts as a service principal that holds the service role, or delivers nothing",
          "[lims][events]") {
    lims::test::LabFixture const fixture;
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    morph::ladder::testkit::BackendRig rig{morph::ladder::testkit::Mode::Socket, 0,
                                           lims::test::limsAuthorizer(lims::test::kServiceSecret)};
    CHECK_THROWS_AS((lims::app::InternalClient{rig.server(), lims::test::kServiceSecret, "alice"}),
                    lims::ValidationError);
    // The events principal holds no role yet: the edge refuses the line, the relay fails and the event stays.
    {
        lims::app::InternalClient events{rig.server(), lims::test::kServiceSecret,
                                         std::string{lims::app::kEventsPrincipal}};
        CHECK_THROWS_WITH(lims::app::relayLabEvents(events), Catch::Matchers::ContainsSubstring("unauthorized"));
    }
    CHECK(lims::drainLabOutbox().size() == 1U);
    CHECK(linesOf(lab.sample).empty());
    // Granted the role, the same relay delivers.
    lims::test::grantService(std::string{lims::app::kEventsPrincipal});
    lims::app::InternalClient events{rig.server(), lims::test::kServiceSecret,
                                     std::string{lims::app::kEventsPrincipal}};
    CHECK(lims::app::relayLabEvents(events) == 1U);
    CHECK(linesOf(lab.sample).size() == 1U);
}

// Mutation: drop the unique index on lims_outbox(event_id) (the second row is accepted).
TEST_CASE("domain events: the outbox refuses a second row with an event id it holds", "[lims][events]") {
    lims::test::LabFixture const fixture;
    insertOutboxRow("lab.AnalysisPublished:7:1");
    CHECK_THROWS(insertOutboxRow("lab.AnalysisPublished:7:1"));
    CHECK_NOTHROW(insertOutboxRow("lab.AnalysisPublished:7:2"));
}

// Mutation: price the line from a list that is not yet in force on the publishing date (the line is Unbilled).
TEST_CASE("domain events: a client registered for billing whose list starts after the publishing date is unpriced",
          "[lims][events]") {
    lims::test::LabFixture const fixture;
    auto const lab = lims::test::publishedSample();
    lims::test::grant("bea", lims::LimsRole::BillingClerk);
    {
        ScopedPrincipal const bea{"bea"};
        lims::PriceListModel prices;
        prices.execute(lims::RegisterBillingClient{
            .client = lab.client,
            .company = lims::test::seededCompany(),
            .name = "Waterworks Ltd",
            .currency = lims::CurrencyCode::EUR,
        });
        auto const list = prices.execute(lims::CreatePriceList{
            .company = lims::test::seededCompany(),
            .name = "future list",
            .currency = lims::CurrencyCode::EUR,
            .validFrom = morph::time::Timestamp{lims::test::at(2099, 1, 1, 0)},
        });
        prices.execute(lims::SetPrice{
            .priceList = list.id,
            .analysis = lab.nitrate,
            .client = {},
            .amount = lims::Amount{morph::math::Rational{40, morph::math::DecimalPlaces{2}}},
        });
    }
    CHECK(lims::test::relayLabEvents() == 1U);
    auto const lines = linesOf(lab.sample);
    REQUIRE(lines.size() == 1U);
    CHECK(lines.at(0).state == lims::BillableLineState::Unpriced);
    CHECK_FALSE(lines.at(0).amount.hasValue());
}

// Mutation: make ListBillableLines ignore its sample filter (the list for sample 9999 shows the line).
TEST_CASE("InvoiceModel over the full backend-mode matrix: a line is recorded once and listed",
          "[lims][billing][matrix]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    lims::test::LabFixture const fixture;
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    lims::test::grantService(std::string{lims::app::kEventsPrincipal});
    morph::session::TokenIssuer const issuer{std::string{lims::test::kServiceSecret}, morph::session::hmacSha256};
    morph::ladder::testkit::BackendRig rig{mode, 1, lims::test::limsAuthorizer(lims::test::kServiceSecret)};
    // The local modes share one bridge among all clients, so the session is set before each caller's calls.
    auto invoices = rig.client<lims::InvoiceModel>(0);
    auto const actAs = [&](std::string principal) {
        rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, std::move(principal)));
    };
    lims::RecordBillableLine const event{
        .eventId = "lab.AnalysisPublished:1:9",
        .result = lims::ResultId{1},
        .sample = lab.sample,
        .analysis = lab.nitrate,
        .client = lab.client,
        .site = lims::test::seededSite(),
        .publishedAt = morph::time::Timestamp{lims::test::at(2026, 11, 10)},
    };
    actAs(std::string{lims::app::kEventsPrincipal});
    auto const first = awaitQt(invoices.execute(event));
    CHECK(first.state == lims::BillableLineState::Unbilled);
    CHECK(*first.amount == morph::math::Rational{40, morph::math::DecimalPlaces{2}});
    CHECK(awaitQt(invoices.execute(event)).id == first.id);
    actAs("bea");
    auto const listed =
        awaitQt(invoices.execute(lims::ListBillableLines{.company = {}, .client = {}, .sample = lab.sample}));
    REQUIRE(listed.lines.size() == 1U);
    CHECK(listed.lines.at(0).id == first.id);
    CHECK(
        awaitQt(invoices.execute(lims::ListBillableLines{.company = {}, .client = {}, .sample = lims::SampleId{9999}}))
            .lines.empty());
}

// Mutation: drop BRIDGE_REGISTER_EFFECTS for RecordBillableLine (the watcher hears nothing of the line).
TEST_CASE("domain events: a line recorded by the relay is fanned out to a connection watching Billing_Line",
          "[lims][events]") {
    lims::test::LabFixture const fixture;
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    lims::test::grantService(std::string{lims::app::kEventsPrincipal});
    morph::ladder::testkit::BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(lims::test::kServiceSecret)};
    morph::session::TokenIssuer const issuer{std::string{lims::test::kServiceSecret}, morph::session::hmacSha256};
    auto watcher = rig.connect();
    REQUIRE(watcher->takesEvents());
    REQUIRE(watcher->watch({"Billing_Line"}, lims::test::signedAs(issuer, "bea")).kind == "ok");
    CHECK_FALSE(watcher->heardOf("Billing_Line"));
    lims::app::InternalClient events{rig.server(), lims::test::kServiceSecret,
                                     std::string{lims::app::kEventsPrincipal}};
    CHECK(lims::app::relayLabEvents(events) == 1U);
    CHECK(morph::ladder::testkit::pumpUntil([&watcher] { return watcher->heardOf("Billing_Line"); }));
    watcher->close();
}

// The lims screens' load check (`lims_bundle_loads_without_warnings`) loads only the documents the screens serve, none
// of which names InvoiceModel, so it cannot see this model's declarations; these are the ones that matter to it.
// Mutation: drop BRIDGE_REGISTER_EFFECTS for RecordBillableLine.
TEST_CASE("InvoiceModel: RecordBillableLine declares that it changes Billing_Line", "[lims][events]") {
    auto const found = morph::effects::effectsOf("RecordBillableLine");
    REQUIRE(found.has_value());
    auto const declared = found.value_or(decltype(found)::value_type{});
    REQUIRE(declared.size() == 1U);
    CHECK(declared.front().kind == morph::effects::Kind::Changes);
    CHECK(declared.front().record == "Billing_Line");
}
