// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 2, second instance: lab's result-captured event reaches inventory through the same outbox and relay as
// its published event reaches billing, routed by type, and consumes reagent from the first-expiring lot once, across a
// relay that dies before marking and a restart. The consumption's cost is posted through the one ledger port in both
// posting modes.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlConnection.hpp>
#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <map>
#include <memory>
#include <morph/journal/outbox.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "billing_test_support.hpp"
#include "clock.hpp"
#include "inventory_test_support.hpp"
#include "lims/app/event_wiring.hpp"
#include "lims/app/internal_client.hpp"
#include "lims/app/service_principals.hpp"
#include "lims/inventory/core/fault_hook.hpp"
#include "lims/inventory/db/inventory_entity.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/lab/events/lab_events.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "testkit/backend_rig.hpp"

using lims::test::at;
using lims::test::litres;
using lims::test::millilitres;
using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::Mode;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

/// @brief The reagent's lots, by number, as kim sees them.
/// @return Each lot's stock on hand, by lot number.
std::map<std::string, Rational> onHand() {
    ScopedPrincipal const kim{"kim"};
    std::map<std::string, Rational> lots;
    for (auto const& lot :
         lims::StockModel{}.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}}).lots) {
        lots.emplace(lot.lotNumber, *lot.onHand);
    }
    return lots;
}

/// @brief @p thousandths / 1000 at six places.
/// @param thousandths The amount.
/// @return It.
Rational milli(std::int64_t thousandths) {
    return Rational{Numerator{thousandths}, Denominator{1000}, DecimalPlaces{6}};
}

/// @brief An amount of the company's currency, at two places.
/// @param cents The amount in hundredths.
/// @return It.
Rational money(std::int64_t cents) { return Rational{Numerator{cents}, Denominator{100}, DecimalPlaces{2}}; }

/// @brief The seeded company's journal entries, as acc sees them.
/// @return The entries.
std::vector<lims::JournalEntryView> entries() {
    ScopedPrincipal const acc{"acc"};
    return lims::LedgerModel{}.execute(lims::ListEntries{.company = lims::test::seededCompany()}).entries;
}

/// @brief The movements of the reagent, as kim sees them.
/// @param item The reagent.
/// @return The movements, by id.
std::vector<lims::StockMovementView> movementsOf(lims::ItemId item) {
    ScopedPrincipal const kim{"kim"};
    return lims::StockModel{}.execute(lims::ListStockMovements{.item = item, .query = {}}).rows;
}

/// @brief The ledger entry each consumed event holds, in event order.
/// @return Each event's entry id; empty for an event whose posting is owed or that had no cost.
std::vector<std::optional<std::int64_t>> consumedEntries() {
    Lightweight::DataMapper mapper;
    std::vector<std::optional<std::int64_t>> held;
    auto rows = mapper.Query<lims::db::ConsumptionEventRecord>().All();
    std::ranges::sort(rows, {}, [](lims::db::ConsumptionEventRecord const& row) { return row.id.Value(); });
    for (auto const& row : rows) {
        held.push_back(row.entryId.Value());
    }
    return held;
}

/// @brief Clears the inventory fault hook when a case ends.
struct FaultHookGuard {
    FaultHookGuard() = default;
    FaultHookGuard(FaultHookGuard const&) = delete;
    FaultHookGuard& operator=(FaultHookGuard const&) = delete;
    FaultHookGuard(FaultHookGuard&&) = delete;
    FaultHookGuard& operator=(FaultHookGuard&&) = delete;
    ~FaultHookGuard() { lims::setInventoryFaultHook({}); }
};

