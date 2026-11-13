// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 3, second instance: a bench reserves the lots a sample's analyses will consume; two benches racing for
// the last lot give one reservation and a refusal naming its holder; the reservation lapses after its hour and when
// its holder's connection closes; what a watching connection is told of the lapse is measured.

#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlError.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <functional>
#include <glaze/glaze.hpp>
#include <latch>
#include <map>
#include <morph/session/session_auth.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "clock.hpp"
#include "inventory_test_support.hpp"
#include "lims/app/service_principals.hpp"
#include "lims/inventory/core/fault_hook.hpp"
#include "lims/inventory/models/reservation_model.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/org/core/errors.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::at;
using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::ServerConnection;

namespace {

constexpr std::string_view kSecret = "lims-reservation-test-secret-at-least-32";

/// @brief `consumptionSetUp` with only the early lot (40 mL, enough for one analysis of 25 mL), and two bench
///        analysts at the seeded site, `alice` and `bob`.
void lastLot() {
    static_cast<void>(lims::test::consumptionSetUp(false));
    lims::test::grant("alice", lims::LimsRole::Analyst, lims::Scope{lims::test::seededSite()});
    lims::test::grant("bob", lims::LimsRole::Analyst, lims::Scope{lims::test::seededSite()});
}

/// @brief The reservation for @p sample, nitrate analysis 1, at the seeded site.
/// @param sample The sample.
/// @return The action.
lims::ReserveForSample reserve(std::int64_t sample) {
    return lims::ReserveForSample{
        .sample = lims::SampleId{sample}, .site = lims::test::seededSite(), .analyses = {lims::AnalysisId{1}}};
}

/// @brief Who holds @p sample's reservation, as kim sees it.
/// @param sample The sample.
/// @return The holder; empty when nothing is reserved.
std::string holderOf(std::int64_t sample) {
    ScopedPrincipal const kim{"kim"};
    return lims::StockModel{}.execute(lims::GetSampleReservation{.sample = lims::SampleId{sample}}).holder;
}

/// @brief A session signed with the case's secret.
/// @param principal The principal.
/// @return The session.
morph::session::Context as(std::string const& principal) {
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    return lims::test::signedAs(issuer, principal);
}

}  // namespace

// Mutation: make availableOf ignore every reservation (bob reserves the 40 mL alice holds).
TEST_CASE("reservation: a second bench is told who holds the last lot, and takes it once it lapses",
          "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    ScopedClockOverride const nine{at(2026, 11, 10, 9)};
    lims::ReservationModel aliceBench;
    lims::ReservationModel bobBench;
    {
        ScopedPrincipal const alice{"alice"};
        auto const held = aliceBench.execute(reserve(101));
        CHECK(held.holder == "alice");
        REQUIRE(held.lots.size() == 1);
        CHECK(held.lots.front().lotNumber == "L-EARLY");
    }
    {
        ScopedPrincipal const bob{"bob"};
        CHECK_THROWS_WITH(bobBench.execute(reserve(102)), Catch::Matchers::ContainsSubstring("reserved by 'alice'"));
    }
    CHECK(holderOf(101) == "alice");
    ScopedClockOverride const later{at(2026, 11, 10, 11)};
    ScopedPrincipal const bob{"bob"};
    CHECK(bobBench.execute(reserve(102)).holder == "bob");
}

TEST_CASE("reservation: a bench that goes away releases its reservation", "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    ScopedClockOverride const nine{at(2026, 11, 10, 9)};
    {
        ScopedPrincipal const alice{"alice"};
        lims::ReservationModel bench;
        static_cast<void>(bench.execute(reserve(101)));
        CHECK(holderOf(101) == "alice");
    }
    CHECK(holderOf(101).empty());
}

