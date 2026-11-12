// SPDX-License-Identifier: Apache-2.0
//
// Hypothesis 5, second instance: a requisition is raised, submitted and approved as ordinary actions on its row, four
// eyes and a value limit per role hold, its approval escalates from the site to the company at its deadline, and
// an open approvals inbox is not told when the deadline passes.

#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <exception>
#include <glaze/glaze.hpp>
#include <morph/core/action_guard.hpp>
#include <morph/session/session_auth.hpp>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "clock.hpp"
#include "inventory_test_support.hpp"
#include "lims/inventory/core/fault_hook.hpp"
#include "lims/inventory/models/procurement_model.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/dto/company_dto.hpp"
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

constexpr std::string_view kSecret = "lims-requisition-test-secret-at-least-32";

/// @brief Submits @p requisition as rita at 09:00 on 10 November 2026.
/// @param requisition The requisition.
/// @return It, submitted.
lims::RequisitionView submitted(lims::RequisitionView const& requisition) {
    ScopedClockOverride const clock{at(2026, 11, 10)};
    ScopedPrincipal const rita{"rita"};
    return lims::ProcurementModel{}.execute(lims::SubmitRequisition{.requisitionId = requisition.id});
}

/// @brief Approves @p requisition as @p principal.
/// @param principal The approver.
/// @param requisition The requisition.
/// @return It, approved.
lims::RequisitionView approveAs(std::string const& principal, lims::RequisitionView const& requisition) {
    ScopedPrincipal const approver{principal};
    return lims::ProcurementModel{}.execute(lims::ApproveRequisition{.requisitionId = requisition.id});
}

/// @brief The requisitions @p principal could approve now.
/// @param principal The principal.
/// @return Their ids.
std::vector<std::int64_t> pendingFor(std::string const& principal) {
    ScopedPrincipal const who{principal};
    std::vector<std::int64_t> ids;
    for (auto const& requisition : lims::ProcurementModel{}.execute(lims::ListPendingRequisitions{}).requisitions) {
        ids.push_back(*requisition.id);
    }
    return ids;
}

/// @brief Makes @p sql run on a connection of its own, committed, when the next requisition state change is about to
///        be written, as a concurrent action that committed first would have.
/// @param sql The statement.
void raceWith(std::string sql) {
    lims::setInventoryFaultHook([sql = std::move(sql)](std::string_view point, Lightweight::SqlConnection&) {
        if (point == "requisition.beforeStateChange") {
            Lightweight::SqlConnection other;
            Lightweight::SqlStatement statement{other};
            static_cast<void>(statement.ExecuteDirect(sql));
        }
    });
}

/// @brief A session signed with the case's secret.
/// @param principal The principal.
/// @return The session.
morph::session::Context as(std::string const& principal) {
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    return lims::test::signedAs(issuer, principal);
}

}  // namespace

// Mutation: drop the CreateRequisition pair from Authorization<ApproveRequisition>::separations (rita, made a purchase
// approver, approves her own requisition).
TEST_CASE("requisitions: the requester cannot approve her own requisition", "[lims][inventory][requisition]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    auto const requisition = submitted(setUp.draft);
    CHECK(requisition.state == lims::RequisitionState::Submitted);
    lims::test::grant("rita", lims::LimsRole::PurchaseApprover);
    {
        ScopedPrincipal const sam{"sam"};
        lims::OrgModel{}.execute(
            lims::SetValueLimit{.principal = "rita",
                                .company = lims::test::seededCompany(),
                                .limit = lims::Amount{morph::math::Rational{100000, morph::math::DecimalPlaces{2}}},
                                .role = lims::LimsRole::PurchaseApprover});
    }
    ScopedClockOverride const clock{at(2026, 11, 10, 10)};
    CHECK_THROWS_WITH(approveAs("rita", requisition),
                      Catch::Matchers::ContainsSubstring("requested this requisition and cannot also approve it"));
    CHECK(approveAs("anna", requisition).approvedBy == "anna");
}