/// @brief Records the reagent's lot of @p number as kim, at @p cents a litre.
/// @param setUp What `stockSetUp` made.
/// @param location Where the lot is kept.
/// @param number The lot number.
/// @param expires When the lot expires.
/// @param quantity How much, in litres.
/// @param cents The cost of a litre, in hundredths.
void lotAt(lims::test::StockSetUp const& setUp, lims::LocationId location, std::string number,
           morph::time::DateTime expires, lims::Measure quantity, std::int64_t cents) {
    ScopedPrincipal const kim{"kim"};
    static_cast<void>(lims::StockModel{}.execute(lims::RecordOpeningLot{
        .item = setUp.reagent,
        .location = location,
        .lotNumber = std::move(number),
        .expiresAt = morph::time::Timestamp{expires},
        .quantity = std::move(quantity),
        .unit = lims::StockUnit::litre,
        .unitCost = lims::Amount{money(cents)},
    }));
}

/// @brief A consumption event of analysis @p analysis at the seeded site, captured at 09:00 on 10 November 2026.
/// @param eventId The event's id.
/// @param analysis The analysis.
/// @return The action.
lims::RecordConsumption deliveryOf(std::string eventId, std::int64_t analysis) {
    return lims::RecordConsumption{.eventId = std::move(eventId),
                                   .result = lims::ResultId{1},
                                   .sample = lims::SampleId{1},
                                   .analysis = lims::AnalysisId{analysis},
                                   .site = lims::test::seededSite(),
                                   .capturedAt = morph::time::Timestamp{at(2026, 11, 10)}};
}

}  // namespace

// Mutation: drop the writeResultCaptured call from applyCapture (no row is written; the size check fails).
TEST_CASE("consumption: a capture writes one result-captured event in its transaction, and a refused capture none",
          "[lims][inventory][events]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{at(2026, 11, 10)};
    auto const lab = lims::test::readyToPublish();
    auto const events = lims::drainLabOutbox();
    REQUIRE(events.size() == 1);
    CHECK(events.front().actionType == lims::kResultCaptured);
    CHECK(events.front().principal == "alice");
    CHECK(
        events.front().idempotencyKey.starts_with("lab.ResultCaptured:" + std::to_string(*lab.results.front()) + ":"));
    lims::ResultCaptured event;
    REQUIRE_FALSE(glz::read<glz::opts{.error_on_unknown_keys = false}>(event, events.front().payload));
    CHECK(event.result == lab.results.front());
    CHECK(event.sample == lab.sample);
    CHECK(event.analysis == lab.nitrate);
    CHECK(event.site == lims::test::seededSite());
    CHECK(event.capturedAt == morph::time::Timestamp{at(2026, 11, 10)});
    {
        // The sample is awaiting verification: a capture is refused, and its event with it.
        ScopedPrincipal const alice{"alice"};
        lims::SampleModel bench;
        bench.execute(lims::OpenSample{.sampleId = lab.sample});
        CHECK_THROWS_AS(bench.execute(lims::CaptureConcentration{
                            .analysisVersionId = lims::AnalysisVersionId{1},
                            .value = lims::Concentration{Rational{Numerator{12}, Denominator{5}, DecimalPlaces{3}}}}),
                        lims::IllegalTransition);
    }
    CHECK(lims::drainLabOutbox().size() == 1);
}

// Mutation: order lots by id instead of expiry (the lot recorded last, expiring first, is consumed last).
TEST_CASE("consumption: reagent is taken from the first-expiring lot, then the next", "[lims][inventory][events]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{at(2026, 11, 10)};
    lims::test::inventoryBooks();
    auto const setUp = lims::test::consumptionSetUp();
    // Recorded after the other two, so it has the highest id and the earliest expiry.
    lotAt(setUp.stock, setUp.stock.coldRoom, "L-SOONEST", at(2026, 12, 15), litres(10), 1250);
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    static_cast<void>(lims::test::readyToPublish("WW-1"));
    CHECK(lims::test::relayLabEvents() == 1);
    // 25 mL: all 10 mL of L-SOONEST, then 15 mL of L-EARLY.
    CHECK(onHand().at("L-SOONEST") == milli(0));
    CHECK(onHand().at("L-EARLY") == milli(25));
    CHECK(onHand().at("L-LATE") == milli(1000));
    static_cast<void>(lims::test::readyToPublish("WW-2"));
    CHECK(lims::test::relayLabEvents() == 1);
    CHECK(onHand().at("L-EARLY") == milli(0));
    CHECK(onHand().at("L-LATE") == milli(1000));
    // L-EARLY and L-SOONEST are empty and are passed over.
    static_cast<void>(lims::test::readyToPublish("WW-3"));
    CHECK(lims::test::relayLabEvents() == 1);
    CHECK(onHand().at("L-LATE") == milli(975));
}

