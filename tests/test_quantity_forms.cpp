// SPDX-License-Identifier: Apache-2.0

#include <morph/forms.hpp>
#include <morph/quantity.hpp>
#include <morph/rational.hpp>
#include <morph/registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using morph::math::DecimalPlaces;
using morph::math::Rational;

// ---------------------------------------------------------------------------
// A miniature application unit system: enum + UnitTraits + consteval algebra.
// ---------------------------------------------------------------------------

enum class QFUnit : std::uint16_t { scalar, percent, kg, m3, kg_per_m3 };

template <>
struct morph::units::UnitTraits<QFUnit> {
    static constexpr morph::units::UnitMeta meta(QFUnit unit) noexcept {
        switch (unit) {
            case QFUnit::scalar:    return {"scalar", "", 3};
            case QFUnit::percent:   return {"percent", "%", 1};
            case QFUnit::kg:        return {"kg", "kg", 3};
            case QFUnit::m3:        return {"m3", "m³", 3};
            case QFUnit::kg_per_m3: return {"kg_per_m3", "kg/m³", 1};
        }
        return {"?", "?", 3};
    }
};

consteval QFUnit operator*(QFUnit lhs, QFUnit rhs) {
    if (lhs == QFUnit::scalar) return rhs;
    if (rhs == QFUnit::scalar) return lhs;
    if ((lhs == QFUnit::kg_per_m3 && rhs == QFUnit::m3) || (lhs == QFUnit::m3 && rhs == QFUnit::kg_per_m3)) {
        return QFUnit::kg;
    }
    throw "unsupported unit product";
}

consteval QFUnit operator/(QFUnit lhs, QFUnit rhs) {
    if (rhs == QFUnit::scalar) return lhs;
    if (lhs == rhs) return QFUnit::scalar;
    if (lhs == QFUnit::kg && rhs == QFUnit::m3) return QFUnit::kg_per_m3;
    throw "unsupported unit quotient";
}

template <QFUnit U>
using Q = morph::units::Quantity<U>;

namespace {

constexpr DecimalPlaces dp1{1};
constexpr DecimalPlaces dp3{3};

[[nodiscard]] Q<QFUnit::kg> kilograms(std::int64_t num, std::int64_t den = 1) {
    return {Rational{num, den, dp3}};
}

[[nodiscard]] Q<QFUnit::m3> cubicMetres(std::int64_t num, std::int64_t den = 1) {
    return {Rational{num, den, dp3}};
}

// Compile-time facts: unit algebra deduction and the quantity trait.
static_assert(std::same_as<decltype(kilograms(1) / cubicMetres(1)), Q<QFUnit::kg_per_m3>>);
static_assert(std::same_as<decltype((kilograms(1) / cubicMetres(1)) * cubicMetres(1)), Q<QFUnit::kg>>);
static_assert(std::same_as<decltype(kilograms(1) / kilograms(1)), Q<QFUnit::scalar>>);
static_assert(morph::units::is_quantity_v<Q<QFUnit::kg>>);
static_assert(!morph::units::is_quantity_v<Rational>);
static_assert(!morph::units::is_quantity_v<std::optional<Q<QFUnit::kg>>>);

// Declared precision: defaults from UnitTraits, overridable per field, and
// binary results fall back to the unit default (a computed temporary is not
// a declared field).
using PreciseMass = morph::units::Quantity<QFUnit::kg, 5>;
static_assert(Q<QFUnit::kg>::declaredDecimals == 3);
static_assert(Q<QFUnit::percent>::declaredDecimals == 1);
static_assert(PreciseMass::declaredDecimals == 5);
static_assert(morph::units::is_quantity_v<PreciseMass>);
static_assert(std::same_as<decltype(std::declval<PreciseMass>() + std::declval<Q<QFUnit::kg>>()), Q<QFUnit::kg>>);
static_assert(std::same_as<decltype(std::declval<PreciseMass>() / std::declval<Q<QFUnit::m3>>()),
                           Q<QFUnit::kg_per_m3>>);
static_assert(std::same_as<decltype(-std::declval<PreciseMass>()), PreciseMass>);

}  // namespace

// ---------------------------------------------------------------------------
// Action + model exercising the registry integration end to end.
// ---------------------------------------------------------------------------

struct QFRecordMeasurement {
    std::int64_t sampleId = 0;
    Q<QFUnit::kg_per_m3> density{};
    Q<QFUnit::percent> moisture{};
    std::optional<std::string> note{};