// Mutation: make Authorization<ApproveRequisition>::limitOf read the invoice approver's limit (tom's purchase limit of
// 10.00 is never consulted; his invoice limit of 1,000,000.00 covers the requisition, so he approves it and the
// refusal check fails).
TEST_CASE("requisitions: the value limit is the approver's limit for this role", "[lims][inventory][requisition]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    {
        ScopedPrincipal const sam{"sam"};
        lims::OrgModel{}.execute(
            lims::SetValueLimit{.principal = "tom",
                                .company = lims::test::seededCompany(),
                                .limit = lims::Amount{morph::math::Rational{100000000, morph::math::DecimalPlaces{2}}},
                                .role = lims::LimsRole::InvoiceApprover});
    }
    auto const requisition = submitted(setUp.draft);
    ScopedClockOverride const clock{at(2026, 11, 10, 10)};
    CHECK_THROWS_WITH(approveAs("tom", requisition), Catch::Matchers::ContainsSubstring("value limit"));
    CHECK(approveAs("anna", requisition).state == lims::RequisitionState::Approved);
}

// Hypothesis 4 is not this: escalation is the deadline the rule reads. Mutation: make scopeOf answer the site whatever
// the deadline (anna approves after it, and julia's pending list misses the requisition before it).
TEST_CASE("requisitions: approval moves from the site to the company at the deadline",
          "[lims][inventory][requisition]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    auto const requisition = submitted(setUp.draft);
    {
        ScopedClockOverride const before{at(2026, 11, 11)};
        CHECK(pendingFor("anna") == std::vector<std::int64_t>{*requisition.id});
        CHECK(pendingFor("julia") == std::vector<std::int64_t>{*requisition.id});
    }
    ScopedClockOverride const after{at(2026, 11, 12, 10)};
    CHECK(pendingFor("anna").empty());
    CHECK(pendingFor("julia") == std::vector<std::int64_t>{*requisition.id});
    CHECK_THROWS_AS(approveAs("anna", requisition), lims::Forbidden);
    auto const approved = approveAs("julia", requisition);
    CHECK(approved.level == lims::RequisitionLevel::Company);
    CHECK(approved.approvedBy == "julia");
}

// Mutation: drop the state condition from ApproveRequisition's update (the second approval overwrites the first).
TEST_CASE("requisitions: an approval that lost the race to another is a Conflict", "[lims][inventory][requisition]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    auto const requisition = submitted(setUp.draft);
    ScopedClockOverride const clock{at(2026, 11, 10, 10)};
    // Between anna's read and her write, julia's approval commits on the same row.
    raceWith("UPDATE inventory_requisitions SET state = 2, approved_by = 'julia' WHERE id = " +
             std::to_string(*requisition.id));
    CHECK_THROWS_AS(approveAs("anna", requisition), lims::Conflict);
    lims::setInventoryFaultHook({});
    ScopedPrincipal const rita{"rita"};
    CHECK(lims::ProcurementModel{}.execute(lims::GetRequisition{.requisitionId = requisition.id}).approvedBy ==
          "julia");
}

// The lesson Part B paid for, measured: two approvers acting at once give one success and one Conflict, never a raw
// SQL error. The clock override is process-wide, so both threads read the same instant. Mutation: read the
// requisition inside the write's transaction (the loser sees "database is locked").
TEST_CASE("requisitions: two approvers at once give one approval and one Conflict", "[lims][inventory][requisition]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    for (int round = 0; round < 10; ++round) {
        auto draft = setUp.draft;
        if (round > 0) {
            ScopedPrincipal const rita{"rita"};
            lims::ProcurementModel procurement;
            auto const fresh = procurement.execute(lims::CreateRequisition{
                .company = lims::test::seededCompany(), .site = lims::test::seededSite(), .reason = "round"});
            draft = procurement.execute(lims::AddRequisitionLine{.requisition = fresh.id,
                                                                 .item = setUp.stock.reagent,
                                                                 .quantity = lims::test::litres(1000),
                                                                 .unit = lims::StockUnit::litre,
                                                                 .estimate = setUp.draft.total});
        }
        auto const requisition = submitted(draft);
        ScopedClockOverride const clock{at(2026, 11, 10, 10)};
        std::atomic<int> approved{0};
        std::atomic<int> conflicts{0};
        std::vector<std::string> other;
        std::mutex otherMutex;
        auto const attempt = [&](std::string const& principal) {
            // Each thread has its own principal (the session is thread-local) and its own connection.
            try {
                ScopedPrincipal const who{principal};
                static_cast<void>(
                    lims::ProcurementModel{}.execute(lims::ApproveRequisition{.requisitionId = requisition.id}));
                ++approved;
            } catch (lims::Conflict const&) {
                ++conflicts;
            } catch (std::exception const& error) {
                std::scoped_lock const lock{otherMutex};
                other.emplace_back(error.what());
            }
        };
        std::thread first{attempt, std::string{"anna"}};
        std::thread second{attempt, std::string{"julia"}};
        first.join();
        second.join();
        INFO("round " << round << ", other failures: " << other.size());
        CHECK(approved == 1);
        CHECK(conflicts == 1);
        CHECK(other.empty());
    }
}

