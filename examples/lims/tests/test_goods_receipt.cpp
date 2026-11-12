// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 1, second instance: a goods receipt creates its lots, moves its order and takes the next number of its
// series, and the ledger records its value against goods received not invoiced. Measured in both posting modes under
// a failure injected between the halves.

#include <Lightweight/SqlConnection.hpp>
#include <algorithm>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <morph/core/effects.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "clock.hpp"
#include "inventory_test_support.hpp"
#include "lims/inventory/core/fault_hook.hpp"
#include "lims/inventory/core/stock_math.hpp"
#include "lims/inventory/models/procurement_model.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/ledger/core/balance.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/dto/company_dto.hpp"
#include "lims/posting/ledger_port.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::at;
using lims::test::litres;
using lims::test::millilitres;
using lims::test::ScopedPrincipal;
using morph::ladder::ScopedClockOverride;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::ServerConnection;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

/// @brief Clears the inventory fault hook when a case ends.
struct FaultHookGuard {
    FaultHookGuard() = default;
    FaultHookGuard(FaultHookGuard const&) = delete;
    FaultHookGuard& operator=(FaultHookGuard const&) = delete;
    FaultHookGuard(FaultHookGuard&&) = delete;
    FaultHookGuard& operator=(FaultHookGuard&&) = delete;
    ~FaultHookGuard() { lims::setInventoryFaultHook({}); }
};

/// @brief Fails the receipt at @p point for as long as the hook is installed.
/// @param point The fault point's name.
void failAt(std::string const& point) {
    lims::setInventoryFaultHook([point](std::string_view reached, Lightweight::SqlConnection&) {
        if (reached == point) {
            throw std::runtime_error{"server stopped while receiving"};
        }
    });
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

/// @brief A lot a delivery brings: its number, its quantity and the unit the quantity is in.
using Delivered = std::tuple<std::string, lims::Measure, lims::StockUnit>;

/// @brief A session for @p principal, signed with the rig's secret.
/// @param principal The principal.
/// @return The session.
[[nodiscard]] morph::session::Context as(std::string const& principal) {
    morph::session::TokenIssuer const issuer{std::string{lims::test::kServiceSecret}, morph::session::hmacSha256};
    return lims::test::signedAs(issuer, principal);
}

/// @brief The receipt request for @p order's first line, one lot per element of @p lots.
/// @param order The order.
/// @param room The location.
/// @param when The receipt's date.
/// @param lots The lots.
/// @return The request.
[[nodiscard]] lims::ReceiveGoods deliveryOf(lims::PurchaseOrderView const& order, lims::LocationId room,
                                            morph::time::DateTime when, std::vector<Delivered> const& lots) {
    lims::ReceiveGoods request{.order = order.id, .location = room, .date = morph::time::Timestamp{when}, .lines = {}};
    for (auto const& [number, quantity, unit] : lots) {
        request.lines.push_back(lims::ReceiptLineInput{.orderLine = order.lines.front().id,
                                                       .lotNumber = number,
                                                       .expiresAt = morph::time::Timestamp{at(2027, 5, 31)},
                                                       .quantity = quantity,
                                                       .unit = unit});
    }
    return request;
}

/// @brief Receives @p lots against @p order as kim, dispatched as a client sends it, on 9 November 2026.
/// @param order The order.
/// @param room The location.
/// @param lots The lots.
/// @param mode The backend mode.
/// @return The receipt.
lims::ReceiptView receive(lims::PurchaseOrderView const& order, lims::LocationId room,
                          std::vector<Delivered> const& lots, Mode mode = Mode::Local) {
    ScopedClockOverride const clock{at(2026, 11, 9)};
    return lims::test::dispatchAs<lims::ProcurementModel>("kim", deliveryOf(order, room, at(2026, 11, 9), lots), mode);
}

/// @brief Receives @p order's single line as two lots of 1 L each (the second entered as 1000 mL).
/// @param order The open order.
/// @param room The location.
/// @return The receipt.
lims::ReceiptView receiveTwoLots(lims::PurchaseOrderView const& order, lims::LocationId room) {
    return receive(order, room,
                   {Delivered{"MK-1101", litres(1000), lims::StockUnit::litre},
                    Delivered{"MK-1102", millilitres(1000), lims::StockUnit::millilitre}});
}

/// @brief How many lots the seeded company holds.
/// @return The count.
std::size_t lotCount() {
    ScopedPrincipal const kim{"kim"};
    return lims::StockModel{}
        .execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}})
        .lots.size();
}

/// @brief The seeded company's journal entries, as acc sees them.
/// @return The entries.
std::vector<lims::JournalEntryView> entries() {
    ScopedPrincipal const acc{"acc"};
    return lims::LedgerModel{}.execute(lims::ListEntries{.company = lims::test::seededCompany()}).entries;
}

/// @brief How many journal entries the seeded company's books hold.
/// @return The count.
std::size_t entryCount() { return entries().size(); }

/// @brief The seeded company's receipts, as kim sees them.
/// @return The receipts.
std::vector<lims::ReceiptView> receipts() {
    ScopedPrincipal const kim{"kim"};
    return lims::ProcurementModel{}.execute(lims::ListReceipts{.company = lims::test::seededCompany()}).receipts;
}