// Two benches at once give one reservation and one refusal naming its holder, never a raw SQL error.
// Mutation: make bumpVersion report success without writing (both benches reserve the same 40 mL in some round).
TEST_CASE("reservation: two benches racing for the last lot give one reservation and one named refusal",
          "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    ScopedClockOverride const nine{at(2026, 11, 10, 9)};
    for (int round = 0; round < 10; ++round) {
        std::atomic<int> reserved{0};
        std::atomic<int> named{0};
        std::vector<std::string> other;
        std::mutex otherMutex;
        auto const attempt = [&](std::string const& principal, std::int64_t sample) {
            try {
                ScopedPrincipal const who{principal};
                lims::ReservationModel bench;
                static_cast<void>(bench.execute(reserve(sample)));
                ++reserved;
                // The bench goes away at the end of the round, releasing its reservation for the next round.
            } catch (lims::Conflict const& error) {
                if (std::string_view{error.what()}.find("reserved by '") != std::string_view::npos) {
                    ++named;
                } else {
                    std::scoped_lock const lock{otherMutex};
                    other.emplace_back(error.what());
                }
            } catch (std::exception const& error) {
                std::scoped_lock const lock{otherMutex};
                other.emplace_back(error.what());
            }
        };
        std::thread first{attempt, std::string{"alice"}, std::int64_t{201 + (2 * round)}};
        std::thread second{attempt, std::string{"bob"}, std::int64_t{202 + (2 * round)}};
        first.join();
        second.join();
        INFO("round " << round << ": " << (other.empty() ? std::string{} : other.front()));
        CHECK(reserved + named == 2);
        CHECK(reserved >= 1);
        CHECK(other.empty());
    }
}

// Mutation: as above (both benches hold the lot in some round, and the run does not finish).
TEST_CASE("reservation: while the winner's bench lives, the loser is refused naming it",
          "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    ScopedClockOverride const nine{at(2026, 11, 10, 9)};
    for (int round = 0; round < 10; ++round) {
        lims::ReservationModel aliceBench;
        lims::ReservationModel bobBench;
        std::latch start{2};
        std::atomic<int> reserved{0};
        std::atomic<int> named{0};
        auto const attempt = [&](std::string const& principal, lims::ReservationModel& bench, std::int64_t sample) {
            ScopedPrincipal const who{principal};
            start.arrive_and_wait();
            try {
                static_cast<void>(bench.execute(reserve(sample)));
                ++reserved;
            } catch (lims::Conflict const& error) {
                if (std::string_view{error.what()}.find("reserved by '") != std::string_view::npos) {
                    ++named;
                }
            }
        };
        std::thread first{attempt, std::string{"alice"}, std::ref(aliceBench), std::int64_t{301 + (2 * round)}};
        std::thread second{attempt, std::string{"bob"}, std::ref(bobBench), std::int64_t{302 + (2 * round)}};
        first.join();
        second.join();
        INFO("round " << round);
        CHECK(reserved == 1);
        CHECK(named == 1);
        // Both benches go out of scope here and release what they hold before the next round.
    }
}

namespace {

/// @brief The lots of the reagent as kim sees them, by lot number: stock on hand and the part reserved.
/// @return Each lot's on hand and reserved quantity.
std::map<std::string, std::pair<morph::math::Rational, morph::math::Rational>> stockByLot() {
    ScopedPrincipal const kim{"kim"};
    std::map<std::string, std::pair<morph::math::Rational, morph::math::Rational>> lots;
    for (auto const& lot :
         lims::StockModel{}.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}}).lots) {
        lots.emplace(lot.lotNumber, std::pair{*lot.onHand, *lot.reserved});
    }
    return lots;
}

/// @brief @p thousandths / 1000 at six places.
/// @param thousandths The amount.
/// @return It.
morph::math::Rational milli(std::int64_t thousandths) {
    return morph::math::Rational{morph::math::Numerator{thousandths}, morph::math::Denominator{1000},
                                 morph::math::DecimalPlaces{6}};
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

/// @brief A consumption event of analysis 1 for @p sample at the seeded site, captured at 09:00 on 10 November 2026.
/// @param eventId The event's id.
/// @param sample The sample.
/// @return The action.
lims::RecordConsumption deliveryFor(std::string eventId, std::int64_t sample) {
    return lims::RecordConsumption{.eventId = std::move(eventId),
                                   .result = lims::ResultId{1},
                                   .sample = lims::SampleId{sample},
                                   .analysis = lims::AnalysisId{1},
                                   .site = lims::test::seededSite(),
                                   .capturedAt = morph::time::Timestamp{at(2026, 11, 10)}};
}

}  // namespace