// Hypothesis 5. Mutation: keep the submitter in the model instance instead of the row (the approver on the new rig is
// told nothing was submitted).
TEST_CASE("Hypothesis 5: a requisition submitted on one rig is approved on another a day later",
          "[lims][inventory][requisition][h5]") {
    for (Mode const mode : {Mode::Local, Mode::LocalSingleThread, Mode::Socket}) {
        DYNAMIC_SECTION("mode " << static_cast<int>(mode)) {
            DbFixture fixture;
            auto const setUp = lims::test::requisitionSetUp();
            {
                ScopedClockOverride const clock{at(2026, 11, 10)};
                CHECK(lims::test::dispatchAs<lims::ProcurementModel>(
                          "rita", lims::SubmitRequisition{.requisitionId = setUp.draft.id}, mode)
                          .state == lims::RequisitionState::Submitted);
            }
            ScopedClockOverride const nextDay{at(2026, 11, 11)};
            auto const approved = lims::test::dispatchAs<lims::ProcurementModel>(
                "anna", lims::ApproveRequisition{.requisitionId = setUp.draft.id}, mode);
            CHECK(approved.state == lims::RequisitionState::Approved);
            CHECK(approved.submittedBy == "rita");
        }
    }
}

// The second time this is measured (decision 35 is the first): nothing tells an open inbox that a deadline passed.
// The witness is anna, a site approver, whose pending list loses the requisition at the deadline. The control is in
// the case: after the silence, a requisition raised through the server reaches the same connection, so the
// connection can hear a change of the record and the silence before it is the measurement. Mutation: watch a record
// name no record has ("Inventory_Requisitions"): the silence still holds, and the control's CHECK fails.
TEST_CASE("requisitions: an approver's open inbox is not told when a deadline passes",
          "[lims][inventory][requisition][effects]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    static_cast<void>(submitted(setUp.draft));
    BackendRig rig{Mode::Socket, 0, lims::test::limsAuthorizer(kSecret)};
    auto const anna = rig.connect();
    REQUIRE(anna->watch({"Inventory_Requisition"}, as("anna")).kind == "ok");
    {
        ScopedClockOverride const before{at(2026, 11, 11)};
        REQUIRE(pendingFor("anna") == std::vector<std::int64_t>{*setUp.draft.id});
    }
    auto const heardBefore = anna->changes().size();
    ScopedClockOverride const past{at(2026, 11, 12, 10)};
    bool const told = ServerConnection::eventually([&] { return anna->changes().size() > heardBefore; },
                                                   std::chrono::milliseconds{1000});
    CHECK_FALSE(told);
    CHECK(pendingFor("anna").empty());
    // The control: a change of the same record, made through the server after the deadline, is heard.
    auto const rita = rig.connect();
    auto const procurement = rita->registerModel("ProcurementModel", as("rita"));
    REQUIRE(procurement != 0);
    auto const body = glz::write_json(lims::CreateRequisition{.company = lims::test::seededCompany(),
                                                              .site = lims::test::seededSite(),
                                                              .reason = "control"})
                          .value_or(std::string{});
    REQUIRE(rita->execute(procurement, "ProcurementModel", "CreateRequisition", body, as("rita")).kind == "ok");
    CHECK(ServerConnection::eventually([&] { return anna->changes().size() > heardBefore; },
                                       std::chrono::milliseconds{1000}));
}

