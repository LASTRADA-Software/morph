// SPDX-License-Identifier: Apache-2.0
//
// `x-exactMinimum`/`x-exactMaximum`: exact decimal companions for a numeric bound
// a double cannot hold (morph#213).
//
// `mergeSchemaExtras` already reads the schema in u64 number mode so int64
// bounds are not rounded on the C++ side. They are rounded anyway the moment a
// renderer runs `JSON.parse(controller.schemasJson)` -- which every shipped app
// does -- so `INT64_MAX` reaches the renderer as 9223372036854775808 and a gate
// comparing against it admits `INT64_MAX + 1` as "not greater". A string
// survives JSON.parse intact; these tests pin that the string is emitted, and
// emitted *only* where a double would actually lose something.
//
// src/qt/forms/tests/tst_DynamicFormExactBounds.qml pins the renderer half
// against the same key names.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <string>

// File-scope (not anonymous-namespaced): glaze's reflection needs a type with
// linkage. EB prefix keeps these unique for the file-scope-collision CI check.
//
// NOLINTBEGIN(misc-use-internal-linkage) -- an anonymous namespace is exactly
// what these cannot have; same suppression as tests/test_shared_instances.cpp.
// NOLINTBEGIN(cert-err58-cpp,bugprone-throwing-static-initialization,misc-const-correctness) -- the
// BRIDGE_REGISTER_* macros register through throwing static initialisers by
// design; every test that registers a model has this shape.
struct EBWideAction {
    std::int64_t id = 0;
    std::uint64_t tag = 0;
};

struct EBWideModel {
    std::int64_t lastSeen = 0;

    bool execute(const EBWideAction& action) {
        lastSeen = action.id;
        return true;
    }
};

BRIDGE_REGISTER_MODEL(EBWideModel, "Test_EBWide_Model")
BRIDGE_REGISTER_ACTION(EBWideModel, EBWideAction, "Test_EBWide_Action")

struct EBNarrowAction {
    std::int32_t small = 0;
};

struct EBNarrowModel {
    std::int32_t lastSeen = 0;

    bool execute(const EBNarrowAction& action) {
        lastSeen = action.small;
        return true;
    }
};

BRIDGE_REGISTER_MODEL(EBNarrowModel, "Test_EBNarrow_Model")
BRIDGE_REGISTER_ACTION(EBNarrowModel, EBNarrowAction, "Test_EBNarrow_Action")
// NOLINTEND(cert-err58-cpp,bugprone-throwing-static-initialization,misc-const-correctness)
// NOLINTEND(misc-use-internal-linkage)

TEST_CASE("schemaJson emits exact text companions for int64 bounds", "[forms][bounds]") {
    auto const schema = morph::forms::schemaJson<EBWideAction>();
    // The digits must be exact, not the double-rounded 9223372036854775808.
    CHECK(schema.contains(R"("x-exactMaximum":"9223372036854775807")"));
    CHECK(schema.contains(R"("x-exactMinimum":"-9223372036854775808")"));
}

TEST_CASE("schemaJson emits an exact text companion for a uint64 maximum", "[forms][bounds]") {
    auto const schema = morph::forms::schemaJson<EBWideAction>();
    CHECK(schema.contains(R"("x-exactMaximum":"18446744073709551615")"));
}

TEST_CASE("schemaJson leaves bounds a double holds exactly untouched", "[forms][bounds]") {
    // The reason this is not emitted unconditionally: an ordinary schema loses
    // nothing to a double, and stays byte-for-byte what it was before #213.
    auto const schema = morph::forms::schemaJson<EBNarrowAction>();
    CHECK_FALSE(schema.contains("x-exactMinimum"));
    CHECK_FALSE(schema.contains("x-exactMaximum"));
}

TEST_CASE("the exact companion sits beside the bound it belongs to, in $defs", "[forms][bounds]") {
    // Parsed rather than substring-matched: the renderer resolves a property's
    // `$ref` into `$defs` and reads the companion from the merged node, so a
    // companion written to the wrong node would be invisible to it.
    auto const schema = morph::forms::schemaJson<EBWideAction>();
    auto parsed = glz::read_json<glz::generic>(schema);
    REQUIRE(parsed.has_value());
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- glaze DOM requires operator[]
    auto const& root = parsed.value();
    REQUIRE(root.contains("$defs"));
    auto const& defs = root["$defs"];
    REQUIRE(defs.contains("int64_t"));
    auto const& int64Def = defs["int64_t"];
    REQUIRE(int64Def.contains("x-exactMaximum"));
    CHECK(int64Def["x-exactMaximum"].get<std::string>() == "9223372036854775807");
    // And the numeric bound it shadows is still present, unchanged.
    CHECK(int64Def.contains("maximum"));
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}
