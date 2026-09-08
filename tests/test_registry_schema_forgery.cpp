// SPDX-License-Identifier: Apache-2.0
//
// registry.hpp R1/R3: `buildActionDescription<A>()`'s degrade-on-malformed-
// schema paths (lines 375-378, 380) and `ActionDispatcher::schemasJson`'s
// empty-schema substitution (line 621).
//
// Every real, `forms.hpp`-registered action produces valid, parseable
// object JSON via `morph::forms::schemaJson<A>()`, so
// `buildActionDescription`'s own "degrade, don't throw" fallback -- for
// when that JSON fails to parse, or parses to something other than an
// object -- has never fired. There is no public way to make
// `morph::forms::schemaJson<A>()` itself misbehave for a real action type;
// the only seam is a full explicit specialization of the function template
// for a test-only action type, standing in for the real, glaze-driven
// primary template. This is an ordinary, ODR-legal C++ technique (the
// primary template is never instantiated for these two types -- our
// specializations are the only definitions that ever exist for them), the
// same idea test code elsewhere in this tree already uses to specialize
// `morph::units::UnitTraits<E>` for a test-local unit enum. It is not a
// white-box hook into `buildActionDescription` itself, which is exercised
// completely normally through the public `BRIDGE_REGISTER_ACTION` +
// `ActionDispatcher` surface.
//
// R2 (`glz::write_json(dom, merged)` failing at line 386-388) is NOT
// exercised here: by the time that write runs, `dom` was already
// successfully parsed from the forged JSON and is a valid `glz::generic_u64`
// object with two ordinary string keys added to it. Making `glz::write_json`
// fail on that DOM would require forging a failure inside glaze's own
// serializer over data this code produces itself -- not something
// `schemaJson<A>()`'s return text can reach into. No trigger for this arm
// was found; see the shared task report for the full reasoning.
//
// Two more forged types below close the remaining R1 sub-conditions that
// the two above don't reach: line 378's `dom["required"].is_array()` false
// arm (a `"required"` key present but holding something other than an
// array) and line 380's `name.is_string()` false arm (a non-string element
// inside an otherwise-valid `"required"` array).

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <string>

// Model/action types need external linkage for glaze reflection (see
// tests/test_policy_hardening.cpp).

struct RegSchemaForgeEmptyAction {
    int value = 0;
};
struct RegSchemaForgeEmptyResult {
    int value = 0;
};

struct RegSchemaForgeArrayAction {
    int value = 0;
};
struct RegSchemaForgeArrayResult {
    int value = 0;
};

struct RegSchemaForgeRequiredNotArrayAction {
    int value = 0;
};
struct RegSchemaForgeRequiredNotArrayResult {
    int value = 0;
};

struct RegSchemaForgeRequiredNonStringAction {
    int value = 0;
};
struct RegSchemaForgeRequiredNonStringResult {
    int value = 0;
};

struct RegSchemaForgeModel {
    RegSchemaForgeEmptyResult execute(const RegSchemaForgeEmptyAction& action) { return {.value = action.value}; }
    RegSchemaForgeArrayResult execute(const RegSchemaForgeArrayAction& action) { return {.value = action.value}; }
    RegSchemaForgeRequiredNotArrayResult execute(const RegSchemaForgeRequiredNotArrayAction& action) {
        return {.value = action.value};
    }
    RegSchemaForgeRequiredNonStringResult execute(const RegSchemaForgeRequiredNonStringAction& action) {
        return {.value = action.value};
    }
};

// Forged BEFORE the BRIDGE_REGISTER_ACTION lines below, so
// buildActionDescription<A>() -- instantiated when those macros register the
// pair -- resolves `::morph::forms::schemaJson<A>()` to this specialization
// rather than implicitly instantiating (and thereby closing off
// specialization of) the primary, glaze-driven template.
namespace morph::forms {

/// R1's first sub-condition (`glz::read_json` fails) and R3
/// (`desc.schema.empty()`) at once: an empty string is not valid JSON, and
/// `buildActionDescription`'s degrade path leaves `desc.schema` exactly as
/// `schemaJson<A>()` returned it (unmodified) on a read failure.
template <>
inline std::string schemaJson<RegSchemaForgeEmptyAction>() {
    return "";
}

/// R1's second sub-condition: valid, parseable JSON that is not an object.
template <>
inline std::string schemaJson<RegSchemaForgeArrayAction>() {
    return "[1,2,3]";
}

/// R1's third sub-condition (registry.hpp:378): a `"required"` key that is
/// present but not a JSON array -- `dom.contains("required")` is true while
/// `dom["required"].is_array()` is false, so the `required`-collection loop
/// is skipped entirely and `desc.required` stays empty.
template <>
inline std::string schemaJson<RegSchemaForgeRequiredNotArrayAction>() {
    return R"({"required":"nope"})";
}

/// R1's fourth sub-condition (registry.hpp:380): a `"required"` array that
/// parses fine but holds a non-string element alongside a string one --
/// `name.is_string()` is false for the number, so it's skipped, while the
/// string element after it is still collected.
template <>
inline std::string schemaJson<RegSchemaForgeRequiredNonStringAction>() {
    return R"({"required":[1,"a"]})";
}

}  // namespace morph::forms