// Mutation: throw ValidationError when the lots cannot cover a recipe (the relay stops at the event, and the second
// event behind it is never consumed).
TEST_CASE("consumption: what the lots cannot cover is a shortfall, not a refusal", "[lims][inventory][events]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{at(2026, 11, 10)};
    lims::test::inventoryBooks();
    auto const setUp = lims::test::consumptionSetUp();
    {
        // Setting the recipe again replaces its quantity.
        ScopedPrincipal const kim{"kim"};
        auto const replaced = lims::StockModel{}.execute(lims::SetRecipe{.company = lims::test::seededCompany(),
                                                                         .analysis = lims::AnalysisId{1},
                                                                         .item = setUp.stock.reagent,
                                                                         .quantity = litres(2000),
                                                                         .unit = lims::StockUnit::litre});
        CHECK(*replaced.quantity == milli(2000));
    }
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    static_cast<void>(lims::test::readyToPublish("WW-1"));
    static_cast<void>(lims::test::readyToPublish("WW-2"));
    CHECK_NOTHROW(lims::test::relayLabEvents());
    CHECK(onHand().at("L-EARLY") == milli(0));
    CHECK(onHand().at("L-LATE") == milli(0));
    auto const moves = movementsOf(setUp.stock.reagent);
    auto const shortfalls = std::ranges::count_if(
        moves, [](lims::StockMovementView const& move) { return move.kind == lims::MovementKind::Shortfall; });
    CHECK(shortfalls == 2);
    // 2 L asked for, 1.040 L on hand: 0.960 L short; the second event found nothing, 25 mL short.
    std::vector<Rational> missing;
    for (auto const& move : moves) {
        if (move.kind == lims::MovementKind::Shortfall) {
            CHECK_FALSE(move.lot.hasValue());
            missing.push_back(*move.quantity);
        }
    }
    REQUIRE(missing.size() == 2);
    CHECK(missing.at(0) == milli(-960));
    CHECK(missing.at(1) == milli(-25));
    // The stock taken is posted: 0.040 L at 12.50 and 1 L at 12.50, to the cent per lot.
    REQUIRE(entries().size() == 1);
    CHECK(*entries().front().lines.front().amount == money(1300));
}

// Hypothesis 2. Mutation: make consumptionOfEvent always empty (the redelivery is refused at the unique index and the
// relay throws).
TEST_CASE("Hypothesis 2: a capture consumes once, across a relay that dies before marking it and a restart",
          "[lims][inventory][events][h2]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{at(2026, 11, 10)};
    lims::test::inventoryBooks();
    static_cast<void>(lims::test::consumptionSetUp());
    lims::test::ReceiptRig const posting{lims::PostingMode::Dispatched};
    static_cast<void>(lims::test::readyToPublish("WW-1"));
    static_cast<void>(lims::test::readyToPublish("WW-2"));
    {
        BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(lims::test::kServiceSecret)};
        lims::app::InternalClient events{rig.server(), lims::test::kServiceSecret,
                                         std::string{lims::app::kEventsPrincipal}};
        morph::journal::OutboxRelay dying;
        dying.drainOutbox = [] { return lims::drainLabOutbox(); };
        dying.markRelayed = [](std::span<morph::journal::LogEntry const>) {
            throw std::runtime_error{"the relay dies before marking"};
        };
        dying.sink = lims::app::labEventRouter(events);
        CHECK_THROWS_WITH(dying.relay(), Catch::Matchers::ContainsSubstring("the relay dies before marking"));
    }
    auto const afterFirst = onHand();
    CHECK(afterFirst.at("L-LATE") == milli(990));
    CHECK(entries().size() == 2);
    CHECK_FALSE(lims::drainLabOutbox().empty());
    // A fresh server and relay over the same database: the restart.
    CHECK(lims::test::relayLabEvents() == 2);
    CHECK(onHand() == afterFirst);
    CHECK(entries().size() == 2);
    CHECK(lims::drainLabOutbox().empty());
}

