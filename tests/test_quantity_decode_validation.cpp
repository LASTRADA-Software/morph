// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>

#include "test_support.hpp"

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

// ---------------------------------------------------------------------------
// A miniature unit system where one unit (percent) declares bounds [0, 100]
// via the optional UnitTraits<E>::bounds(E) customisation point, and another
// (mass) declares none -- exercising both the opt-in check and its absence.
// ---------------------------------------------------------------------------

enum class QDVUnit : std::uint8_t { percent, mass };

template <>
struct morph::units::UnitTraits<QDVUnit> {
    static constexpr morph::units::UnitMeta meta(QDVUnit unit) noexcept {
        switch (unit) {
            case QDVUnit::percent:
                return {.id = "percent", .display = "%", .defaultDecimals = 1};
            case QDVUnit::mass:
                return {.id = "mass", .display = "kg", .defaultDecimals = 3};
            default:
                return {.id = "?", .display = "?", .defaultDecimals = 1};
        }
    }

    // Only `percent` declares bounds; `mass` intentionally declares none, so
    // HasUnitBounds<QDVUnit> is still true overall (bounds() is defined for
    // the enum) but the mass branch below returns a wide-open range,
    // matching "the unit declares none" behaviour per-value rather than
    // per-enum. See the per-unit test below.
    static constexpr morph::units::QuantityBounds bounds(QDVUnit unit) noexcept {
        switch (unit) {
            case QDVUnit::percent:
                return {.min = Rational{Numerator{0}, Denominator{1}, DecimalPlaces{1}},
                        .max = Rational{Numerator{100}, Denominator{1}, DecimalPlaces{1}}};
            default:
                return {.min = Rational{Numerator{-1'000'000'000}, Denominator{1}, DecimalPlaces{1}},
                        .max = Rational{Numerator{1'000'000'000}, Denominator{1}, DecimalPlaces{1}}};
        }
    }
};

using Percent = morph::units::Quantity<QDVUnit::percent>;
using Mass = morph::units::Quantity<QDVUnit::mass>;

static_assert(morph::units::HasUnitBounds<QDVUnit>);

// A unit system with NO bounds() at all, to confirm the opt-in default.
enum class QDVNoBoundsUnit : std::uint8_t { scalar };

template <>
struct morph::units::UnitTraits<QDVNoBoundsUnit> {
    static constexpr morph::units::UnitMeta meta(QDVNoBoundsUnit) noexcept {
        return {.id = "scalar", .display = "", .defaultDecimals = 2};
    }
};

using UnboundedScalar = morph::units::Quantity<QDVNoBoundsUnit::scalar>;

static_assert(!morph::units::HasUnitBounds<QDVNoBoundsUnit>);

// ---------------------------------------------------------------------------
// Quantity::withinDeclaredBounds() -- the type-level check.
// ---------------------------------------------------------------------------

TEST_CASE("Quantity::WithinDeclaredBounds::EmptyIsAlwaysWithinBounds", "[quantity][decode]") {
    Percent const empty{};
    CHECK(empty.withinDeclaredBounds());
}

TEST_CASE("Quantity::WithinDeclaredBounds::InRangeValuePasses", "[quantity][decode]") {
    Percent const p{Rational{Numerator{50}, Denominator{1}, DecimalPlaces{1}}};
    CHECK(p.withinDeclaredBounds());

    Percent const atMin{Rational{Numerator{0}, Denominator{1}, DecimalPlaces{1}}};
    CHECK(atMin.withinDeclaredBounds());  // inclusive lower bound

    Percent const atMax{Rational{Numerator{100}, Denominator{1}, DecimalPlaces{1}}};
    CHECK(atMax.withinDeclaredBounds());  // inclusive upper bound
}

TEST_CASE("Quantity::WithinDeclaredBounds::OutOfRangeValueFails", "[quantity][decode]") {
    Percent const tooHigh{Rational{Numerator{101}, Denominator{1}, DecimalPlaces{1}}};
    CHECK_FALSE(tooHigh.withinDeclaredBounds());

    Percent const negative{Rational{Numerator{-1}, Denominator{1}, DecimalPlaces{1}}};
    CHECK_FALSE(negative.withinDeclaredBounds());
}

TEST_CASE("Quantity::WithinDeclaredBounds::NoBoundsDeclaredAlwaysPasses", "[quantity][decode]") {
    UnboundedScalar const huge{Rational{Numerator{999'999'999}, Denominator{1}, DecimalPlaces{2}}};
    CHECK(huge.withinDeclaredBounds());
    UnboundedScalar const negative{Rational{Numerator{-999'999'999}, Denominator{1}, DecimalPlaces{2}}};
    CHECK(negative.withinDeclaredBounds());
}

// ---------------------------------------------------------------------------
// morph::forms::checkQuantityBounds<A> / enforceQuantityBounds<A>.
// ---------------------------------------------------------------------------

struct QDVReading {
    Percent moisture;
    Mass sampleMass;
    std::optional<std::string> note;

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

TEST_CASE("Forms::CheckQuantityBounds::NoOffenderWhenEverythingWithinBounds", "[forms][decode]") {
    QDVReading reading{.moisture = Percent{Rational{Numerator{45}, Denominator{1}, DecimalPlaces{1}}},
                       .sampleMass = Mass{Rational{Numerator{10}, Denominator{1}, DecimalPlaces{1}}}};
    CHECK_FALSE(morph::forms::checkQuantityBounds(reading).has_value());
}

TEST_CASE("Forms::CheckQuantityBounds::ReportsFirstOffendingFieldName", "[forms][decode]") {
    QDVReading reading{.moisture = Percent{Rational{Numerator{250}, Denominator{1}, DecimalPlaces{1}}}};
    auto const offender = morph::forms::checkQuantityBounds(reading);
    REQUIRE(offender.has_value());
    CHECK(*offender == "moisture");
}

TEST_CASE("Forms::CheckQuantityBounds::EmptyFieldsNeverOffend", "[forms][decode]") {
    QDVReading const reading{};  // both Quantity members empty
    CHECK_FALSE(morph::forms::checkQuantityBounds(reading).has_value());
}

TEST_CASE("Forms::EnforceQuantityBounds::ThrowsQuantityDecodeErrorNamingTheField", "[forms][decode]") {
    QDVReading reading{.moisture = Percent{Rational{Numerator{-5}, Denominator{1}, DecimalPlaces{1}}}};
    REQUIRE_THROWS_AS(morph::forms::enforceQuantityBounds(reading), morph::forms::QuantityDecodeError);
    try {
        morph::forms::enforceQuantityBounds(reading);
        FAIL("expected QuantityDecodeError");
    } catch (const morph::forms::QuantityDecodeError& err) {
        CHECK(std::string{err.what()}.find("moisture") != std::string::npos);
    }
}

TEST_CASE("Forms::EnforceQuantityBounds::NoOpWhenWithinBounds", "[forms][decode]") {
    QDVReading reading{.moisture = Percent{Rational{Numerator{50}, Denominator{1}, DecimalPlaces{1}}}};
    CHECK_NOTHROW(morph::forms::enforceQuantityBounds(reading));
}

// ---------------------------------------------------------------------------
// No-drift: the decode-level rejection happens before validate() runs, on the
// server dispatch path (ActionDispatcher) and the client bridge dispatch path
// (ActionExecuteRegistry via BridgeHandler::executeJson) -- distinct from a
// ValidationError, which stays reserved for validate()'s own business rules.
// ---------------------------------------------------------------------------

struct QDVResult {
    bool accepted = false;
};

struct QDVModel {
    QDVResult execute(const QDVReading&) { return QDVResult{.accepted = true}; }
};

BRIDGE_REGISTER_MODEL(QDVModel, "QDV_Model")
BRIDGE_REGISTER_ACTION(QDVModel, QDVReading, "QDV_Reading")

TEST_CASE("Forms::NoDrift::ActionDispatcherRejectsOutOfBoundsQuantityAsDecodeError", "[forms][decode][dispatch]") {
    auto holder = morph::model::detail::ModelFactory::create<QDVModel>();
    QDVReading const badReading{.moisture = Percent{Rational{Numerator{250}, Denominator{1}, DecimalPlaces{1}}}};
    auto const payload = morph::model::ActionTraits<QDVReading>::toJson(badReading);

    REQUIRE_THROWS_AS(
        morph::model::detail::ActionDispatcher::instance().dispatch("QDV_Model", "QDV_Reading", *holder, payload),
        morph::forms::QuantityDecodeError);
}

TEST_CASE("Forms::NoDrift::ActionDispatcherAcceptsInBoundsQuantity", "[forms][decode][dispatch]") {
    auto holder = morph::model::detail::ModelFactory::create<QDVModel>();
    QDVReading const goodReading{.moisture = Percent{Rational{Numerator{50}, Denominator{1}, DecimalPlaces{1}}},
                                 .sampleMass = Mass{Rational{Numerator{10}, Denominator{1}, DecimalPlaces{1}}}};
    auto const payload = morph::model::ActionTraits<QDVReading>::toJson(goodReading);

    auto const resultJson =
        morph::model::detail::ActionDispatcher::instance().dispatch("QDV_Model", "QDV_Reading", *holder, payload);
    auto const result = morph::model::ActionTraits<QDVReading>::resultFromJson(resultJson);
    CHECK(result.accepted);
}

TEST_CASE("Forms::NoDrift::LocalBackendRejectsOutOfBoundsQuantityViaOnError", "[forms][decode][dispatch]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::testing::InlineExecutor cbExec;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    morph::bridge::BridgeHandler<QDVModel> handler{bridge, &cbExec};

    QDVReading const badReading{.moisture = Percent{Rational{Numerator{-10}, Denominator{1}, DecimalPlaces{1}}}};
    auto const payload = morph::model::ActionTraits<QDVReading>::toJson(badReading);

    std::atomic<bool> sawDecodeError{false};
    std::atomic<bool> done{false};
    handler.executeJson("QDV_Reading", payload)
        .then([&](std::string) { done.store(true); })
        .onError([&](const std::exception_ptr& err) {
            try {
                std::rethrow_exception(err);
            } catch (const morph::forms::QuantityDecodeError&) {
                sawDecodeError.store(true);
            } catch (...) {
            }
            done.store(true);
        });

    REQUIRE(morph::testing::waitUntil([&] { return done.load(); }));
    REQUIRE(sawDecodeError.load());
}
