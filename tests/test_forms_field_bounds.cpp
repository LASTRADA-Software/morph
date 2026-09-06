// SPDX-License-Identifier: Apache-2.0
//
// Per-field scalar bounds: `FieldMeta::minimum` / `::maximum` / `::multipleOf`
// (morph#310).
//
// The rule vocabulary compares a field to *another field*; only `equals`
// accepts a literal, and it expresses equality alone. So "this quantity is at
// least 1" and "this quantity is a whole number" had no spelling that could
// reach the served schema, and a DTO enforcing either in `validate()` was a
// second source of truth the client could not see -- contradicting
// examples/IMPLEMENTATION.md rule 3.
//
// `UnitTraits::bounds` is not the answer: it is keyed by *unit*, so a floor
// declared for one field constrains every field sharing that unit. These tests
// pin that the bound is per **field** -- the sibling of the same `Quantity`
// type is left unconstrained -- and that one declaration drives both the
// emitted schema and the C++ predicate a `validate()` calls.
//
// src/qt/forms/tests/tst_DynamicFormFieldBounds.qml pins the renderer half
// against the same key names.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <limits>
#include <morph/forms/forms.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <string>

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

// ---------------------------------------------------------------------------
// A miniature unit system for the field-bounds tests. FB prefix keeps these
// unique for the file-scope-collision CI check.
// ---------------------------------------------------------------------------
//
// File-scope (not anonymous-namespaced): glaze's reflection needs a type with
// linkage, which is precisely what an anonymous namespace takes away. Same
// suppression, for the same reason, as tests/test_forms_exact_bounds.cpp and
// tests/test_shared_instances.cpp.
// NOLINTBEGIN(misc-use-internal-linkage)

enum class FBUnit : std::uint8_t { count };

template <>
struct morph::units::UnitTraits<FBUnit> {
    static constexpr morph::units::UnitMeta meta(FBUnit /*unit*/) noexcept {
        return {.id = "count", .display = "", .defaultDecimals = 1};
    }
};

using FBReads = morph::units::Quantity<FBUnit::count, 1>;

/// The motivating shape: a budget that must be a whole number of at least one,
/// beside a tally of the *same* `Quantity` type that legitimately starts at 0.
struct FBBudgetAction {
    FBReads budget;
    FBReads tally;

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{
            .field = "budget", .minimum = Rational{1, DecimalPlaces{1}}, .multipleOf = Rational{1, DecimalPlaces{1}}},
    };

    [[nodiscard]] bool validate() const noexcept { return morph::forms::allFieldBoundsSatisfied(*this); }
};

/// A closed range over a plain integral member, and a fractional `multipleOf`.
struct FBRangeAction {
    std::int64_t retries = 0;
    FBReads step;

    static constexpr std::array<morph::forms::FieldMeta, 2> fieldMetadata{
        morph::forms::FieldMeta{
            .field = "retries", .minimum = Rational{0, DecimalPlaces{0}}, .maximum = Rational{5, DecimalPlaces{0}}},
        morph::forms::FieldMeta{.field = "step",
                                .multipleOf = Rational{Numerator{1}, Denominator{2}, DecimalPlaces{1}}},
    };
};

/// Declares nothing: its schema must be byte-for-byte what it was before this
/// feature existed.
struct FBUnannotatedAction {
    FBReads budget;
};
// NOLINTEND(misc-use-internal-linkage)

// ---------------------------------------------------------------------------
// Schema emission
// ---------------------------------------------------------------------------