BRIDGE_REGISTER_MODEL(RegSchemaForgeModel, "RegSchemaForge_Model")
BRIDGE_REGISTER_ACTION(RegSchemaForgeModel, RegSchemaForgeEmptyAction, "RegSchemaForge_Empty")
BRIDGE_REGISTER_ACTION(RegSchemaForgeModel, RegSchemaForgeArrayAction, "RegSchemaForge_Array")
BRIDGE_REGISTER_ACTION(RegSchemaForgeModel, RegSchemaForgeRequiredNotArrayAction, "RegSchemaForge_RequiredNotArray")
BRIDGE_REGISTER_ACTION(RegSchemaForgeModel, RegSchemaForgeRequiredNonStringAction, "RegSchemaForge_RequiredNonString")

TEST_CASE("registry.hpp R1/R3: an unparseable forged schema degrades to an empty, non-null required list",
          "[registry][schemas][r1][r3]") {
    using morph::model::detail::ActionDispatcher;

    // buildActionDescription's read-failure arm (375-377) returns `desc`
    // with `required` left at its default-constructed (empty) state --
    // distinct from nullptr, which only an *unregistered* pair reports
    // (see "ActionDispatcher::requiredFieldsFor returns nullptr for an
    // unregistered pair" in test_wire_schemas.cpp). The pair is registered
    // here, so this must be non-null but empty.
    const auto* required =
        ActionDispatcher::instance().requiredFieldsFor("RegSchemaForge_Model", "RegSchemaForge_Empty");
    REQUIRE(required != nullptr);
    REQUIRE(required->empty());

    // R3: schemasJson substitutes "{}" for this action's entry because
    // desc.schema itself stayed empty.
    auto const document = ActionDispatcher::instance().schemasJson("RegSchemaForge_Model");
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, document));
    REQUIRE(dom.contains("RegSchemaForge_Empty"));
    REQUIRE(dom["RegSchemaForge_Empty"].is_object());
    REQUIRE(dom["RegSchemaForge_Empty"].get_object().empty());
}

TEST_CASE("registry.hpp R1: a forged non-object schema degrades, and schemasJson serves it verbatim",
          "[registry][schemas][r1]") {
    using morph::model::detail::ActionDispatcher;

    // buildActionDescription's `!dom.is_object()` arm (also 375-377) leaves
    // `desc.schema` as the raw forged text (never reaches the
    // required-array/x-payloadFingerprint/x-payloadShape merge below it) and
    // `required` empty.
    const auto* required =
        ActionDispatcher::instance().requiredFieldsFor("RegSchemaForge_Model", "RegSchemaForge_Array");
    REQUIRE(required != nullptr);
    REQUIRE(required->empty());

    // schemasJson's `desc.schema.empty()` is false here (the raw text is
    // "[1,2,3]", not empty), so it is spliced in verbatim -- observably an
    // array, not the object every real action's entry is.
    auto const document = ActionDispatcher::instance().schemasJson("RegSchemaForge_Model");
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, document));
    REQUIRE(dom.contains("RegSchemaForge_Array"));
    REQUIRE(dom["RegSchemaForge_Array"].is_array());
    REQUIRE(dom["RegSchemaForge_Array"].get_array().size() == 3);
}

TEST_CASE("registry.hpp R1: a \"required\" key that isn't an array is ignored, not iterated",
          "[registry][schemas][r1]") {
    using morph::model::detail::ActionDispatcher;

    // buildActionDescription's `dom["required"].is_array()` false arm
    // (registry.hpp:378): the DOM parses fine and is an object, so the
    // read-failure/non-object arms above don't fire, but "required" holds a
    // string rather than an array, so the collection loop is skipped and
    // `required` stays empty rather than throwing or misreading the string
    // as a single-element list.
    const auto* required =
        ActionDispatcher::instance().requiredFieldsFor("RegSchemaForge_Model", "RegSchemaForge_RequiredNotArray");
    REQUIRE(required != nullptr);
    REQUIRE(required->empty());

    // The merge past line 378 still runs (this arm doesn't return early),
    // so schemasJson's entry carries the forged "required" string plus the
    // two x-payload* annotations, and is served as an object.
    auto const document = ActionDispatcher::instance().schemasJson("RegSchemaForge_Model");
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, document));
    REQUIRE(dom.contains("RegSchemaForge_RequiredNotArray"));
    const auto& entry = dom["RegSchemaForge_RequiredNotArray"];
    REQUIRE(entry.is_object());
    REQUIRE(entry.contains("required"));
    REQUIRE(entry["required"].is_string());
    REQUIRE(entry["required"].get_string() == "nope");
}

TEST_CASE("registry.hpp R1: a non-string element inside \"required\" is skipped, its string sibling kept",
          "[registry][schemas][r1]") {
    using morph::model::detail::ActionDispatcher;

    // buildActionDescription's `name.is_string()` false arm (registry.hpp:380):
    // the array itself is valid, so the loop runs, but its first element (a
    // number) is silently skipped while the second (a string) is collected --
    // proving the per-element guard, not just the outer is_array() check,
    // does real filtering.
    const auto* required =
        ActionDispatcher::instance().requiredFieldsFor("RegSchemaForge_Model", "RegSchemaForge_RequiredNonString");
    REQUIRE(required != nullptr);
    REQUIRE(required->size() == 1);
    REQUIRE(required->at(0) == "a");
}
