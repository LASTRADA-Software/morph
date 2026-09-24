// SPDX-License-Identifier: Apache-2.0
//
// A plain member's display unit and decimals: `FieldMeta::unit` / `::decimals`
//
// A DTO whose numeric members are plain `double`s has no type to carry a unit
// or a precision, so a renderer had nothing to show beside the control and no
// fraction-digit count to accept. These tests pin that the declaration reaches
// the served schema -- the unit as `ExtUnits`, the key a `Quantity` already
// carries, and the precision as `x-displayDecimals`, deliberately *not*
// `x-decimalPlaces`, which would hand the property the `{num,den,dp}` encoding
// a `double` cannot decode.
//
// src/qt/forms/tests/tst_DynamicFormDisplayUnit.qml pins the renderer half
// against the same key names.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

using morph::math::DecimalPlaces;

// File-scope (not anonymous-namespaced): glaze's reflection needs a type with
// linkage. Same suppression, for the same reason, as
// tests/test_forms_field_bounds.cpp.
// NOLINTBEGIN(misc-use-internal-linkage)

enum class FDUUnit : std::uint8_t { kg };

template <>
struct morph::units::UnitTraits<FDUUnit> {
    static constexpr morph::units::UnitMeta meta(FDUUnit /*unit*/) noexcept {
        return {.id = "kg", .display = "kg", .defaultDecimals = 3};
    }
};

using FDUMass = morph::units::Quantity<FDUUnit::kg, 3>;

/// The motivating shape: lab readings held as plain `double`s.
struct FDUReadingAction {
    double density = 0.0;
    double temperature = 0.0;
    std::int64_t specimens = 0;
    double ratio = 0.0;

    static constexpr std::array<morph::forms::FieldMeta, 3> fieldMetadata{
        morph::forms::FieldMeta{.field = "density", .unit = "kg/m³", .decimals = DecimalPlaces{3}},
        morph::forms::FieldMeta{.field = "temperature", .unit = "°C"},
        morph::forms::FieldMeta{.field = "specimens", .unit = "pcs"},
    };
};

/// A `Quantity` already states both; a `FieldMeta` restating them is ignored.
struct FDUQuantityAction {
    FDUMass mass;

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{.field = "mass", .unit = "lb", .decimals = DecimalPlaces{1}},
    };
};

/// A precision past `kMaxDecimalPlaces` has no meaning and is not emitted.
struct FDUOverPreciseAction {
    double value = 0.0;

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{.field = "value", .decimals = DecimalPlaces{morph::math::kMaxDecimalPlaces + 1}},
    };
};

/// The same declaration one level down: the element of a repeated aggregate.
struct FDURow {
    double sieve = 0.0;
    double passing = 0.0;

    static constexpr std::array<morph::forms::FieldMeta, 2> fieldMetadata{
        morph::forms::FieldMeta{.field = "sieve", .unit = "mm", .decimals = DecimalPlaces{1}},
        morph::forms::FieldMeta{.field = "passing", .unit = "%", .decimals = DecimalPlaces{2}},
    };
};

struct FDUGradingAction {
    std::vector<FDURow> rows;
};

/// The fluent builders, which must produce what the literal produces.
struct FDUFluentAction {
    double density = 0.0;

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{.field = "density"}.withUnit("kg/m³").withDecimals(DecimalPlaces{3}),
    };
};

// NOLINTEND(misc-use-internal-linkage)

namespace {

glz::generic_u64 schemaDom(std::string const& schema) {
    glz::generic_u64 dom{};
    REQUIRE_FALSE(schema.empty());
    REQUIRE_FALSE(glz::read_json(dom, schema));
    return dom;
}

// Property @p name of the action's schema -- or, with @p items, the element
// schema of that array property -- with a `$ref` into `$defs` resolved. A
// copy, so nothing returned refers into a caller's temporary.
glz::generic_u64 resolvedProperty(const glz::generic_u64& dom, std::string const& name, bool items) {
    // Copy-initialised, never brace-initialised: `generic_u64{x}` is
    // list-initialisation, which under GCC/Clang builds a one-element array
    // holding `x` rather than a copy of it.
    const glz::generic_u64& property = dom["properties"][name];
    glz::generic_u64 node = items ? property["items"] : property;
    if (node.contains("$ref")) {
        constexpr std::string_view kPrefix = "#/$defs/";
        std::string const ref = node["$ref"].get<std::string>();
        REQUIRE(ref.starts_with(kPrefix));
        return dom["$defs"][ref.substr(kPrefix.size())];
    }
    return node;
}

}  // namespace

TEST_CASE("Forms::FieldMeta::DisplayUnitAndDecimalsOnAPlainDouble", "[forms][field_meta][display_unit]") {
    auto const dom = schemaDom(morph::forms::schemaJson<FDUReadingAction>());
    auto const& density = dom["properties"]["density"];

    REQUIRE(density.contains("ExtUnits"));
    CHECK(density["ExtUnits"]["unitAscii"].get<std::string>() == "kg/m³");
    CHECK(density["ExtUnits"]["unitUnicode"].get<std::string>() == "kg/m³");
    REQUIRE(density.contains("x-displayDecimals"));
    CHECK(density["x-displayDecimals"].as<std::uint64_t>() == 3);
    // The exact-decimal key would re-type the member on the wire.
    CHECK_FALSE(density.contains("x-decimalPlaces"));
}