/// @brief Another approved requisition for 2 L of the reagent, ordered by paul from the seeded supplier at 85.00 a
///        litre.
/// @param setUp What `stockSetUp` made.
/// @return The open order.
lims::PurchaseOrderView anotherOrder(lims::test::StockSetUp const& setUp) {
    ScopedClockOverride const clock{at(2026, 11, 10, 10)};
    lims::RequisitionView requisition;
    {
        ScopedPrincipal const rita{"rita"};
        lims::ProcurementModel procurement;
        auto const draft = procurement.execute(lims::CreateRequisition{
            .company = lims::test::seededCompany(), .site = lims::test::seededSite(), .reason = "another round"});
        static_cast<void>(procurement.execute(lims::AddRequisitionLine{
            .requisition = draft.id,
            .item = setUp.reagent,
            .quantity = litres(2000),
            .unit = lims::StockUnit::litre,
            .estimate = lims::Amount{Rational{Numerator{18000}, Denominator{100}, DecimalPlaces{2}}}}));
        requisition = procurement.execute(lims::SubmitRequisition{.requisitionId = draft.id});
    }
    {
        ScopedPrincipal const anna{"anna"};
        requisition = lims::ProcurementModel{}.execute(lims::ApproveRequisition{.requisitionId = requisition.id});
    }
    requisition = [&] {
        ScopedPrincipal const rita{"rita"};
        return lims::ProcurementModel{}.execute(lims::GetRequisition{.requisitionId = requisition.id});
    }();
    ScopedPrincipal const paul{"paul"};
    lims::ProcurementModel procurement;
    auto const supplier =
        procurement.execute(lims::ListSuppliers{.company = lims::test::seededCompany()}).suppliers.front();
    return procurement.execute(lims::CreatePurchaseOrder{
        .requisition = requisition.id,
        .supplier = supplier.id,
        .prices = {lims::OrderLinePrice{
            .line = requisition.lines.front().id,
            .unitPrice = lims::Amount{Rational{Numerator{8500}, Denominator{100}, DecimalPlaces{2}}}}}});
}

/// @brief Receives @p lots against @p order as kim over socket client @p index of @p rig.
/// @param rig The rig whose port is installed.
/// @param order The order.
/// @param room The location.
/// @param lots The lots.
/// @param index The socket client.
/// @return The receipt.
lims::ReceiptView receiveOver(lims::test::ReceiptRig& rig, lims::PurchaseOrderView const& order, lims::LocationId room,
                              std::vector<Delivered> const& lots, std::size_t index = 0) {
    ScopedClockOverride const clock{at(2026, 11, 9)};
    rig.rig().bridge(index).setDefaultSession(as("kim"));
    auto keeper = rig.rig().client<lims::ProcurementModel>(index);
    return morph::ladder::testkit::awaitQt(keeper.execute(deliveryOf(order, room, at(2026, 11, 9), lots)));
}

}  // namespace

// Mutation: credit goods received not invoiced with the sum of the rounded line values (the rounding line disappears,
// and the line-count check fails).
TEST_CASE("goods receipt: a CHF receipt posts each line rounded and carries the cent as a rounding line",
          "[lims][inventory][receipt]") {
    // 0.33, 0.33 and 0.34 CHF at 0.95 are 0.3135, 0.3135 and 0.323 EUR, rounded to 0.31, 0.31 and 0.32 (0.94); the
    // total of 1.00 CHF is 0.95 EUR. The cent between them is a debit to the rounding account.
    auto const request = lims::receiptPostingFor(
        lims::ReceiptToPost{.company = lims::test::seededCompany(),
                            .companyCurrency = lims::CurrencyCode::EUR,
                            .currency = lims::CurrencyCode::CHF,
                            .rate = Rational{Numerator{95}, Denominator{100}, DecimalPlaces{6}},
                            .date = morph::time::Timestamp{at(2026, 11, 9)},
                            .sourceKey = "inventory.receipt:1:1",
                            .sourceReference = "receipt",
                            .lineValues = {Rational{Numerator{33}, Denominator{100}, DecimalPlaces{2}},
                                           Rational{Numerator{33}, Denominator{100}, DecimalPlaces{2}},
                                           Rational{Numerator{34}, Denominator{100}, DecimalPlaces{2}}},
                            .stockAccount = "3100",
                            .grniAccount = "1600",
                            .roundingAccount = "6900"});
    REQUIRE(request.lines.size() == 5);  // three stock debits, one GRNI credit, one rounding line
    CHECK(request.lines.at(3).accountCode == "1600");
    CHECK(request.lines.at(3).side == lims::PostingSide::Credit);
    CHECK(*request.lines.at(3).companyAmount == Rational{Numerator{95}, Denominator{100}, DecimalPlaces{2}});
    CHECK(*request.lines.at(2).companyAmount == Rational{Numerator{32}, Denominator{100}, DecimalPlaces{2}});
    CHECK(request.lines.back().accountCode == "6900");
    CHECK(request.lines.back().side == lims::PostingSide::Debit);
    CHECK(*request.lines.back().amount == Rational{0, DecimalPlaces{2}});
    CHECK(*request.lines.back().companyAmount == Rational{Numerator{1}, Denominator{100}, DecimalPlaces{2}});
    CHECK_FALSE(
        lims::entryProblem(request.lines, lims::CurrencyCode::CHF, lims::CurrencyCode::EUR, request.rate).has_value());
}

