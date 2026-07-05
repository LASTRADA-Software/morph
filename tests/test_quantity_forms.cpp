// SPDX-License-Identifier: Apache-2.0

#include <morph/choice.hpp>
#include <morph/datetime.hpp>
#include <morph/forms.hpp>
#include <morph/quantity.hpp>
#include <morph/rational.hpp>
#include <morph/registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using morph::math::DecimalPlaces;
using morph::math::Rational;

// ---------------------------------------------------------------------------
// A miniature application unit system: enum + UnitTraits + consteval algebra.
// ---------------------------------------------------------------------------

enum class QFUnit : std::uint8_t { scalar, percent, kg, m3, kg_per_m3 };

template <>
struct morph::units::UnitTraits<QFUnit> {
    static constexpr morph::units::UnitMeta meta(QFUnit unit) noexcept {
        switch (unit) {
            case QFUnit::scalar:    return {.id = "scalar", .display = "", .defaultDecimals = 3};
            case QFUnit::percent:   return {.id = "percent", .display = "%", .defaultDecimals = 1};
            case QFUnit::kg:        return {.id = "kg", .display = "kg", .defaultDecimals = 3};
            case QFUnit::m3:        return {.id = "m3", .display = "m³", .defaultDecimals = 3};
            case QFUnit::kg_per_m3: return {.id = "kg_per_m3", .display = "kg/m³", .defaultDecimals = 1};
        }
        return {.id = "?", .display = "?", .defaultDecimals = 3};
    }
};

consteval QFUnit operator*(QFUnit lhs, QFUnit rhs) {
    if (lhs == QFUnit::scalar) {
        return rhs;
    }
    if (rhs == QFUnit::scalar) {
        return lhs;
    }
    if ((lhs == QFUnit::kg_per_m3 && rhs == QFUnit::m3) || (lhs == QFUnit::m3 && rhs == QFUnit::kg_per_m3)) {
        return QFUnit::kg;
    }
    throw "unsupported unit product";
}

consteval QFUnit operator/(QFUnit lhs, QFUnit rhs) {
    if (rhs == QFUnit::scalar) {
        return lhs;
    }
    if (lhs == rhs) {
        return QFUnit::scalar;
    }
    if (lhs == QFUnit::kg && rhs == QFUnit::m3) {
        return QFUnit::kg_per_m3;
    }
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
static_assert(morph::units::isQuantity<Q<QFUnit::kg>>);
static_assert(!morph::units::isQuantity<Rational>);
static_assert(!morph::units::isQuantity<std::optional<Q<QFUnit::kg>>>);

// Declared precision: defaults from UnitTraits, overridable per field, and
// binary results fall back to the unit default (a computed temporary is not
// a declared field).
using PreciseMass = morph::units::Quantity<QFUnit::kg, 5>;
static_assert(Q<QFUnit::kg>::declaredDecimals == 3);
static_assert(Q<QFUnit::percent>::declaredDecimals == 1);
static_assert(PreciseMass::declaredDecimals == 5);
static_assert(morph::units::isQuantity<PreciseMass>);
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
    Q<QFUnit::kg_per_m3> density;
    Q<QFUnit::percent> moisture;
    std::optional<std::string> note;

    static constexpr std::array optionalFields{std::string_view{"moisture"}};

    [[nodiscard]] bool validate() const { return sampleId > 0 && morph::forms::allRequiredEngaged(*this); }
};