namespace {

/// @brief Clears the fault hook when the case ends, however it ends.
struct FaultHookGuard {
    FaultHookGuard() = default;
    FaultHookGuard(FaultHookGuard const&) = delete;
    FaultHookGuard& operator=(FaultHookGuard const&) = delete;
    FaultHookGuard(FaultHookGuard&&) = delete;
    FaultHookGuard& operator=(FaultHookGuard&&) = delete;
    ~FaultHookGuard() { lims::setInventoryFaultHook({}); }
};

/// @brief An amount of 2-decimal money.
/// @param cents The amount in hundredths.
/// @return The amount.
lims::Amount money(std::int64_t cents) {
    return lims::Amount{morph::math::Rational{morph::math::Numerator{cents}, morph::math::Denominator{100},
                                              morph::math::DecimalPlaces{2}}};
}

/// @brief What @p error says with the number @p id taken out, so two refusals about different ids compare.
/// @param error The failure.
/// @param id The id.
/// @return The words.
std::string withoutId(std::exception const& error, std::int64_t id) {
    std::string words = error.what();
    for (auto at = words.find(std::to_string(id)); at != std::string::npos; at = words.find(std::to_string(id))) {
        words.replace(at, std::to_string(id).size(), "N");
    }
    return words;
}

/// @brief The words of the refusal @p call raises.
/// @tparam Call Callable that must throw a `std::exception`.
/// @param call The call.
/// @param id The id the call names, taken out of the words.
/// @return The words.
template <typename Call>
std::string refusalOf(Call const& call, std::int64_t id) {
    try {
        call();
    } catch (std::exception const& error) {
        return withoutId(error, id);
    }
    return "no refusal";
}

/// @brief `kenji`, a purchase approver at a second company with a limit, who can see nothing of the seeded one.
/// @return The second company.
lims::CompanyId kenjiAtOsaka() {
    ScopedPrincipal const sam{"sam"};
    lims::OrgModel org;
    auto const company = org.execute(lims::CreateCompany{
        .name = "Rheinlab Japan", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});
    org.execute(lims::AssignRole{
        .principal = "kenji", .role = lims::LimsRole::PurchaseApprover, .company = company.id, .site = {}});
    org.execute(
        lims::SetValueLimit{.principal = "kenji",
                            .company = company.id,
                            .limit = lims::Amount{morph::math::Rational{1000000, morph::math::DecimalPlaces{0}}},
                            .role = lims::LimsRole::PurchaseApprover});
    return company.id;
}

}  // namespace

// Mutation: make Authorization<ApproveRequisition>::scopeOf name the requisition's own site for a requisition that
// exists (kenji's refusal on the real requisition then differs from his refusal on a missing id).
TEST_CASE("requisitions: a missing requisition and another company's are refused in the same words",
          "[lims][inventory][requisition]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    auto const requisition = submitted(setUp.draft);
    kenjiAtOsaka();
    ScopedClockOverride const clock{at(2026, 11, 10, 10)};
    ScopedPrincipal const kenji{"kenji"};
    constexpr std::int64_t kMissing = 9999;
    lims::ProcurementModel procurement;
    auto const approve = [&](std::int64_t id) {
        return [&procurement, id] {
            static_cast<void>(::morph::model::executeChecked(
                procurement, lims::ApproveRequisition{.requisitionId = lims::RequisitionId{id}}));
        };
    };
    auto const real = refusalOf(approve(*requisition.id), *requisition.id);
    CHECK_THAT(real,
               Catch::Matchers::ContainsSubstring("does not hold the purchase-approver role where this applies"));
    CHECK(real == refusalOf(approve(kMissing), kMissing));
    auto const get = [&](std::int64_t id) {
        return [&procurement, id] {
            static_cast<void>(procurement.execute(lims::GetRequisition{.requisitionId = lims::RequisitionId{id}}));
        };
    };
    auto const readReal = refusalOf(get(*requisition.id), *requisition.id);
    CHECK_THAT(readReal, Catch::Matchers::ContainsSubstring("does not exist"));
    CHECK(readReal == refusalOf(get(kMissing), kMissing));
    CHECK(procurement.execute(lims::ListRequisitions{.company = lims::test::seededCompany()}).requisitions.empty());
    CHECK(procurement.execute(lims::ListPendingRequisitions{}).requisitions.empty());
}

