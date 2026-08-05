// SPDX-License-Identifier: Apache-2.0
//
// Coverage for issue #25: form generation recurses one level into a
// nested-aggregate member's object schema -- a directly-nested struct member
// or a `std::vector<Sub>` repeated aggregate -- applying the same
// title/x-order/required/widget rules the top level already applies, instead
// of leaving it entirely unannotated. Two distinct schema shapes exist for a
// nested aggregate (see forms.hpp's `annotateNestedAggregateRef`): glaze
// *inlines* the object schema directly into the property when the nested
// type is used exactly once in the whole schema, and *deduplicates* it via a
// shared `$defs` entry (referenced by `$ref`) when it is used two or more
// times. Both are exercised below. Recursion stops after one level (see
// docs/spec/forms/forms.md, "Nested aggregates (one level)").

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/forms/choice.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <vector>

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

// A minimal application unit system, purely so a nested-aggregate member can
// carry a Quantity field (see RichSub below) -- exercises the
// `annotateBasicMemberProperty` Quantity branch one level down, which none of
// the plain-scalar nested types above (Specimen/Attachment/Provenance)
// touch. `relations` declares a g<->kg conversion so RichSub's `mass` field
// also exercises the `unitAlternatives()`-non-empty branch one level down
// (see the "FieldMeta/Quantity/Choice" test case below).
enum class NestedFormUnit : std::uint8_t { scalar, kg, g };

template <>
struct morph::units::UnitTraits<NestedFormUnit> {
    static constexpr morph::units::UnitMeta meta(NestedFormUnit unit) noexcept {
        switch (unit) {
            case NestedFormUnit::kg:
                return {.id = "kg", .display = "kg", .defaultDecimals = 3};
            case NestedFormUnit::g:
                return {.id = "g", .display = "g", .defaultDecimals = 1};
            case NestedFormUnit::scalar:
            default:
                return {.id = "scalar", .display = "", .defaultDecimals = 3};
        }
    }

    static constexpr std::array<morph::units::UnitRelation<NestedFormUnit>, 1> relations{
        {{NestedFormUnit::g, NestedFormUnit::kg, Rational{Numerator{1}, Denominator{1000}, DecimalPlaces{3}}}}};
};

// Named namespace (not anonymous): glaze reflection requires the reflected
// type to have linkage (see test_bridge_fixes.cpp for the same note).
namespace nestedforms {

struct Specimen {
    double massDry = 0.0;
    double massWet = 0.0;
    std::optional<std::string> note;  // never required, one level down either
};

struct Attachment {
    std::string filename;
    std::int64_t sizeBytes = 0;
};

struct Provenance {
    std::string collectedBy;
};

// A nested aggregate whose own member is itself a nested aggregate -- the
// depth-limit case: `provenance`'s own sub-members must stay unannotated.
struct DeepSpecimen {
    double massDry = 0.0;
    Provenance provenance;
};

// Specimen and Attachment are each used from two places below, so glaze
// deduplicates both via a shared `$defs` entry referenced by `$ref`.
struct Record {
    std::string operatorName;         // flat -- unaffected by this feature
    double temperature = 0.0;         // flat -- unaffected by this feature
    Specimen reference;                // single nested aggregate ($ref form)
    Specimen secondary;                // second use of Specimen -> forces $ref/$defs
    Attachment primary;                // single nested aggregate ($ref form)
    std::vector<Attachment> files;     // second use of Attachment -> forces $ref/$defs
};

// Specimen used exactly once here -- glaze inlines the object schema
// directly into the "reference" property instead of using $defs/$ref.
struct SingleUseRecord {
    Specimen reference;
};

// Attachment used exactly once here (only via the vector) -- glaze inlines
// the object schema into the "files" property's "items" instead of $defs/$ref.
struct SingleUseVectorRecord {
    std::vector<Attachment> files;
};

struct DeepRecord {
    DeepSpecimen sample;
};

// A nested aggregate whose own members carry the same annotation-worthy
// shapes the top-level pass already covers elsewhere (FieldMeta, Quantity,
// Choice): proves `annotateBasicMemberProperty` applies those rules one level
// down too, not just title/x-order/required (the only things Specimen/
// Attachment/Provenance above exercise).
struct RichSub {
    std::int64_t code = 0;
    morph::units::Quantity<NestedFormUnit::kg> mass{};
    morph::forms::Choice<std::int64_t, "NestedFormListOptions"> option;