// Mutation: swap the rounding line's side (a credit-side difference is posted as a debit, and the entry is refused as
// unbalanced).
TEST_CASE("goods receipt: a rounding difference the other way is a credit to the rounding account",
          "[lims][inventory][receipt]") {
    // 0.15 CHF twice at 1.1 is 0.165, rounded up to 0.17 each (0.34); the total of 0.30 CHF is 0.33. The line sum is a
    // cent above the credit, so the cent is a credit to the rounding account.
    auto const request = lims::receiptPostingFor(
        lims::ReceiptToPost{.company = lims::test::seededCompany(),
                            .companyCurrency = lims::CurrencyCode::EUR,
                            .currency = lims::CurrencyCode::CHF,
                            .rate = Rational{Numerator{11}, Denominator{10}, DecimalPlaces{6}},
                            .date = morph::time::Timestamp{at(2026, 11, 9)},
                            .sourceKey = "inventory.receipt:1:1",
                            .sourceReference = "receipt",
                            .lineValues = {Rational{Numerator{15}, Denominator{100}, DecimalPlaces{2}},
                                           Rational{Numerator{15}, Denominator{100}, DecimalPlaces{2}}},
                            .stockAccount = "3100",
                            .grniAccount = "1600",
                            .roundingAccount = "6900"});
    REQUIRE(request.lines.size() == 4);
    CHECK(*request.lines.at(0).companyAmount == Rational{Numerator{17}, Denominator{100}, DecimalPlaces{2}});
    CHECK(*request.lines.at(2).companyAmount == Rational{Numerator{33}, Denominator{100}, DecimalPlaces{2}});
    CHECK(request.lines.back().accountCode == "6900");
    CHECK(request.lines.back().side == lims::PostingSide::Credit);
    CHECK(*request.lines.back().companyAmount == Rational{Numerator{1}, Denominator{100}, DecimalPlaces{2}});
    CHECK_FALSE(
        lims::entryProblem(request.lines, lims::CurrencyCode::CHF, lims::CurrencyCode::EUR, request.rate).has_value());
}

// Mutation: skip the unit conversion of a receipt line (the second lot holds 1000 L).
TEST_CASE("goods receipt: one order line arrives as two lots, one entered in millilitres",
          "[lims][inventory][receipt]") {
    DbFixture fixture;
    lims::test::inventoryBooks();
    auto const opened = lims::test::openOrder(lims::CurrencyCode::EUR);
    auto const& order = opened.order;
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    auto const receipt = receiveTwoLots(order, opened.stock.coldRoom);
    CHECK(receipt.number == "GR-2026-000001");
    CHECK(receipt.lots.size() == 2);
    CHECK(*receipt.value == Rational{Numerator{17000}, Denominator{100}, DecimalPlaces{2}});
    CHECK(*receipt.companyValue == Rational{Numerator{17000}, Denominator{100}, DecimalPlaces{2}});
    CHECK(receipt.receivedBy == "kim");
    REQUIRE(entryCount() == 1);
    CHECK(entries().front().id == receipt.entry);
    CHECK(entries().front().sourceKey == "inventory.receipt:" + std::to_string(*order.id) + ":1");
    ScopedPrincipal const kim{"kim"};
    auto const stock =
        lims::StockModel{}.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}});
    REQUIRE(stock.lots.size() == 2);
    CHECK(*stock.lots.at(1).onHand == Rational{Numerator{1}, Denominator{1}, DecimalPlaces{6}});
    CHECK(*stock.lots.at(1).unitCost == Rational{Numerator{8500}, Denominator{100}, DecimalPlaces{2}});
    auto const now = lims::ProcurementModel{}.execute(lims::GetPurchaseOrder{.orderId = order.id});
    CHECK(now.state == lims::OrderState::Received);
    CHECK(*now.lines.front().received == Rational{Numerator{2}, Denominator{1}, DecimalPlaces{6}});
    CHECK(lims::ProcurementModel{}.execute(lims::GetReceipt{.receiptId = receipt.id}).number == receipt.number);
}