TEST_CASE("reservation: a sample's reservation has one holder", "[lims][inventory][reservation]") {
    DbFixture fixture;
    static_cast<void>(lims::test::consumptionSetUp());
    lims::test::grant("alice", lims::LimsRole::Analyst, lims::Scope{lims::test::seededSite()});
    lims::test::grant("bob", lims::LimsRole::Analyst, lims::Scope{lims::test::seededSite()});
    ScopedClockOverride const nine{at(2026, 11, 10, 9)};
    lims::ReservationModel aliceBench;
    lims::ReservationModel bobBench;
    {
        ScopedPrincipal const alice{"alice"};
        static_cast<void>(aliceBench.execute(reserve(101)));
    }
    ScopedPrincipal const bob{"bob"};
    CHECK_THROWS_WITH(bobBench.execute(reserve(101)),
                      Catch::Matchers::ContainsSubstring("sample 101 is reserved by 'alice'"));
    // Another sample takes the late lot, since the early one's 40 mL no longer covers 25 mL after alice's 25.
    auto const other = bobBench.execute(reserve(102));
    REQUIRE(other.lots.size() == 1);
    CHECK(other.lots.front().lotNumber == "L-LATE");
}

// Mutation: leave a reservation's own earlier rows out of the bench's available stock (alice, moving from sample 101
// to 102, is refused by her own hold on the last lot).
TEST_CASE("reservation: a bench that reserves again replaces its earlier reservation",
          "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    ScopedClockOverride const nine{at(2026, 11, 10, 9)};
    lims::ReservationModel bench;
    ScopedPrincipal const alice{"alice"};
    static_cast<void>(bench.execute(reserve(101)));
    CHECK(holderOf(101) == "alice");
    auto const second = bench.execute(reserve(102));
    CHECK(second.holder == "alice");
    CHECK(holderOf(101).empty());
    CHECK(holderOf(102) == "alice");
    CHECK(stockByLot().at("L-EARLY").second == milli(25));
    // The same sample again renews it and keeps one set of rows.
    static_cast<void>(bench.execute(reserve(102)));
    CHECK(stockByLot().at("L-EARLY").second == milli(25));
}

// Mutation: leave the release out of ReleaseReservation (the sample is still held afterwards).
TEST_CASE("reservation: a release frees the sample, and answers who holds it now", "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    ScopedClockOverride const nine{at(2026, 11, 10, 9)};
    lims::ReservationModel aliceBench;
    lims::ReservationModel bobBench;
    {
        ScopedPrincipal const alice{"alice"};
        CHECK(aliceBench.execute(lims::ReleaseReservation{}).holder.empty());
        static_cast<void>(aliceBench.execute(reserve(101)));
        auto const released = aliceBench.execute(lims::ReleaseReservation{});
        CHECK(released.holder.empty());
        CHECK(holderOf(101).empty());
        CHECK(aliceBench.execute(lims::ReleaseReservation{}).holder.empty());
        static_cast<void>(aliceBench.execute(reserve(101)));
    }
    ScopedClockOverride const later{at(2026, 11, 10, 11)};
    {
        // alice's reservation lapsed and bob took the sample; her release leaves bob's alone and says so.
        ScopedPrincipal const bob{"bob"};
        CHECK(bobBench.execute(reserve(101)).holder == "bob");
    }
    ScopedPrincipal const alice{"alice"};
    CHECK(aliceBench.execute(lims::ReleaseReservation{}).holder == "bob");
    CHECK(holderOf(101) == "bob");
}