    static constexpr std::array fieldMetadata{
        morph::forms::FieldMeta{.field = "code",
                                 .label = "Custom Code",
                                 .help = "Help text",
                                 .placeholder = "e.g. 42",
                                 .i18nKey = "custom.code",
                                 .widget = "custom-widget",
                                 .readOnly = true,
                                 .hidden = true},
    };
};

// RichSub used exactly once -- inlined, not deduplicated via $defs/$ref (see
// the "inline form" tests above for why that matters to resolution).
struct RichRecord {
    RichSub rich;
};

// A nested aggregate whose FieldMeta entry sets only .field/.label -- the
// mirror of RichSub's "code" above: fieldMeta is found, but every optional
// attribute (help/placeholder/readOnly/hidden/i18nKey) is left at its default,
// so each of their "found but set" branches must resolve false one level down.
struct PlainMetaSub {
    std::int64_t code = 0;

    static constexpr std::array fieldMetadata{
        morph::forms::FieldMeta{.field = "code", .label = "Plain Code"},
    };
};

struct PlainMetaRecord {
    PlainMetaSub plain;
};

// A nested aggregate declaring a non-`std::optional`-typed member optional via
// `optionalFields` -- exercises `declaredOptional<Sub>` one level down
// (Specimen/DeepSpecimen above only ever exercise the std::optional path).
struct DeclaredOptionalSub {
    std::string label;

    static constexpr std::array<std::string_view, 1> optionalFields{"label"};
};

struct DeclaredOptionalRecord {
    DeclaredOptionalSub sub;
};

}  // namespace nestedforms

// A unit with no declared relations, so `unitAlternatives()` is empty --
// exercises the "no unit alternatives" branch of `annotateBasicMemberProperty`
// one level down (RichSub's `mass` above only ever exercises the non-empty
// case).
enum class BareFormUnit : std::uint8_t { scalar };

template <>
struct morph::units::UnitTraits<BareFormUnit> {
    static constexpr morph::units::UnitMeta meta(BareFormUnit) noexcept {
        return {.id = "scalar", .display = "", .defaultDecimals = 2};
    }

    static constexpr std::array<morph::units::UnitRelation<BareFormUnit>, 0> relations{};
};

namespace nestedforms {

struct BareQuantitySub {
    morph::units::Quantity<BareFormUnit::scalar> amount{};
};

struct BareQuantityRecord {
    BareQuantitySub bare;
};

}  // namespace nestedforms

using nestedforms::Attachment;
using nestedforms::BareQuantityRecord;
using nestedforms::DeclaredOptionalRecord;
using nestedforms::DeepRecord;
using nestedforms::DeepSpecimen;
using nestedforms::PlainMetaRecord;
using nestedforms::Provenance;
using nestedforms::Record;
using nestedforms::RichRecord;
using nestedforms::SingleUseRecord;
using nestedforms::SingleUseVectorRecord;
using nestedforms::Specimen;

namespace {

// Resolves the object-schema DOM node for a nested-aggregate member, given
// the property (or array `items`) node glaze wrote for it -- mirroring
// exactly what forms.hpp's `annotateNestedAggregateRef` resolves in
// production: a `$ref` into `$defs` (2+ uses) or the node itself, inlined
// (exactly one use).
const glz::generic_u64& resolveNestedSchema(const glz::generic_u64& dom, const glz::generic_u64& propertyOrItems) {
    if (propertyOrItems.contains("$ref")) {
        constexpr std::string_view kPrefix = "#/$defs/";
        std::string const ref = propertyOrItems["$ref"].get<std::string>();
        REQUIRE(ref.starts_with(kPrefix));
        return dom["$defs"][ref.substr(kPrefix.size())];
    }
    REQUIRE(propertyOrItems.contains("properties"));
    return propertyOrItems;
}

std::vector<std::string> requiredNamesOf(const glz::generic_u64& node) {
    std::vector<std::string> out;
    for (auto const& entry : node["required"].get<glz::generic_u64::array_t>()) {
        out.push_back(entry.get<std::string>());
    }
    return out;
}

}  // namespace