// Mutation: drop the unique index on inventory_consumed_events (the two racing deliveries both consume: the answer
// holds two movements and L-EARLY 5 mL).
TEST_CASE("consumption: two deliveries of one event that race past the lookup consume once",
          "[lims][inventory][events]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{at(2026, 11, 10)};
    lims::test::inventoryBooks();
    static_cast<void>(lims::test::consumptionSetUp());
    lims::test::ReceiptRig const rig{lims::PostingMode::SharedTransaction};
    auto const delivery = deliveryOf("lab.ResultCaptured:1:2", 1);
    ScopedPrincipal const service{"system:test"};
    lims::ConsumptionView winner;
    auto const loser = lims::StockModel::storeConsumption(delivery, lims::test::seededCompany(), [&] {
        // The winner commits between the loser's lookup and its write.
        winner = lims::StockModel::storeConsumption(delivery, lims::test::seededCompany(), {});
    });
    REQUIRE(loser.movements.size() == 1);
    CHECK(loser.movements.front().id == winner.movements.front().id);
    CHECK(onHand().at("L-EARLY") == milli(15));
    CHECK(entries().size() == 1);
}

// Mutation: drop requireServicePrincipal from RecordConsumption (eve's consumption is recorded).
TEST_CASE("consumption: only the server's own client records a consumption", "[lims][inventory][events]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(lims::test::consumptionSetUp());
    lims::test::grant("eve", lims::LimsRole::Service);
    CHECK_THROWS_WITH(lims::test::dispatchAs<lims::StockModel>("eve", deliveryOf("lab.ResultCaptured:1:2", 1)),
                      Catch::Matchers::ContainsSubstring("only the server's own clients"));
    CHECK(onHand().at("L-EARLY") == milli(40));
    CHECK(consumedEntries().empty());
}

// Mutation: make LabEventRouter skip an event type it has no route for (the unknown event is deleted unconsumed by the
// relay's mark; the check expects a throw).
TEST_CASE("consumption: the router refuses an event type it has no consumer for", "[lims][inventory][events]") {
    lims::app::LabEventRouter router{{}};
    morph::journal::LogEntry entry;
    entry.actionType = "lab.SomethingElse";
    entry.idempotencyKey = "lab.SomethingElse:1:1";
    CHECK_THROWS_WITH(router.append(entry), Catch::Matchers::ContainsSubstring("no consumer"));
}

// Mutation: drop the expiry filter (the stale lot, expiring first, is consumed); drop the site filter (the annex lot
// is consumed).
TEST_CASE("consumption: an expired lot and a lot at another site are not used", "[lims][inventory][events]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{at(2026, 11, 10)};
    lims::test::inventoryBooks();
    auto const setUp = lims::test::consumptionSetUp();
    lotAt(setUp.stock, setUp.stock.coldRoom, "L-STALE", at(2026, 11, 1), litres(1000), 1250);
    lims::SiteId annex;
    {
        ScopedPrincipal const sam{"sam"};
        annex =
            lims::OrgModel{}.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Annex"}).id;
    }
    lims::test::grant("kim", lims::LimsRole::StockKeeper, lims::Scope{annex});
    lims::LocationId annexRoom;
    {
        ScopedPrincipal const kim{"kim"};
        annexRoom = lims::StockModel{}.execute(lims::CreateLocation{.site = annex, .name = "Annex room"}).id;
    }
    lotAt(setUp.stock, annexRoom, "L-ANNEX", at(2026, 12, 1), litres(1000), 1250);
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    auto const view = lims::test::dispatchAs<lims::StockModel>(std::string{lims::app::kEventsPrincipal},
                                                               deliveryOf("lab.ResultCaptured:1:2", 1));
    REQUIRE(view.movements.size() == 1);
    CHECK(*view.movements.front().quantity == milli(-25));
    CHECK(onHand().at("L-EARLY") == milli(15));
    CHECK(onHand().at("L-STALE") == milli(1000));
    CHECK(onHand().at("L-ANNEX") == milli(1000));
    CHECK(*view.shortfall == milli(0));
}

