// SPDX-License-Identifier: Apache-2.0
//
// The C++ half of morph#189. DynamicForm used to emit a boolean field and an
// `anyOf` integer field as JSON *strings*; this file pins the two facts that
// make that a defect rather than a cosmetic difference:
//
//   1. `schemaJson<A>()` really does emit `{"type":"boolean"}` for a `bool`
//      member, and an `anyOf` with **no top-level "type" key** for a bare
//      `std::optional<std::int64_t>`. Those are the exact shapes the QML
//      renderer has to recognise, so if glaze's schema output ever changes
//      shape, this fails here rather than silently un-fixing the renderer.
//   2. `ActionTraits<A>::fromJson` -- the same codec `bridge.hpp` and
//      `registry.hpp` use to admit an action payload -- *accepts* the bare
//      literals the fixed renderer now emits and *rejects* the quoted ones it
//      used to emit. glaze does not coerce.
//
// The emission side is asserted in src/qt/forms/tests/
// tst_DynamicFormBooleanAndAnyOf.qml against the same literals. The two
// halves cannot run in one process (the renderer is QML, the codec is C++),
// so they are deliberately written against identical payload text.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <optional>
#include <string>

// File-scope names are prefixed to stay unique across translation units --
// the repo has a CI check for file-scope type-name collisions, and a clash
// would be an ODR violation rather than a mere shadowing nuisance.
struct BAOWireAction {
    bool flag = false;
    std::optional<std::int64_t> optI64;
};

struct BAOWireModel {
    bool execute(const BAOWireAction& action) { return action.flag; }
};

BRIDGE_REGISTER_MODEL(BAOWireModel, "Test_BAOWire_Model")
BRIDGE_REGISTER_ACTION(BAOWireModel, BAOWireAction, "Test_BAOWire_Action")

TEST_CASE("morph::forms: a bool member emits {\"type\":\"boolean\"}", "[forms][boolean]") {
    auto const schema = morph::forms::schemaJson<BAOWireAction>();
    // Asserted as the exact property text rather than a bare "boolean"
    // substring: the loose version would also pass on a schema that merely
    // mentioned the word somewhere, which is the shape of check this repo
    // keeps finding to be measuring nothing.
    CHECK(schema.find(R"("flag":{"type":"boolean")") != std::string::npos);
    // A plain `bool` is *required*, which is why the renderer seeds an
    // untouched required checkbox to false instead of leaving the form
    // unsatisfiable.
    CHECK(schema.find(R"("required":["flag"])") != std::string::npos);
}

TEST_CASE("morph::forms: a bare std::optional<int64_t> member emits anyOf with no top-level type",
          "[forms][boolean]") {
    auto const schema = morph::forms::schemaJson<BAOWireAction>();
    // This is *why* the renderer has to resolve through anyOf: the property
    // carries no "type" key of its own, so every kind flag was false and the
    // value fell through to the plain-text (quoted) encoding.
    CHECK(schema.find(R"("optI64":{"anyOf":[{"$ref":"#/$defs/int64_t"},{"type":"null"}])") != std::string::npos);
    // The integer type lives behind the $ref, in $defs -- one indirection
    // further than the top-level-$ref shape that already worked.
    CHECK(schema.find(R"("int64_t":{"type":"integer")") != std::string::npos);
}

TEST_CASE("morph::core: fromJson accepts the bare literals the fixed renderer emits", "[forms][boolean]") {
    SECTION("bare true") {
        auto const action = morph::model::ActionTraits<BAOWireAction>::fromJson(R"({"flag":true})");
        CHECK(action.flag == true);
    }
    SECTION("bare false") {
        auto const action = morph::model::ActionTraits<BAOWireAction>::fromJson(R"({"flag":false})");
        CHECK(action.flag == false);
    }
    SECTION("an int64 beyond 2^53 survives exactly") {
        auto const action =
            morph::model::ActionTraits<BAOWireAction>::fromJson(R"({"flag":true,"optI64":9007199254740993})");
        REQUIRE(action.optI64.has_value());
        CHECK(*action.optI64 == 9007199254740993LL);
    }
    SECTION("INT64_MAX survives exactly") {
        auto const action =
            morph::model::ActionTraits<BAOWireAction>::fromJson(R"({"flag":true,"optI64":9223372036854775807})");
        REQUIRE(action.optI64.has_value());
        CHECK(*action.optI64 == 9223372036854775807LL);
    }
}

TEST_CASE("morph::core: fromJson rejects the quoted literals the renderer used to emit", "[forms][boolean]") {
    // These are the payloads morph#189 measured as rejected. If glaze ever
    // started coercing them, the renderer defect would stop being observable
    // end-to-end and this test would tell us the severity had changed.
    SECTION("a stringified boolean") {
        CHECK_THROWS(morph::model::ActionTraits<BAOWireAction>::fromJson(R"({"flag":"true"})"));
    }
    SECTION("free text in a boolean field -- what the unvalidated TextField allowed") {
        CHECK_THROWS(morph::model::ActionTraits<BAOWireAction>::fromJson(R"({"flag":"banana"})"));
    }
    SECTION("a stringified integer") {
        CHECK_THROWS(
            morph::model::ActionTraits<BAOWireAction>::fromJson(R"({"flag":true,"optI64":"9007199254740992"})"));
    }
}
