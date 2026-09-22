// SPDX-License-Identifier: Apache-2.0
//
// Coverage for issue #25: form generation recurses into a nested-aggregate
// member's object schema -- a directly-nested struct member or a
// `std::vector<Sub>` repeated aggregate -- applying the same
// title/x-order/required/widget rules the top level already applies, instead
// of leaving it entirely unannotated. Two distinct schema shapes exist for a
// nested aggregate (see forms.hpp's `annotateNestedAggregateRef`): glaze
// *inlines* the object schema directly into the property when the nested
// type is used exactly once in the whole schema, and *deduplicates* it via a
// shared `$defs` entry (referenced by `$ref`) when it is used two or more
// times. Both are exercised below. Recursion continues into the type graph to
// whatever depth it has: there is no depth limit, and a self- or mutually-
// referential type is described rather than rejected (morph#703 -- see
// docs/spec/forms/forms.md, "Nested aggregates (recursive, cycle-safe)"). Both
// of those cases are exercised at the bottom of this file.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/attributes.hpp>
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

struct Origin {
    std::string country;
};

// Three levels deep: DeepSpecimen -> Provenance -> Origin. Provenance's own
// member (origin) is itself a nested aggregate too -- proving recursion
// continues past one level.
struct Provenance {
    std::string collectedBy;
    Origin origin;
};

struct DeepSpecimen {
    double massDry = 0.0;
    Provenance provenance;
};

// A self-referential nested-aggregate type (a tree node). Until morph#703 this
// could not be passed to morph::forms::schemaJson<A>() at all -- either use,
// as the action type or nested inside one, tripped forms.hpp's kMaxNestDepth
// static_assert. It now can be, and is: see "Cyclic nested-aggregate types"
// at the bottom of this file.
struct TreeNode {
    std::string name;
    std::vector<TreeNode> children;
};

// A mutually referential pair, the other shape a cycle takes: Ay -> Bee -> Ay.
// Deliberately without default member initialisers -- a `std::vector<Bee>{}`
// NSDMI instantiates ~vector<Bee> while Bee is still incomplete, which is a
// libstdc++ hard error having nothing to do with morph.
struct Bee;

struct Ay {
    std::string tag;
    std::vector<Bee> bees;
};

struct Bee {
    std::string tag;
    std::vector<Ay> ays;
};

// An acyclic chain four levels past the old 16-level cap, which is the other
// thing morph#703 removed. Written out rather than macro-generated so the
// fixture reads as what it is.
struct Deep0 {
    int leaf = 0;
};
struct Deep1 {
    Deep0 inner;
};
struct Deep2 {
    Deep1 inner;
};
struct Deep3 {
    Deep2 inner;
};
struct Deep4 {
    Deep3 inner;
};
struct Deep5 {
    Deep4 inner;
};
struct Deep6 {
    Deep5 inner;
};
struct Deep7 {
    Deep6 inner;
};
struct Deep8 {
    Deep7 inner;
};
struct Deep9 {
    Deep8 inner;
};
struct Deep10 {
    Deep9 inner;
};
struct Deep11 {
    Deep10 inner;
};
struct Deep12 {
    Deep11 inner;
};
struct Deep13 {
    Deep12 inner;
};
struct Deep14 {
    Deep13 inner;
};
struct Deep15 {
    Deep14 inner;
};
struct Deep16 {
    Deep15 inner;
};
struct Deep17 {
    Deep16 inner;
};
struct Deep18 {
    Deep17 inner;
};
struct Deep19 {
    Deep18 inner;
};
struct Deep20 {
    Deep19 inner;
};

