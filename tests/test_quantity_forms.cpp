// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <format>
#include <limits>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/choice.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

// ---------------------------------------------------------------------------
// A miniature application unit system: enum + UnitTraits + consteval algebra.
// ---------------------------------------------------------------------------

enum class QFUnit : std::uint8_t { scalar, percent, kg, m3, kg_per_m3, g };

template <>
struct morph::units::UnitTraits<QFUnit> {
    static constexpr morph::units::UnitMeta meta(QFUnit unit) noexcept {
        switch (unit) {
            case QFUnit::scalar:
                return {.id = "scalar", .display = "", .defaultDecimals = 3};
            case QFUnit::percent:
                return {.id = "percent", .display = "%", .defaultDecimals = 1};
            case QFUnit::kg:
                return {.id = "kg", .display = "kg", .defaultDecimals = 3};
            case QFUnit::m3:
                return {.id = "m3", .display = "m³", .defaultDecimals = 3};
            case QFUnit::kg_per_m3:
                return {.id = "kg_per_m3", .display = "kg/m³", .defaultDecimals = 1};
            case QFUnit::g:
                return {.id = "g", .display = "g", .defaultDecimals = 1};
            default:
                return {.id = "?", .display = "?", .defaultDecimals = 3};
        }
    }

    static constexpr std::array<morph::units::UnitRelation<QFUnit>, 1> relations{
        {{QFUnit::g, QFUnit::kg, Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}}}}};
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
    return {Rational{Numerator{num}, Denominator{den}, dp3}};
}