// A CHF receipt is valued at the ledger's rate on its date, and a lot's unit cost is its company value over its
// quantity, exactly. Mutation: divide by the line's value in CHF (the lot costs 85.00, not 80.75).
TEST_CASE(
    "goods receipt: a CHF order is posted at the ledger's rate and a lot costs its company value over its "
    "quantity",
    "[lims][inventory][receipt]") {
    DbFixture fixture;
    lims::test::inventoryBooks();
    auto const opened = lims::test::openOrder(lims::CurrencyCode::CHF);
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    // 0.375 L at 85.00 CHF is 31.875 → 31.88 CHF; at 0.95 that is 30.286 → 30.29 EUR; per litre 80.7733... exactly.
    auto const receipt =
        receive(opened.order, opened.stock.coldRoom, {Delivered{"MK-1101", litres(375), lims::StockUnit::litre}});
    CHECK(receipt.currency == lims::CurrencyCode::CHF);
    CHECK(*receipt.value == Rational{Numerator{3188}, Denominator{100}, DecimalPlaces{2}});
    CHECK(*receipt.companyValue == Rational{Numerator{3029}, Denominator{100}, DecimalPlaces{2}});
    REQUIRE(entryCount() == 1);
    auto const entry = entries().front();
    CHECK(entry.currency == lims::CurrencyCode::CHF);
    CHECK(entry.lines.size() == 2);
    ScopedPrincipal const kim{"kim"};
    auto const lot =
        lims::StockModel{}.execute(lims::StockOnHand{.company = lims::test::seededCompany(), .item = {}}).lots.front();
    // 30.29 / 0.375 is 80.77333..., kept as a fraction, not rounded to cents: times the quantity it is 30.29 again.
    auto const back = morph::math::checkedMul(*lot.unitCost, *lot.onHand);
    REQUIRE(back.has_value());
    CHECK(*back == Rational{Numerator{3029}, Denominator{100}, DecimalPlaces{2}});
}

// Mutation: take a missing rate as no refusal and post at a rate of one (the receipt is accepted and leaves an entry).
TEST_CASE("goods receipt: refused without a rate on its date", "[lims][inventory][receipt]") {
    DbFixture fixture;
    lims::test::books();  // no CHF rate
    auto const opened = lims::test::openOrder(lims::CurrencyCode::CHF);
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    auto const entriesBefore = entryCount();
    CHECK_THROWS_WITH(receiveTwoLots(opened.order, opened.stock.coldRoom),
                      Catch::Matchers::ContainsSubstring("no CHF/EUR rate on 09-Nov-2026"));
    CHECK(lotCount() == 0);
    CHECK(entryCount() == entriesBefore);
}

// Mutation: drop the comparison with what was ordered (the 2.5 L receipt is accepted).
TEST_CASE("goods receipt: a duplicate lot and a receipt beyond what was ordered are refused",
          "[lims][inventory][receipt]") {
    DbFixture fixture;
    lims::test::inventoryBooks();
    auto const opened = lims::test::openOrder(lims::CurrencyCode::EUR);
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    auto const receiveOne = [&opened](std::string lotNumber, lims::Measure quantity) {
        return receive(opened.order, opened.stock.coldRoom,
                       {Delivered{std::move(lotNumber), std::move(quantity), lims::StockUnit::litre}});
    };
    static_cast<void>(receiveOne("MK-1101", litres(1000)));
    auto const entriesBefore = entryCount();
    CHECK_THROWS_WITH(receiveOne("MK-1101", litres(500)), Catch::Matchers::ContainsSubstring("MK-1101"));
    CHECK_THROWS_WITH(receiveOne("MK-1103", litres(1500)),
                      Catch::Matchers::ContainsSubstring("more than was ordered"));
    CHECK(lotCount() == 1);
    CHECK(entryCount() == entriesBefore);  // both refusals came before the ledger was called
    ScopedPrincipal const kim{"kim"};
    auto const order = lims::ProcurementModel{}.execute(lims::GetPurchaseOrder{.orderId = opened.order.id});
    CHECK(order.state == lims::OrderState::PartlyReceived);
}

// Mutation: drop the site comparison (a location at another site takes the delivery).
TEST_CASE("goods receipt: the request is refused for the order's state, its lines, its location and its units",
          "[lims][inventory][receipt]") {
    DbFixture fixture;
    lims::test::inventoryBooks();
    auto const opened = lims::test::openOrder(lims::CurrencyCode::EUR);
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    lims::SiteId otherSite;
    {
        ScopedPrincipal const sam{"sam"};
        otherSite =
            lims::OrgModel{}.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Annex"}).id;
    }
    lims::test::grant("kim", lims::LimsRole::StockKeeper, lims::Scope{otherSite});
    lims::LocationId annexRoom;
    {
        ScopedPrincipal const kim{"kim"};
        annexRoom = lims::StockModel{}.execute(lims::CreateLocation{.site = otherSite, .name = "Annex room"}).id;
    }
    CHECK_THROWS_WITH(receive(opened.order, annexRoom, {Delivered{"MK-1", litres(1000), lims::StockUnit::litre}}),
                      Catch::Matchers::ContainsSubstring("is not at the site of order"));
    CHECK_THROWS_WITH(
        receive(opened.order, opened.stock.coldRoom, {Delivered{"MK-1", litres(1000), lims::StockUnit::kilogram}}),
        Catch::Matchers::ContainsSubstring("kg"));
    CHECK_THROWS_WITH(
        receive(opened.order, opened.stock.coldRoom, {Delivered{"MK-1", litres(0), lims::StockUnit::litre}}),
        Catch::Matchers::ContainsSubstring("must be positive"));
    auto foreign = deliveryOf(opened.order, opened.stock.coldRoom, at(2026, 11, 9),
                              {Delivered{"MK-1", litres(1000), lims::StockUnit::litre}});
    foreign.lines.front().orderLine = lims::OrderLineId{999};
    CHECK_THROWS_WITH(lims::test::dispatchAs<lims::ProcurementModel>("kim", foreign),
                      Catch::Matchers::ContainsSubstring("is not a line of order"));
    CHECK_THROWS_WITH(lims::test::dispatchAs<lims::ProcurementModel>(
                          "kim", lims::ReceiveGoods{.order = opened.order.id,
                                                    .location = opened.stock.coldRoom,
                                                    .date = morph::time::Timestamp{at(2026, 11, 9)},
                                                    .lines = {}}),
                      Catch::Matchers::ContainsSubstring("failed validation"));
    auto twice = deliveryOf(opened.order, opened.stock.coldRoom, at(2026, 11, 9),
                            {Delivered{"MK-1", litres(500), lims::StockUnit::litre},
                             Delivered{"MK-1", litres(500), lims::StockUnit::litre}});
    CHECK_THROWS_WITH(lims::test::dispatchAs<lims::ProcurementModel>("kim", twice),
                      Catch::Matchers::ContainsSubstring("failed validation"));
    CHECK(entryCount() == 0);
    // A fully received order has nothing left to receive.
    static_cast<void>(receiveTwoLots(opened.order, opened.stock.coldRoom));
    CHECK_THROWS_WITH(
        receive(opened.order, opened.stock.coldRoom, {Delivered{"MK-9", litres(1), lims::StockUnit::litre}}),
        Catch::Matchers::ContainsSubstring("nothing left to receive"));
    CHECK(entryCount() == 1);
}