TEST_CASE("consumption: an analysis with no recipe is consumed and moves nothing", "[lims][inventory][events]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(lims::test::consumptionSetUp());
    auto const view = lims::test::dispatchAs<lims::StockModel>(std::string{lims::app::kEventsPrincipal},
                                                               deliveryOf("lab.ResultCaptured:1:2", 99));
    CHECK(view.movements.empty());
    CHECK(onHand().at("L-EARLY") == milli(40));
    CHECK(consumedEntries().size() == 1);
    CHECK_FALSE(consumedEntries().front().has_value());
    // Delivered again, it is the same consumption, still nothing owed.
    CHECK(lims::test::dispatchAs<lims::StockModel>(std::string{lims::app::kEventsPrincipal},
                                                   deliveryOf("lab.ResultCaptured:1:2", 99))
              .movements.empty());
    CHECK(consumedEntries().size() == 1);
}

// Mutation: let a consumption with a cost commit when no ledger is wired (the lots move and the relay fails after the
// commit).
TEST_CASE("consumption: a cost with no ledger wired to post it is refused, and nothing moves",
          "[lims][inventory][events]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(lims::test::consumptionSetUp());
    CHECK_THROWS_WITH(lims::test::dispatchAs<lims::StockModel>(std::string{lims::app::kEventsPrincipal},
                                                               deliveryOf("lab.ResultCaptured:1:2", 1)),
                      Catch::Matchers::ContainsSubstring("no ledger is wired"));
    CHECK(onHand().at("L-EARLY") == milli(40));
    CHECK(consumedEntries().empty());
}

// Mutation: price every lot at the first lot's unit cost (the entry is not 0.61).
TEST_CASE("consumption: its cost is posted once, at each lot's unit cost", "[lims][inventory][events]") {
    auto const mode = GENERATE(lims::PostingMode::Dispatched, lims::PostingMode::SharedTransaction);
    CAPTURE(mode);
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{at(2026, 11, 10)};
    lims::test::inventoryBooks();
    auto const stock = lims::test::stockSetUp();
    // 50 mL: 0.040 L from the first lot at 10.30 = 0.412 -> 0.41, 0.010 L from the second at 20.30 = 0.203 -> 0.20.
    lotAt(stock, stock.coldRoom, "L-EARLY", at(2027, 1, 31), litres(40), 1030);
    lotAt(stock, stock.coldRoom, "L-LATE", at(2027, 6, 30), litres(1000), 2030);
    {
        ScopedPrincipal const kim{"kim"};
        static_cast<void>(lims::StockModel{}.execute(lims::SetRecipe{.company = lims::test::seededCompany(),
                                                                     .analysis = lims::AnalysisId{1},
                                                                     .item = stock.reagent,
                                                                     .quantity = millilitres(50),
                                                                     .unit = lims::StockUnit::millilitre}));
    }
    lims::test::grantService(std::string{lims::app::kEventsPrincipal});
    lims::test::ReceiptRig const rig{mode};
    auto const first = lims::test::dispatchAs<lims::StockModel>(std::string{lims::app::kEventsPrincipal},
                                                                deliveryOf("lab.ResultCaptured:1:2", 1));
    REQUIRE(first.movements.size() == 2);
    REQUIRE(entries().size() == 1);
    auto const entry = entries().front();
    CHECK(entry.sourceKey == "inventory.consumption:lab.ResultCaptured:1:2");
    REQUIRE(entry.lines.size() == 2);
    CHECK(entry.lines.at(0).accountCode == "5100");
    CHECK(entry.lines.at(0).side == lims::PostingSide::Debit);
    CHECK(*entry.lines.at(0).companyAmount == money(61));
    CHECK(entry.lines.at(1).accountCode == "3100");
    CHECK(entry.lines.at(1).side == lims::PostingSide::Credit);
    CHECK(*entry.lines.at(1).companyAmount == money(61));
    REQUIRE(consumedEntries().size() == 1);
    CHECK(consumedEntries().front() == *entry.id);
    // Delivered again, the consumption is the first one and the books hold the one entry.
    CHECK(lims::test::dispatchAs<lims::StockModel>(std::string{lims::app::kEventsPrincipal},
                                                   deliveryOf("lab.ResultCaptured:1:2", 1))
              .movements.size() == 2);
    CHECK(entries().size() == 1);
    CHECK(onHand().at("L-EARLY") == milli(0));
    CHECK(onHand().at("L-LATE") == milli(990));
}