// ── Flat top-level fields are unaffected ────────────────────────────────────

TEST_CASE("Forms::SchemaJson::NestedAggregate: flat top-level members still render exactly as before",
         "[forms][nested]") {
    auto const schema = morph::forms::schemaJson<Record>();
    REQUIRE_FALSE(schema.empty());

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    CHECK(dom["properties"]["operatorName"]["x-order"].as<std::uint64_t>() == 0);
    CHECK(dom["properties"]["temperature"]["x-order"].as<std::uint64_t>() == 1);
    CHECK(dom["properties"]["operatorName"]["title"].get<std::string>() == "Operator Name");
}

// ── Single nested aggregate member, deduplicated ($ref/$defs form) ─────────

TEST_CASE("Forms::SchemaJson::NestedAggregate: a nested struct member's $defs entry gets annotated ($ref form)",
         "[forms][nested][issue25]") {
    auto const schema = morph::forms::schemaJson<Record>();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    REQUIRE(dom["properties"].contains("reference"));
    auto const& def = resolveNestedSchema(dom, dom["properties"]["reference"]);
    REQUIRE(dom["properties"]["reference"].contains("$ref"));  // Specimen used twice -> deduplicated

    CHECK(def["properties"]["massDry"]["x-order"].as<std::uint64_t>() == 0);
    CHECK(def["properties"]["massWet"]["x-order"].as<std::uint64_t>() == 1);
    CHECK(def["properties"]["massDry"]["title"].get<std::string>() == "Mass Dry");
    CHECK(def["properties"]["massWet"]["title"].get<std::string>() == "Mass Wet");

    // required derives the same way one level down: massDry/massWet are
    // required, the std::optional note is not.
    REQUIRE(def.contains("required"));
    auto const requiredNames = requiredNamesOf(def);
    CHECK(std::find(requiredNames.begin(), requiredNames.end(), "massDry") != requiredNames.end());
    CHECK(std::find(requiredNames.begin(), requiredNames.end(), "massWet") != requiredNames.end());
    CHECK(std::find(requiredNames.begin(), requiredNames.end(), "note") == requiredNames.end());
}

// ── Repeated aggregate (std::vector<Sub>), deduplicated ($ref/$defs form) ──

TEST_CASE(
    "Forms::SchemaJson::NestedAggregate: a std::vector<Sub> repeated-aggregate member's items def is annotated "
    "($ref form)",
    "[forms][nested][issue25]") {
    auto const schema = morph::forms::schemaJson<Record>();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    REQUIRE(dom["properties"].contains("files"));
    REQUIRE(dom["properties"]["files"].contains("items"));
    auto const& itemsNode = dom["properties"]["files"]["items"];
    REQUIRE(itemsNode.contains("$ref"));  // Attachment used twice -> deduplicated
    auto const& def = resolveNestedSchema(dom, itemsNode);

    CHECK(def["properties"]["filename"]["x-order"].as<std::uint64_t>() == 0);
    CHECK(def["properties"]["sizeBytes"]["x-order"].as<std::uint64_t>() == 1);
    CHECK(def["properties"]["filename"]["title"].get<std::string>() == "Filename");

    REQUIRE(def.contains("required"));
    auto const requiredNames = requiredNamesOf(def);
    CHECK(std::find(requiredNames.begin(), requiredNames.end(), "filename") != requiredNames.end());
    CHECK(std::find(requiredNames.begin(), requiredNames.end(), "sizeBytes") != requiredNames.end());
}

