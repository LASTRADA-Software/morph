// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <mutex>
#include <string_view>
#include <thread>

#include "test_support.hpp"

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

// ---------------------------------------------------------------------------
// A miniature unit system: qty * price = total. Reused by every task in this
// file (Tasks 2-4 append models/actions built on the same CFQ/CFLineItem).
// ---------------------------------------------------------------------------

// Named CFLineUnit, not CFUnit: test_forms_conformance_corpus.cpp's own
// unrelated CFUnit is also file-scope (not anonymous-namespaced, deliberately
// -- see that file's comment), so an identically-named enum here would be an
// ODR violation (two conflicting definitions of the same external-linkage
// symbol across translation units, silently resolved to just one of them by
// the linker) rather than a harmless prefix collision.
enum class CFLineUnit : std::uint8_t { qty, price, total };

template <>
struct morph::units::UnitTraits<CFLineUnit> {
    static constexpr morph::units::UnitMeta meta(CFLineUnit unit) noexcept {
        switch (unit) {
            case CFLineUnit::qty:
                return {.id = "qty", .display = "qty", .defaultDecimals = 2};
            case CFLineUnit::price:
                return {.id = "price", .display = "$/u", .defaultDecimals = 2};
            case CFLineUnit::total:
                return {.id = "total", .display = "$", .defaultDecimals = 2};
            default:
                return {.id = "?", .display = "?", .defaultDecimals = 2};
        }
    }
    static constexpr std::array<morph::units::UnitRelation<CFLineUnit>, 0> relations{};
};

consteval CFLineUnit operator*(CFLineUnit lhs, CFLineUnit rhs) {
    if ((lhs == CFLineUnit::qty && rhs == CFLineUnit::price) || (lhs == CFLineUnit::price && rhs == CFLineUnit::qty)) {
        return CFLineUnit::total;
    }
    throw "unsupported unit product";
}

template <CFLineUnit U, std::uint32_t Dec = morph::units::UnitTraits<CFLineUnit>::meta(U).defaultDecimals>
using CFQ = morph::units::Quantity<U, Dec>;

namespace {
constexpr DecimalPlaces dp2{2};
constexpr DecimalPlaces dp4{4};
}  // namespace

// ---------------------------------------------------------------------------
// Fixture: an action with one computed field, total = qty * price.
// ---------------------------------------------------------------------------

struct CFLineItem {
    CFQ<CFLineUnit::qty> qty;
    CFQ<CFLineUnit::price> price;
    CFQ<CFLineUnit::total> total;

    // A generic (auto) lambda parameter, not `const CFLineItem&`: this
    // initializer runs while CFLineItem is still incomplete (a static data
    // member initializer is not a complete-class context the way a
    // non-static default member initializer or member function body is), so
    // the lambda body's member access must stay dependent until the first
    // call -- which happens later, once the class is complete.
    static constexpr auto computedFields =
        morph::forms::computeList(morph::forms::computed<&CFLineItem::total, &CFLineItem::qty, &CFLineItem::price>(
            [](const auto& s) { return s.qty * s.price; }));

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

// A destination whose declared precision (4) overrides the unit default (2),
// to prove the result is retagged to the *destination's* declared precision,
// not the multiplication result's.
struct CFPreciseLineItem {
    CFQ<CFLineUnit::qty> qty;
    CFQ<CFLineUnit::price> price;
    CFQ<CFLineUnit::total, 4> total;