TEST_CASE("Forms::FieldMeta::UnitWithoutDecimalsEmitsOnlyTheUnit", "[forms][field_meta][display_unit]") {
    auto const dom = schemaDom(morph::forms::schemaJson<FDUReadingAction>());
    auto const& temperature = dom["properties"]["temperature"];
    REQUIRE(temperature.contains("ExtUnits"));
    CHECK(temperature["ExtUnits"]["unitAscii"].get<std::string>() == "°C");
    CHECK_FALSE(temperature.contains("x-displayDecimals"));

    // An integral member takes a unit as readily as a floating one.
    auto const& specimens = dom["properties"]["specimens"];
    REQUIRE(specimens.contains("ExtUnits"));
    CHECK(specimens["ExtUnits"]["unitAscii"].get<std::string>() == "pcs");
}

TEST_CASE("Forms::FieldMeta::UndeclaredDisplayUnitEmitsNothing", "[forms][field_meta][display_unit]") {
    auto const dom = schemaDom(morph::forms::schemaJson<FDUReadingAction>());
    auto const& ratio = dom["properties"]["ratio"];
    CHECK_FALSE(ratio.contains("ExtUnits"));
    CHECK_FALSE(ratio.contains("x-displayDecimals"));
}

TEST_CASE("Forms::FieldMeta::DisplayUnitIsIgnoredOnAQuantity", "[forms][field_meta][display_unit]") {
    auto const& schema = morph::forms::schemaJson<FDUQuantityAction>();
    auto const dom = schemaDom(schema);
    auto const& mass = dom["properties"]["mass"];

    CHECK(mass["x-decimalPlaces"].as<std::uint64_t>() == 3);
    CHECK_FALSE(mass.contains("x-displayDecimals"));
    // The type's own unit is the only one anywhere in the schema.
    CHECK_FALSE(schema.contains(R"("lb")"));
    CHECK(resolvedProperty(dom, "mass", false)["ExtUnits"]["unitAscii"].get<std::string>() == "kg");
}

TEST_CASE("Forms::FieldMeta::DecimalsPastTheMaximumAreIgnored", "[forms][field_meta][display_unit]") {
    auto const dom = schemaDom(morph::forms::schemaJson<FDUOverPreciseAction>());
    CHECK_FALSE(dom["properties"]["value"].contains("x-displayDecimals"));
}

TEST_CASE("Forms::FieldMeta::DisplayUnitReachesARepeatedAggregatesElement", "[forms][field_meta][display_unit]") {
    auto const dom = schemaDom(morph::forms::schemaJson<FDUGradingAction>());
    auto const row = resolvedProperty(dom, "rows", true);

    CHECK(row["properties"]["sieve"]["ExtUnits"]["unitAscii"].get<std::string>() == "mm");
    CHECK(row["properties"]["sieve"]["x-displayDecimals"].as<std::uint64_t>() == 1);
    CHECK(row["properties"]["passing"]["ExtUnits"]["unitAscii"].get<std::string>() == "%");
    CHECK(row["properties"]["passing"]["x-displayDecimals"].as<std::uint64_t>() == 2);
}

TEST_CASE("Forms::FieldMeta::DisplayUnitBuildersMatchTheLiteral", "[forms][field_meta][display_unit]") {
    auto const fluent = schemaDom(morph::forms::schemaJson<FDUFluentAction>());
    auto const literal = schemaDom(morph::forms::schemaJson<FDUReadingAction>());
    CHECK(fluent["properties"]["density"]["ExtUnits"]["unitAscii"].get<std::string>() ==
          literal["properties"]["density"]["ExtUnits"]["unitAscii"].get<std::string>());
    CHECK(fluent["properties"]["density"]["x-displayDecimals"].as<std::uint64_t>() ==
          literal["properties"]["density"]["x-displayDecimals"].as<std::uint64_t>());
}

TEST_CASE("Forms::FieldMeta::DisplayUnitLeavesTheWireUntouched", "[forms][field_meta][display_unit]") {
    FDUReadingAction reading{};
    reading.density = 2.5;
    std::string json{};
    REQUIRE_FALSE(glz::write_json(reading, json));
    CHECK(json.contains(R"("density":2.5)"));
    CHECK_FALSE(json.contains("kg/m"));

    FDUReadingAction decoded{};
    REQUIRE_FALSE(glz::read_json(decoded, R"({"density":1.25,"temperature":20,"specimens":3,"ratio":0.5})"));
    std::string roundTripped{};
    REQUIRE_FALSE(glz::write_json(decoded, roundTripped));
    CHECK(roundTripped.contains(R"("density":1.25)"));
}