// Mutation: make unpostedCost always empty (no consumption is posted after its commit: the redelivery posts nothing
// and no entry exists).
TEST_CASE("consumption: a dispatched cost posting that failed is posted when the event is redelivered",
          "[lims][inventory][events]") {
    auto const mode = GENERATE(lims::PostingMode::Dispatched, lims::PostingMode::SharedTransaction);
    CAPTURE(mode);
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{at(2026, 11, 10)};
    lims::test::inventoryBooks();
    static_cast<void>(lims::test::consumptionSetUp());
    lims::test::ReceiptRig const rig{mode};
    static_cast<void>(lims::test::readyToPublish("WW-1"));
    FaultHookGuard const guard;
    std::atomic<int> reached{0};
    lims::setInventoryFaultHook([&reached](std::string_view point, Lightweight::SqlConnection&) {
        if (point == "consumption.afterCommit" && reached.fetch_add(1) == 0) {
            throw std::runtime_error{"server stopped between the consumption and its posting"};
        }
    });
    if (mode == lims::PostingMode::Dispatched) {
        CHECK_THROWS_WITH(lims::test::relayLabEvents(),
                          Catch::Matchers::ContainsSubstring("server stopped between the consumption"));
        // The consumption committed; its cost is owed and the outbox still holds the event.
        CHECK(onHand().at("L-EARLY") == milli(15));
        CHECK(entries().empty());
        REQUIRE(consumedEntries().size() == 1);
        CHECK_FALSE(consumedEntries().front().has_value());
        CHECK_FALSE(lims::drainLabOutbox().empty());
    }
    CHECK(lims::test::relayLabEvents() == 1);
    CHECK(reached.load() == (mode == lims::PostingMode::Dispatched ? 2 : 0));
    CHECK(onHand().at("L-EARLY") == milli(15));
    REQUIRE(entries().size() == 1);
    REQUIRE(consumedEntries().size() == 1);
    CHECK(consumedEntries().front() == *entries().front().id);
    CHECK(lims::drainLabOutbox().empty());
}

