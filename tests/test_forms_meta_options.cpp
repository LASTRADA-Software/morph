// SPDX-License-Identifier: Apache-2.0
//
// Options from field metadata: a plain member whose `FieldMeta` names an options action is emitted with the
// annotations a `Choice` carries and reads as a `Choice` field; a `Choice` whose meta also names one, and a meta that
// names only part of the three, are refused when the schema is generated.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/forms.hpp>
#include <optional>
#include <string>

// File-scope (not anonymous-namespaced): glaze's reflection needs a type with linkage.
// NOLINTBEGIN(misc-use-internal-linkage)

struct FMOAssignAction {
    std::optional<std::int64_t> ownerId;
    std::string note;

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{
            .field = "ownerId", .optionsAction = "ListOwners", .valueField = "ownerId", .labelField = "fullName"},
    };
};

struct FMOConflictingAction {
    morph::forms::Choice<std::int64_t, "ListOwners"> ownerId;

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{
            .field = "ownerId", .optionsAction = "ListOthers", .valueField = "id", .labelField = "name"},
    };
};

struct FMOIncompleteAction {
    std::optional<std::int64_t> ownerId;

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{.field = "ownerId", .optionsAction = "ListOwners", .valueField = "id"},
    };
};

struct FMOStrayAction {
    std::optional<std::int64_t> ownerId;

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{.field = "ownerId", .labelField = "name"},
    };
};

// NOLINTEND(misc-use-internal-linkage)

// Mutation: skip the emission in annotateMetaOptions (the member carries no options and reads as a number).
TEST_CASE("Forms::FieldMeta::OptionsMakeAPlainMemberAChoice", "[forms][field_meta][choice]") {
    std::string const schema = morph::forms::schemaJson<FMOAssignAction>();
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));
    auto const& owner = dom["properties"]["ownerId"];
    REQUIRE(owner.contains("x-optionsAction"));
    CHECK(owner["x-optionsAction"].get<std::string>() == "ListOwners");
    CHECK(owner["x-optionValue"].get<std::string>() == "ownerId");
    CHECK(owner["x-optionLabel"].get<std::string>() == "fullName");
    CHECK_FALSE(owner.contains("x-optionsDependsOn"));
    CHECK_FALSE(dom["properties"]["note"].contains("x-optionsAction"));

    auto const model = morph::forms::FormModel::fromSchema("FMOAssignAction", schema);
    REQUIRE(model.has_value());
    morph::forms::FieldSpec const* found = nullptr;
    for (morph::forms::FieldSpec const& field : model->fields()) {
        if (field.name == "ownerId") {
            found = &field;
        }
    }
    REQUIRE(found != nullptr);
    CHECK(found->kind == morph::forms::FieldKind::Choice);
    REQUIRE(found->choice.has_value());
    CHECK(found->choice->optionsAction == "ListOwners");
    CHECK(found->choice->valueField == "ownerId");
    CHECK(found->choice->labelField == "fullName");
}

TEST_CASE("Forms::FieldMeta::OptionsThatCannotHoldAreRefused", "[forms][field_meta][choice]") {
    CHECK_THROWS_AS(morph::forms::schemaJson<FMOConflictingAction>(), morph::forms::ConflictingOptionsError);
    try {
        static_cast<void>(morph::forms::schemaJson<FMOIncompleteAction>());
        FAIL("an options action without its label field is refused");
    } catch (morph::forms::IncompleteOptionsError const& error) {
        CHECK(std::string{error.what()}.contains("labelField"));
        CHECK_FALSE(std::string{error.what()}.contains("valueField,"));
    }
    try {
        static_cast<void>(morph::forms::schemaJson<FMOStrayAction>());
        FAIL("a label field without an options action is refused");
    } catch (morph::forms::IncompleteOptionsError const& error) {
        CHECK(std::string{error.what()}.contains("optionsAction, valueField"));
    }
}
