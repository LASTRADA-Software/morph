// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 2, first instance: lab's AnalysisPublished reaches billing's consumer through lab's outbox, morph's
// OutboxRelay and an IReplayLedger in billing, and bills each published result once, across redelivery and restart.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <memory>
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

namespace {

/// @brief The lines billing holds for @p sample, as bea sees them.
/// @param sample The sample.
/// @return The lines.
[[nodiscard]] std::vector<lims::BillableLineView> linesOf(lims::SampleId sample) {
    ScopedPrincipal const bea{"bea"};
    return lims::InvoiceModel{}.execute(lims::ListBillableLines{.company = {}, .client = {}, .sample = sample}).lines;
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
    CHECK_THROWS_AS(bench.execute(lims::PublishSample{}), lims::IllegalTransition);
    CHECK(lims::drainLabOutbox().empty());
}

// Hypothesis 2. Mutation: drop the replay-ledger lookup from InvoiceModel's RecordBillableLine (the redelivered batch
// bills the result twice).
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
    {
        Lightweight::DataMapper mapper;
        lims::db::LabOutboxRecord poison;
        poison.eventId = Lightweight::SqlAnsiString<128>{"lab.AnalysisPublished:999:1"};
        poison.eventType = Lightweight::SqlAnsiString<64>{std::string{lims::kAnalysisPublished}};
        poison.entityKey = Lightweight::SqlAnsiString<64>{"999"};
        poison.payload = Lightweight::SqlDynamicAnsiString<4096>{"{\"result\":999}"};
        poison.principal = Lightweight::SqlAnsiString<64>{"alice"};
        mapper.Create(poison);
    }
    auto const lab = lims::test::publishedSample();
    lims::test::billingFor(lab.client);
    CHECK_THROWS(lims::test::relayLabEvents());
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
        CHECK_THROWS(lims::app::relayLabEvents(events));
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