struct QFComputeDryDensity {
    Q<QFUnit::kg> massDry;
    Q<QFUnit::m3> volume;

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

struct QFCalibrate {
    morph::units::Quantity<QFUnit::kg, 5> referenceMass;
};

struct QFSlotInfo {
    std::int64_t id = 0;
    std::string name;
};

struct QFSlotList {
    std::vector<QFSlotInfo> slots;
};

struct QFListSlots {};

struct QFSchedule {
    morph::forms::Choice<std::int64_t, "QFListSlots"> slot;
    morph::time::Timestamp startsAt;

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

struct QFLabModel {
    Q<QFUnit::kg_per_m3> execute(const QFComputeDryDensity& action) { return action.massDry / action.volume; }
    std::int64_t execute(const QFRecordMeasurement& action) { return action.sampleId; }
    bool execute(const QFCalibrate& action) { return action.referenceMass.hasValue(); }
    QFSlotList execute(const QFListSlots& action) {
        static_cast<void>(action);
        return QFSlotList{.slots = {{.id = 4, .name = "Morning"}, {.id = 9, .name = "Afternoon"}}};
    }
    std::string execute(const QFSchedule& action) {
        return std::format("slot {} at {}", *action.slot, *action.startsAt);
    }
};

BRIDGE_REGISTER_MODEL(QFLabModel, "QFLabModel")
BRIDGE_REGISTER_ACTION(QFLabModel, QFComputeDryDensity, "QFComputeDryDensity")
BRIDGE_REGISTER_ACTION(QFLabModel, QFRecordMeasurement, "QFRecordMeasurement")
BRIDGE_REGISTER_ACTION(QFLabModel, QFCalibrate, "QFCalibrate")
BRIDGE_REGISTER_ACTION(QFLabModel, QFListSlots, "QFListSlots", morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(QFLabModel, QFSchedule, "QFSchedule")

// ---------------------------------------------------------------------------
// Arithmetic semantics.
// ---------------------------------------------------------------------------

TEST_CASE("Quantity::Arithmetic::ExactWithPrecisionPropagation", "[quantity]") {
    auto const mass = Q<QFUnit::kg>{Rational{26505, 10, dp1}};
    auto const volume = cubicMetres(1);

    auto const density = mass / volume;
    REQUIRE(density.hasValue());
    CHECK(*density == Rational{5301, 2, dp1});
    CHECK((*density).getDecimalPlaces() == dp3);  // max(1, 3) propagates

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
    CHECK_FALSE((kilograms(5) / Rational::zero(dp1)).hasValue());
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
    // fromDouble converts at the field's declared precision.
    auto const coarse = Q<QFUnit::kg>::fromDouble(2.5);
    REQUIRE(coarse.hasValue());
    CHECK(*coarse == Rational{5, 2, dp3});
    CHECK((*coarse).getDecimalPlaces() == dp3);

    auto const fine = PreciseMass::fromDouble(2.5);
    REQUIRE(fine.hasValue());
    CHECK((*fine).getDecimalPlaces() == DecimalPlaces{5});

    CHECK_FALSE(Q<QFUnit::kg>::fromDouble(std::numeric_limits<double>::quiet_NaN()).hasValue());

    // The value's runtime precision is data and can be retagged; the exact
    // value never changes.
    auto const retagged = coarse.withDecimalPlaces(DecimalPlaces{9});
    REQUIRE(retagged.hasValue());
    CHECK(*retagged == *coarse);
    CHECK((*retagged).getDecimalPlaces() == DecimalPlaces{9});
    CHECK((*retagged.atDeclaredPrecision()).getDecimalPlaces() == dp3);
    CHECK_FALSE(Q<QFUnit::kg>{}.withDecimalPlaces(DecimalPlaces{9}).hasValue());

    // Out-of-range runtime precision clamps silently (runtime data, no assert).
    CHECK((*coarse.withDecimalPlaces(DecimalPlaces{99})).getDecimalPlaces()
          == DecimalPlaces{morph::math::kMaxDecimalPlaces});

    // Mixed declared precisions combine; the runtime tag max-propagates.
    auto const sum = fine + coarse;
    REQUIRE(sum.hasValue());
    CHECK(*sum == Rational{5, dp3});
    CHECK((*sum).getDecimalPlaces() == DecimalPlaces{5});
}

// ---------------------------------------------------------------------------
// Wire shape.
// ---------------------------------------------------------------------------

TEST_CASE("Quantity::Wire::UnitNeverTravels", "[quantity][forms]") {
    QFRecordMeasurement action;
    action.sampleId = 9;
    action.density = Rational{23, 10, dp1};

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
    QFRecordMeasurement draft;
    draft.sampleId = 42;
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
    QFRecordMeasurement blank;
    blank.sampleId = 1;
    CHECK_FALSE(morph::forms::allRequiredEngaged(blank));  // density missing

    QFRecordMeasurement densityOnly;
    densityOnly.sampleId = 1;
    densityOnly.density = Rational{23, 10, dp1};
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
    CHECK(schema.contains(R"("required":["sampleId","density"])"));

    // Quantity fields carry the unit and its default decimals.
    CHECK(schema.contains(R"("unitAscii":"kg_per_m3")"));
    CHECK(schema.contains(R"("unitUnicode":"kg/m³")"));
    CHECK(schema.contains(R"("x-decimalPlaces":1)"));

    // Every property records its declaration index for renderer layout.
    CHECK(schema.contains(R"("x-order":0)"));
    CHECK(schema.contains(R"("x-order":3)"));

    // The Rational wire object shape is described, not an opaque blob.
    CHECK(schema.contains(R"("num")"));
    CHECK(schema.contains(R"("den")"));
    CHECK(schema.contains(R"("dp")"));

    // int64 bounds survive the post-merge exactly (u64 DOM, no rounding).
    CHECK(schema.contains("9223372036854775807"));
    CHECK(schema.contains("-9223372036854775808"));
}

TEST_CASE("Forms::SchemaJson::AllFieldsRequiredWithoutOptOut", "[forms]") {
    auto const schema = morph::forms::schemaJson<QFComputeDryDensity>();
    CHECK(schema.contains(R"("required":["massDry","volume"])"));
}

TEST_CASE("Forms::SchemaJson::DeclaredPrecisionOverrideSurfaces", "[forms]") {
    // A field-level declared-precision override (Quantity<kg, 5>) beats the
    // unit default (3) in the generated schema.
    auto const schema = morph::forms::schemaJson<QFCalibrate>();
    CHECK(schema.contains(R"("x-decimalPlaces":5)"));
    CHECK(schema.contains(R"("required":["referenceMass"])"));
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

// ---------------------------------------------------------------------------
// Choice fields: declared options provider + wire + schema + engagement.
// ---------------------------------------------------------------------------

namespace {

using SlotChoice = morph::forms::Choice<std::int64_t, "QFListSlots">;
using CodeChoice = morph::forms::Choice<std::string, "QFListCodes", "code", "title">;

static_assert(SlotChoice::optionsAction() == "QFListSlots");
static_assert(SlotChoice::valueField() == "id");
static_assert(SlotChoice::labelField() == "name");
static_assert(CodeChoice::valueField() == "code");
static_assert(CodeChoice::labelField() == "title");
static_assert(morph::forms::isChoice<SlotChoice>);
static_assert(!morph::forms::isChoice<Q<QFUnit::kg>>);
static_assert(morph::forms::EmptyCapableField<SlotChoice>);
static_assert(morph::forms::EmptyCapableField<morph::time::Timestamp>);
static_assert(morph::forms::EmptyCapableField<Q<QFUnit::kg>>);
static_assert(!morph::forms::EmptyCapableField<std::int64_t>);

}  // namespace

TEST_CASE("Choice::EmptyStateAndWire", "[forms]") {
    SlotChoice blank;
    CHECK_FALSE(blank.hasValue());
    CHECK(SlotChoice{std::optional<std::int64_t>{}} == blank);

    auto const engaged = SlotChoice{9};
    CHECK(engaged.hasValue());
    CHECK(*engaged == 9);

    QFSchedule action;
    action.slot = engaged;
    action.startsAt = morph::time::Timestamp{morph::time::DateTime{
        std::chrono::year{2026}, std::chrono::month{7}, std::chrono::day{6}, std::chrono::hours{9},
        std::chrono::minutes{0}, std::chrono::seconds{0}}};

    // On the wire a Choice is its bare underlying value; the options
    // metadata never travels.
    auto const json = glz::write_json(action);
    REQUIRE(json.has_value());
    CHECK(*json == R"({"slot":9,"startsAt":"2026-07-06T09:00:00.000Z"})");

    QFSchedule restored{};
    REQUIRE_FALSE(glz::read_json(restored, *json));
    CHECK(restored.slot == action.slot);
    CHECK(restored.startsAt == action.startsAt);

    // Explicit null clears the selection.
    REQUIRE_FALSE(glz::read_json(restored, R"({"slot":null})"));
    CHECK_FALSE(restored.slot.hasValue());

    // String-valued choices work the same way.
    CodeChoice code{std::string{"EN-13286"}};
    CHECK(*code == "EN-13286");
}

TEST_CASE("Choice::DrivesReadiness", "[forms]") {
    QFSchedule draft;
    CHECK_FALSE(draft.validate());
    draft.slot = 4;
    CHECK_FALSE(draft.validate());  // timestamp still missing
    draft.startsAt = morph::time::Timestamp::now();
    CHECK(draft.validate());
    CHECK_FALSE(morph::model::ActionValidator<QFSchedule>::ready(QFSchedule{}));
}

TEST_CASE("Forms::SchemaJson::ChoiceAndDateTime", "[forms]") {
    auto const schema = morph::forms::schemaJson<QFSchedule>();

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    // The declared options provider and its row fields surface as x- keys.
    CHECK(schema.contains(R"("x-optionsAction":"QFListSlots")"));
    CHECK(schema.contains(R"("x-optionValue":"id")"));
    CHECK(schema.contains(R"("x-optionLabel":"name")"));

    // The timestamp carries the standard date-time format annotation.
    CHECK(schema.contains(R"("format":"date-time")"));

    // Both fields are required (non-optional, no opt-out).
    CHECK(schema.contains(R"("required":["slot","startsAt"])"));
}

TEST_CASE("Forms::DispatchChoiceActionThroughRegistry", "[forms]") {
    using morph::model::ActionTraits;

    auto holder = morph::model::detail::ModelFactory::create<QFLabModel>();

    // The options provider is itself just an action.
    auto const rows = morph::model::detail::ActionDispatcher::instance().dispatch("QFLabModel", "QFListSlots",
                                                                                  *holder, "{}");
    CHECK(rows == R"({"slots":[{"id":4,"name":"Morning"},{"id":9,"name":"Afternoon"}]})");

    // Submitting the selected value round-trips through the same seam.
    auto const result = morph::model::detail::ActionDispatcher::instance().dispatch(
        "QFLabModel", "QFSchedule", *holder, R"({"slot":4,"startsAt":"2026-07-06T09:00:00Z"})");
    CHECK(ActionTraits<QFSchedule>::resultFromJson(result) == "slot 4 at 2026-07-06T09:00:00.000Z");
}