// Hypothesis 1, two commits in sequence. Mutation: take the receipt's number before the ledger call (the failed
// receipt consumes GR-2026-000001, and the retry's number check fails).
TEST_CASE(
    "Hypothesis 1: a dispatched receipt that fails after the ledger leaves an entry for goods not on hand, and its "
    "retry converges",
    "[lims][inventory][receipt][h1]") {
    DbFixture fixture;
    lims::test::inventoryBooks();
    auto const opened = lims::test::openOrder(lims::CurrencyCode::EUR);
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    auto const entriesBefore = entryCount();
    FaultHookGuard const guard;
    failAt("receipt.afterLedger");
    CHECK_THROWS_WITH(receiveTwoLots(opened.order, opened.stock.coldRoom),
                      Catch::Matchers::ContainsSubstring("server stopped"));

    // The partial state: the ledger holds the entry, inventory holds neither lots nor receipt.
    CHECK(entryCount() == entriesBefore + 1);
    CHECK(lotCount() == 0);
    CHECK(receipts().empty());
    auto const stranded = entries().back();

    // No hole: the number was never assigned. The retry converges on the one entry.
    lims::setInventoryFaultHook({});
    auto const retried = receiveTwoLots(opened.order, opened.stock.coldRoom);
    CHECK(retried.number == "GR-2026-000001");
    CHECK(retried.entry == stranded.id);
    CHECK(entryCount() == entriesBefore + 1);
    CHECK(lotCount() == 2);
}

// A lot taken between the pre-ledger check and the write is the unique index's refusal; the entry stays bound to the
// key and the delivery is received again with another lot number. Mutation: put the lot number in sourceReference (the
// corrected retry is the ledger's Conflict and the order is stranded).
TEST_CASE(
    "goods receipt: a dispatched receipt whose lot was taken after the ledger call is received again with another lot "
    "number",
    "[lims][inventory][receipt]") {
    DbFixture fixture;
    lims::test::inventoryBooks();
    auto const opened = lims::test::openOrder(lims::CurrencyCode::EUR);
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    auto const entriesBefore = entryCount();
    {
        FaultHookGuard const guard;
        lims::setInventoryFaultHook([&](std::string_view point, Lightweight::SqlConnection&) {
            if (point == "receipt.afterLedger") {
                // The race the pre-ledger check cannot see: another stock keeper records the lot meanwhile.
                static_cast<void>(lims::test::openingLot(opened.stock.reagent, opened.stock.coldRoom, "MK-1101",
                                                         at(2027, 3, 31), litres(500), lims::StockUnit::litre));
            }
        });
        CHECK_THROWS_WITH(receiveTwoLots(opened.order, opened.stock.coldRoom),
                          Catch::Matchers::ContainsSubstring("MK-1101"));
    }
    CHECK(entryCount() == entriesBefore + 1);
    CHECK(lotCount() == 1);  // the race's lot only
    auto const stranded = entries().back();

    auto const retried = receive(opened.order, opened.stock.coldRoom,
                                 {Delivered{"MK-1103", litres(1000), lims::StockUnit::litre},
                                  Delivered{"MK-1102", millilitres(1000), lims::StockUnit::millilitre}});
    CHECK(retried.number == "GR-2026-000001");
    CHECK(retried.entry == stranded.id);
    CHECK(entryCount() == entriesBefore + 1);
    CHECK(lotCount() == 3);
}