// Mutation: drop checkState from Authorization<ApproveRequisition> (anna, approving a draft, is told who raised it).
TEST_CASE(
    "requisitions: approving what is not submitted is an IllegalTransition for an approver, a refusal for others",
    "[lims][inventory][requisition]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    lims::ProcurementModel procurement;
    auto const approve = [&](std::string const& principal, lims::RequisitionView const& requisition) {
        ScopedPrincipal const who{principal};
        return ::morph::model::executeChecked(procurement, lims::ApproveRequisition{.requisitionId = requisition.id});
    };
    CHECK_THROWS_AS(approve("anna", setUp.draft), lims::IllegalTransition);
    CHECK_THROWS_AS(approve("rita", setUp.draft), lims::Forbidden);
    // An approver who raised it is told its state, not that she raised it: the state is asked before the pair.
    lims::test::grant("rita", lims::LimsRole::PurchaseApprover, lims::Scope{lims::test::seededSite()});
    {
        ScopedPrincipal const sam{"sam"};
        lims::OrgModel{}.execute(lims::SetValueLimit{.principal = "rita",
                                                     .company = lims::test::seededCompany(),
                                                     .limit = money(100000),
                                                     .role = lims::LimsRole::PurchaseApprover});
    }
    CHECK_THROWS_AS(approve("rita", setUp.draft), lims::IllegalTransition);
    auto const requisition = submitted(setUp.draft);
    ScopedClockOverride const clock{at(2026, 11, 10, 10)};
    CHECK(approve("anna", requisition).state == lims::RequisitionState::Approved);
    CHECK_THROWS_AS(approve("anna", requisition), lims::IllegalTransition);
    CHECK_THROWS_AS(approve("julia", requisition), lims::IllegalTransition);
}

// Mutation: drop the requester test from requireOwnDraft (colleague adds a line to and submits rita's draft).
TEST_CASE("requisitions: only the requester fills and submits a draft, and a submitted one is not changed",
          "[lims][inventory][requisition]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    lims::test::grant("colleague", lims::LimsRole::Analyst, lims::Scope{lims::test::seededSite()});
    lims::ProcurementModel procurement;
    {
        ScopedPrincipal const colleague{"colleague"};
        CHECK_THROWS_AS(procurement.execute(lims::SubmitRequisition{.requisitionId = setUp.draft.id}),
                        lims::Forbidden);
        CHECK_THROWS_AS(procurement.execute(lims::AddRequisitionLine{.requisition = setUp.draft.id,
                                                                     .item = setUp.stock.reagent,
                                                                     .quantity = lims::test::litres(1000),
                                                                     .unit = lims::StockUnit::litre,
                                                                     .estimate = money(100)}),
                        lims::Forbidden);
    }
    auto const requisition = submitted(setUp.draft);
    ScopedPrincipal const rita{"rita"};
    CHECK_THROWS_AS(procurement.execute(lims::SubmitRequisition{.requisitionId = requisition.id}),
                    lims::IllegalTransition);
    CHECK_THROWS_AS(procurement.execute(lims::AddRequisitionLine{.requisition = requisition.id,
                                                                 .item = setUp.stock.reagent,
                                                                 .quantity = lims::test::litres(1000),
                                                                 .unit = lims::StockUnit::litre,
                                                                 .estimate = money(100)}),
                    lims::IllegalTransition);
}

// Mutation: store the quantity as entered instead of converted (250 mL is stored as 250 L), and skip the sum (the
// total stays at the first estimate).
TEST_CASE("requisitions: a line's quantity is converted to the item's unit and its estimate adds to the total",
          "[lims][inventory][requisition]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    ScopedPrincipal const rita{"rita"};
    lims::ProcurementModel procurement;
    auto const view = procurement.execute(lims::AddRequisitionLine{.requisition = setUp.draft.id,
                                                                   .item = setUp.stock.reagent,
                                                                   .quantity = lims::test::millilitres(250),
                                                                   .unit = lims::StockUnit::millilitre,
                                                                   .estimate = money(1234)});
    REQUIRE(view.lines.size() == 2U);
    CHECK(view.lines.back().quantity == lims::test::litres(250));
    CHECK(view.lines.back().unit == lims::StockUnit::litre);
    CHECK(view.lines.back().itemCode == "NO3-R");
    CHECK(*view.total == morph::math::Rational{morph::math::Numerator{19234}, morph::math::Denominator{100},
                                               morph::math::DecimalPlaces{2}});
    CHECK(view.state == lims::RequisitionState::Draft);
    CHECK(view.level == lims::RequisitionLevel::Site);
}

