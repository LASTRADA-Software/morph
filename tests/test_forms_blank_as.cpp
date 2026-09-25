// SPDX-License-Identifier: Apache-2.0
//
// `FieldMeta::blankAs` -- what a renderer submits for a string field the user
// left blank. `BlankAs::Empty` emits `"x-blankAs": "empty"` so an edit form
// can clear a stored `std::optional<std::string>` (submit `""`) instead of
// omitting the member, which the model reads as "leave it unchanged".
//
// src/qt/forms/tests/tst_DynamicFormBlankAs.qml pins the renderer half
// against the same key.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/forms/forms.hpp>
#include <optional>
#include <string>

// File-scope (not anonymous-namespaced): glaze's reflection needs a type with
// linkage. Same suppression, for the same reason, as
// tests/test_forms_display_unit.cpp.
// NOLINTBEGIN(misc-use-internal-linkage)

struct FBARemarkAction {
    std::optional<std::string> remark;
    std::string label;
    std::optional<std::string> untouched;
    double weight = 0.0;

    static constexpr std::array<morph::forms::FieldMeta, 3> fieldMetadata{
        morph::forms::FieldMeta{.field = "remark", .blankAs = morph::forms::BlankAs::Empty},
        morph::forms::FieldMeta{.field = "label"}.withBlankAs(morph::forms::BlankAs::Empty),
        // Not a string: there is no "" to submit, so nothing is emitted.
        morph::forms::FieldMeta{.field = "weight", .blankAs = morph::forms::BlankAs::Empty},
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

}  // namespace

TEST_CASE("Forms::FieldMeta::BlankAsEmptyOnAStringMemberEmitsTheKey", "[forms][field_meta][blank_as]") {
    auto const dom = schemaDom(morph::forms::schemaJson<FBARemarkAction>());
    auto const& remark = dom["properties"]["remark"];
    REQUIRE(remark.contains("x-blankAs"));
    CHECK(remark["x-blankAs"].get<std::string>() == "empty");
    auto const& label = dom["properties"]["label"];
    REQUIRE(label.contains("x-blankAs"));
    CHECK(label["x-blankAs"].get<std::string>() == "empty");
}

TEST_CASE("Forms::FieldMeta::BlankAsIsOmittedByDefaultAndOnANonString", "[forms][field_meta][blank_as]") {
    auto const dom = schemaDom(morph::forms::schemaJson<FBARemarkAction>());
    CHECK_FALSE(dom["properties"]["untouched"].contains("x-blankAs"));
    CHECK_FALSE(dom["properties"]["weight"].contains("x-blankAs"));
    constexpr morph::forms::FieldMeta kPlain{.field = "remark"};
    STATIC_CHECK(kPlain.blankAs == morph::forms::BlankAs::Omit);
}

TEST_CASE("Forms::FieldMeta::BlankAsLeavesTheWireUntouched", "[forms][field_meta][blank_as]") {
    // Presentation only: "" and nullopt still travel as they always have.
    FBARemarkAction const cleared{.remark = std::string{}, .label = "L", .untouched = std::nullopt};
    std::string json{};
    REQUIRE_FALSE(glz::write_json(cleared, json));
    CHECK(json.contains(R"("remark":"")"));
    FBARemarkAction decoded{};
    REQUIRE_FALSE(glz::read_json(decoded, R"({"remark":"","label":"L"})"));
    REQUIRE(decoded.remark.has_value());
    CHECK(decoded.remark->empty());
    CHECK_FALSE(decoded.untouched.has_value());
}