// Mutation: post a SharedTransaction consumption after its commit, as Dispatched does (the refused posting leaves the
// lots moved).
TEST_CASE("consumption: a posting the ledger refuses leaves the lots moved only when it was posted after the commit",
          "[lims][inventory][events]") {
    auto const mode = GENERATE(lims::PostingMode::Dispatched, lims::PostingMode::SharedTransaction);
    CAPTURE(mode);
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{at(2026, 11, 10)};
    lims::test::books();  // no stock or consumption account in the chart
    static_cast<void>(lims::test::consumptionSetUp());
    lims::test::ReceiptRig const rig{mode};
    static_cast<void>(lims::test::readyToPublish("WW-1"));
    CHECK_THROWS(lims::test::relayLabEvents());
    CHECK_FALSE(lims::drainLabOutbox().empty());
    CHECK(entries().empty());
    if (mode == lims::PostingMode::SharedTransaction) {
        // One transaction: the refused entry took the consumption with it.
        CHECK(onHand().at("L-EARLY") == milli(40));
        CHECK(consumedEntries().empty());
    } else {
        // Two commits: the consumption stands and its posting is owed.
        CHECK(onHand().at("L-EARLY") == milli(15));
        REQUIRE(consumedEntries().size() == 1);
        CHECK_FALSE(consumedEntries().front().has_value());
    }
    {
        ScopedPrincipal const acc{"acc"};
        lims::LedgerModel ledger;
        for (auto const& [code, type] :
             {std::pair{"3100", lims::AccountType::Asset}, std::pair{"5100", lims::AccountType::Expense}}) {
            static_cast<void>(ledger.execute(lims::CreateAccount{
                .company = lims::test::seededCompany(), .code = code, .name = code, .type = type}));
        }
    }
    CHECK(lims::test::relayLabEvents() == 1);
    CHECK(onHand().at("L-EARLY") == milli(15));
    CHECK(entries().size() == 1);
    CHECK(lims::drainLabOutbox().empty());
}

// Mutation: drop the positivity check in SetRecipe (the zero quantity is stored).
TEST_CASE("recipes: a quantity is positive, in the item's dimension, of an item of the company, and listed",
          "[lims][inventory]") {
    lims::test::LabFixture const fixture;
    auto const setUp = lims::test::consumptionSetUp();
    ScopedPrincipal const kim{"kim"};
    lims::StockModel stock;
    auto const set = [&](lims::Measure quantity, lims::StockUnit unit, lims::ItemId item = {}) {
        return stock.execute(lims::SetRecipe{.company = lims::test::seededCompany(),
                                             .analysis = lims::AnalysisId{30},
                                             .item = item.hasValue() ? item : setUp.stock.reagent,
                                             .quantity = std::move(quantity),
                                             .unit = unit});
    };
    CHECK_THROWS_WITH(set(millilitres(0), lims::StockUnit::millilitre),
                      Catch::Matchers::ContainsSubstring("positive"));
    CHECK_THROWS_WITH(set(millilitres(5), lims::StockUnit::kilogram), Catch::Matchers::ContainsSubstring("kg"));
    CHECK_THROWS_WITH(set(millilitres(5), lims::StockUnit::millilitre, lims::ItemId{999}),
                      Catch::Matchers::ContainsSubstring("is not one of"));
    CHECK_THROWS_WITH(stock.execute(lims::SetRecipe{}), Catch::Matchers::ContainsSubstring("are required"));
    CHECK_THROWS_WITH(stock.execute(lims::SetRecipe{.company = lims::CompanyId{999},
                                                    .analysis = lims::AnalysisId{30},
                                                    .item = setUp.stock.reagent,
                                                    .quantity = millilitres(5),
                                                    .unit = lims::StockUnit::millilitre}),
                      Catch::Matchers::ContainsSubstring("does not exist"));
    auto const converted = set(litres(2), lims::StockUnit::litre);
    CHECK(*converted.quantity == milli(2));
    CHECK(converted.itemCode == "NO3-R");
    CHECK(converted.unit == lims::StockUnit::litre);
    auto const listed = stock.execute(lims::ListRecipes{.company = lims::test::seededCompany()}).recipes;
    REQUIRE(listed.size() == 21);
    CHECK(*listed.front().analysis == 1);
    CHECK(*listed.back().analysis == 30);
    CHECK_THROWS_WITH(stock.execute(lims::ListRecipes{}), Catch::Matchers::ContainsSubstring("company is required"));
    CHECK(stock.execute(lims::ListRecipes{.company = lims::CompanyId{999}}).recipes.empty());
}