    static constexpr std::array optionalFields{std::string_view{"moisture"}};

    [[nodiscard]] bool validate() const { return sampleId > 0 && morph::forms::allRequiredEngaged(*this); }
};

struct QFComputeDryDensity {
    Q<QFUnit::kg> massDry{};
    Q<QFUnit::m3> volume{};

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

struct QFCalibrate {
    morph::units::Quantity<QFUnit::kg, 5> referenceMass{};
};

struct QFLabModel {
    Q<QFUnit::kg_per_m3> execute(const QFComputeDryDensity& action) { return action.massDry / action.volume; }
    std::int64_t execute(const QFRecordMeasurement& action) { return action.sampleId; }
    bool execute(const QFCalibrate& action) { return action.referenceMass.hasValue(); }
};

BRIDGE_REGISTER_MODEL(QFLabModel, "QFLabModel")
BRIDGE_REGISTER_ACTION(QFLabModel, QFComputeDryDensity, "QFComputeDryDensity")
BRIDGE_REGISTER_ACTION(QFLabModel, QFRecordMeasurement, "QFRecordMeasurement")
BRIDGE_REGISTER_ACTION(QFLabModel, QFCalibrate, "QFCalibrate")

// ---------------------------------------------------------------------------
// Arithmetic semantics.
// ---------------------------------------------------------------------------

TEST_CASE("Quantity::Arithmetic::ExactWithPrecisionPropagation", "[quantity]") {
    auto const mass = Q<QFUnit::kg>{Rational{26505, 10, dp1}};
    auto const volume = cubicMetres(1);

    auto const density = mass / volume;
    REQUIRE(density.hasValue());
    CHECK(*density == Rational{5301, 2, dp1});
    CHECK((*density).GetDecimalPlaces() == dp3);  // max(1, 3) propagates

    auto const massBack = density * volume;
    REQUIRE(massBack.hasValue());
    CHECK(*massBack == *mass);

    CHECK(*(kilograms(3) + kilograms(4)) == Rational{7, dp3});
    CHECK(*(kilograms(3) - kilograms(4)) == Rational{-1, dp3});
    CHECK(*(-kilograms(3)) == Rational{-3, dp3});

    // Dimensionless scaling keeps the unit.
    auto const scaled = kilograms(3) * Rational{2, dp1};
    static_assert(std::same_as<decltype(scaled), const Q<QFUnit::kg>>);
    CHECK(*scaled == Rational{6, dp3});
    CHECK(*(Rational{2, dp1} * kilograms(3)) == Rational{6, dp3});
    CHECK(*(kilograms(3) / Rational{2, dp1}) == Rational{3, 2, dp3});
}

TEST_CASE("Quantity::Arithmetic::EmptyPropagates", "[quantity]") {
    auto const empty = Q<QFUnit::kg>{};
    auto const engaged = kilograms(5);

    CHECK_FALSE((empty + engaged).hasValue());
    CHECK_FALSE((engaged - empty).hasValue());
    CHECK_FALSE((empty / cubicMetres(2)).hasValue());
    CHECK_FALSE((engaged / Q<QFUnit::m3>{}).hasValue());
    CHECK_FALSE((-empty).hasValue());
    CHECK_FALSE((empty * Rational{2, dp1}).hasValue());
    CHECK_FALSE((empty / Rational{2, dp1}).hasValue());

    // Cross-unit product propagates emptiness from either side.
    CHECK_FALSE((Q<QFUnit::kg_per_m3>{} * cubicMetres(2)).hasValue());
    CHECK_FALSE((Q<QFUnit::kg_per_m3>{Rational{5, 2, dp1}} * Q<QFUnit::m3>{}).hasValue());
}

TEST_CASE("Quantity::Arithmetic::DivisionByZeroYieldsEmpty", "[quantity]") {
    CHECK_FALSE((kilograms(5) / cubicMetres(0)).hasValue());
    CHECK_FALSE((kilograms(5) / Rational::Zero(dp1)).hasValue());
}

TEST_CASE("Quantity::Comparison", "[quantity]") {
    CHECK(Q<QFUnit::kg>{} < kilograms(0));  // empty sorts before engaged
    CHECK(kilograms(1) < kilograms(2));
    CHECK(kilograms(2, 4) == kilograms(1, 2));  // exact value equality
    CHECK(Q<QFUnit::kg>{} == Q<QFUnit::kg>{});

    // Declared precisions are transparent to comparison and conversion.
    PreciseMass const precise{Rational{1, 2, dp3}};
    CHECK(precise == kilograms(1, 2));
    CHECK(precise < kilograms(1));
    Q<QFUnit::kg> const widened = precise;  // same unit, different declared precision
    CHECK(widened == precise);
}

TEST_CASE("Quantity::DeclaredPrecision", "[quantity]") {
    // FromDouble converts at the field's declared precision.
    auto const coarse = Q<QFUnit::kg>::FromDouble(2.5);
    REQUIRE(coarse.hasValue());
    CHECK(*coarse == Rational{5, 2, dp3});
    CHECK((*coarse).GetDecimalPlaces() == dp3);

    auto const fine = PreciseMass::FromDouble(2.5);
    REQUIRE(fine.hasValue());
    CHECK((*fine).GetDecimalPlaces() == DecimalPlaces{5});

    CHECK_FALSE(Q<QFUnit::kg>::FromDouble(std::numeric_limits<double>::quiet_NaN()).hasValue());

    // The value's runtime precision is data and can be retagged; the exact
    // value never changes.
    auto const retagged = coarse.withDecimalPlaces(DecimalPlaces{9});
    REQUIRE(retagged.hasValue());
    CHECK(*retagged == *coarse);
    CHECK((*retagged).GetDecimalPlaces() == DecimalPlaces{9});
    CHECK((*retagged.atDeclaredPrecision()).GetDecimalPlaces() == dp3);
    CHECK_FALSE(Q<QFUnit::kg>{}.withDecimalPlaces(DecimalPlaces{9}).hasValue());

    // Out-of-range runtime precision clamps silently (runtime data, no assert).
    CHECK((*coarse.withDecimalPlaces(DecimalPlaces{99})).GetDecimalPlaces()
          == DecimalPlaces{morph::math::kMaxDecimalPlaces});

    // Mixed declared precisions combine; the runtime tag max-propagates.
    auto const sum = fine + coarse;
    REQUIRE(sum.hasValue());
    CHECK(*sum == Rational{5, dp3});
    CHECK((*sum).GetDecimalPlaces() == DecimalPlaces{5});
}

// ---------------------------------------------------------------------------
// Wire shape.
// ---------------------------------------------------------------------------

TEST_CASE("Quantity::Wire::UnitNeverTravels", "[quantity][forms]") {
    QFRecordMeasurement action{.sampleId = 9, .density = {Rational{23, 10, dp1}}};

    auto const json = glz::write_json(action);
    REQUIRE(json.has_value());
    // Engaged quantity is its rational payload; empty quantity and empty
    // optional are omitted entirely; no unit appears anywhere.
    CHECK(*json == R"({"sampleId":9,"density":{"num":23,"den":10,"dp":1}})");

    QFRecordMeasurement restored{};
    REQUIRE_FALSE(glz::read_json(restored, *json));
    CHECK(restored.density == action.density);
    CHECK_FALSE(restored.moisture.hasValue());
    CHECK_FALSE(restored.note.has_value());
}

TEST_CASE("Quantity::Wire::PartialPatchEngagesDraft", "[quantity][forms]") {
    QFRecordMeasurement draft{.sampleId = 42};
    REQUIRE_FALSE(glz::read_json(draft, R"({"density":{"num":5,"den":2,"dp":1}})"));
    CHECK(draft.sampleId == 42);  // untouched by the partial patch
    REQUIRE(draft.density.hasValue());
    CHECK(*draft.density == Rational{5, 2, dp1});

    // Explicit null clears the field again.
    REQUIRE_FALSE(glz::read_json(draft, R"({"density":null})"));
    CHECK_FALSE(draft.density.hasValue());
}

// ---------------------------------------------------------------------------
// Required-field policy: allRequiredEngaged + ActionValidator integration.
// ---------------------------------------------------------------------------

TEST_CASE("Forms::AllRequiredEngaged", "[forms]") {
    QFRecordMeasurement blank{.sampleId = 1};
    CHECK_FALSE(morph::forms::allRequiredEngaged(blank));  // density missing

    QFRecordMeasurement densityOnly{.sampleId = 1, .density = {Rational{23, 10, dp1}}};
    CHECK(morph::forms::allRequiredEngaged(densityOnly));  // moisture is opt-out

    // The validator machinery picks up validate() (which delegates here).
    CHECK_FALSE(morph::model::ActionValidator<QFRecordMeasurement>::ready(blank));
    CHECK(morph::model::ActionValidator<QFRecordMeasurement>::ready(densityOnly));

    QFComputeDryDensity bothRequired{};
    CHECK_FALSE(morph::forms::allRequiredEngaged(bothRequired));
    bothRequired.massDry = kilograms(1);
    CHECK_FALSE(morph::forms::allRequiredEngaged(bothRequired));
    bothRequired.volume = cubicMetres(2);
    CHECK(morph::forms::allRequiredEngaged(bothRequired));
}

// ---------------------------------------------------------------------------
// Schema generation.
// ---------------------------------------------------------------------------

TEST_CASE("Forms::SchemaJson", "[forms]") {
    auto const schema = morph::forms::schemaJson<QFRecordMeasurement>();
    REQUIRE_FALSE(schema.empty());

    // Valid JSON.
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    // Required derives from the member types + the opt-out list: moisture
    // (declared optional) and note (std::optional) are excluded.
    CHECK(schema.find(R"("required":["sampleId","density"])") != std::string::npos);

    // Quantity fields carry the unit and its default decimals.
    CHECK(schema.find(R"("unitAscii":"kg_per_m3")") != std::string::npos);
    CHECK(schema.find(R"("unitUnicode":"kg/m³")") != std::string::npos);
    CHECK(schema.find(R"("x-decimalPlaces":1)") != std::string::npos);

    // Every property records its declaration index for renderer layout.
    CHECK(schema.find(R"("x-order":0)") != std::string::npos);
    CHECK(schema.find(R"("x-order":3)") != std::string::npos);

    // The Rational wire object shape is described, not an opaque blob.
    CHECK(schema.find(R"("num")") != std::string::npos);
    CHECK(schema.find(R"("den")") != std::string::npos);
    CHECK(schema.find(R"("dp")") != std::string::npos);

    // int64 bounds survive the post-merge exactly (u64 DOM, no rounding).
    CHECK(schema.find("9223372036854775807") != std::string::npos);
    CHECK(schema.find("-9223372036854775808") != std::string::npos);
}

TEST_CASE("Forms::SchemaJson::AllFieldsRequiredWithoutOptOut", "[forms]") {
    auto const schema = morph::forms::schemaJson<QFComputeDryDensity>();
    CHECK(schema.find(R"("required":["massDry","volume"])") != std::string::npos);
}

TEST_CASE("Forms::SchemaJson::DeclaredPrecisionOverrideSurfaces", "[forms]") {
    // A field-level declared-precision override (Quantity<kg, 5>) beats the
    // unit default (3) in the generated schema.
    auto const schema = morph::forms::schemaJson<QFCalibrate>();
    CHECK(schema.find(R"("x-decimalPlaces":5)") != std::string::npos);
    CHECK(schema.find(R"("required":["referenceMass"])") != std::string::npos);
}

TEST_CASE("Forms::SchemaJson::Memoized", "[forms]") {
    // The schema is fixed per type; repeated calls return the cached result.
    CHECK(morph::forms::schemaJson<QFComputeDryDensity>() == morph::forms::schemaJson<QFComputeDryDensity>());
}

TEST_CASE("Forms::MergeSchemaExtras::MalformedInputPassesThrough", "[forms]") {
    // The post-merge is deliberately non-throwing: input the DOM cannot parse
    // is returned unchanged (including the empty string from an upstream
    // schema-generation failure).
    CHECK(morph::forms::detail::mergeSchemaExtras<QFComputeDryDensity>("not json") == "not json");
    CHECK(morph::forms::detail::mergeSchemaExtras<QFComputeDryDensity>(std::string{}).empty());
}

// ---------------------------------------------------------------------------
// Registry integration: JSON in, model execute, JSON out.
// ---------------------------------------------------------------------------

TEST_CASE("Forms::DispatchQuantityActionThroughRegistry", "[forms][quantity]") {
    using morph::model::ActionTraits;

    QFComputeDryDensity action{.massDry = Q<QFUnit::kg>{Rational{26505, 10, dp1}}, .volume = cubicMetres(1)};
    auto const payload = ActionTraits<QFComputeDryDensity>::toJson(action);

    auto holder = morph::model::detail::ModelFactory::create<QFLabModel>();
    auto const resultJson = morph::model::detail::ActionDispatcher::instance().dispatch(
        "QFLabModel", "QFComputeDryDensity", *holder, payload);

    auto const result = ActionTraits<QFComputeDryDensity>::resultFromJson(resultJson);
    REQUIRE(result.hasValue());
    CHECK(*result == Rational{5301, 2, dp1});
}
