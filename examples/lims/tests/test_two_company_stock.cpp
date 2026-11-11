// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 7, second instance: two companies' stock keepers watch lots and movements on one server; a company-B lot
// is recorded. Does the company-A connection hear of it, and what does it learn?

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <glaze/glaze.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "inventory_test_support.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/org/models/org_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::at;
using lims::test::litres;
using lims::test::ScopedPrincipal;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::ServerConnection;

namespace {

constexpr std::string_view kSecret = "lims-two-company-stock-secret-at-least-32";

/// @brief A session for @p principal, signed with the case's secret.
/// @param principal The principal.
/// @return The session.
[[nodiscard]] morph::session::Context as(std::string const& principal) {
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    return lims::test::signedAs(issuer, principal);
}

/// @brief The Osaka company (JPY) with one site, inventory settings, a reagent stocked in litres and a cold room, set
///        up by sam; `kenji` is its stock keeper.
/// @return The reagent and the room.
lims::test::StockSetUp osaka() {
    ScopedPrincipal const sam{"sam"};
    lims::OrgModel org;
    auto const company = org.execute(lims::CreateCompany{
        .name = "Rheinlab Japan", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});
    auto const site = org.execute(lims::CreateSite{.companyId = company.id, .name = "Osaka"});
    org.execute(lims::AssignRole{
        .principal = "kenji", .role = lims::LimsRole::StockKeeper, .company = company.id, .site = {}});
    lims::StockModel stock;
    stock.execute(lims::SetInventorySettings{
        .company = company.id,
        .stockAccount = "3100",
        .grniAccount = "1600",
        .consumptionAccount = "5100",
        .roundingAccount = "6900",
        .approvalHours = lims::Count{morph::math::Rational{48, morph::math::DecimalPlaces{0}}}});
    ScopedPrincipal const kenji{"kenji"};
    auto const item = stock.execute(lims::CreateItem{
        .company = company.id, .code = "NO3-R", .name = "Nitrate reagent", .unit = lims::StockUnit::litre});
    auto const room = stock.execute(lims::CreateLocation{.site = site.id, .name = "Cold room"});
    return lims::test::StockSetUp{.reagent = item.id, .coldRoom = room.id};
}

/// @brief Records an opening lot of @p item at @p room through @p connection, as @p principal: dispatched to the
///        server, so the server derives and fans out its effects, as a client's call is.
/// @param connection The connection.
/// @param principal Who records it.
/// @param item The item.
/// @param room The location.
/// @param lotNumber The lot number.
/// @return The reply's kind.
std::string recordThrough(ServerConnection& connection, std::string const& principal, lims::ItemId item,
                          lims::LocationId room, std::string lotNumber) {
    auto const model = connection.registerModel("StockModel", as(principal));
    lims::RecordOpeningLot const action{
        .item = item,
        .location = room,
        .lotNumber = std::move(lotNumber),
        .expiresAt = morph::time::Timestamp{at(2027, 3, 31)},
        .quantity = litres(1000),
        .unit = lims::StockUnit::litre,
        .unitCost = lims::Amount{morph::math::Rational{1500, morph::math::DecimalPlaces{0}}}};
    return connection
        .execute(model, "StockModel", "RecordOpeningLot", glz::write_json(action).value_or(""), as(principal))
        .kind;
}

}  // namespace

// Hypothesis 7. The control: kim's connection hears a lot recorded at her own company, so the check can see a change.
// Mutation: watch {"Inventory_Item"} instead of the lot and movement records; kim hears neither lot, so heardOfB,
// lotWhole, movementWhole and the control fail. CHECK_FALSE(anyKeys) stays green under it: its detector is decision
// 40's ApproveInvoice case, which sends a key through the same fan-out.
TEST_CASE("Hypothesis 7: a company-B lot and what a company-A connection hears of it", "[lims][inventory][h7]") {
    DbFixture fixture;
    auto const rhein = lims::test::stockSetUp();
    auto const japan = osaka();
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(kSecret)};
    auto const kim = rig.connect();
    auto const kenji = rig.connect();
    auto const keeper = rig.connect();
    REQUIRE(kim->watch({"Inventory_Lot", "Inventory_Movement"}, as("kim")).kind == "ok");

    auto const before = kim->changes().size();
    REQUIRE(recordThrough(*kenji, "kenji", japan.reagent, japan.coldRoom, "OS-1") == "ok");
    bool const heardOfB =
        ServerConnection::eventually([&] { return kim->changes().size() > before; }, std::chrono::milliseconds{1000});
    INFO("company-A connection heard of the company-B lot: " << heardOfB);
    bool lotWhole = false;
    bool movementWhole = false;
    bool anyKeys = false;
    for (auto const& body : kim->changes()) {
        for (auto const& change : body.records) {
            anyKeys = anyKeys || change.keys.has_value();
            lotWhole = lotWhole || (change.record == "Inventory_Lot" && !change.keys);
            movementWhole = movementWhole || (change.record == "Inventory_Movement" && !change.keys);
            UNSCOPED_INFO("sent to company A: " << change.record << (change.keys ? " with keys" : " as a whole"));
            if (change.keys) {
                for (auto const& key : *change.keys) {
                    UNSCOPED_INFO("  key " << key.str);
                }
            }
        }
    }
    // RecordOpeningLot declares changes<>, so the fan-out names the records without a key: company A learns that
    // the other company's lots and movements changed, not which lot.
    CHECK(heardOfB);
    CHECK(lotWhole);
    CHECK(movementWhole);
    CHECK_FALSE(anyKeys);

    auto const beforeOwn = kim->changes().size();
    REQUIRE(recordThrough(*keeper, "kim", rhein.reagent, rhein.coldRoom, "RH-1") == "ok");
    CHECK(ServerConnection::eventually([&] { return kim->changes().size() > beforeOwn; }));
}