// Mutation: drop the settings check in SetRecipe (a recipe is accepted for a company with no accounts to post to).
TEST_CASE("recipes: a company with no inventory settings keeps no recipes", "[lims][inventory]") {
    lims::test::LabFixture const fixture;
    lims::test::grant("kim", lims::LimsRole::StockKeeper);
    ScopedPrincipal const kim{"kim"};
    CHECK_THROWS_WITH(lims::StockModel{}.execute(lims::SetRecipe{.company = lims::test::seededCompany(),
                                                                 .analysis = lims::AnalysisId{1},
                                                                 .item = lims::ItemId{1},
                                                                 .quantity = millilitres(5),
                                                                 .unit = lims::StockUnit::millilitre}),
                      Catch::Matchers::ContainsSubstring("no inventory settings"));
}

TEST_CASE("consumption: a malformed event and an unknown site are refused", "[lims][inventory][events]") {
    lims::test::LabFixture const fixture;
    static_cast<void>(lims::test::consumptionSetUp());
    auto const events = std::string{lims::app::kEventsPrincipal};
    CHECK_THROWS_WITH(lims::test::dispatchAs<lims::StockModel>(events, deliveryOf("", 1)),
                      Catch::Matchers::ContainsSubstring("action failed validation"));
    CHECK_THROWS_WITH(lims::test::dispatchAs<lims::StockModel>(events, deliveryOf(std::string(107, 'x'), 1)),
                      Catch::Matchers::ContainsSubstring("action failed validation"));
    auto unknownSite = deliveryOf("lab.ResultCaptured:1:2", 1);
    unknownSite.site = lims::SiteId{999};
    CHECK_THROWS(lims::test::dispatchAs<lims::StockModel>(events, unknownSite));
    CHECK(consumedEntries().empty());
}

// Mutation: make consumptionOfEvent always empty (the second delivery of the matrix is refused at the unique index).
TEST_CASE("StockModel over the full backend-mode matrix: a recipe is set and listed, an event is consumed once",
          "[lims][inventory][matrix]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{at(2026, 11, 10)};
    lims::test::inventoryBooks();
    auto const setUp = lims::test::consumptionSetUp();
    lims::test::ReceiptRig const posting{lims::PostingMode::Dispatched};
    morph::session::TokenIssuer const issuer{std::string{lims::test::kServiceSecret}, morph::session::hmacSha256};
    BackendRig rig{mode, 1, lims::test::limsAuthorizer(lims::test::kServiceSecret)};
    // The local modes share one bridge among all clients, so the session is set before each caller's calls.
    auto stock = rig.client<lims::StockModel>(0);
    auto const actAs = [&](std::string principal) {
        rig.bridge(0).setDefaultSession(lims::test::signedAs(issuer, std::move(principal)));
    };
    actAs("kim");
    auto const recipe =
        morph::ladder::testkit::awaitQt(stock.execute(lims::SetRecipe{.company = lims::test::seededCompany(),
                                                                      .analysis = lims::AnalysisId{40},
                                                                      .item = setUp.stock.reagent,
                                                                      .quantity = millilitres(10),
                                                                      .unit = lims::StockUnit::millilitre}));
    CHECK(*recipe.quantity == milli(10));
    auto const listed =
        morph::ladder::testkit::awaitQt(stock.execute(lims::ListRecipes{.company = lims::test::seededCompany()}));
    CHECK(listed.recipes.size() == 21);
    actAs(std::string{lims::app::kEventsPrincipal});
    auto const first = morph::ladder::testkit::awaitQt(stock.execute(deliveryOf("lab.ResultCaptured:1:2", 40)));
    REQUIRE(first.movements.size() == 1);
    CHECK(*first.movements.front().quantity == milli(-10));
    auto const again = morph::ladder::testkit::awaitQt(stock.execute(deliveryOf("lab.ResultCaptured:1:2", 40)));
    REQUIRE(again.movements.size() == 1);
    CHECK(again.movements.front().id == first.movements.front().id);
    actAs("kim");
    auto const lots = morph::ladder::testkit::awaitQt(
        stock.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}}));
    CHECK(*lots.lots.front().onHand == milli(30));
    CHECK(entries().size() == 1);
}