// A retry with another body is never a second entry and never a silent move to the next ordinal. Mutation: rethrow the
// ledger's refusal of the other body as it is (the entry is not named).
TEST_CASE("goods receipt: a stranded receipt received again with other lines is refused, naming its entry",
          "[lims][inventory][receipt]") {
    DbFixture fixture;
    lims::test::inventoryBooks();
    auto const opened = lims::test::openOrder(lims::CurrencyCode::EUR);
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    {
        FaultHookGuard const guard;
        failAt("receipt.afterLedger");
        CHECK_THROWS_WITH(receive(opened.order, opened.stock.coldRoom,
                                  {Delivered{"MK-1101", litres(500), lims::StockUnit::litre},
                                   Delivered{"MK-1102", litres(500), lims::StockUnit::litre}}),
                          Catch::Matchers::ContainsSubstring("server stopped"));
    }
    REQUIRE(entryCount() == 1);
    auto const stranded = entries().front();
    CHECK_THROWS_WITH(receive(opened.order, opened.stock.coldRoom,
                              {Delivered{"MK-1101", litres(500), lims::StockUnit::litre},
                               Delivered{"MK-1102", millilitres(1500), lims::StockUnit::millilitre}}),
                      Catch::Matchers::ContainsSubstring("was posted as entry " + std::to_string(*stranded.id) +
                                                         " with other lines"));
    CHECK(entryCount() == 1);
    CHECK(lotCount() == 0);
    // Received as first sent, it converges.
    auto const retried = receive(opened.order, opened.stock.coldRoom,
                                 {Delivered{"MK-1101", litres(500), lims::StockUnit::litre},
                                  Delivered{"MK-1102", litres(500), lims::StockUnit::litre}});
    CHECK(retried.entry == stranded.id);
    CHECK(retried.number == "GR-2026-000001");
    CHECK(entryCount() == 1);
}

// Hypothesis 1, one shared transaction. Mutation: commit inventory's transaction before postIn (the failure leaves the
// receipt).
TEST_CASE("Hypothesis 1: a receipt in one shared transaction that fails leaves nothing and no hole",
          "[lims][inventory][receipt][h1]") {
    DbFixture fixture;
    lims::test::inventoryBooks();
    auto const opened = lims::test::openOrder(lims::CurrencyCode::EUR);
    lims::test::ReceiptRig const rig{lims::PostingMode::SharedTransaction};
    FaultHookGuard const guard;
    failAt("receipt.afterLedger");
    CHECK_THROWS_WITH(receiveTwoLots(opened.order, opened.stock.coldRoom),
                      Catch::Matchers::ContainsSubstring("server stopped"));
    CHECK(entryCount() == 0);
    CHECK(lotCount() == 0);
    CHECK(receipts().empty());
    {
        ScopedPrincipal const kim{"kim"};
        auto const order = lims::ProcurementModel{}.execute(lims::GetPurchaseOrder{.orderId = opened.order.id});
        CHECK(order.state == lims::OrderState::Open);
        CHECK(*order.lines.front().received == Rational{0, DecimalPlaces{6}});
    }

    lims::setInventoryFaultHook({});
    auto const retried = receiveTwoLots(opened.order, opened.stock.coldRoom);
    CHECK(retried.number == "GR-2026-000001");
    REQUIRE(entryCount() == 1);
    CHECK(retried.entry == entries().front().id);
}

// What each way keeps of the ledger's own action: its rule (the posting principal's service role), its effect on a
// watching connection. The control is inventory's own effect, which both keep. Mutation: in SharedTransaction,
// dispatch a repeat RecordPosting after the commit (the accountant hears it: the shared case's silence check fails).
TEST_CASE("Hypothesis 1: what a receipt in one shared transaction gives up", "[lims][inventory][receipt][h1]") {
    auto const mode = GENERATE(lims::PostingMode::Dispatched, lims::PostingMode::SharedTransaction);
    CAPTURE(mode);
    bool const dispatched = mode == lims::PostingMode::Dispatched;
    DbFixture fixture;
    lims::test::inventoryBooks();
    auto const opened = lims::test::openOrder(lims::CurrencyCode::EUR);
    // The ledger's rule, first, while the posting principal holds no role: the dispatched receipt is refused by the
    // edge's role table before the ledger's rule can run, and the shared one succeeds because inventory writes the
    // entry itself.
    {
        lims::test::ReceiptRig withheld{mode, lims::test::PostingRole::Withheld};
        auto const entriesBefore = entryCount();
        auto const first = [&] {
            return receiveOver(withheld, opened.order, opened.stock.coldRoom,
                               {Delivered{"MK-1102", litres(1000), lims::StockUnit::litre}});
        };
        if (dispatched) {
            CHECK_THROWS_WITH(first(), Catch::Matchers::ContainsSubstring("unauthorized"));
            CHECK(entryCount() == entriesBefore);
            CHECK(lotCount() == 0);
        } else {
            CHECK(first().number == "GR-2026-000001");
            CHECK(entryCount() == entriesBefore + 1);
        }
    }
    // The ledger's effect: an accountant watching the ledger hears the dispatched receipt's entry and not the shared
    // one's; the stock keeper hears inventory's own change in both.
    lims::test::ReceiptRig rig{mode};
    auto const accountant = rig.rig().connect();
    auto const keeper = rig.rig().connect();
    REQUIRE(accountant->watch({"Ledger_Entry"}, as("acc")).kind == "ok");
    REQUIRE(keeper->watch({"Inventory_Receipt"}, as("kim")).kind == "ok");
    static_cast<void>(receiveOver(rig, opened.order, opened.stock.coldRoom,
                                  {Delivered{"MK-1101", litres(1000), lims::StockUnit::litre}}));
    CHECK(ServerConnection::eventually([&] { return keeper->heardOf("Inventory_Receipt"); }));
    bool const ledgerHeard =
        ServerConnection::eventually([&] { return accountant->heardOf("Ledger_Entry"); }, std::chrono::seconds{1});
    CHECK(ledgerHeard == dispatched);
}