    static constexpr auto computedFields = morph::forms::computeList(
        morph::forms::computed<&CFPreciseLineItem::total, &CFPreciseLineItem::qty, &CFPreciseLineItem::price>(
            [](const auto& s) { return s.qty * s.price; }));
};

// An action with no computedFields, to prove recomputeAll/allRequiredEngaged
// (and, from Task 2, mergeSchemaExtras) are true no-ops for it.
struct CFPlainAction {
    CFQ<CFLineUnit::qty> qty;
    CFQ<CFLineUnit::price> price;
};

static_assert(morph::forms::detail::HasComputedFields<CFLineItem>);
static_assert(!morph::forms::detail::HasComputedFields<CFPlainAction>);

TEST_CASE("recomputeAll writes qty * price into total", "[forms][computed]") {
    CFLineItem item{};
    item.qty = Rational{Numerator{3}, Denominator{1}, dp2};
    item.price = Rational{Numerator{250}, Denominator{100}, dp2};  // 2.50

    morph::forms::recomputeAll(item);

    REQUIRE(item.total.hasValue());
    CHECK(*item.total == Rational{Numerator{15}, Denominator{2}, dp2});  // 3 * 2.50 = 7.50
}

TEST_CASE("recomputeAll leaves the destination unengaged when an input is unengaged", "[forms][computed]") {
    CFLineItem item{};
    item.qty = Rational{Numerator{3}, Denominator{1}, dp2};
    // price left empty.

    morph::forms::recomputeAll(item);

    CHECK_FALSE(item.total.hasValue());
}

TEST_CASE("recomputeAll overwrites a stale total when an input changes", "[forms][computed]") {
    CFLineItem item{};
    item.qty = Rational{Numerator{2}, Denominator{1}, dp2};
    item.price = Rational{Numerator{5}, Denominator{1}, dp2};
    morph::forms::recomputeAll(item);
    REQUIRE(*item.total == Rational{10, dp2});

    // A tampered/stale total is discarded and re-derived on the next recompute.
    item.total = Rational{Numerator{999}, Denominator{1}, dp2};
    item.qty = Rational{Numerator{4}, Denominator{1}, dp2};
    morph::forms::recomputeAll(item);
    CHECK(*item.total == Rational{20, dp2});
}

TEST_CASE("recomputeAll retags the result to the destination's declared precision", "[forms][computed]") {
    CFPreciseLineItem item{};
    item.qty = Rational{Numerator{3}, Denominator{1}, dp2};
    item.price = Rational{Numerator{2}, Denominator{1}, dp2};

    morph::forms::recomputeAll(item);

    REQUIRE(item.total.hasValue());
    CHECK(*item.total == Rational{6, dp2});
    CHECK((*item.total).getDecimalPlaces() == dp4);
}

TEST_CASE("recomputeAll is a no-op for an action with no computedFields", "[forms][computed]") {
    CFPlainAction action{};
    action.qty = Rational{Numerator{1}, Denominator{1}, dp2};
    action.price = Rational{Numerator{2}, Denominator{1}, dp2};
    morph::forms::recomputeAll(action);  // must compile and do nothing
    CHECK(*action.qty == Rational{1, dp2});
    CHECK(*action.price == Rational{2, dp2});
}

TEST_CASE("allRequiredEngaged does not require a computed destination to already be engaged", "[forms][computed]") {
    CFLineItem item{};
    item.qty = Rational{Numerator{3}, Denominator{1}, dp2};
    item.price = Rational{Numerator{2}, Denominator{1}, dp2};
    // total is still empty -- recomputeAll has not run yet.
    CHECK_FALSE(item.total.hasValue());
    CHECK(morph::forms::allRequiredEngaged(item));  // total is computed, not required

    CFLineItem missingPrice{};
    missingPrice.qty = Rational{Numerator{3}, Denominator{1}, dp2};
    CHECK_FALSE(morph::forms::allRequiredEngaged(missingPrice));  // price is a real required field
}

// ---------------------------------------------------------------------------
// Schema emission: x-computed / x-readonly, and exclusion from `required`.
// ---------------------------------------------------------------------------

TEST_CASE("schemaJson emits x-computed and x-readonly on the destination property", "[forms][computed]") {
    auto const schema = morph::forms::schemaJson<CFLineItem>();
    REQUIRE_FALSE(schema.empty());

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    CHECK(schema.contains(R"("x-readonly":true)"));
    CHECK(schema.contains(R"("x-computed":{"inputs":["qty","price"]})"));
}

TEST_CASE("schemaJson excludes a computed field from required", "[forms][computed]") {
    auto const schema = morph::forms::schemaJson<CFLineItem>();
    CHECK(schema.contains(R"("required":["qty","price"])"));
    CHECK_FALSE(schema.contains(R"("required":["qty","price","total"])"));
}

TEST_CASE("schemaJson emits neither key for an action with no computedFields", "[forms][computed]") {
    auto const schema = morph::forms::schemaJson<CFPlainAction>();
    CHECK_FALSE(schema.contains("x-computed"));
    CHECK_FALSE(schema.contains("x-readonly"));
    CHECK(schema.contains(R"("required":["qty","price"])"));
}

// ---------------------------------------------------------------------------
// Client reactive path: BridgeHandler::set<> recomputes live before firing.
// ---------------------------------------------------------------------------

struct CFModel {
    CFLineItem execute(const CFLineItem& action) { return action; }
};

BRIDGE_REGISTER_MODEL(CFModel, "Test_CF_Model")
BRIDGE_REGISTER_ACTION(CFModel, CFLineItem, "Test_CF_LineItem")

using SyncExecutor = morph::testing::InlineExecutor;

TEST_CASE("BridgeHandler::set<> recomputes total live before firing", "[bridge][computed]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CFModel> handler{bridge, &cbExec};

    std::mutex totalMtx;
    std::atomic<bool> haveTotal{false};
    Rational lastTotal{0, dp2};
    handler.subscribe<CFLineItem>([&](CFLineItem result) {
        std::scoped_lock lock{totalMtx};
        if (result.total.hasValue()) {
            lastTotal = *result.total;
            haveTotal.store(true);
        }
    });

    // `total` is left unengaged on purpose: recomputeAll overwrites it from
    // qty*price on the dispatch path, which is what this asserts.
    handler.execute(CFLineItem{.qty = Rational{Numerator{3}, Denominator{1}, dp2},
                               .price = Rational{Numerator{2}, Denominator{1}, dp2},
                               .total = {}});

    REQUIRE(morph::testing::waitUntil([&] { return haveTotal.load(); }));
    std::scoped_lock lock{totalMtx};
    CHECK(lastTotal == Rational{6, dp2});
}

TEST_CASE("an action with a computed input missing fails its validator", "[bridge][computed]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CFModel> handler{bridge, &cbExec};

    std::atomic<bool> fired{false};
    std::atomic<bool> failed{false};
    handler.subscribe<CFLineItem>([&](CFLineItem /*unused*/) { fired.store(true); });

    // price is missing, so total stays unengaged and validate() is false. The
    // validator gate now sits on the dispatch path rather than in a client-side
    // draft, so the action is rejected instead of simply never firing -- and a
    // failed action notifies no subscriber.
    handler.execute(CFLineItem{.qty = Rational{Numerator{3}, Denominator{1}, dp2}, .price = {}, .total = {}})
        .onError([&](const std::exception_ptr&) { failed.store(true); });

    REQUIRE(morph::testing::waitUntil([&] { return failed.load(); }));
    CHECK_FALSE(fired.load());
}

// ---------------------------------------------------------------------------
// Authoritative server-side recompute: a tampered wire value is discarded on
// every dispatch path, before Model::execute runs.
// ---------------------------------------------------------------------------

struct CFPlainModel {
    CFPlainAction execute(const CFPlainAction& action) { return action; }
};

BRIDGE_REGISTER_MODEL(CFPlainModel, "Test_CF_PlainModel")
BRIDGE_REGISTER_ACTION(CFPlainModel, CFPlainAction, "Test_CF_PlainAction")

TEST_CASE("ActionDispatcher's runner overwrites a tampered computed field before Model::execute",
          "[registry][computed]") {
    auto holder = morph::model::detail::ModelFactory::create<CFModel>();
    // qty=3, price=2 => true total = 6.00; the wire body lies and claims total=999.00.
    auto const resultJson = morph::model::detail::ActionDispatcher::instance().dispatch(
        "Test_CF_Model", "Test_CF_LineItem", *holder,
        R"({"qty":{"num":3,"den":1,"dp":2},"price":{"num":2,"den":1,"dp":2},)"
        R"("total":{"num":99900,"den":100,"dp":2}})");

    auto const result = morph::model::ActionTraits<CFLineItem>::resultFromJson(resultJson);
    REQUIRE(result.total.hasValue());
    CHECK(*result.total == Rational{6, dp2});
}

TEST_CASE("ActionDispatcher's runner reconciles declared Quantity precision too", "[registry][computed]") {
    auto holder = morph::model::detail::ModelFactory::create<CFPlainModel>();
    // qty's declared precision is 2; the wire claims dp:5.
    auto const resultJson = morph::model::detail::ActionDispatcher::instance().dispatch(
        "Test_CF_PlainModel", "Test_CF_PlainAction", *holder,
        R"({"qty":{"num":314,"den":100,"dp":5},"price":{"num":1,"den":1,"dp":2}})");

    auto const result = morph::model::ActionTraits<CFPlainAction>::resultFromJson(resultJson);
    REQUIRE(result.qty.hasValue());
    CHECK((*result.qty).getDecimalPlaces() == dp2);  // declared precision, not the wire's dp:5
}

TEST_CASE("ActionDispatcher's runner dispatches an action with no computedFields unchanged", "[registry][computed]") {
    auto holder = morph::model::detail::ModelFactory::create<CFPlainModel>();
    auto const resultJson = morph::model::detail::ActionDispatcher::instance().dispatch(
        "Test_CF_PlainModel", "Test_CF_PlainAction", *holder,
        R"({"qty":{"num":5,"den":1,"dp":2},"price":{"num":7,"den":1,"dp":2}})");
    auto const result = morph::model::ActionTraits<CFPlainAction>::resultFromJson(resultJson);
    REQUIRE(result.qty.hasValue());
    CHECK(*result.qty == Rational{5, dp2});
    CHECK(*result.price == Rational{7, dp2});
}

TEST_CASE("Bridge::executeVia's localOp overwrites a tampered computed field on LocalBackend",
          "[bridge][local][computed]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CFModel> handler{bridge, &cbExec};

    CFLineItem tampered{};
    tampered.qty = Rational{Numerator{3}, Denominator{1}, dp2};
    tampered.price = Rational{Numerator{2}, Denominator{1}, dp2};
    tampered.total = Rational{Numerator{99900}, Denominator{100}, dp2};  // hand-built, wrong

    std::atomic<bool> done{false};
    Rational observedTotal{0, dp2};
    handler.execute(tampered)
        .then([&](CFLineItem result) {
            if (result.total.hasValue()) {
                observedTotal = *result.total;
            }
            done.store(true);
        })
        .onError([&](const std::exception_ptr&) { done.store(true); });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    CHECK(observedTotal == Rational{6, dp2});
}

TEST_CASE("BridgeHandler::executeJson overwrites a tampered computed field before dispatch", "[bridge][computed]") {
    morph::exec::ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<CFModel> handler{bridge, &cbExec};

    std::atomic<bool> done{false};
    std::string resultJson;
    handler
        .executeJson("Test_CF_LineItem", R"({"qty":{"num":3,"den":1,"dp":2},"price":{"num":2,"den":1,"dp":2},)"
                                         R"("total":{"num":99900,"den":100,"dp":2}})")
        .then([&](std::string json) {
            resultJson = std::move(json);
            done.store(true);
        })
        .onError([&](const std::exception_ptr&) { done.store(true); });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    auto const result = morph::model::ActionTraits<CFLineItem>::resultFromJson(resultJson);
    REQUIRE(result.total.hasValue());
    CHECK(*result.total == Rational{6, dp2});
}

TEST_CASE("SimulatedRemoteBackend overwrites a tampered computed field before Model::execute",
          "[bridge][remote][computed]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    SyncExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server)};
    morph::bridge::BridgeHandler<CFModel> handler{bridge, &cbExec};

    CFLineItem tampered{};
    tampered.qty = Rational{Numerator{4}, Denominator{1}, dp2};
    tampered.price = Rational{Numerator{5}, Denominator{1}, dp2};
    tampered.total = Rational{Numerator{100000}, Denominator{100}, dp2};  // hand-built; true = 20.00

    std::atomic<bool> done{false};
    Rational observedTotal{0, dp2};
    handler.execute(tampered)
        .then([&](CFLineItem result) {
            if (result.total.hasValue()) {
                observedTotal = *result.total;
            }
            done.store(true);
        })
        .onError([&](const std::exception_ptr&) { done.store(true); });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }, std::chrono::milliseconds{4000}));
    CHECK(observedTotal == Rational{20, dp2});
}