// ── Single nested aggregate member, inlined (used exactly once) ────────────

TEST_CASE("Forms::SchemaJson::NestedAggregate: a singly-used nested struct member is annotated in place (inline form)",
         "[forms][nested][issue25]") {
    auto const schema = morph::forms::schemaJson<SingleUseRecord>();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    REQUIRE(dom["properties"].contains("reference"));
    CHECK_FALSE(dom["properties"]["reference"].contains("$ref"));  // inlined, not deduplicated
    auto const& def = resolveNestedSchema(dom, dom["properties"]["reference"]);

    CHECK(def["properties"]["massDry"]["x-order"].as<std::uint64_t>() == 0);
    CHECK(def["properties"]["massDry"]["title"].get<std::string>() == "Mass Dry");
    REQUIRE(def.contains("required"));
    auto const requiredNames = requiredNamesOf(def);
    CHECK(std::find(requiredNames.begin(), requiredNames.end(), "massDry") != requiredNames.end());
    CHECK(std::find(requiredNames.begin(), requiredNames.end(), "note") == requiredNames.end());
}

// ── Repeated aggregate (std::vector<Sub>), inlined (used exactly once) ─────

TEST_CASE(
    "Forms::SchemaJson::NestedAggregate: a singly-used std::vector<Sub> member's items are annotated in place "
    "(inline form)",
    "[forms][nested][issue25]") {
    auto const schema = morph::forms::schemaJson<SingleUseVectorRecord>();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    REQUIRE(dom["properties"].contains("files"));
    REQUIRE(dom["properties"]["files"].contains("items"));
    auto const& itemsNode = dom["properties"]["files"]["items"];
    CHECK_FALSE(itemsNode.contains("$ref"));  // inlined, not deduplicated
    auto const& def = resolveNestedSchema(dom, itemsNode);

    CHECK(def["properties"]["filename"]["x-order"].as<std::uint64_t>() == 0);
    CHECK(def["properties"]["sizeBytes"]["x-order"].as<std::uint64_t>() == 1);
    REQUIRE(def.contains("required"));
}

// ── Depth limit: exactly one level ──────────────────────────────────────────

TEST_CASE("Forms::SchemaJson::NestedAggregate: recursion stops after one level (depth cap)",
         "[forms][nested][issue25]") {
    auto const schema = morph::forms::schemaJson<DeepRecord>();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    REQUIRE(dom["properties"].contains("sample"));
    auto const& outerDef = resolveNestedSchema(dom, dom["properties"]["sample"]);

    // Level 1 (DeepSpecimen's own members) IS annotated.
    CHECK(outerDef["properties"]["massDry"].contains("x-order"));
    CHECK(outerDef["properties"]["massDry"].contains("title"));
    REQUIRE(outerDef.contains("required"));

    // Level 2 (Provenance, nested inside DeepSpecimen) is NOT annotated: its
    // object schema has no "required" key and its own properties carry no
    // x-order/title -- exactly today's pre-existing behaviour for anything
    // deeper than one level. "provenance" itself (a level-1 member) DOES get
    // an x-order/title on its own property node, same as any other member;
    // only its *inner* properties are left untouched.
    REQUIRE(outerDef["properties"].contains("provenance"));
    CHECK(outerDef["properties"]["provenance"].contains("x-order"));
    CHECK(outerDef["properties"]["provenance"].contains("title"));
    auto const& innerDef = resolveNestedSchema(dom, outerDef["properties"]["provenance"]);
    CHECK_FALSE(innerDef.contains("required"));
    CHECK_FALSE(innerDef["properties"]["collectedBy"].contains("x-order"));
    CHECK_FALSE(innerDef["properties"]["collectedBy"].contains("title"));
}

// ── FieldMeta/Quantity/Choice rules apply one level down too ────────────────