TEST_CASE("reservation: a malformed request, an unknown site and a caller without the role are refused",
          "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    ScopedClockOverride const nine{at(2026, 11, 10, 9)};
    lims::ReservationModel bench;
    {
        ScopedPrincipal const alice{"alice"};
        auto none = reserve(101);
        none.analyses.clear();
        CHECK_THROWS_AS(bench.execute(none), lims::ValidationError);
        auto unnamed = reserve(101);
        unnamed.analyses = {lims::AnalysisId{}};
        CHECK_THROWS_AS(bench.execute(unnamed), lims::ValidationError);
        auto noSample = reserve(101);
        noSample.sample = {};
        CHECK_THROWS_AS(bench.execute(noSample), lims::ValidationError);
        auto unknownSite = reserve(101);
        unknownSite.site = lims::SiteId{999};
        CHECK_THROWS_AS(bench.execute(unknownSite), lims::NotFound);
    }
    {
        ScopedPrincipal const stranger{"stranger"};
        CHECK_THROWS_AS(bench.execute(reserve(101)), lims::Forbidden);
    }
    {
        // A stock keeper is no analyst.
        ScopedPrincipal const kim{"kim"};
        CHECK_THROWS_AS(bench.execute(reserve(101)), lims::Forbidden);
    }
    CHECK_THROWS_AS(bench.execute(reserve(101)), lims::EmptyPrincipalError);
    CHECK(holderOf(101).empty());
}

// Mutation: leave the unexpired test out of the lots a reservation may use (a lot past its expiry is reserved).
TEST_CASE("reservation: too little stock, an expired lot and an analysis with no recipe",
          "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    ScopedClockOverride const nine{at(2026, 11, 10, 9)};
    lims::ReservationModel bench;
    ScopedPrincipal const alice{"alice"};
    // Two analyses need 50 mL and the only lot holds 40.
    auto two = reserve(101);
    two.analyses = {lims::AnalysisId{1}, lims::AnalysisId{2}};
    CHECK_THROWS_WITH(bench.execute(two), Catch::Matchers::ContainsSubstring("not enough NO3-R at site"));
    // An analysis nobody gave a recipe uses nothing and reserves nothing.
    auto bare = reserve(101);
    bare.analyses = {lims::AnalysisId{99}};
    auto const nothing = bench.execute(bare);
    CHECK(nothing.holder.empty());
    CHECK(nothing.lots.empty());
    ScopedClockOverride const afterExpiry{at(2027, 2, 1)};
    CHECK_THROWS_WITH(bench.execute(reserve(101)), Catch::Matchers::ContainsSubstring("not enough NO3-R at site"));
}

// Mutation: drop the Where on reservation_version from the lot's conditional update (alice reserves the lot bob took
// between her read and her write).
TEST_CASE("reservation: a bench that lost the lot between its read and its write is told who took it",
          "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    ScopedClockOverride const nine{at(2026, 11, 10, 9)};
    FaultHookGuard const guard;
    lims::ReservationModel aliceBench;
    lims::ReservationModel bobBench;
    bool fired = false;
    lims::setInventoryFaultHook([&](std::string_view point, Lightweight::SqlConnection&) {
        if (point != "reservation.beforeVersion" || fired) {
            return;
        }
        fired = true;
        ScopedPrincipal const bob{"bob"};
        static_cast<void>(bobBench.execute(reserve(102)));
    });
    ScopedPrincipal const alice{"alice"};
    CHECK_THROWS_WITH(aliceBench.execute(reserve(101)), Catch::Matchers::ContainsSubstring("reserved by 'bob'"));
    CHECK(fired);
    CHECK(holderOf(101).empty());
    CHECK(holderOf(102) == "bob");
}

// Mutation: leave the busy test out of the failure arm (a busy database surfaces as its raw error).
TEST_CASE("reservation: a busy database is a Conflict, and any other failure surfaces as itself",
          "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    ScopedClockOverride const nine{at(2026, 11, 10, 9)};
    lims::ReservationModel bench;
    ScopedPrincipal const alice{"alice"};
    {
        lims::test::FailingStatements const busy{"inventory_lots", "UPDATE", std::string{lims::test::kBusyMessage}};
        CHECK_THROWS_WITH(bench.execute(reserve(101)), Catch::Matchers::ContainsSubstring("another bench"));
    }
    {
        lims::test::FailingStatements const broken{"inventory_lots", "UPDATE", "disk exploded"};
        CHECK_THROWS_AS(bench.execute(reserve(101)), Lightweight::SqlException);
    }
    CHECK(holderOf(101).empty());
    // Busy, and bob took the lot meanwhile: the refusal names him.
    FaultHookGuard const guard;
    lims::ReservationModel bobBench;
    std::optional<lims::test::FailingStatements> busy;
    bool fired = false;
    lims::setInventoryFaultHook([&](std::string_view point, Lightweight::SqlConnection&) {
        if (point != "reservation.beforeVersion" || fired) {
            return;
        }
        fired = true;
        {
            ScopedPrincipal const bob{"bob"};
            static_cast<void>(bobBench.execute(reserve(102)));
        }
        busy.emplace("inventory_lots", "UPDATE", std::string{lims::test::kBusyMessage});
    });
    CHECK_THROWS_WITH(bench.execute(reserve(101)), Catch::Matchers::ContainsSubstring("reserved by 'bob'"));
    busy.reset();
}