// Specimen and Attachment are each used from two places below, so glaze
// deduplicates both via a shared `$defs` entry referenced by `$ref`.
struct Record {
    std::string operatorName;       // flat -- unaffected by this feature
    double temperature = 0.0;       // flat -- unaffected by this feature
    Specimen reference;             // single nested aggregate ($ref form)
    Specimen secondary;             // second use of Specimen -> forces $ref/$defs
    Attachment primary;             // single nested aggregate ($ref form)
    std::vector<Attachment> files;  // second use of Attachment -> forces $ref/$defs
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
using nestedforms::Ay;
using nestedforms::BareQuantityRecord;
using nestedforms::DeclaredOptionalRecord;
using nestedforms::Deep20;
using nestedforms::DeepRecord;
using nestedforms::DeepSpecimen;
using nestedforms::Origin;
using nestedforms::PlainMetaRecord;
using nestedforms::Provenance;
using nestedforms::Record;
using nestedforms::RichRecord;
using nestedforms::SingleUseRecord;
using nestedforms::SingleUseVectorRecord;
using nestedforms::Specimen;
using nestedforms::TreeNode;

namespace {

// Resolves the object-schema DOM node for a nested-aggregate member, given
// the property (or array `items`) node glaze wrote for it -- mirroring
// exactly what forms.hpp's `annotateNestedAggregateRef` resolves in
// production: a `$ref` into `$defs` (2+ uses) or the node itself, inlined
// (exactly one use).
const glz::generic_u64& resolveNestedSchema(const glz::generic_u64& dom,
                                            const glz::generic_u64& propertyOrItems MORPH_LIFETIMEBOUND) {
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

// ── Recursion continues past one level (no depth cap) ───────────────────────

TEST_CASE("Forms::SchemaJson::NestedAggregate: recursion continues past one level to whatever depth exists",
          "[forms][nested][issue25]") {
    auto const schema = morph::forms::schemaJson<DeepRecord>();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    REQUIRE(dom["properties"].contains("sample"));
    auto const& level1Def = resolveNestedSchema(dom, dom["properties"]["sample"]);

    // Level 1 (DeepSpecimen's own members) is annotated.
    CHECK(level1Def["properties"]["massDry"].contains("x-order"));
    CHECK(level1Def["properties"]["massDry"].contains("title"));
    REQUIRE(level1Def.contains("required"));

    // Level 2 (Provenance, nested inside DeepSpecimen) is now annotated too --
    // both its own property node (x-order/title, same as any level-1 member)
    // and, unlike the old one-level cap, its own "required" array.
    REQUIRE(level1Def["properties"].contains("provenance"));
    CHECK(level1Def["properties"]["provenance"].contains("x-order"));
    CHECK(level1Def["properties"]["provenance"].contains("title"));
    auto const& level2Def = resolveNestedSchema(dom, level1Def["properties"]["provenance"]);
    REQUIRE(level2Def.contains("required"));
    CHECK(level2Def["properties"]["collectedBy"].contains("x-order"));
    CHECK(level2Def["properties"]["collectedBy"].contains("title"));

    // Level 3 (Origin, nested inside Provenance) is annotated too -- proving
    // recursion does not stop at two levels either.
    REQUIRE(level2Def["properties"].contains("origin"));
    CHECK(level2Def["properties"]["origin"].contains("x-order"));
    CHECK(level2Def["properties"]["origin"].contains("title"));
    auto const& level3Def = resolveNestedSchema(dom, level2Def["properties"]["origin"]);
    REQUIRE(level3Def.contains("required"));
    CHECK(level3Def["properties"]["country"].contains("x-order"));
    CHECK(level3Def["properties"]["country"].contains("title"));
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

TEST_CASE("Forms::SchemaJson::NestedAggregate: a FieldMeta entry with no optional attributes leaves them all unset",
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

TEST_CASE("Forms::SchemaJson::NestedAggregate: optionalFields marks a non-std::optional nested member as not required",
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
//
// The recursion carries no depth NTTP since morph#703 -- only the `visited`
// set, the shared-$defs bookkeeping it threads through. A fresh, empty set per
// call is what mergeSchemaExtras hands the recursion at the start of each
// schema.

TEST_CASE("Forms::SchemaJson::NestedAggregate: annotateNestedAggregateRef leaves a non-string $ref untouched",
          "[forms][nested][issue25]") {
    glz::generic_u64 dom{};
    glz::generic_u64 property{};
    property["$ref"] = std::uint64_t{42};  // malformed: $ref present but not a string
    morph::forms::detail::NestedDefsVisited visited{};
    morph::forms::detail::annotateNestedAggregateRef<Specimen>(morph::forms::detail::SchemaDomRef{dom}, property,
                                                               visited);
    CHECK_FALSE(property.contains("required"));
}

TEST_CASE("Forms::SchemaJson::NestedAggregate: annotateNestedAggregateRef leaves a $ref outside #/$defs/ untouched",
          "[forms][nested][issue25]") {
    glz::generic_u64 dom{};
    glz::generic_u64 property{};
    property["$ref"] = std::string{"#/other/Specimen"};
    morph::forms::detail::NestedDefsVisited visited{};
    morph::forms::detail::annotateNestedAggregateRef<Specimen>(morph::forms::detail::SchemaDomRef{dom}, property,
                                                               visited);
    CHECK_FALSE(dom.contains("$defs"));
}

TEST_CASE(
    "Forms::SchemaJson::NestedAggregate: annotateNestedAggregateRef leaves a schema with neither $ref nor "
    "properties untouched",
    "[forms][nested][issue25]") {
    glz::generic_u64 dom{};
    glz::generic_u64 property{};
    property["type"] = std::string{"string"};  // glaze emitted something other than an object schema
    morph::forms::detail::NestedDefsVisited visited{};
    morph::forms::detail::annotateNestedAggregateRef<Specimen>(morph::forms::detail::SchemaDomRef{dom}, property,
                                                               visited);
    CHECK_FALSE(property.contains("required"));
    CHECK(property["type"].get<std::string>() == "string");
}

TEST_CASE(
    "Forms::SchemaJson::NestedAggregate: annotateNestedAggregateRef leaves a $ref untouched when dom has no $defs "
    "at all",
    "[forms][nested][issue25]") {
    // A #/$defs/-prefixed $ref, but the whole dom never got a "$defs" object
    // in the first place -- the first half of the `dom.contains("$defs") &&
    // dom["$defs"].contains(key)` guard. Well-formed glaze output never
    // produces this (a $ref into $defs implies $defs exists), so this only
    // changes behavior for malformed input, which is left untouched (see the
    // function's own doc comment).
    glz::generic_u64 dom{};
    glz::generic_u64 property{};
    property["$ref"] = std::string{"#/$defs/Specimen"};
    morph::forms::detail::NestedDefsVisited visited{};
    morph::forms::detail::annotateNestedAggregateRef<Specimen>(morph::forms::detail::SchemaDomRef{dom}, property,
                                                               visited);
    CHECK_FALSE(dom.contains("$defs"));
    CHECK(property["$ref"].get<std::string>() == "#/$defs/Specimen");
}

TEST_CASE(
    "Forms::SchemaJson::NestedAggregate: annotateNestedAggregateRef leaves a $ref untouched when the named $defs "
    "key doesn't exist",
    "[forms][nested][issue25]") {
    // $defs exists this time, but not under the referenced key -- the second
    // half of the same guard. Also malformed-input-only (see above).
    glz::generic_u64 dom{};
    dom["$defs"]["SomeOtherType"]["properties"] = glz::generic_u64::object_t{};
    glz::generic_u64 property{};
    property["$ref"] = std::string{"#/$defs/Specimen"};
    morph::forms::detail::NestedDefsVisited visited{};
    morph::forms::detail::annotateNestedAggregateRef<Specimen>(morph::forms::detail::SchemaDomRef{dom}, property,
                                                               visited);
    CHECK_FALSE(dom["$defs"].contains("Specimen"));
}

TEST_CASE(
    "Forms::SchemaJson::NestedAggregate: recurseIntoNestedAggregateIfAny leaves a std::vector<Sub> property "
    "untouched when it has no \"items\" node",
    "[forms][nested][issue25]") {
    // Every std::vector<Sub> member schemaJson<A>() actually produces (via
    // glaze) has an "items" node -- annotateNestedAggregate always calls
    // this with real glaze output, never a hand-built one, so this arm has
    // never been driven false. Malformed-input-only, same as
    // annotateNestedAggregateRef's own defensive fallbacks above.
    glz::generic_u64 dom{};
    glz::generic_u64 property{};
    property["type"] = std::string{"array"};  // no "items" key
    morph::forms::detail::NestedDefsVisited visited{};
    morph::forms::detail::recurseIntoNestedAggregateIfAny<std::vector<Specimen>>(
        morph::forms::detail::SchemaDomRef{dom}, property, visited);
    CHECK_FALSE(property.contains("items"));
    CHECK_FALSE(dom.contains("$defs"));
}

// ── Self-referential nested-aggregate type, standalone ─────────────────────

TEST_CASE("Forms::SchemaJson::NestedAggregate: a self-referential nested-aggregate type round-trips fine on its own",
          "[forms][nested][issue25]") {
    // The narrow claim: TreeNode and ordinary glaze JSON round-tripping over
    // it work on their own. Kept from when that was all this file could say
    // about a cyclic type; the schema cases below are the interesting ones
    // now.
    TreeNode root{};
    root.name = "root";
    TreeNode child{};
    child.name = "child";
    root.children.push_back(child);

    std::string const json = glz::write_json(root).value_or(std::string{});
    REQUIRE_FALSE(json.empty());

    TreeNode decoded{};
    REQUIRE_FALSE(glz::read_json(decoded, json));
    CHECK(decoded.name == "root");
    REQUIRE(decoded.children.size() == 1);
    CHECK(decoded.children[0].name == "child");
}

// ── Cyclic nested-aggregate types (morph#703) ──────────────────────────────
//
// Every case below was a hard `static_assert` before morph#703 removed the
// `Depth` NTTP, `kMaxNestDepth` and the 16-level cap: the *compilation* of
// this section is therefore itself the regression test, and reinstating the
// NTTP turns these into build failures rather than assertion failures. The
// assertions on top of that pin the shape of what is emitted, so a change that
// kept it compiling while emitting a truncated or unannotated schema is caught
// too.
//
// Why instantiation terminates: the recursion carries no template argument
// that varies down it, so `annotateNestedAggregate<Sub>` calling itself is
// ordinary function recursion over a finite reachable type set. The *runtime*
// walk is stopped by the `$defs` visited set -- a cyclic type is always in
// `$defs`, because glaze inlines only a type used exactly once in the whole
// schema and a self-reference is never that.

struct SelfReferentialAction {
    std::int64_t id = 0;
    TreeNode root;
};

struct MutuallyReferentialAction {
    std::int64_t id = 0;
    Ay top;
};

struct DeeplyNestedAction {
    std::int64_t id = 0;
    Deep20 deep;
};

TEST_CASE("Forms::SchemaJson::NestedAggregate: a self-referential member yields a finite $ref-cyclic schema",
          "[forms][nested][issue703]") {
    auto const& json = morph::forms::schemaJson<SelfReferentialAction>();
    REQUIRE_FALSE(json.empty());

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, json));

    // One $defs entry for TreeNode, and it refers to itself rather than
    // expanding: that is what makes the document finite.
    auto const* const defs = morph::forms::detail::findMember(dom, "$defs");
    REQUIRE(defs != nullptr);
    auto const* const treeDef = morph::forms::detail::findMember(*defs, "nestedforms::TreeNode");
    REQUIRE(treeDef != nullptr);
    auto const* const props = morph::forms::detail::findMember(*treeDef, "properties");
    REQUIRE(props != nullptr);
    auto const* const children = morph::forms::detail::findMember(*props, "children");
    REQUIRE(children != nullptr);
    auto const* const items = morph::forms::detail::findMember(*children, "items");
    REQUIRE(items != nullptr);
    auto const* const backRef = morph::forms::detail::findMember(*items, "$ref");
    REQUIRE(backRef != nullptr);
    CHECK(backRef->get_string() == "#/$defs/nestedforms::TreeNode");

    // And it is annotated like any other nested aggregate -- the point of the
    // recursion, not merely that it stopped.
    auto const* const order = morph::forms::detail::findMember(*children, "x-order");
    REQUIRE(order != nullptr);
    CHECK(order->get<std::uint64_t>() == 1);
    auto const* const required = morph::forms::detail::findMember(*treeDef, "required");
    REQUIRE(required != nullptr);
    CHECK(required->get_array().size() == 2);
}

TEST_CASE("Forms::SchemaJson::NestedAggregate: a mutually referential pair yields a finite schema",
          "[forms][nested][issue703]") {
    auto const& json = morph::forms::schemaJson<MutuallyReferentialAction>();
    REQUIRE_FALSE(json.empty());

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, json));

    // Ay is the shared $defs entry; Bee is inlined inside it (used once), and
    // its own `ays` member refers back to Ay.
    auto const* const defs = morph::forms::detail::findMember(dom, "$defs");
    REQUIRE(defs != nullptr);
    auto const* const ayDef = morph::forms::detail::findMember(*defs, "nestedforms::Ay");
    REQUIRE(ayDef != nullptr);
    auto const* const ayProps = morph::forms::detail::findMember(*ayDef, "properties");
    REQUIRE(ayProps != nullptr);
    auto const* const bees = morph::forms::detail::findMember(*ayProps, "bees");
    REQUIRE(bees != nullptr);
    auto const* const beeItems = morph::forms::detail::findMember(*bees, "items");
    REQUIRE(beeItems != nullptr);
    auto const* const beeProps = morph::forms::detail::findMember(*beeItems, "properties");
    REQUIRE(beeProps != nullptr);
    auto const* const ays = morph::forms::detail::findMember(*beeProps, "ays");
    REQUIRE(ays != nullptr);
    auto const* const ayItems = morph::forms::detail::findMember(*ays, "items");
    REQUIRE(ayItems != nullptr);
    auto const* const backRef = morph::forms::detail::findMember(*ayItems, "$ref");
    REQUIRE(backRef != nullptr);
    CHECK(backRef->get_string() == "#/$defs/nestedforms::Ay");

    // Bee's inlined schema is annotated too, which is what says the recursion
    // descended through the cycle rather than stopping at its rim.
    auto const* const beeRequired = morph::forms::detail::findMember(*beeItems, "required");
    REQUIRE(beeRequired != nullptr);
    CHECK(beeRequired->get_array().size() == 2);
}

TEST_CASE("Forms::SchemaJson::NestedAggregate: nesting past the old 16-level cap compiles and is annotated",
          "[forms][nested][issue703]") {
    auto const& json = morph::forms::schemaJson<DeeplyNestedAction>();
    REQUIRE_FALSE(json.empty());

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, json));

    // Walk all twenty levels down and check the leaf is annotated: a
    // recursion that gave up part-way would leave `x-order` missing somewhere
    // along this chain.
    auto const* node = morph::forms::detail::findMember(dom, "properties");
    REQUIRE(node != nullptr);
    node = morph::forms::detail::findMember(*node, "deep");
    REQUIRE(node != nullptr);
    for (int level = 0; level < 20; ++level) {
        auto const* const props = morph::forms::detail::findMember(*node, "properties");
        REQUIRE(props != nullptr);
        auto const* const inner = morph::forms::detail::findMember(*props, "inner");
        REQUIRE(inner != nullptr);
        node = inner;
    }
    auto const* const leafProps = morph::forms::detail::findMember(*node, "properties");
    REQUIRE(leafProps != nullptr);
    auto const* const leaf = morph::forms::detail::findMember(*leafProps, "leaf");
    REQUIRE(leaf != nullptr);
    auto const* const leafOrder = morph::forms::detail::findMember(*leaf, "x-order");
    REQUIRE(leafOrder != nullptr);
    CHECK(leafOrder->get<std::uint64_t>() == 0);
}
