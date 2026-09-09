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

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/rational.hpp>
#include <string>

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

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
    //
    // This is named for a boundary and does not reach it: EBNarrowAction's
    // field is a std::int32_t, whose type-range bound (+-2^31) sits 22 binary
    // orders of magnitude short of kExactDoubleLimit (2^53, 9007199254740992),
    // the actual value annotateExactBound()'s std::cmp_greater/std::cmp_less
    // compare against. A schema whose bound is anywhere in that 4-quintillion-
    // wide interior would pass this case unchanged no matter where the real
    // comparison's edge sits -- it proves "well inside", not "at the edge"
    // (morph#484). The four cases below pin the edge itself, in the
    // tests/test_wire_hardening.cpp style: a control at the limit (no
    // companion) and one step past it on each side (a companion, with the
    // exact digits).
    auto const schema = morph::forms::schemaJson<EBNarrowAction>();
    CHECK_FALSE(schema.contains("x-exactMinimum"));
    CHECK_FALSE(schema.contains("x-exactMaximum"));
}

// A user-declared FieldMeta bound, not glaze's own type-range one: per
// test_forms_field_bounds.cpp's own note, emitDeclaredBound() always writes an
// int64_t (Rational::numerator), so this always lands on
// annotateExactBound()'s int64_t branch --
// `std::cmp_greater(value, kExactDoubleLimit) ||
// std::cmp_less(value, -kExactDoubleLimitSigned)` -- regardless of sign.
struct EBAtPositiveLimitAction {
    std::int64_t reading = 0;

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{.field = "reading",
                                .minimum = Rational{Numerator{9007199254740992}, Denominator{1}, DecimalPlaces{0}}},
    };
};

struct EBOnePastPositiveLimitAction {
    std::int64_t reading = 0;

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{.field = "reading",
                                .minimum = Rational{Numerator{9007199254740993}, Denominator{1}, DecimalPlaces{0}}},
    };
};

struct EBAtNegativeLimitAction {
    std::int64_t reading = 0;

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{.field = "reading",
                                .minimum = Rational{Numerator{-9007199254740992}, Denominator{1}, DecimalPlaces{0}}},
    };
};

struct EBOnePastNegativeLimitAction {
    std::int64_t reading = 0;

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{.field = "reading",
                                .minimum = Rational{Numerator{-9007199254740993}, Denominator{1}, DecimalPlaces{0}}},
    };
};

// Parsed and scoped to the "reading" property rather than substring-matched
// against the whole document: every int64_t field, this one included, shares
// `$defs/int64_t`, whose own type-range bound (INT64_MIN/INT64_MAX) is always
// far past kExactDoubleLimit and therefore always carries its own
// `x-exactMinimum`/`x-exactMaximum` there (see the $defs test below). A
// whole-document `contains("x-exactMinimum")` would read that unrelated
// companion as if it were this field's own declared minimum's, and could
// never observe an absence. `FieldMeta`'s declared bound is written directly
// onto the property node (test_forms_field_bounds.cpp's
// HugePositiveMinimumGetsAnExactTextCompanion confirms the same node), so
// that is the node to check.
namespace {
[[nodiscard]] const glz::generic& readingProperty(const glz::generic& root) {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- glaze DOM requires operator[]
    return root["properties"]["reading"];
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}
}  // namespace

TEST_CASE("schemaJson omits the exact companion for a minimum exactly at the positive limit", "[forms][bounds]") {
    auto const schema = morph::forms::schemaJson<EBAtPositiveLimitAction>();
    auto parsed = glz::read_json<glz::generic>(schema);
    REQUIRE(parsed.has_value());
    CHECK_FALSE(readingProperty(parsed.value()).contains("x-exactMinimum"));
}

TEST_CASE("schemaJson emits the exact companion for a minimum one past the positive limit", "[forms][bounds]") {
    auto const schema = morph::forms::schemaJson<EBOnePastPositiveLimitAction>();
    auto parsed = glz::read_json<glz::generic>(schema);
    REQUIRE(parsed.has_value());
    auto const& reading = readingProperty(parsed.value());
    REQUIRE(reading.contains("x-exactMinimum"));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(reading["x-exactMinimum"].get<std::string>() == "9007199254740993");
}

TEST_CASE("schemaJson omits the exact companion for a minimum exactly at the negative limit", "[forms][bounds]") {
    auto const schema = morph::forms::schemaJson<EBAtNegativeLimitAction>();
    auto parsed = glz::read_json<glz::generic>(schema);
    REQUIRE(parsed.has_value());
    CHECK_FALSE(readingProperty(parsed.value()).contains("x-exactMinimum"));
}

TEST_CASE("schemaJson emits the exact companion for a minimum one past the negative limit", "[forms][bounds]") {
    auto const schema = morph::forms::schemaJson<EBOnePastNegativeLimitAction>();
    auto parsed = glz::read_json<glz::generic>(schema);
    REQUIRE(parsed.has_value());
    auto const& reading = readingProperty(parsed.value());
    REQUIRE(reading.contains("x-exactMinimum"));
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    CHECK(reading["x-exactMinimum"].get<std::string>() == "-9007199254740993");
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
