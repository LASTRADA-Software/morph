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
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/forms/forms.hpp>
#include <optional>
#include <string>
#include <vector>

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

}  // namespace nestedforms

using nestedforms::Attachment;
using nestedforms::DeepRecord;
using nestedforms::DeepSpecimen;
using nestedforms::Provenance;
using nestedforms::Record;
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