// Mutation: take the number before the ledger call in Dispatched mode (a failed receipt consumes one, and the retry is
// GR-2026-000002).
TEST_CASE("goods receipt: numbers are gap-free per company and fiscal year", "[lims][inventory][receipt]") {
    auto const mode = GENERATE(lims::PostingMode::Dispatched, lims::PostingMode::SharedTransaction);
    CAPTURE(mode);
    DbFixture fixture;
    lims::test::inventoryBooks();
    {
        ScopedPrincipal const acc{"acc"};
        lims::LedgerModel{}.execute(
            lims::OpenPeriod{.company = lims::test::seededCompany(), .month = morph::time::Timestamp{at(2027, 1, 1)}});
    }
    auto const opened = lims::test::openOrder(lims::CurrencyCode::EUR);
    lims::test::ReceiptRig const rig{mode};
    auto const half = [&](std::string number, morph::time::DateTime when) {
        ScopedClockOverride const clock{when};
        return lims::test::dispatchAs<lims::ProcurementModel>(
            "kim", deliveryOf(opened.order, opened.stock.coldRoom, when,
                              {Delivered{std::move(number), litres(500), lims::StockUnit::litre}}));
    };
    {
        // A receipt that fails after the ledger's half, in either mode, spends no number.
        FaultHookGuard const guard;
        failAt("receipt.afterLedger");
        CHECK_THROWS_WITH(half("MK-1101", at(2026, 11, 9)), Catch::Matchers::ContainsSubstring("server stopped"));
    }
    CHECK(half("MK-1101", at(2026, 11, 9)).number == "GR-2026-000001");
    CHECK(half("MK-1102", at(2026, 11, 9)).number == "GR-2026-000002");
    // The seeded company's fiscal year starts in January: the next year's series starts again.
    CHECK(half("MK-1103", at(2027, 1, 2)).number == "GR-2027-000001");
}

// Mutation: drop the received-quantity condition from the order line's update (on the odd rounds both receipts are
// accepted and the line holds more than was ordered).
TEST_CASE("goods receipt: two receipts against the last of an order line give one receipt and one Conflict",
          "[lims][inventory][receipt]") {
    DbFixture fixture;
    lims::test::inventoryBooks();
    auto const first = lims::test::openOrder(lims::CurrencyCode::EUR);
    lims::test::ReceiptRig rig{lims::PostingMode::Dispatched};
    for (int round = 0; round < 10; ++round) {
        auto const order = round == 0 ? first.order : anotherOrder(first.stock);
        INFO("round " << round);
        // Odd rounds leave the order partly received when the first keeper's receipt lands, so the order's state does
        // not change and only the order line's own condition can turn the second keeper away.
        bool const partial = round % 2 == 1;
        if (partial) {
            static_cast<void>(receive(order, first.stock.coldRoom,
                                      {Delivered{"P-" + std::to_string(round), litres(500), lims::StockUnit::litre}}));
        }
        lims::Measure const sent = partial ? litres(1000) : litres(2000);
        // Both receipts are held after the ledger's half until both have arrived, so both passed the pre-ledger
        // checks and both write their order line.
        Rendezvous rendezvous;
        FaultHookGuard const guard;
        lims::setInventoryFaultHook([&rendezvous](std::string_view point, Lightweight::SqlConnection&) {
            if (point == "receipt.afterLedger") {
                rendezvous.arrive(2);
            }
        });
        int received = 0;
        int conflicts = 0;
        std::vector<std::string> other;
        // Two stock keepers over two connections send the full receipt at once; each is awaited in turn.
        ScopedClockOverride const clock{at(2026, 11, 9)};
        rig.rig().bridge(0).setDefaultSession(as("kim"));
        rig.rig().bridge(1).setDefaultSession(as("kim"));
        auto keeperA = rig.rig().client<lims::ProcurementModel>(0);
        auto keeperB = rig.rig().client<lims::ProcurementModel>(1);
        auto callA =
            keeperA.execute(deliveryOf(order, first.stock.coldRoom, at(2026, 11, 9),
                                       {Delivered{"A-" + std::to_string(round), sent, lims::StockUnit::litre}}));
        auto callB =
            keeperB.execute(deliveryOf(order, first.stock.coldRoom, at(2026, 11, 9),
                                       {Delivered{"B-" + std::to_string(round), sent, lims::StockUnit::litre}}));
        for (auto* call : {&callA, &callB}) {
            try {
                static_cast<void>(morph::ladder::testkit::awaitQt(std::move(*call)));
                ++received;
            } catch (std::exception const& error) {
                std::string const text = error.what();
                if (text.contains("meanwhile") || text.contains("busy")) {
                    ++conflicts;
                } else {
                    other.push_back(text);
                }
            }
        }
        CHECK(received == 1);
        CHECK(conflicts == 1);
        CHECK(other == std::vector<std::string>{});
        lims::setInventoryFaultHook({});
        ScopedPrincipal const kim{"kim"};
        auto const now = lims::ProcurementModel{}.execute(lims::GetPurchaseOrder{.orderId = order.id});
        CHECK(now.state == (partial ? lims::OrderState::PartlyReceived : lims::OrderState::Received));
        CHECK(*now.lines.front().received == (partial ? Rational{Numerator{3}, Denominator{2}, DecimalPlaces{6}}
                                                      : Rational{Numerator{2}, Denominator{1}, DecimalPlaces{6}}));
    }
    CHECK(receipts().size() == 15);
    CHECK(lotCount() == 15);
}

