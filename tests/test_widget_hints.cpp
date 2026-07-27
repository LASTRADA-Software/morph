// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <morph/forms/widget_hints.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace {

using Level = morph::forms::Ranged<0, 100, 5>;
using Fraction = morph::forms::Ranged<0.5, 2.5, 0.5>;

static_assert(Level::min() == 0);
static_assert(Level::max() == 100);
static_assert(Level::step() == 5);
static_assert(Level::widget() == "slider");
static_assert(Fraction::min() == 0.5);
static_assert(Fraction::max() == 2.5);
static_assert(Fraction::step() == 0.5);
static_assert(morph::forms::Multiline::widget() == "textarea");

static_assert(morph::forms::EmptyCapableField<Level>);
static_assert(morph::forms::EmptyCapableField<Fraction>);
static_assert(!morph::forms::EmptyCapableField<morph::forms::Multiline>);

}  // namespace

// Declared at plain file scope (external linkage), not inside the anonymous
// namespace above and not function-local: glaze's reflection needs a type
// with linkage to name its members, and both a function-local class and a
// type in an unnamed namespace fail that requirement (the latter has had
// internal linkage since the unnamed-namespace linkage DR — see
// tests/test_quantity_forms.cpp's QF-prefixed action structs for the same
// file-scope convention).
struct WHNote {
    morph::forms::Multiline notes;
};

struct WHReading {
    Level intensity;
};

struct WHValidatedReading {
    Level intensity;

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

TEST_CASE("Multiline::WireAndEquality", "[forms][widget-hints]") {
    WHNote blank{};
    CHECK(blank.notes.value.empty());

    WHNote engaged{.notes = morph::forms::Multiline{"hello\nworld"}};
    CHECK(engaged.notes == morph::forms::Multiline{"hello\nworld"});
    CHECK_FALSE(engaged.notes == blank.notes);

    auto const json = glz::write_json(engaged);
    REQUIRE(json.has_value());
    CHECK(*json == R"({"notes":"hello\nworld"})");

    WHNote restored{};
    REQUIRE_FALSE(glz::read_json(restored, *json));
    CHECK(restored.notes == engaged.notes);
}

TEST_CASE("Ranged::WireAndEmptyState", "[forms][widget-hints]") {
    WHReading blank{};
    CHECK_FALSE(blank.intensity.hasValue());

    WHReading engaged{.intensity = Level{42}};
    CHECK(engaged.intensity.hasValue());
    CHECK(*engaged.intensity == 42);

    auto const json = glz::write_json(engaged);
    REQUIRE(json.has_value());
    CHECK((*json).contains(R"("intensity":42)"));

    WHReading restored{};
    REQUIRE_FALSE(glz::read_json(restored, *json));
    CHECK(restored.intensity == engaged.intensity);

    // Explicit null clears the field again (same pattern as Quantity/Choice
    // in tests/test_quantity_forms.cpp).
    REQUIRE_FALSE(glz::read_json(restored, R"({"intensity":null})"));
    CHECK_FALSE(restored.intensity.hasValue());
}

TEST_CASE("Ranged::RequiredByDefault", "[forms][widget-hints]") {
    WHValidatedReading draft;
    CHECK_FALSE(morph::forms::allRequiredEngaged(draft));
    draft.intensity = Level{7};
    CHECK(morph::forms::allRequiredEngaged(draft));
}

// ---------------------------------------------------------------------------
// Schema generation: x-widget / x-min / x-max / x-step.
//
// All action structs below are declared at plain file scope (external
// linkage), not inside an anonymous namespace: see the WHNote/WHReading
// comment above for why glaze's reflection requires that.
// ---------------------------------------------------------------------------

struct WHNotesAction {
    std::int64_t id = 0;
    morph::forms::Multiline notes;
    morph::forms::Ranged<0, 100, 5> intensity;

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

struct WHFractionAction {
    morph::forms::Ranged<0.5, 2.5, 0.5> fraction;

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

struct WHPlainAction {
    std::int64_t count = 0;
    std::string label;
    bool flag = false;

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

// Duck-typed field-metadata shape: any type exposing `.field` / `.widget`
// (string-view-convertible) is honoured as a widget override, independent of
// gui_field_metadata.md's own (possibly not-yet-implemented) `FieldMeta` type.
struct WHFieldMeta {
    std::string_view field;
    std::string_view widget{};
};

struct WHOverrideAction {
    morph::forms::Ranged<0, 100, 5> level{};
    std::string status;