TEST_CASE(
    "Forms::SchemaJson::NestedAggregate: FieldMeta/Quantity/Choice annotations apply to a nested member's own "
    "properties",
    "[forms][nested][issue25]") {
    auto const schema = morph::forms::schemaJson<RichRecord>();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    REQUIRE(dom["properties"].contains("rich"));
    CHECK_FALSE(dom["properties"]["rich"].contains("$ref"));  // RichSub used exactly once -> inlined
    auto const& def = resolveNestedSchema(dom, dom["properties"]["rich"]);

    // FieldMeta: label/help/placeholder/readOnly/hidden, from RichSub's own
    // fieldMetadata (looked up against RichSub, the Owner one level down —
    // not against RichRecord).
    auto const& codeProp = def["properties"]["code"];
    CHECK(codeProp["title"].get<std::string>() == "Custom Code");
    CHECK(codeProp["description"].get<std::string>() == "Help text");
    CHECK(codeProp["x-placeholder"].get<std::string>() == "e.g. 42");
    CHECK(codeProp["x-readonly"].get<bool>() == true);
    CHECK(codeProp["x-hidden"].get<bool>() == true);
    CHECK(codeProp["x-i18nKey"].get<std::string>() == "custom.code");
    CHECK(codeProp["x-widget"].get<std::string>() == "custom-widget");
    // code has no FieldMeta-driven readOnly/hidden peers to compare against in
    // this fixture, so also confirm a field the fieldMetadata list does not
    // name -- mass -- carries none of these keys.
    auto const& massProp = def["properties"]["mass"];
    CHECK_FALSE(massProp.contains("x-readonly"));
    CHECK_FALSE(massProp.contains("x-hidden"));
    CHECK_FALSE(massProp.contains("x-placeholder"));

    // Quantity: x-decimalPlaces from the unit's declared decimals, plus
    // x-unitAlternatives from the g<->kg relation declared above.
    CHECK(massProp["x-decimalPlaces"].as<std::uint64_t>() == 3);
    REQUIRE(massProp.contains("x-unitAlternatives"));
    auto const& alternatives = massProp["x-unitAlternatives"].get<glz::generic_u64::array_t>();
    REQUIRE(alternatives.size() == 1);
    CHECK(alternatives[0]["id"].get<std::string>() == "g");
    CHECK(alternatives[0]["num"].as<std::int64_t>() == 1);
    CHECK(alternatives[0]["den"].as<std::int64_t>() == 1000);

    // Choice: x-optionsAction/x-optionValue/x-optionLabel.
    auto const& optionProp = def["properties"]["option"];
    CHECK(optionProp["x-optionsAction"].get<std::string>() == "NestedFormListOptions");
    CHECK(optionProp.contains("x-optionValue"));
    CHECK(optionProp.contains("x-optionLabel"));

    // required still derives correctly one level down alongside all of the above.
    REQUIRE(def.contains("required"));
    auto const requiredNames = requiredNamesOf(def);
    CHECK(std::find(requiredNames.begin(), requiredNames.end(), "code") != requiredNames.end());
}

// ── Idempotence: two members sharing the same nested type ──────────────────

TEST_CASE("Forms::SchemaJson::NestedAggregate: re-annotating a shared $defs entry is harmless", "[forms][nested]") {
    // Record reuses Specimen across "reference" and "secondary" (and
    // Attachment across "primary" and "files"): the annotation pass runs once
    // per property that resolves to a given def, so this proves multiple
    // triggers into the same def produce one consistent, non-corrupted result.
    auto const schema = morph::forms::schemaJson<Record>();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    std::string const refA = dom["properties"]["reference"]["$ref"].get<std::string>();
    std::string const refB = dom["properties"]["secondary"]["$ref"].get<std::string>();
    CHECK(refA == refB);  // same underlying type -> same $defs entry

    auto const& def = resolveNestedSchema(dom, dom["properties"]["reference"]);
    CHECK(def["properties"]["massDry"]["x-order"].as<std::uint64_t>() == 0);
    REQUIRE(def.contains("required"));
}

// ── FieldMeta found but no optional attributes set ──────────────────────────