// Mutation: drop the company check from GetReceipt (a stock keeper of another company reads the receipt).
TEST_CASE("goods receipt: only a stock keeper at the order's company receives, and an unseen order is absent",
          "[lims][inventory][receipt][authorization]") {
    DbFixture fixture;
    lims::test::inventoryBooks();
    auto const opened = lims::test::openOrder(lims::CurrencyCode::EUR);
    lims::test::ReceiptRig const rig{lims::PostingMode::Dispatched};
    {
        ScopedPrincipal const sam{"sam"};
        auto const osaka = lims::OrgModel{}.execute(lims::CreateCompany{.name = "Rheinlab Japan"});
        lims::OrgModel{}.execute(lims::AssignRole{
            .principal = "omar", .role = lims::LimsRole::StockKeeper, .company = osaka.id, .site = {}});
    }
    auto const request = deliveryOf(opened.order, opened.stock.coldRoom, at(2026, 11, 9),
                                    {Delivered{"MK-1101", litres(1000), lims::StockUnit::litre}});
    CHECK_THROWS_WITH(lims::test::dispatchAs<lims::ProcurementModel>("rita", request),
                      Catch::Matchers::ContainsSubstring("stock-keeper role"));
    CHECK_THROWS_WITH(lims::test::dispatchAs<lims::ProcurementModel>("omar", request),
                      Catch::Matchers::ContainsSubstring("stock-keeper role"));
    CHECK(entryCount() == 0);
    // The reads: a receipt is read by those who see its company.
    auto const receipt =
        receive(opened.order, opened.stock.coldRoom, {Delivered{"MK-1101", litres(1000), lims::StockUnit::litre}});
    ScopedPrincipal const omar{"omar"};
    CHECK_THROWS_AS(lims::ProcurementModel{}.execute(lims::GetReceipt{.receiptId = receipt.id}), lims::NotFound);
    CHECK(
        lims::ProcurementModel{}.execute(lims::ListReceipts{.company = lims::test::seededCompany()}).receipts.empty());
}

// Mutation: drop the PurchaseOrderView effect from ReceiveGoods's registration (an order list that holds the order is
// not told it was received).
TEST_CASE("goods receipt: receiving declares that it changes the receipt, the lots, the movements and the order",
          "[lims][inventory][receipt]") {
    auto const found = morph::effects::effectsOf("ReceiveGoods");
    REQUIRE(found.has_value());
    std::vector<std::string> names;
    for (auto const& effect : *found) {
        CHECK(effect.kind == morph::effects::Kind::Changes);
        names.push_back(std::string{effect.record});
    }
    std::ranges::sort(names);
    CHECK(names ==
          std::vector<std::string>{"Inventory_Lot", "Inventory_Movement", "Inventory_Order", "Inventory_Receipt"});
}

// The new actions cross every backend: the Task handler, its nested lines, the reads.
TEST_CASE("goods receipt: receiving and reading across the backend matrix", "[lims][inventory][receipt][matrix]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    lims::test::inventoryBooks();
    auto const opened = lims::test::openOrder(lims::CurrencyCode::EUR);
    lims::test::ReceiptRig const rig{lims::PostingMode::SharedTransaction};
    auto const receipt = receive(opened.order, opened.stock.coldRoom,
                                 {Delivered{"MK-1101", litres(1000), lims::StockUnit::litre},
                                  Delivered{"MK-1102", millilitres(1000), lims::StockUnit::millilitre}},
                                 mode);
    CHECK(receipt.number == "GR-2026-000001");
    auto const read =
        lims::test::dispatchAs<lims::ProcurementModel>("kim", lims::GetReceipt{.receiptId = receipt.id}, mode);
    CHECK(read.entry == receipt.entry);
    CHECK(read.lots == receipt.lots);
    auto const listed = lims::test::dispatchAs<lims::ProcurementModel>(
        "kim", lims::ListReceipts{.company = lims::test::seededCompany()}, mode);
    REQUIRE(listed.receipts.size() == 1);
    CHECK(listed.receipts.front().id == receipt.id);
}