    static constexpr std::array fieldMetadata{
        WHFieldMeta{.field = "level", .widget = "combo"},   // overrides the derived "slider"
        WHFieldMeta{.field = "status", .widget = "radio"},  // plain type, override-only
    };

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

// Interop check (not from the original plan draft, added per task instructions):
// the widget-override lookup is duck-typed, but it must also work with the
// *real* morph::forms::FieldMeta type landed by the field-metadata feature
// (which already carries a `.widget` member, previously unread by forms.hpp).
struct WHRealFieldMetaAction {
    morph::forms::Ranged<0, 10, 1> volume{};
    std::string mode;

    static constexpr std::array<morph::forms::FieldMeta, 2> fieldMetadata{
        morph::forms::FieldMeta{.field = "volume", .widget = "combo"},
        morph::forms::FieldMeta{.field = "mode", .widget = "radio"},
    };

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

TEST_CASE("Forms::SchemaJson::WidgetHintsSurface", "[forms][widget-hints]") {
    auto const schema = morph::forms::schemaJson<WHNotesAction>();

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    CHECK(schema.contains(R"("required":["id","notes","intensity"])"));
    CHECK(schema.contains(R"("x-widget":"textarea")"));
    CHECK(schema.contains(R"("x-widget":"slider")"));
    CHECK(schema.contains(R"("x-min":0)"));
    CHECK(schema.contains(R"("x-max":100)"));
    CHECK(schema.contains(R"("x-step":5)"));
}

TEST_CASE("Forms::SchemaJson::RangedFloatingBounds", "[forms][widget-hints]") {
    auto const schema = morph::forms::schemaJson<WHFractionAction>();
    CHECK(schema.contains(R"("x-widget":"slider")"));
    CHECK(schema.contains(R"("x-min":0.5)"));
    CHECK(schema.contains(R"("x-max":2.5)"));
    CHECK(schema.contains(R"("x-step":0.5)"));
}

TEST_CASE("Forms::SchemaJson::PlainFieldsEmitNoWidgetHint", "[forms][widget-hints]") {
    // Regression guard: a plain field with no wrapper and no override emits
    // no x-widget/x-min/x-max/x-step at all — today's schema is unchanged.
    auto const schema = morph::forms::schemaJson<WHPlainAction>();
    CHECK_FALSE(schema.contains("x-widget"));
    CHECK_FALSE(schema.contains("x-min"));
    CHECK_FALSE(schema.contains("x-max"));
    CHECK_FALSE(schema.contains("x-step"));
}

TEST_CASE("Forms::SchemaJson::FieldMetaOverrideWinsOverWrapper", "[forms][widget-hints]") {
    auto const schema = morph::forms::schemaJson<WHOverrideAction>();

    // The override replaces the Ranged-derived "slider"...
    CHECK(schema.contains(R"("x-widget":"combo")"));
    // ...but the slider's own range subfields still come from the type.
    CHECK(schema.contains(R"("x-min":0)"));
    CHECK(schema.contains(R"("x-max":100)"));
    CHECK(schema.contains(R"("x-step":5)"));
    // A plain `std::string` field gets x-widget purely from the override.
    CHECK(schema.contains(R"("x-widget":"radio")"));
}

TEST_CASE("Forms::AllRequiredEngaged::RangedWithOverrideGatesReadiness", "[forms][widget-hints]") {
    WHOverrideAction draft;
    CHECK_FALSE(morph::forms::allRequiredEngaged(draft));  // level not engaged
    draft.level = morph::forms::Ranged<0, 100, 5>{50};
    CHECK(morph::forms::allRequiredEngaged(draft));  // status is a plain string, always engaged

    CHECK_FALSE(morph::model::ActionValidator<WHOverrideAction>::ready(WHOverrideAction{}));
    CHECK(morph::model::ActionValidator<WHOverrideAction>::ready(draft));
}

TEST_CASE("Forms::SchemaJson::WidgetOverrideInteropsWithRealFieldMeta", "[forms][widget-hints]") {
    // Same duck-typed lookup as WHOverrideAction above, but with the actual
    // morph::forms::FieldMeta type (forms.hpp) rather than a hand-rolled
    // lookalike — proving the structural concept really is satisfied by the
    // concrete type shipped by the field-metadata feature, not merely by a
    // shape built to match it.
    auto const schema = morph::forms::schemaJson<WHRealFieldMetaAction>();

    CHECK(schema.contains(R"("x-widget":"combo")"));
    CHECK(schema.contains(R"("x-min":0)"));
    CHECK(schema.contains(R"("x-max":10)"));
    CHECK(schema.contains(R"("x-step":1)"));
    CHECK(schema.contains(R"("x-widget":"radio")"));
}