// Mutation: rethrow from the destructor's catch instead of logging (the failure leaves the destructor and ends the
// process).
TEST_CASE("reservation: a release that fails when the bench goes away is logged, not thrown",
          "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    ScopedClockOverride const nine{at(2026, 11, 10, 9)};
    {
        ScopedPrincipal const alice{"alice"};
        // Declared before the bench, so it is still in force when the bench's destructor runs.
        lims::test::FailingStatements const broken{"inventory_reservations", "DELETE", "disk exploded"};
        lims::ReservationModel bench;
        static_cast<void>(bench.execute(reserve(101)));
    }
    // The rows stay, to lapse at their hour.
    CHECK(holderOf(101) == "alice");
}

TEST_CASE("reservation: a sample's reservation is read by those who see its company",
          "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    ScopedClockOverride const nine{at(2026, 11, 10, 9)};
    lims::ReservationModel bench;
    {
        ScopedPrincipal const alice{"alice"};
        static_cast<void>(bench.execute(reserve(101)));
    }
    CHECK(holderOf(101) == "alice");
    CHECK(stockByLot().at("L-EARLY").second == milli(25));
    {
        ScopedPrincipal const stranger{"stranger"};
        CHECK(lims::StockModel{}.execute(lims::GetSampleReservation{.sample = lims::SampleId{101}}).holder.empty());
    }
    ScopedPrincipal const kim{"kim"};
    CHECK_THROWS_AS(lims::StockModel{}.execute(lims::GetSampleReservation{}), lims::ValidationError);
    auto const view = lims::StockModel{}.execute(lims::GetSampleReservation{.sample = lims::SampleId{101}});
    REQUIRE(view.lots.size() == 1);
    CHECK(view.lots.front().itemCode == "NO3-R");
    CHECK(*view.lots.front().quantity == milli(25));
    CHECK(view.lots.front().unit == lims::StockUnit::litre);
    CHECK(view.expiresAt == morph::time::Timestamp{at(2026, 11, 10, 10)});
}

TEST_CASE("reservation: the actions across the backend matrix", "[lims][inventory][reservation][matrix]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    lastLot();
    ScopedClockOverride const nine{at(2026, 11, 10, 9)};
    lims::ReservationModel held;
    {
        ScopedPrincipal const alice{"alice"};
        static_cast<void>(held.execute(reserve(101)));
    }
    auto const read = lims::test::dispatchAs<lims::StockModel>(
        "kim", lims::GetSampleReservation{.sample = lims::SampleId{101}}, mode);
    CHECK(read.holder == "alice");
    REQUIRE(read.lots.size() == 1);
    CHECK(read.lots.front().lotNumber == "L-EARLY");
    auto const refused = [&] {
        try {
            static_cast<void>(lims::test::dispatchAs<lims::ReservationModel>("bob", reserve(101), mode));
        } catch (std::exception const& error) {
            return std::string{error.what()};
        }
        return std::string{};
    }();
    CHECK_THAT(refused, Catch::Matchers::ContainsSubstring("reserved by 'alice'"));
    {
        ScopedPrincipal const alice{"alice"};
        CHECK(held.execute(lims::ReleaseReservation{}).holder.empty());
    }
    auto const reserved = lims::test::dispatchAs<lims::ReservationModel>("bob", reserve(102), mode);
    CHECK(reserved.holder == "bob");
    auto const released = lims::test::dispatchAs<lims::ReservationModel>("bob", lims::ReleaseReservation{}, mode);
    CHECK(released.holder.empty());
}