[[nodiscard]] Q<QFUnit::m3> cubicMetres(std::int64_t num, std::int64_t den = 1) {
    return {Rational{Numerator{num}, Denominator{den}, dp3}};
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
static_assert(
    std::same_as<decltype(std::declval<PreciseMass>() / std::declval<Q<QFUnit::m3>>()), Q<QFUnit::kg_per_m3>>);
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

struct QFWholeCount {
    // A zero-decimal declared-precision override: a whole-unit count (or a
    // zero-decimal currency such as JPY/KRW) carries no fractional digit.
    morph::units::Quantity<QFUnit::kg, 0> wholeUnits;
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

struct QFCountryInfo {
    std::int64_t id = 0;
    std::string name;
};

struct QFCountryList {
    std::vector<QFCountryInfo> countries;
};

struct QFListCountries {};

struct QFCityInfo {
    std::int64_t id = 0;
    std::string name;
};

struct QFCityList {
    std::vector<QFCityInfo> cities;
};

// The options action for a dependent Choice is an ordinary registered action
// whose input field ("country") is exactly the DependsOn name the sibling
// Choice declares.
struct QFListCities {
    std::int64_t country = 0;
};

struct QFShippingAddress {
    morph::forms::Choice<std::int64_t, "QFListCountries"> country;
    morph::forms::Choice<std::int64_t, "QFListCities", "id", "name", "country"> city;

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
    QFCountryList execute(const QFListCountries& action) {
        static_cast<void>(action);
        return QFCountryList{.countries = {{.id = 1, .name = "Wonderland"}, {.id = 2, .name = "Narnia"}}};
    }
    // Filtered by the sibling "country" value the caller sends as the body —
    // exactly the same registered-action dispatch every other action uses.
    QFCityList execute(const QFListCities& action) {
        if (action.country == 1) {
            return QFCityList{.cities = {{.id = 10, .name = "Looking-Glass City"}}};
        }
        if (action.country == 2) {
            return QFCityList{.cities = {{.id = 20, .name = "Cair Paravel"}}};
        }
        return QFCityList{};
    }
    std::int64_t execute(const QFShippingAddress& action) { return *action.city; }
};

BRIDGE_REGISTER_MODEL(QFLabModel, "QFLabModel")
BRIDGE_REGISTER_ACTION(QFLabModel, QFComputeDryDensity, "QFComputeDryDensity")
BRIDGE_REGISTER_ACTION(QFLabModel, QFRecordMeasurement, "QFRecordMeasurement")
BRIDGE_REGISTER_ACTION(QFLabModel, QFCalibrate, "QFCalibrate")
BRIDGE_REGISTER_ACTION(QFLabModel, QFListSlots, "QFListSlots", morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(QFLabModel, QFSchedule, "QFSchedule")
BRIDGE_REGISTER_ACTION(QFLabModel, QFListCountries, "QFListCountries", morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(QFLabModel, QFListCities, "QFListCities", morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(QFLabModel, QFShippingAddress, "QFShippingAddress")

// ---------------------------------------------------------------------------
// Arithmetic semantics.
// ---------------------------------------------------------------------------

TEST_CASE("Quantity::Arithmetic::ExactWithPrecisionPropagation", "[quantity]") {
    auto const mass = Q<QFUnit::kg>{Rational{Numerator{26505}, Denominator{10}, dp1}};
    auto const volume = cubicMetres(1);

    auto const density = mass / volume;
    REQUIRE(density.hasValue());
    CHECK(*density == Rational{Numerator{5301}, Denominator{2}, dp1});
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
    CHECK(*(kilograms(3) / Rational{2, dp1}) == Rational{Numerator{3}, Denominator{2}, dp3});
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
    CHECK_FALSE((Q<QFUnit::kg_per_m3>{Rational{Numerator{5}, Denominator{2}, dp1}} * Q<QFUnit::m3>{}).hasValue());
}

TEST_CASE("Quantity::Arithmetic::DivisionByZeroYieldsEmpty", "[quantity]") {
    CHECK_FALSE((kilograms(5) / cubicMetres(0)).hasValue());
    CHECK_FALSE((kilograms(5) / Rational::zero(dp1)).hasValue());
}

TEST_CASE("Quantity::Comparison", "[quantity]") {
    // Relational ordering has an engaged precondition (empty is a contract
    // violation, not a sortable value); equality stays total. See test_quantity.
    CHECK(kilograms(1) < kilograms(2));
    CHECK(kilograms(2, 4) == kilograms(1, 2));  // exact value equality
    CHECK(Q<QFUnit::kg>{} == Q<QFUnit::kg>{});

    // Declared precisions are transparent to comparison and conversion.
    PreciseMass const precise{Rational{Numerator{1}, Denominator{2}, dp3}};
    CHECK(precise == kilograms(1, 2));
    CHECK(precise < kilograms(1));
    Q<QFUnit::kg> const widened = precise;  // same unit, different declared precision
    CHECK(widened == precise);
}

TEST_CASE("Quantity::DeclaredPrecision", "[quantity]") {
    // fromDouble converts at the field's declared precision.
    auto const coarse = Q<QFUnit::kg>::fromDouble(2.5);
    REQUIRE(coarse.hasValue());
    CHECK(*coarse == Rational{Numerator{5}, Denominator{2}, dp3});
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
    CHECK((*coarse.withDecimalPlaces(DecimalPlaces{99})).getDecimalPlaces() ==
          DecimalPlaces{morph::math::kMaxDecimalPlaces});

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
    action.density = Rational{Numerator{23}, Denominator{10}, dp1};

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
    CHECK(*draft.density == Rational{Numerator{5}, Denominator{2}, dp1});

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
    densityOnly.density = Rational{Numerator{23}, Denominator{10}, dp1};
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

TEST_CASE("Forms::SchemaJson::ZeroDecimalPlacesOverrideSurfaces", "[forms]") {
    // A field-level Quantity<kg, 0> override advertises x-decimalPlaces:0 --
    // the declared-decimals floor is 0, not 1, and the field is still
    // required like any other non-optional Quantity member.
    auto const schema = morph::forms::schemaJson<QFWholeCount>();
    CHECK(schema.contains(R"("x-decimalPlaces":0)"));
    CHECK(schema.contains(R"("required":["wholeUnits"])"));
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

    QFComputeDryDensity action{.massDry = Q<QFUnit::kg>{Rational{Numerator{26505}, Denominator{10}, dp1}},
                               .volume = cubicMetres(1)};
    auto const payload = ActionTraits<QFComputeDryDensity>::toJson(action);

    auto holder = morph::model::detail::ModelFactory::create<QFLabModel>();
    auto const resultJson = morph::model::detail::ActionDispatcher::instance().dispatch(
        "QFLabModel", "QFComputeDryDensity", *holder, payload);

    auto const result = ActionTraits<QFComputeDryDensity>::resultFromJson(resultJson);
    REQUIRE(result.hasValue());
    CHECK(*result == Rational{Numerator{5301}, Denominator{2}, dp1});
}

// ---------------------------------------------------------------------------
// Choice fields: declared options provider + wire + schema + engagement.
// ---------------------------------------------------------------------------

namespace {

using SlotChoice = morph::forms::Choice<std::int64_t, "QFListSlots">;
using CodeChoice = morph::forms::Choice<std::string, "QFListCodes", "code", "title">;
using CityChoice = morph::forms::Choice<std::int64_t, "QFListCities", "id", "name", "country">;
// Compile-time check that the pack captures more than one name, in order.
using RegionCityChoice = morph::forms::Choice<std::int64_t, "QFListRegionCities", "id", "name", "country", "region">;

static_assert(SlotChoice::optionsAction() == "QFListSlots");
static_assert(SlotChoice::valueField() == "id");
static_assert(SlotChoice::labelField() == "name");
static_assert(SlotChoice::optionsDependsOn().empty());  // independent Choice: unchanged
static_assert(CodeChoice::valueField() == "code");
static_assert(CodeChoice::labelField() == "title");
static_assert(CodeChoice::optionsDependsOn().empty());
static_assert(CityChoice::optionsAction() == "QFListCities");
static_assert(CityChoice::valueField() == "id");
static_assert(CityChoice::labelField() == "name");
static_assert(CityChoice::optionsDependsOn().size() == 1);
static_assert(CityChoice::optionsDependsOn()[0] == "country");
static_assert(RegionCityChoice::optionsDependsOn().size() == 2);
static_assert(RegionCityChoice::optionsDependsOn()[0] == "country");
static_assert(RegionCityChoice::optionsDependsOn()[1] == "region");
static_assert(morph::forms::isChoice<SlotChoice>);
static_assert(morph::forms::isChoice<CityChoice>);
static_assert(!morph::forms::isChoice<Q<QFUnit::kg>>);
static_assert(morph::forms::EmptyCapableField<SlotChoice>);
static_assert(morph::forms::EmptyCapableField<CityChoice>);
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
    action.startsAt = morph::time::Timestamp{morph::time::DateTime{std::chrono::year{2026}, std::chrono::month{7},
                                                                   std::chrono::day{6}, std::chrono::hours{9},
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

TEST_CASE("Choice::DependsOn::WireUnaffected", "[forms]") {
    // A dependent Choice still serialises as a bare nullable value — the
    // DependsOn names never travel with payloads, exactly like OptionsAction.
    CityChoice const engaged{10};
    CHECK(engaged.hasValue());
    CHECK(*engaged == 10);

    QFShippingAddress action;
    action.country = 1;
    action.city = 10;
    auto const json = glz::write_json(action);
    REQUIRE(json.has_value());
    CHECK(*json == R"({"country":1,"city":10})");

    QFShippingAddress restored{};
    REQUIRE_FALSE(glz::read_json(restored, *json));
    CHECK(restored.country == action.country);
    CHECK(restored.city == action.city);
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
    auto const rows =
        morph::model::detail::ActionDispatcher::instance().dispatch("QFLabModel", "QFListSlots", *holder, "{}");
    CHECK(rows == R"({"slots":[{"id":4,"name":"Morning"},{"id":9,"name":"Afternoon"}]})");

    // Submitting the selected value round-trips through the same seam.
    auto const result = morph::model::detail::ActionDispatcher::instance().dispatch(
        "QFLabModel", "QFSchedule", *holder, R"({"slot":4,"startsAt":"2026-07-06T09:00:00Z"})");
    CHECK(ActionTraits<QFSchedule>::resultFromJson(result) == "slot 4 at 2026-07-06T09:00:00.000Z");
}

TEST_CASE("Forms::SchemaJson::OptionsDependsOnEmission", "[forms]") {
    auto const schema = morph::forms::schemaJson<QFShippingAddress>();

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    // The dependent field ("city") carries the new key, naming its one parent.
    CHECK(schema.contains(R"("x-optionsDependsOn":["country"])"));
    CHECK(schema.contains(R"("x-optionsAction":"QFListCities")"));

    // Only "city" carries it — "country" (independent) does not.
    std::size_t occurrences = 0;
    std::size_t pos = 0;
    while ((pos = schema.find("x-optionsDependsOn", pos)) != std::string::npos) {
        ++occurrences;
        ++pos;
    }
    CHECK(occurrences == 1);

    // An independent Choice elsewhere is unaffected: no key, schema unchanged
    // (backward compatibility).
    CHECK_FALSE(morph::forms::schemaJson<QFSchedule>().contains("x-optionsDependsOn"));
}

TEST_CASE("Forms::DispatchDependentChoiceThroughRegistry", "[forms]") {
    using morph::model::ActionTraits;

    auto holder = morph::model::detail::ModelFactory::create<QFLabModel>();

    auto const countries =
        morph::model::detail::ActionDispatcher::instance().dispatch("QFLabModel", "QFListCountries", *holder, "{}");
    CHECK(countries == R"({"countries":[{"id":1,"name":"Wonderland"},{"id":2,"name":"Narnia"}]})");

    // The options action's own body is the parent's current value — an
    // ordinary registered action, no new dispatch mechanism.
    auto const cities = morph::model::detail::ActionDispatcher::instance().dispatch("QFLabModel", "QFListCities",
                                                                                    *holder, R"({"country":1})");
    CHECK(cities == R"({"cities":[{"id":10,"name":"Looking-Glass City"}]})");

    auto const differentCities = morph::model::detail::ActionDispatcher::instance().dispatch(
        "QFLabModel", "QFListCities", *holder, R"({"country":2})");
    CHECK(differentCities == R"({"cities":[{"id":20,"name":"Cair Paravel"}]})");

    auto const result = morph::model::detail::ActionDispatcher::instance().dispatch(
        "QFLabModel", "QFShippingAddress", *holder, R"({"country":1,"city":10})");
    CHECK(result == "10");
}

// ---------------------------------------------------------------------------
// Convertible entry units: declared per unit system, surfaced in schemas.
// ---------------------------------------------------------------------------

namespace {

static_assert(morph::units::HasUnitRelations<QFUnit>);
static_assert(Q<QFUnit::kg>::unitAlternatives().size() == 1);
static_assert(Q<QFUnit::kg>::unitAlternatives()[0].unit == QFUnit::g);
static_assert(Q<QFUnit::kg>::unitAlternatives()[0].num == 1);
static_assert(Q<QFUnit::kg>::unitAlternatives()[0].den == 1000);
static_assert(Q<QFUnit::percent>::unitAlternatives().empty());

}  // namespace

TEST_CASE("Forms::SchemaJson::UnitAlternativesSurface", "[forms]") {
    // massDry is kg, which declares grams as a convertible entry unit: the
    // schema carries the exact alternative-to-canonical ratio.
    auto const schema = morph::forms::schemaJson<QFComputeDryDensity>();
    CHECK(schema.contains(R"("x-unitAlternatives":[{"id":"g","display":"g","decimals":1,"num":1,"den":1000}])"));

    // Units without declared alternatives get no such key.
    auto const percentSchema = morph::forms::schemaJson<QFRecordMeasurement>();
    CHECK_FALSE(percentSchema.contains("x-unitAlternatives"));
}

// ---------------------------------------------------------------------------
// Field metadata: labels, help, placeholder, read-only, hidden
// (docs/spec/forms/forms.md, "Field metadata").
// ---------------------------------------------------------------------------

// No fieldMetadata at all: every property still gets an inferred title, and
// no x-placeholder/x-readonly/x-hidden key appears anywhere (regression guard
// — the rest of the schema is unaffected by this feature). Declared at file
// scope, not inside an anonymous namespace — matching every other
// action/data struct already in this file (QFRecordMeasurement,
// QFComputeDryDensity, QFSlotInfo, ...), none of which use one either.
struct QFNoFieldMeta {
    std::int64_t dryMassPct = 0;
    std::int64_t sample_id = 0;
    std::int64_t notes = 0;
};

// Explicit literal FieldMeta overrides, including one entry naming a field
// that does not exist on the action (must be silently ignored).
struct QFFieldMetaLiteral {
    std::int64_t dryMassPct = 0;
    std::int64_t sample_id = 0;
    std::int64_t notes = 0;

    static constexpr std::array fieldMetadata{
        morph::forms::FieldMeta{
            .field = "dryMassPct", .label = "Custom Label", .help = "Help text", .placeholder = "e.g. 42"},
        morph::forms::FieldMeta{.field = "sample_id", .readOnly = true},
        morph::forms::FieldMeta{.field = "notes", .hidden = true},
        morph::forms::FieldMeta{.field = "doesNotExist"},  // ignored: no matching member
    };
};

// A FieldMeta::help override must beat a glz::json_schema<A>-authored
// description; a field with no FieldMeta entry keeps its glaze-authored
// description untouched.
struct QFHelpOverrideAction {
    std::int64_t sampleId = 0;
    std::int64_t plainField = 0;

    static constexpr std::array fieldMetadata{
        morph::forms::FieldMeta{.field = "sampleId", .help = "Overridden help"},
    };
};

template <>
struct glz::json_schema<QFHelpOverrideAction> {
    schema sampleId{.description = "Glaze-authored description"};
    schema plainField{.description = "Untouched description"};
};

TEST_CASE("Forms::FieldMeta::InferredTitleWithNoDeclaration", "[forms][field_meta]") {
    auto const schema = morph::forms::schemaJson<QFNoFieldMeta>();
    CHECK(schema.contains(R"("title":"Dry Mass Pct")"));
    CHECK(schema.contains(R"("title":"Sample Id")"));
    CHECK(schema.contains(R"("title":"Notes")"));
    CHECK_FALSE(schema.contains("x-placeholder"));
    CHECK_FALSE(schema.contains("x-readonly"));
    CHECK_FALSE(schema.contains("x-hidden"));
}

TEST_CASE("Forms::FieldMeta::ExistingActionsGainInferredTitleOnly", "[forms][field_meta]") {
    // A pre-existing, already-registered action with no fieldMetadata gains
    // titles for free: zero-declaration inference, proven against a real
    // action struct rather than a purpose-built one.
    auto const schema = morph::forms::schemaJson<QFComputeDryDensity>();
    CHECK(schema.contains(R"("title":"Mass Dry")"));
    CHECK(schema.contains(R"("title":"Volume")"));
}

TEST_CASE("Forms::FieldMeta::LabelHelpPlaceholderOverride", "[forms][field_meta]") {
    auto const schema = morph::forms::schemaJson<QFFieldMetaLiteral>();
    CHECK(schema.contains(R"("title":"Custom Label")"));
    CHECK_FALSE(schema.contains(R"("title":"Dry Mass Pct")"));
    CHECK(schema.contains(R"("description":"Help text")"));
    CHECK(schema.contains(R"("x-placeholder":"e.g. 42")"));
    // A field left undeclared in fieldMetadata still gets its inferred title.
    CHECK(schema.contains(R"("title":"Notes")"));
}

TEST_CASE("Forms::FieldMeta::ReadOnlyAndHiddenEmitOnlyWhenTrue", "[forms][field_meta]") {
    auto const schema = morph::forms::schemaJson<QFFieldMetaLiteral>();
    CHECK(schema.contains(R"("x-readonly":true)"));
    CHECK(schema.contains(R"("x-hidden":true)"));
    // dryMassPct has a FieldMeta entry (label/help/placeholder only) that
    // leaves readOnly/hidden at their default false: its property must carry
    // neither key, proving the omission is per-flag, not merely "no entry at
    // all" (already covered by InferredTitleWithNoDeclaration above). The
    // exact substring below is the whole property node, so no trailing
    // x-readonly/x-hidden key can be hiding after it.
    CHECK(schema.contains(
        R"("dryMassPct":{"$ref":"#/$defs/int64_t","x-order":0,"title":"Custom Label","description":"Help text","x-placeholder":"e.g. 42"})"));
}

TEST_CASE("Forms::FieldMeta::UnknownFieldNameIsIgnored", "[forms][field_meta]") {
    // The fourth fieldMetadata entry names a field that does not exist on the
    // action; schema generation must not crash and must not emit a stray
    // "doesNotExist" property.
    auto const schema = morph::forms::schemaJson<QFFieldMetaLiteral>();
    CHECK_FALSE(schema.contains("doesNotExist"));
}

// FieldMeta::i18nKey — a stem override for morph::forms::i18n's explicit-key
// derivation (docs/spec/forms/forms.md, "Field metadata"): consumed as a
// *stem*, not a complete key (docs/superpowers/plans/2026-07-20-gui-i18n.md's
// resolved key-derivation contract), and emitted verbatim as x-i18nKey only
// when non-empty.
struct QFFieldMetaI18nKey {
    std::int64_t sampleId = 0;
    std::int64_t plainField = 0;
    std::int64_t untouchedField = 0;

    static constexpr std::array fieldMetadata{
        morph::forms::FieldMeta{.field = "sampleId", .i18nKey = "catalog.sample"},
        morph::forms::FieldMeta{.field = "plainField", .label = "Plain"},  // i18nKey left at "" (no override)
    };
};

TEST_CASE("Forms::FieldMeta::I18nKeyEmitsOnlyWhenNonEmpty", "[forms][field_meta]") {
    auto const schema = morph::forms::schemaJson<QFFieldMetaI18nKey>();
    // The whole property node, so no stray x-i18nKey key can be hiding
    // elsewhere in it.
    CHECK(schema.contains(
        R"("sampleId":{"$ref":"#/$defs/int64_t","x-order":0,"title":"Sample Id","x-i18nKey":"catalog.sample"})"));
    // A FieldMeta entry that leaves i18nKey at its default ("") must not
    // emit the key at all -- proving the omission is per-field, not merely
    // "no fieldMetadata entry at all" (already covered by
    // InferredTitleWithNoDeclaration above).
    CHECK(schema.contains(R"("plainField":{"$ref":"#/$defs/int64_t","x-order":1,"title":"Plain"})"));
    // A field with no fieldMetadata entry whatsoever also gets no x-i18nKey.
    CHECK(schema.contains(R"("untouchedField":{"$ref":"#/$defs/int64_t","x-order":2,"title":"Untouched Field"})"));
    CHECK_FALSE(schema.contains("x-i18nKey\":\"\""));
}

TEST_CASE("Forms::FieldMeta::HelpOverridesGlazeDescription", "[forms][field_meta]") {
    auto const schema = morph::forms::schemaJson<QFHelpOverrideAction>();
    CHECK(schema.contains(R"("description":"Overridden help")"));
    CHECK_FALSE(schema.contains("Glaze-authored description"));
    // plainField has no FieldMeta entry: its glaze-authored description survives.
    CHECK(schema.contains(R"("description":"Untouched description")"));
}

TEST_CASE("Forms::FieldMeta::LabelInference", "[forms][field_meta]") {
    CHECK(morph::forms::detail::inferTitle("dryMassPct") == "Dry Mass Pct");
    CHECK(morph::forms::detail::inferTitle("sample_id") == "Sample Id");
    CHECK(morph::forms::detail::inferTitle("notes") == "Notes");
}

TEST_CASE("Forms::FieldMeta::FluentBuildersComposeWithLiteralForm", "[forms][field_meta]") {
    constexpr morph::forms::FieldMeta base{.field = "x"};
    constexpr auto withPh = base.withPlaceholder("hint");
    constexpr auto withRO = base.withReadOnly();
    constexpr auto withHidden = base.withHidden();
    static_assert(withPh.placeholder == "hint");
    static_assert(withRO.readOnly);
    static_assert(withHidden.hidden);
    static_assert(!base.readOnly && !base.hidden && base.placeholder.empty());
}

// describe<>() sugar: declared in-class, defined out-of-line (see this
// plan's Task 2 note on why). Must produce the same property annotations as
// an equivalent hand-written FieldMeta literal (QFDescribeLiteral, below).
// File scope, not an anonymous namespace, matching every other action/data
// struct already in this file.
struct QFDescribeSugar {
    std::int64_t sampleId = 0;
    Q<QFUnit::kg> mass;

    static const std::array<morph::forms::FieldMeta, 2> fieldMetadata;
};

struct QFDescribeLiteral {
    std::int64_t sampleId = 0;
    Q<QFUnit::kg> mass;

    static constexpr std::array fieldMetadata{
        morph::forms::FieldMeta{.field = "sampleId", .label = "Sample", .help = "Which logged sample"},
        morph::forms::FieldMeta{.field = "mass", .readOnly = true},
    };
};

inline const std::array<morph::forms::FieldMeta, 2> QFDescribeSugar::fieldMetadata{
    morph::forms::describe<&QFDescribeSugar::sampleId>("Sample", "Which logged sample"),
    morph::forms::describe<&QFDescribeSugar::mass>().withReadOnly(),
};

TEST_CASE("Forms::FieldMeta::DescribeSugarMatchesExplicitLiteral", "[forms][field_meta]") {
    // describe<&Action::field>(...) and the equivalent FieldMeta{.field="…"}
    // literal produce identical property-level annotations for the same
    // field shape. The two action *type names* differ (QFDescribeSugar vs
    // QFDescribeLiteral), so only the per-property substrings are compared,
    // not the whole schema string (glaze stamps each type's own name as its
    // top-level "title", which necessarily differs between the two types).
    auto const sugar = morph::forms::schemaJson<QFDescribeSugar>();
    auto const literal = morph::forms::schemaJson<QFDescribeLiteral>();
    CHECK(sugar.contains(R"("title":"Sample","description":"Which logged sample")"));
    CHECK(literal.contains(R"("title":"Sample","description":"Which logged sample")"));
    CHECK(sugar.contains(R"("x-readonly":true)"));
    CHECK(literal.contains(R"("x-readonly":true)"));
}