TEST_CASE("requisitions: a malformed line or requisition is refused", "[lims][inventory][requisition]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    ScopedPrincipal const rita{"rita"};
    lims::ProcurementModel procurement;
    auto const add = [&](lims::Measure quantity, lims::StockUnit unit, lims::Amount estimate) {
        return procurement.execute(lims::AddRequisitionLine{.requisition = setUp.draft.id,
                                                            .item = setUp.stock.reagent,
                                                            .quantity = std::move(quantity),
                                                            .unit = unit,
                                                            .estimate = std::move(estimate)});
    };
    CHECK_THROWS_AS(add(lims::test::litres(0), lims::StockUnit::litre, money(100)), lims::ValidationError);
    CHECK_THROWS_AS(add(lims::test::litres(1000), lims::StockUnit::kilogram, money(100)), lims::ValidationError);
    CHECK_THROWS_AS(add(lims::test::litres(1000), lims::StockUnit::litre, money(0)), lims::ValidationError);
    CHECK_THROWS_AS(add(lims::test::litres(1000), lims::StockUnit::litre, money(-100)), lims::ValidationError);
    CHECK_THROWS_AS(add(lims::test::litres(1000), lims::StockUnit::litre,
                        lims::Amount{morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000},
                                                           morph::math::DecimalPlaces{3}}}),
                    lims::ValidationError);
    CHECK_THROWS_AS(procurement.execute(lims::CreateRequisition{
                        .company = lims::test::seededCompany(), .site = lims::test::seededSite(), .reason = ""}),
                    lims::ValidationError);
    CHECK_THROWS_AS(procurement.execute(lims::AddRequisitionLine{.requisition = setUp.draft.id,
                                                                 .item = lims::ItemId{9999},
                                                                 .quantity = lims::test::litres(1000),
                                                                 .unit = lims::StockUnit::litre,
                                                                 .estimate = money(100)}),
                    lims::NotFound);
    auto const empty = procurement.execute(lims::CreateRequisition{
        .company = lims::test::seededCompany(), .site = lims::test::seededSite(), .reason = "empty"});
    CHECK_THROWS_AS(procurement.execute(lims::SubmitRequisition{.requisitionId = empty.id}), lims::ValidationError);
}

// Mutation: drop the total from SubmitRequisition's conditional update (the submission records a total a line added
// meanwhile has already changed).
TEST_CASE("requisitions: a submission that raced a line is a Conflict", "[lims][inventory][requisition]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    FaultHookGuard const hookGuard;
    raceWith("UPDATE inventory_requisitions SET total_num = total_num + 100 WHERE id = " +
             std::to_string(*setUp.draft.id));
    CHECK_THROWS_AS(submitted(setUp.draft), lims::Conflict);
    ScopedPrincipal const rita{"rita"};
    CHECK(lims::ProcurementModel{}.execute(lims::GetRequisition{.requisitionId = setUp.draft.id}).state ==
          lims::RequisitionState::Draft);
}

TEST_CASE("requisitions: the requisition is raised at one of the company's sites", "[lims][inventory][requisition]") {
    DbFixture fixture;
    auto const setUp = lims::test::requisitionSetUp();
    auto const osaka = kenjiAtOsaka();
    ScopedPrincipal const rita{"rita"};
    lims::ProcurementModel procurement;
    CHECK_THROWS_AS(
        procurement.execute(lims::CreateRequisition{
            .company = lims::CompanyId{9999}, .site = lims::test::seededSite(), .reason = "no such company"}),
        lims::NotFound);
    CHECK_THROWS_AS(procurement.execute(lims::CreateRequisition{
                        .company = osaka, .site = lims::test::seededSite(), .reason = "wrong site"}),
                    lims::ValidationError);
    CHECK(setUp.draft.currency == lims::CurrencyCode::EUR);
    CHECK(setUp.draft.requestedBy == "rita");
    CHECK(setUp.draft.state == lims::RequisitionState::Draft);
}
