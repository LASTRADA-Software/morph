// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/forms/forms.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <string_view>

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

// ---------------------------------------------------------------------------
// A miniature unit system: qty * price = total. Reused by every task in this
// file (Tasks 2-4 append models/actions built on the same CFQ/CFLineItem).
// ---------------------------------------------------------------------------

enum class CFUnit : std::uint8_t { qty, price, total };

template <>
struct morph::units::UnitTraits<CFUnit> {
    static constexpr morph::units::UnitMeta meta(CFUnit unit) noexcept {
        switch (unit) {
            case CFUnit::qty:
                return {.id = "qty", .display = "qty", .defaultDecimals = 2};
            case CFUnit::price:
                return {.id = "price", .display = "$/u", .defaultDecimals = 2};
            case CFUnit::total:
                return {.id = "total", .display = "$", .defaultDecimals = 2};
            default:
                return {.id = "?", .display = "?", .defaultDecimals = 2};
        }
    }
    static constexpr std::array<morph::units::UnitRelation<CFUnit>, 0> relations{};
};

consteval CFUnit operator*(CFUnit lhs, CFUnit rhs) {
    if ((lhs == CFUnit::qty && rhs == CFUnit::price) || (lhs == CFUnit::price && rhs == CFUnit::qty)) {
        return CFUnit::total;
    }
    throw "unsupported unit product";
}

template <CFUnit U, std::uint32_t Dec = morph::units::UnitTraits<CFUnit>::meta(U).defaultDecimals>
using CFQ = morph::units::Quantity<U, Dec>;

namespace {
constexpr DecimalPlaces dp2{2};
constexpr DecimalPlaces dp4{4};
}  // namespace

// ---------------------------------------------------------------------------
// Fixture: an action with one computed field, total = qty * price.
// ---------------------------------------------------------------------------

struct CFLineItem {
    CFQ<CFUnit::qty> qty;
    CFQ<CFUnit::price> price;
    CFQ<CFUnit::total> total;

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
    CFQ<CFUnit::qty> qty;
    CFQ<CFUnit::price> price;
    CFQ<CFUnit::total, 4> total;

    static constexpr auto computedFields = morph::forms::computeList(
        morph::forms::computed<&CFPreciseLineItem::total, &CFPreciseLineItem::qty, &CFPreciseLineItem::price>(
            [](const auto& s) { return s.qty * s.price; }));
};

// An action with no computedFields, to prove recomputeAll/allRequiredEngaged
// (and, from Task 2, mergeSchemaExtras) are true no-ops for it.
struct CFPlainAction {
    CFQ<CFUnit::qty> qty;
    CFQ<CFUnit::price> price;
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