// A capture takes its own sample's reservation first, and what it takes beyond that never comes out of another
// sample's.
// Mutation: skip the sample's own reservation in takeRecipeLine (the capture takes 15 mL of the early lot bob holds
// and 10 mL of the late one).
TEST_CASE("reservation: a capture consumes its own sample's reservation and not another's",
          "[lims][inventory][reservation]") {
    lims::test::LabFixture const fixture;
    ScopedClockOverride const clock{at(2026, 11, 10)};
    lims::test::inventoryBooks();
    static_cast<void>(lims::test::consumptionSetUp());
    lims::test::grant("bob", lims::LimsRole::Analyst, lims::Scope{lims::test::seededSite()});
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    auto const lab = lims::test::readyToPublish("WW-1");
    // Bob's sample holds the early lot's 25 mL first, so the capture's sample, reserving after him, holds the late
    // lot.
    lims::ReservationModel bobBench;
    lims::ReservationModel aliceBench;
    {
        ScopedPrincipal const bob{"bob"};
        static_cast<void>(bobBench.execute(reserve(*lab.sample + 1000)));
    }
    {
        ScopedPrincipal const alice{"alice"};
        auto const held = aliceBench.execute(
            lims::ReserveForSample{.sample = lab.sample, .site = lims::test::seededSite(), .analyses = {lab.nitrate}});
        REQUIRE(held.lots.size() == 1);
        CHECK(held.lots.front().lotNumber == "L-LATE");
    }
    CHECK(lims::test::relayLabEvents() == 1);
    auto const stock = stockByLot();
    CHECK(stock.at("L-EARLY").first == milli(40));
    CHECK(stock.at("L-EARLY").second == milli(25));
    CHECK(stock.at("L-LATE").first == milli(975));
    CHECK(stock.at("L-LATE").second == milli(0));
    CHECK(holderOf(*lab.sample).empty());
    CHECK(holderOf(*lab.sample + 1000) == "bob");
}

// Mutation: leave the cap at what no other sample holds out of the first-expiring order (a sample with no reservation
// of its own takes the early lot bob holds).
TEST_CASE("reservation: a sample with no reservation takes only what others have not reserved",
          "[lims][inventory][reservation]") {
    DbFixture fixture;
    static_cast<void>(lims::test::consumptionSetUp());
    lims::test::inventoryBooks();
    lims::test::grant("bob", lims::LimsRole::Analyst, lims::Scope{lims::test::seededSite()});
    ScopedClockOverride const clock{at(2026, 11, 10)};
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    lims::ReservationModel bobBench;
    {
        ScopedPrincipal const bob{"bob"};
        static_cast<void>(bobBench.execute(reserve(1001)));
    }
    static_cast<void>(lims::test::dispatchAs<lims::StockModel>(std::string{lims::app::kEventsPrincipal},
                                                               deliveryFor("lab.ResultCaptured:1:1", 7)));
    // 25 mL wanted: the 15 mL of the early lot bob has not reserved, then 10 mL of the late lot.
    auto const stock = stockByLot();
    CHECK(stock.at("L-EARLY").first == milli(25));
    CHECK(stock.at("L-EARLY").second == milli(25));
    CHECK(stock.at("L-LATE").first == milli(990));
}

// Mutation: delete the reservation row whole after a capture takes part of it (the second capture finds no
// reservation, takes 25 mL from the early lot instead of the late one's, and the early lot is short).
TEST_CASE("reservation: a reservation for two analyses shrinks with each capture and is gone after the second",
          "[lims][inventory][reservation]") {
    DbFixture fixture;
    static_cast<void>(lims::test::consumptionSetUp());
    lims::test::grant("alice", lims::LimsRole::Analyst, lims::Scope{lims::test::seededSite()});
    lims::test::inventoryBooks();
    ScopedClockOverride const clock{at(2026, 11, 10)};
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    lims::ReservationModel bench;
    {
        ScopedPrincipal const alice{"alice"};
        auto two = reserve(7);
        two.analyses = {lims::AnalysisId{1}, lims::AnalysisId{2}};
        auto const held = bench.execute(two);
        // Fifty millilitres do not fit the early lot's forty; both go to the late lot.
        REQUIRE(held.lots.size() == 1);
        CHECK(held.lots.front().lotNumber == "L-LATE");
        CHECK(*held.lots.front().quantity == milli(50));
    }
    auto const events = std::string{lims::app::kEventsPrincipal};
    static_cast<void>(lims::test::dispatchAs<lims::StockModel>(events, deliveryFor("lab.ResultCaptured:1:1", 7)));
    auto const middle = [] {
        ScopedPrincipal const kim{"kim"};
        return lims::StockModel{}.execute(lims::GetSampleReservation{.sample = lims::SampleId{7}});
    }();
    REQUIRE(middle.lots.size() == 1);
    CHECK(*middle.lots.front().quantity == milli(25));
    static_cast<void>(lims::test::dispatchAs<lims::StockModel>(events, deliveryFor("lab.ResultCaptured:2:1", 7)));
    CHECK(holderOf(7).empty());
    auto const stock = stockByLot();
    CHECK(stock.at("L-EARLY").first == milli(40));
    CHECK(stock.at("L-LATE").first == milli(950));
}

