// SPDX-License-Identifier: Apache-2.0
//
// The conformance kit's C++ half: fixture action types exercising every key
// of forms.md's CURRENT, implemented renderer contract, generated via the
// real morph::forms::schemaJson<A>() (never hand-authored) so a change to an
// emitter that alters the contract shows up here as a failing assertion --
// the corpus "drift guard" (docs/spec/forms/forms.md, "Renderer conformance
// kit").
//
// Wizard / app (w-*/app-*) fixtures are intentionally absent: although the
// emitters now exist (morph::flows::wizardSchemaJson/morph::app::appSchemaJson,
// docs/spec/forms/workflows_navigation.md), no conformance-kit fixture
// exercises them yet -- deferred to future work, exactly as this corpus
// already treats views (below) separately. The view-schema layer (v-*,
// morph::views::viewSchemaJson, docs/spec/forms/views.md) IS implemented;
// its own coverage lives in tests/test_views.cpp and
// tests/test_forms_engine_collection*.cpp rather than this corpus, since a
// view composes existing action schemas rather than adding new per-field
// schema keys of the kind this corpus's CF* fixtures pin.
//
// Types here are prefixed CF (Conformance Fixture) and kept at file scope
// (not inside an anonymous namespace) -- every test .cpp in this directory
// links into one morph_tests binary, and file-scope, non-anonymous-
// namespaced types risk ODR collisions across translation units
// (test_quantity_forms.cpp reserves the QF prefix for the same reason).

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/forms/choice.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>

#include "forms_engine_support.hpp"

// ---------------------------------------------------------------------------
// A miniature, dedicated unit system for the corpus -- independent of
// examples/forms/lab_units.hpp so the corpus does not depend on an example.
// ---------------------------------------------------------------------------

enum class CFUnit : std::uint8_t { kg, g, t };

template <>
struct morph::units::UnitTraits<CFUnit> {
    static constexpr morph::units::UnitMeta meta(CFUnit unit) noexcept {
        switch (unit) {
            case CFUnit::kg:
                return {.id = "kg", .display = "kg", .defaultDecimals = 3};
            case CFUnit::g:
                return {.id = "g", .display = "g", .defaultDecimals = 1};
            case CFUnit::t:
                return {.id = "t", .display = "t", .defaultDecimals = 4};
            default:
                return {.id = "?", .display = "?", .defaultDecimals = 3};
        }
    }

    static constexpr std::array<morph::units::UnitRelation<CFUnit>, 2> relations{{
        {CFUnit::g, CFUnit::kg,
         morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000},
                               morph::math::DecimalPlaces{3}}},
        {CFUnit::t, CFUnit::kg,
         morph::math::Rational{morph::math::Numerator{1000}, morph::math::Denominator{1},
                               morph::math::DecimalPlaces{3}}},
    }};
};

using CFMass = morph::units::Quantity<CFUnit::kg>;

// ---------------------------------------------------------------------------
// Fixture 1: plain scalars + required (x-order, the required-array
// derivation, std::optional opt-out).
// ---------------------------------------------------------------------------

struct CFScalarsAndRequired {
    std::int64_t count = 0;
    std::string label;
    std::optional<std::string> note{};

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

// ---------------------------------------------------------------------------
// Fixture 2: Quantity with convertible alternatives (ExtUnits,
// x-decimalPlaces, x-unitAlternatives).
// ---------------------------------------------------------------------------

struct CFQuantityAlternatives {
    CFMass mass{};

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

// ---------------------------------------------------------------------------
// Fixture 3: Choice (x-optionsAction/x-optionValue/x-optionLabel).
// ---------------------------------------------------------------------------

struct CFListWidgets {};  // the options-providing action; schemaJson needs no registration

struct CFChoiceField {
    morph::forms::Choice<std::int64_t, "CFListWidgets"> widgetId;

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

// ---------------------------------------------------------------------------
// Fixture 4: Timestamp ("format": "date-time").
// ---------------------------------------------------------------------------

struct CFTimestampField {
    morph::time::Timestamp when;

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

// ---------------------------------------------------------------------------
// Fixture 5: two members of the SAME Quantity type -- the mandatory $ref
// dual-read (property x-order differs per field; the $def -- and therefore
// ExtUnits -- is shared between them).
// ---------------------------------------------------------------------------

struct CFSharedDefFields {
    CFMass massA{};
    CFMass massB{};