TEST_CASE(
    "Forms::SchemaJson::NestedAggregate: a FieldMeta entry with no optional attributes leaves them all unset",
    "[forms][nested][issue25]") {
    auto const schema = morph::forms::schemaJson<PlainMetaRecord>();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    REQUIRE(dom["properties"].contains("plain"));
    auto const& def = resolveNestedSchema(dom, dom["properties"]["plain"]);
    auto const& codeProp = def["properties"]["code"];
    CHECK(codeProp["title"].get<std::string>() == "Plain Code");
    CHECK_FALSE(codeProp.contains("description"));
    CHECK_FALSE(codeProp.contains("x-placeholder"));
    CHECK_FALSE(codeProp.contains("x-readonly"));
    CHECK_FALSE(codeProp.contains("x-hidden"));
    CHECK_FALSE(codeProp.contains("x-i18nKey"));
}

// ── Quantity member with no unit alternatives ───────────────────────────────

TEST_CASE(
    "Forms::SchemaJson::NestedAggregate: a nested Quantity member with no unit alternatives omits "
    "x-unitAlternatives",
    "[forms][nested][issue25]") {
    auto const schema = morph::forms::schemaJson<BareQuantityRecord>();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    REQUIRE(dom["properties"].contains("bare"));
    auto const& def = resolveNestedSchema(dom, dom["properties"]["bare"]);
    auto const& amountProp = def["properties"]["amount"];
    CHECK(amountProp.contains("x-decimalPlaces"));
    CHECK_FALSE(amountProp.contains("x-unitAlternatives"));
}

// ── declaredOptional applies one level down too ─────────────────────────────

TEST_CASE(
    "Forms::SchemaJson::NestedAggregate: optionalFields marks a non-std::optional nested member as not required",
    "[forms][nested][issue25]") {
    auto const schema = morph::forms::schemaJson<DeclaredOptionalRecord>();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    REQUIRE(dom["properties"].contains("sub"));
    auto const& def = resolveNestedSchema(dom, dom["properties"]["sub"]);
    REQUIRE(def.contains("required"));
    auto const requiredNames = requiredNamesOf(def);
    CHECK(std::find(requiredNames.begin(), requiredNames.end(), "label") == requiredNames.end());
}

// ── annotateNestedAggregateRef's defensive fallbacks (issue #25) ───────────
//
// These call the detail function directly with hand-built DOM fragments,
// rather than through schemaJson<A>(), because glaze itself never actually
// produces the malformed shapes these branches guard against -- see the
// function's own doc comment ("left untouched rather than guessed at").

TEST_CASE("Forms::SchemaJson::NestedAggregate: annotateNestedAggregateRef leaves a non-string $ref untouched",
         "[forms][nested][issue25]") {
    glz::generic_u64 dom{};
    glz::generic_u64 property{};
    property["$ref"] = std::uint64_t{42};  // malformed: $ref present but not a string
    morph::forms::detail::annotateNestedAggregateRef<Specimen>(dom, property);
    CHECK_FALSE(property.contains("required"));
}

TEST_CASE("Forms::SchemaJson::NestedAggregate: annotateNestedAggregateRef leaves a $ref outside #/$defs/ untouched",
         "[forms][nested][issue25]") {
    glz::generic_u64 dom{};
    glz::generic_u64 property{};
    property["$ref"] = std::string{"#/other/Specimen"};
    morph::forms::detail::annotateNestedAggregateRef<Specimen>(dom, property);
    CHECK_FALSE(dom.contains("$defs"));
}

TEST_CASE(
    "Forms::SchemaJson::NestedAggregate: annotateNestedAggregateRef leaves a schema with neither $ref nor "
    "properties untouched",
    "[forms][nested][issue25]") {
    glz::generic_u64 dom{};
    glz::generic_u64 property{};
    property["type"] = std::string{"string"};  // glaze emitted something other than an object schema
    morph::forms::detail::annotateNestedAggregateRef<Specimen>(dom, property);
    CHECK_FALSE(property.contains("required"));
    CHECK(property["type"].get<std::string>() == "string");
}