TEST_CASE("Forms::FieldBounds::SchemaCarriesMinimumAndMultipleOf", "[forms][bounds]") {
    auto const schema = morph::forms::schemaJson<FBBudgetAction>();
    auto parsed = glz::read_json<glz::generic>(schema);
    REQUIRE(parsed.has_value());
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- glaze DOM requires operator[]
    auto const& properties = parsed.value()["properties"];
    REQUIRE(properties.contains("budget"));
    auto const& budget = properties["budget"];
    REQUIRE(budget.contains("minimum"));
    CHECK(budget["minimum"].get<double>() == 1.0);
    REQUIRE(budget.contains("multipleOf"));
    CHECK(budget["multipleOf"].get<double>() == 1.0);
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

TEST_CASE("Forms::FieldBounds::BoundIsPerFieldNotPerType", "[forms][bounds]") {
    // The whole reason this is not `UnitTraits::bounds`: `tally` shares
    // `budget`'s unit *and* its exact `Quantity` type, and must stay free to
    // hold 0.
    auto const schema = morph::forms::schemaJson<FBBudgetAction>();
    auto parsed = glz::read_json<glz::generic>(schema);
    REQUIRE(parsed.has_value());
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- glaze DOM requires operator[]
    auto const& tally = parsed.value()["properties"]["tally"];
    CHECK_FALSE(tally.contains("minimum"));
    CHECK_FALSE(tally.contains("multipleOf"));
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

TEST_CASE("Forms::FieldBounds::UndeclaredActionGainsNoKeys", "[forms][bounds]") {
    auto const schema = morph::forms::schemaJson<FBUnannotatedAction>();
    // `multipleOf` has no other source at all, so its absence is checked over
    // the whole document. `minimum`/`maximum` do -- glaze stamps them in
    // `$defs` for the int64 numerator a `Quantity` is built from -- so those
    // are checked on the property node, which is the only place this feature
    // writes to.
    CHECK_FALSE(schema.contains("multipleOf"));
    auto parsed = glz::read_json<glz::generic>(schema);
    REQUIRE(parsed.has_value());
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- glaze DOM requires operator[]
    auto const& budget = parsed.value()["properties"]["budget"];
    CHECK_FALSE(budget.contains("minimum"));
    CHECK_FALSE(budget.contains("maximum"));
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

TEST_CASE("Forms::FieldBounds::SchemaCarriesAClosedRange", "[forms][bounds]") {
    auto const schema = morph::forms::schemaJson<FBRangeAction>();
    auto parsed = glz::read_json<glz::generic>(schema);
    REQUIRE(parsed.has_value());
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- glaze DOM requires operator[]
    auto const& retries = parsed.value()["properties"]["retries"];
    REQUIRE(retries.contains("minimum"));
    CHECK(retries["minimum"].get<double>() == 0.0);
    REQUIRE(retries.contains("maximum"));
    CHECK(retries["maximum"].get<double>() == 5.0);
    // A non-integral step is served as its quotient -- the client gate is an
    // approximation; the exact check is allFieldBoundsSatisfied's.
    auto const& step = parsed.value()["properties"]["step"];
    REQUIRE(step.contains("multipleOf"));
    CHECK(step["multipleOf"].get<double>() == 0.5);
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

TEST_CASE("Forms::FieldBounds::PerFieldBoundShadowsNoSharedDefinition", "[forms][bounds]") {
    // `budget` and `tally` are the same type, so glaze shares one `$defs`
    // entry between them. A bound written there would leak onto every field of
    // that type; it belongs on the property node, beside the `$ref`.
    auto const schema = morph::forms::schemaJson<FBBudgetAction>();
    auto parsed = glz::read_json<glz::generic>(schema);
    REQUIRE(parsed.has_value());
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- glaze DOM requires operator[]
    auto const& root = parsed.value();
    if (root.contains("$defs")) {
        for (auto const& [name, definition] : root["$defs"].get_object()) {
            INFO("$defs entry: " << name);
            CHECK_FALSE(definition.contains("multipleOf"));
        }
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

// ---------------------------------------------------------------------------
// The C++ predicate -- the same declaration, evaluated
// ---------------------------------------------------------------------------

TEST_CASE("Forms::FieldBounds::UnengagedFieldIsVacuouslySatisfied", "[forms][bounds]") {
    // A form still being filled in must not fail its own bound: whether the
    // field has to be engaged at all is `required`'s question, not this one.
    FBBudgetAction action{};
    CHECK(action.validate());
}

TEST_CASE("Forms::FieldBounds::ValueBelowMinimumIsRejected", "[forms][bounds]") {
    FBBudgetAction action{};
    action.budget = Rational{0, DecimalPlaces{1}};
    CHECK_FALSE(action.validate());
    action.budget = Rational{-1, DecimalPlaces{1}};
    CHECK_FALSE(action.validate());
}

TEST_CASE("Forms::FieldBounds::ValueAtMinimumIsAccepted", "[forms][bounds]") {
    FBBudgetAction action{};
    action.budget = Rational{1, DecimalPlaces{1}};
    CHECK(action.validate());
}

TEST_CASE("Forms::FieldBounds::FractionalValueFailsMultipleOfOne", "[forms][bounds]") {
    FBBudgetAction action{};
    action.budget = Rational{Numerator{5}, Denominator{2}, DecimalPlaces{1}};
    CHECK_FALSE(action.validate());
    action.budget = Rational{Numerator{6}, Denominator{2}, DecimalPlaces{1}};
    CHECK(action.validate());
}

TEST_CASE("Forms::FieldBounds::SiblingOfTheSameTypeIsUnconstrained", "[forms][bounds]") {
    FBBudgetAction action{};
    action.budget = Rational{1, DecimalPlaces{1}};
    action.tally = Rational{0, DecimalPlaces{1}};
    CHECK(action.validate());
    action.tally = Rational{Numerator{1}, Denominator{2}, DecimalPlaces{1}};
    CHECK(action.validate());
}

TEST_CASE("Forms::FieldBounds::IntegralMemberHonoursItsRange", "[forms][bounds]") {
    FBRangeAction action{};
    CHECK(morph::forms::allFieldBoundsSatisfied(action));
    action.retries = 5;
    CHECK(morph::forms::allFieldBoundsSatisfied(action));
    action.retries = 6;
    CHECK_FALSE(morph::forms::allFieldBoundsSatisfied(action));
    action.retries = -1;
    CHECK_FALSE(morph::forms::allFieldBoundsSatisfied(action));
}

TEST_CASE("Forms::FieldBounds::FractionalMultipleOfIsExact", "[forms][bounds]") {
    // 3/4 is not a multiple of 1/2; 3/2 is. Checked on the exact `Rational`,
    // never on a double.
    FBRangeAction action{};
    action.step = Rational{Numerator{3}, Denominator{4}, DecimalPlaces{1}};
    CHECK_FALSE(morph::forms::allFieldBoundsSatisfied(action));
    action.step = Rational{Numerator{3}, Denominator{2}, DecimalPlaces{1}};
    CHECK(morph::forms::allFieldBoundsSatisfied(action));
}

TEST_CASE("Forms::FieldBounds::ActionWithoutFieldMetadataIsTriviallySatisfied", "[forms][bounds]") {
    FBUnannotatedAction action{};
    CHECK(morph::forms::allFieldBoundsSatisfied(action));
    action.budget = Rational{-99, DecimalPlaces{1}};
    CHECK(morph::forms::allFieldBoundsSatisfied(action));
}

// ---------------------------------------------------------------------------
// detail::declaresAnyBound / detail::hasUsableMultipleOf, directly.
// ---------------------------------------------------------------------------

TEST_CASE("Forms::FieldBounds::DeclaresAnyBoundIsTrueForMaximumAlone", "[forms][bounds]") {
    // declaresAnyBound's `maximum.has_value()` disjunct is only evaluated
    // once `minimum` is disengaged; every FieldMeta declared above sets
    // minimum (alone, or alongside maximum/multipleOf), so that disjunct's
    // true arm was never observed.
    morph::forms::FieldMeta const meta{.field = "x", .maximum = Rational{5, DecimalPlaces{0}}};
    CHECK(morph::forms::detail::declaresAnyBound(meta));
}

TEST_CASE("Forms::FieldBounds::HasUsableMultipleOfRejectsZeroAndNegative", "[forms][bounds]") {
    // JSON Schema requires multipleOf to be strictly positive; the doc
    // comment says a zero or negative declaration is ignored, but neither
    // case was ever exercised.
    morph::forms::FieldMeta const zero{.field = "x", .multipleOf = Rational{0, DecimalPlaces{0}}};
    CHECK_FALSE(morph::forms::detail::hasUsableMultipleOf(zero));

    morph::forms::FieldMeta const negative{.field = "x", .multipleOf = Rational{-1, DecimalPlaces{0}}};
    CHECK_FALSE(morph::forms::detail::hasUsableMultipleOf(negative));
}

// ---------------------------------------------------------------------------
// allFieldBoundsSatisfied: a FieldMeta that declares no bound at all.
// ---------------------------------------------------------------------------

struct FBCosmeticOnlyAction {
    FBReads reading;

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{.field = "reading", .label = "Reading", .help = "No numeric bound declared"},
    };
};

TEST_CASE("Forms::FieldBounds::AFieldMetaWithNoDeclaredBoundIsSkipped", "[forms][bounds]") {
    // meta != nullptr but declaresAnyBound(*meta) is false was never
    // exercised: every FieldMeta declared above sets at least one of
    // minimum/maximum/multipleOf. A FieldMeta that only carries cosmetic
    // properties (label/help) must never gate the value, regardless of it.
    FBCosmeticOnlyAction action{};
    CHECK(morph::forms::allFieldBoundsSatisfied(action));
    action.reading = Rational{-999, DecimalPlaces{1}};
    CHECK(morph::forms::allFieldBoundsSatisfied(action));  // cosmetic-only metadata never gates
}

// ---------------------------------------------------------------------------
// satisfiesDeclaredBounds's checkedDiv failure mode (Overflow).
// ---------------------------------------------------------------------------

struct FBOverflowAction {
    Rational value{};

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{
            .field = "value",
            .multipleOf =
                Rational{Numerator{1}, Denominator{std::numeric_limits<std::int64_t>::max()}, DecimalPlaces{0}}},
    };

    [[nodiscard]] bool validate() const noexcept { return morph::forms::allFieldBoundsSatisfied(*this); }
};

TEST_CASE("Forms::FieldBounds::CheckedDivOverflowIsTreatedAsAViolation", "[forms][bounds]") {
    // satisfiesDeclaredBounds's checkedDiv never actually failed in any
    // exercised call before this: hasUsableMultipleOf's own guard already
    // excludes DivisionByZero at this call site (multipleOf can't be zero),
    // leaving Overflow as the one reachable failure mode. checkedDiv is
    // checkedMul(lhs, rhs.reciprocal()); reciprocal() of a multipleOf with a
    // near-INT64_MAX denominator has a near-INT64_MAX numerator, and
    // multiplying that by anything beyond 1 overflows the internal 64-bit
    // cross-multiply. Nail the exact pair directly first, per the audit's
    // own recipe.
    Rational const multipleOf{Numerator{1}, Denominator{std::numeric_limits<std::int64_t>::max()}, DecimalPlaces{0}};
    REQUIRE_FALSE(morph::math::checkedDiv(Rational{2, DecimalPlaces{0}}, multipleOf).has_value());

    FBOverflowAction const action{.value = Rational{2, DecimalPlaces{0}}};
    // An unrepresentable quotient is treated as a bound violation, not a
    // pass -- satisfiesDeclaredBounds refuses to vouch for a multiple it
    // cannot exactly represent.
    CHECK_FALSE(action.validate());
}

TEST_CASE("Forms::FieldBounds::BareRationalSatisfyingItsDeclaredBoundPasses", "[forms][bounds]") {
    // allFieldBoundsSatisfied's `std::same_as<Member, Rational>` arm
    // (satisfiesDeclaredBounds's result *not* gating `satisfied`) was only
    // ever exercised via the overflow-as-violation case above -- every
    // FBOverflowAction this suite constructed had a nonzero `value`, which
    // multiplied by that field's near-INT64_MAX-reciprocal multipleOf always
    // overflows checkedDiv. Zero is a multiple of everything and 0 *
    // INT64_MAX is exactly representable, so checkedDiv succeeds with an
    // integral quotient and the bound is genuinely satisfied here, not
    // merely un-violated by construction.
    FBOverflowAction const action{.value = Rational{0, DecimalPlaces{0}}};
    CHECK(action.validate());
}

// ---------------------------------------------------------------------------
// annotateExactBound's std::cmp_greater(...) arm for a user-declared bound.
//
// Correction to this file's own audit (task-9-forms-findings.md finding #4):
// fresh `llvm-cov show --show-branches=count` measurement shows the POSITIVE
// arm, not the negative one, was the untested half at this call site.
// emitDeclaredBound always writes `property[key] = bound.numerator` -- a
// `std::int64_t`-typed assignment that glaze's generic_u64 always routes to
// its *signed* overload regardless of the runtime value's sign -- so a
// user-declared FieldMeta bound always lands on annotateExactBound's
// `int64_t` branch, unlike glaze's own $defs/int64_t type-range "maximum"
// (test_forms_exact_bounds.cpp), which happens to be written through the
// uint64_t overload instead. Every FieldMeta bound in this suite is a small
// positive integer well under kExactDoubleLimit, so cmp_greater's true arm
// was never observed -- only cmp_less's, via that other file's int64_t
// $defs minimum (INT64_MIN).
// ---------------------------------------------------------------------------

struct FBHugePositiveBoundAction {
    std::int64_t reading = 0;

    static constexpr std::array<morph::forms::FieldMeta, 1> fieldMetadata{
        morph::forms::FieldMeta{.field = "reading",
                                .minimum = Rational{Numerator{std::numeric_limits<std::int64_t>::max()},
                                                    Denominator{1}, DecimalPlaces{0}}},
    };
};

TEST_CASE("Forms::FieldBounds::HugePositiveMinimumGetsAnExactTextCompanion", "[forms][bounds]") {
    auto const schema = morph::forms::schemaJson<FBHugePositiveBoundAction>();
    CHECK(schema.contains(R"("x-exactMinimum":"9223372036854775807")"));
}