    [[nodiscard]] bool validate() const { return morph::forms::allRequiredEngaged(*this); }
};

namespace {

// Counts non-overlapping occurrences of @p needle in @p haystack.
std::size_t countOccurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

}  // namespace

TEST_CASE("Conformance corpus: CFScalarsAndRequired -- x-order and required", "[conformance]") {
    auto const schema = morph::forms::schemaJson<CFScalarsAndRequired>();
    REQUIRE_FALSE(schema.empty());

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    CHECK(schema.contains(R"("x-order":0)"));
    CHECK(schema.contains(R"("x-order":1)"));
    CHECK(schema.contains(R"("x-order":2)"));

    // count and label are required; note (std::optional) is excluded.
    CHECK(schema.contains(R"("required":["count","label"])"));
}

TEST_CASE("Conformance corpus: CFQuantityAlternatives -- ExtUnits, x-decimalPlaces, x-unitAlternatives",
          "[conformance]") {
    auto const schema = morph::forms::schemaJson<CFQuantityAlternatives>();
    REQUIRE_FALSE(schema.empty());

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    CHECK(schema.contains(R"("unitAscii":"kg")"));
    CHECK(schema.contains(R"("unitUnicode":"kg")"));
    CHECK(schema.contains(R"("x-decimalPlaces":3)"));

    // Two convertible alternatives (g, t), each with its OWN default decimals
    // (not the relation's internal precision) and the exact alternative ->
    // canonical ratio.
    CHECK(schema.contains(R"("id":"g")"));
    CHECK(schema.contains(R"("decimals":1)"));
    CHECK(schema.contains(R"("num":1,"den":1000)"));
    CHECK(schema.contains(R"("id":"t")"));
    CHECK(schema.contains(R"("decimals":4)"));
    CHECK(schema.contains(R"("num":1000,"den":1)"));

    CHECK(schema.contains(R"("required":["mass"])"));
}

TEST_CASE("Conformance corpus: CFChoiceField -- x-optionsAction/x-optionValue/x-optionLabel", "[conformance]") {
    auto const schema = morph::forms::schemaJson<CFChoiceField>();
    REQUIRE_FALSE(schema.empty());

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    CHECK(schema.contains(R"("x-optionsAction":"CFListWidgets")"));
    CHECK(schema.contains(R"("x-optionValue":"id")"));
    CHECK(schema.contains(R"("x-optionLabel":"name")"));
    CHECK(schema.contains(R"("required":["widgetId"])"));
}

TEST_CASE("Conformance corpus: CFTimestampField -- format date-time", "[conformance]") {
    auto const schema = morph::forms::schemaJson<CFTimestampField>();
    REQUIRE_FALSE(schema.empty());

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    CHECK(schema.contains(R"("format":"date-time")"));
    CHECK(schema.contains(R"("required":["when"])"));
}

TEST_CASE("Conformance corpus: CFSharedDefFields -- mandatory $ref dual-read, one shared $def", "[conformance]") {
    auto const schema = morph::forms::schemaJson<CFSharedDefFields>();
    REQUIRE_FALSE(schema.empty());

    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));

    // Each property keeps its own x-order...
    CHECK(schema.contains(R"("x-order":0)"));
    CHECK(schema.contains(R"("x-order":1)"));
    CHECK(schema.contains(R"("required":["massA","massB"])"));

    // ...but massA and massB are the SAME Quantity<CFUnit::kg> type, so
    // glaze emits exactly ONE $def (and therefore one ExtUnits block) that
    // both properties' $ref points at -- forms.md: "many properties of the
    // same unit type share one $def and therefore one ExtUnits."
    CHECK(countOccurrences(schema, R"("unitAscii":"kg")") == 1);
}

// The same fixtures, through the forms engine: the renderer contract read from
// the emitted schemas themselves.

TEST_CASE("Conformance corpus through the engine: x-order and the required gate", "[conformance][forms-engine]") {
    morph::test::formsengine::Harness harness;
    auto const form = harness.session("CFScalarsAndRequired", morph::forms::schemaJson<CFScalarsAndRequired>());
    REQUIRE(form->fields().size() == 3);
    CHECK(form->fields()[0]->spec().name == "count");
    CHECK(form->fields()[1]->spec().name == "label");
    CHECK(form->fields()[2]->spec().name == "note");
    CHECK_FALSE(form->ready());
    harness.type(*form, "count", "3");
    CHECK_FALSE(form->ready());
    harness.type(*form, "label", "widget-7");
    CHECK(form->body() == R"({"count":3,"label":"widget-7"})");
}

TEST_CASE("Conformance corpus through the engine: exact Quantity payloads and unit switching",
          "[conformance][forms-engine]") {
    morph::test::formsengine::Harness harness;
    auto const form = harness.session("CFQuantityAlternatives", morph::forms::schemaJson<CFQuantityAlternatives>());
    harness.type(*form, "mass", "2650.5");
    CHECK(form->body() == R"({"mass":{"num":2650500,"den":1000,"dp":3}})");
    form->field("mass").switchUnit(1);
    harness.settle();
    CHECK(form->field("mass").text().peek() == "2650500.0");
    CHECK(form->body() == R"({"mass":{"num":26505000,"den":10000,"dp":3}})");
    harness.type(*form, "mass", "2650500.01");
    CHECK_FALSE(form->ready());  // more places than grams take
}

TEST_CASE("Conformance corpus through the engine: Choice, Timestamp and the shared $def",
          "[conformance][forms-engine]") {
    morph::test::formsengine::Harness harness;
    auto const choice = harness.session("CFChoiceField", morph::forms::schemaJson<CFChoiceField>());
    auto const& spec = choice->field("widgetId").spec();
    REQUIRE(spec.choice.has_value());
    CHECK(spec.choice->optionsAction == "CFListWidgets");
    CHECK(spec.choice->valueField == "id");
    CHECK(spec.choice->labelField == "name");
    CHECK(harness.server.fetches().size() == 1);
    CHECK(harness.server.fetches()[0].body == "{}");

    auto const stamp = harness.session("CFTimestampField", morph::forms::schemaJson<CFTimestampField>());
    CHECK(stamp->field("when").spec().kind == morph::forms::FieldKind::DateTime);
    CHECK_FALSE(stamp->ready());
    harness.type(*stamp, "when", "2026-07-20T09:00");
    CHECK(stamp->body() == R"({"when":"2026-07-20T09:00:00Z"})");

    auto const shared = harness.session("CFSharedDefFields", morph::forms::schemaJson<CFSharedDefFields>());
    CHECK(shared->field("massA").spec().unit == "kg");
    CHECK(shared->field("massB").spec().unit == "kg");
    CHECK(shared->field("massA").spec().decimalPlaces == 3U);
    CHECK(shared->field("massB").spec().decimalPlaces == 3U);
}
