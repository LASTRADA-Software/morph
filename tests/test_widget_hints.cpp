// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
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