// Hypothesis 3. Mutation: remove the release from ~ReservationModel (bob's reservation is refused until the hour
// passes).
TEST_CASE("Hypothesis 3: a reservation lapses when its holder's connection closes",
          "[lims][inventory][reservation][h3]") {
    DbFixture fixture;
    lastLot();
    ScopedClockOverride const nine{at(2026, 11, 10, 9)};
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(kSecret)};
    auto const aliceAction = glz::write_json(reserve(101)).value_or(std::string{});
    auto const bobAction = glz::write_json(reserve(102)).value_or(std::string{});
    auto const alice = rig.connect();
    auto const aliceBench = alice->registerModel("ReservationModel", as("alice"));
    REQUIRE(alice->execute(aliceBench, "ReservationModel", "ReserveForSample", aliceAction, as("alice")).kind == "ok");

    auto const bob = rig.connect();
    REQUIRE(bob->watch({"Inventory_Reservation", "Inventory_Lot"}, as("bob")).kind == "ok");
    auto const bobBench = bob->registerModel("ReservationModel", as("bob"));
    auto const refused = bob->execute(bobBench, "ReservationModel", "ReserveForSample", bobAction, as("bob"));
    CHECK(refused.kind == "err");
    CHECK_THAT(refused.message, Catch::Matchers::ContainsSubstring("reserved by 'alice'"));
    auto const heardBefore = bob->changes().size();

    alice->close();
    // What bob's connection is told of the lapse itself, before bob acts: the lapse runs no action, so the expected
    // outcome is nothing. If this CHECK_FALSE fails, the lapse was told: record what carried it in decision 47.
    bool const toldOfLapse = ServerConnection::eventually([&] { return bob->changes().size() > heardBefore; },
                                                          std::chrono::milliseconds{1000});
    CHECK_FALSE(toldOfLapse);

    bool const lapsed = ServerConnection::eventually([&] {
        return bob->execute(bobBench, "ReservationModel", "ReserveForSample", bobAction, as("bob")).kind == "ok";
    });
    CHECK(lapsed);
    CHECK(holderOf(102) == "bob");
}

// Mutation: declare ReleaseReservation with no effect (bob's connection hears nothing when alice lets go).
TEST_CASE("reservation: a release reaches a second bench's connection", "[lims][inventory][reservation]") {
    DbFixture fixture;
    lastLot();
    ScopedClockOverride const nine{at(2026, 11, 10, 9)};
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(kSecret)};
    auto const alice = rig.connect();
    auto const bob = rig.connect();
    auto const aliceBench = alice->registerModel("ReservationModel", as("alice"));
    REQUIRE(alice
                ->execute(aliceBench, "ReservationModel", "ReserveForSample",
                          glz::write_json(reserve(101)).value_or(std::string{}), as("alice"))
                .kind == "ok");
    REQUIRE(bob->watch({"Inventory_Reservation"}, as("bob")).kind == "ok");
    auto const before = bob->changes().size();
    REQUIRE(alice->execute(aliceBench, "ReservationModel", "ReleaseReservation", "{}", as("alice")).kind == "ok");
    CHECK(ServerConnection::eventually(
        [&] { return bob->changes().size() > before && bob->heardOf("Inventory_Reservation"); }));
    CHECK(holderOf(101).empty());
}
