// SPDX-License-Identifier: Apache-2.0
//
// `morph::forms::InstanceConstraints` — the per-instance seam (issue #164).
//
// The case under test is the issue's own: two *instances* of one compiled
// action type declare three and one decimal places, and a value outside the
// range one of them declares is submitted. Before this seam the compiled type
// answered for both — the served `x-decimalPlaces` said 3 for the instance
// that declares 1, and the out-of-range value passed `validate()` with nothing
// anywhere in the framework able to name the bound it broke.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <glaze/glaze.hpp>
#include <map>
#include <morph/forms/forms.hpp>
#include <morph/forms/instance_constraints.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <sstream>
#include <string>
#include <vector>

using morph::forms::ConstraintViolation;
using morph::forms::ConstraintViolationKind;
using morph::forms::FieldConstraint;
using morph::forms::InstanceConstraints;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

// ---------------------------------------------------------------------------
// A miniature catalogue unit system, deliberately shaped like rung 6's: the
// action's precision is a *template parameter* (3), and the instances below
// disagree with it.
// ---------------------------------------------------------------------------

enum class ICUnit : std::uint8_t { mg_per_L, scalar };

template <>
struct morph::units::UnitTraits<ICUnit> {
    static constexpr morph::units::UnitMeta meta(ICUnit unit) noexcept {
        switch (unit) {
            case ICUnit::mg_per_L:
                return {.id = "mg_per_L", .display = "mg/L", .defaultDecimals = 3};
            case ICUnit::scalar:
                return {.id = "scalar", .display = "", .defaultDecimals = 3};
            default:
                return {.id = "?", .display = "?", .defaultDecimals = 3};
        }
    }
};

using ICConcentration = morph::units::Quantity<ICUnit::mg_per_L, 3>;

/// @brief The rung-6-shaped action: one exact reading, at a compile-time
///        precision no instance can move.
///
/// Deliberately at namespace scope, not in an anonymous namespace: glaze's
/// reflection takes the address of an `extern const T`, which a type with no
/// linkage cannot have.
struct ICCapture {
    std::int64_t analysisVersionId{0};
    ICConcentration value{};

    /// @brief Field-level readiness, exactly as an action declares it.
    /// @return `true` when the reading is engaged.
    [[nodiscard]] bool validate() const noexcept { return morph::forms::allRequiredEngaged(*this); }
};

namespace {

/// @brief An exact rational at @p places decimal places.
/// @param num Numerator.
/// @param den Denominator.
/// @param places Decimal-precision tag.
/// @return The canonical rational.
[[nodiscard]] Rational exact(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return Rational{Numerator{num}, Denominator{den}, DecimalPlaces{places}};
}

/// @brief The constraints one catalogue version would declare.
/// @param places The version's decimal places.
/// @param highNumerator The version's inclusive upper spec bound, over 1.
/// @return The constraint set for the `value` field.
[[nodiscard]] InstanceConstraints version(std::uint32_t places, std::int64_t highNumerator) {
    InstanceConstraints constraints;
    constraints.declare(FieldConstraint{.field = "value",
                                        .decimalPlaces = places,
                                        .minimum = exact(0, 1, places),
                                        .maximum = exact(highNumerator, 1, places)});
    return constraints;
}

/// @brief Reads one integer key off the `value` property of a schema.
/// @param schema The schema text.
/// @param key The key to read.
/// @return Its value, or -1 when absent.
[[nodiscard]] std::int64_t valuePropertyInt(const std::string& schema, const std::string& key) {
    glz::generic_u64 dom{};
    if (glz::read_json(dom, schema)) {
        return -1;
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM
    const auto& value = dom["properties"]["value"];
    if (!value.contains(key)) {
        return -1;
    }
    return value[key].as<std::int64_t>();
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

/// @brief Reads a `{"num","den","dp"}` bound node's numerator off a schema.
/// @param schema The schema text.
/// @param key The bound key on the `value` property.
/// @return The numerator, or -1 when absent.
[[nodiscard]] std::int64_t boundNumerator(const std::string& schema, const std::string& key) {
    glz::generic_u64 dom{};
    if (glz::read_json(dom, schema)) {
        return -1;
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM
    const auto& value = dom["properties"]["value"];
    if (!value.contains(key)) {
        return -1;
    }
    return value[key]["num"].as<std::int64_t>();
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

/// @brief The document-level `x-instanceConstraints` list.
/// @param schema The schema text.
/// @return The field names named by the stamp; empty when it is absent.
[[nodiscard]] std::vector<std::string> stampedFields(const std::string& schema) {
    glz::generic_u64 dom{};
    std::vector<std::string> names{};
    if (glz::read_json(dom, schema) || !dom.contains("x-instanceConstraints")) {
        return names;
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM
    for (const auto& entry : dom["x-instanceConstraints"].get<glz::generic_u64::array_t>()) {
        names.push_back(entry.get<std::string>());
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return names;
}

}  // namespace

TEST_CASE("Two instances of one compiled action serve their own precision", "[forms][instance-constraints]") {
    // The issue's measurement, inverted: version 1 declares 3 decimal places,
    // version 2 declares 1, and the key the framework's own vocabulary uses
    // now says so for both.
    const auto schemaV1 = morph::forms::instanceSchemaJson<ICCapture>(version(3, 50));
    const auto schemaV2 = morph::forms::instanceSchemaJson<ICCapture>(version(1, 10));

    CHECK(valuePropertyInt(schemaV1, "x-decimalPlaces") == 3);
    CHECK(valuePropertyInt(schemaV2, "x-decimalPlaces") == 1);
    CHECK(schemaV1 != schemaV2);

    // The bounds ride the same keys, in the same `{"num","den","dp"}` shape
    // the value itself uses on the wire.
    CHECK(boundNumerator(schemaV1, "x-maximum") == 50);
    CHECK(boundNumerator(schemaV2, "x-maximum") == 10);
    CHECK(boundNumerator(schemaV1, "x-minimum") == 0);

    // And each says which of its keys are instance-sourced, so a renderer can
    // tell a decorated schema from the compiled one rather than guessing.
    CHECK(stampedFields(schemaV1) == std::vector<std::string>{"value"});
    CHECK(stampedFields(schemaV2) == std::vector<std::string>{"value"});
}

TEST_CASE("The compiled schema is untouched by decoration", "[forms][instance-constraints]") {
    // `schemaJson<A>()` is memoised per type and shared process-wide; decorating
    // must not write through to that cache. Read it *after* two decorations.
    static_cast<void>(morph::forms::instanceSchemaJson<ICCapture>(version(1, 10)));
    const auto compiled = morph::forms::schemaJson<ICCapture>();

    CHECK(valuePropertyInt(compiled, "x-decimalPlaces") == 3);  // the template parameter
    CHECK(compiled.find("x-instanceConstraints") == std::string::npos);
    CHECK(compiled.find("x-maximum") == std::string::npos);

    // Still true structurally: an instance varies values, never shape. Both
    // versions carry the identical `required` array, which is why runtime
    // *custom fields* remain out of reach (see the spec's Limitations).
    CHECK(compiled.find(R"("required":["analysisVersionId","value"])") != std::string::npos);
    CHECK(morph::forms::instanceSchemaJson<ICCapture>(version(1, 10))
              .find(R"("required":["analysisVersionId","value"])") != std::string::npos);
}

TEST_CASE("An out-of-range value is reported instead of passing silently", "[forms][instance-constraints]") {
    const auto constraints = version(1, 10);  // spec 0..10
    const ICCapture overSpec{.analysisVersionId = 2, .value = ICConcentration{exact(40, 1, 1)}};

    // The framework's own field-level gate still accepts it — a bound living
    // in instance data is not something `validate()` can see, and this seam
    // does not change that. What changes is that something can now name it.
    CHECK(overSpec.validate());

    const auto violations = constraints.checkAction(overSpec);
    REQUIRE(violations.size() == 1);
    CHECK(violations.front() == ConstraintViolation{.field = "value", .kind = ConstraintViolationKind::AboveMaximum});
    CHECK(morph::forms::violationKindName(violations.front().kind) == "aboveMaximum");

    // The same payload against the version whose range contains it is clean.
    CHECK(version(3, 50).checkAction(overSpec).empty());
}

TEST_CASE("Below the declared minimum is its own violation", "[forms][instance-constraints]") {
    const ICCapture negative{.analysisVersionId = 1, .value = ICConcentration{exact(-5, 1, 1)}};
    const auto violations = version(1, 10).checkAction(negative);
    REQUIRE(violations.size() == 1);
    CHECK(violations.front().kind == ConstraintViolationKind::BelowMinimum);
    CHECK(morph::forms::violationKindName(violations.front().kind) == "belowMinimum");
}

TEST_CASE("Precision finer than the instance declares is a violation", "[forms][instance-constraints]") {
    // 1.234 = 617/500: exact at three decimals, not at one.
    const ICCapture threeDecimals{.analysisVersionId = 1, .value = ICConcentration{exact(617, 500, 3)}};

    CHECK(version(3, 50).checkAction(threeDecimals).empty());

    const auto violations = version(1, 10).checkAction(threeDecimals);
    REQUIRE(violations.size() == 1);
    CHECK(violations.front().kind == ConstraintViolationKind::PrecisionExceeded);
    CHECK(morph::forms::violationKindName(violations.front().kind) == "precisionExceeded");
}

TEST_CASE("One value can break more than one declared key at once", "[forms][instance-constraints]") {
    // 12.34 = 617/50: over the 0..10 range *and* finer than one decimal.
    const ICCapture both{.analysisVersionId = 1, .value = ICConcentration{exact(617, 50, 2)}};
    const auto violations = version(1, 10).checkAction(both);
    REQUIRE(violations.size() == 2);
    CHECK(violations[0].kind == ConstraintViolationKind::AboveMaximum);
    CHECK(violations[1].kind == ConstraintViolationKind::PrecisionExceeded);
}

TEST_CASE("A derived value is checked by the same declaration", "[forms][instance-constraints]") {
    // The model's stored value is often not a member of the action at all (a
    // reading multiplied by a dilution factor). `checkValue` is how that value
    // reaches the same bounds the served schema advertised.
    const auto constraints = version(1, 10);
    CHECK(constraints.checkValue("value", exact(8, 1, 1)).empty());

    const auto violations = constraints.checkValue("value", exact(80, 1, 1));
    REQUIRE(violations.size() == 1);
    CHECK(violations.front().kind == ConstraintViolationKind::AboveMaximum);

    // An unconstrained field name is simply unconstrained, never an error.
    CHECK(constraints.checkValue("noSuchField", exact(80, 1, 1)).empty());
}

TEST_CASE("Empty quantities and unconstrained fields are left alone", "[forms][instance-constraints]") {
    const ICCapture empty{.analysisVersionId = 1, .value = ICConcentration{}};
    CHECK(version(1, 10).checkAction(empty).empty());  // emptiness is `required`'s business

    // A constraint on a field the action does not have decorates nothing and
    // stamps nothing — an application's declaration mistake is not worth a
    // crash, exactly as `formLayout` treats one.
    InstanceConstraints stray;
    stray.declare(FieldConstraint{.field = "nope", .decimalPlaces = std::uint32_t{1}});
    const auto served = morph::forms::instanceSchemaJson<ICCapture>(stray);
    CHECK(served.find("x-instanceConstraints") == std::string::npos);
    CHECK(valuePropertyInt(served, "x-decimalPlaces") == 3);
    CHECK(stray.checkAction(ICCapture{.analysisVersionId = 1, .value = ICConcentration{exact(1, 1, 0)}}).empty());
}

TEST_CASE("Declaring the same field twice replaces, never duplicates", "[forms][instance-constraints]") {
    InstanceConstraints constraints;
    CHECK(constraints.empty());
    constraints.declare(FieldConstraint{.field = "value", .maximum = exact(50, 1, 1)});
    constraints.declare(FieldConstraint{.field = "value", .maximum = exact(10, 1, 1)});

    REQUIRE(constraints.fields().size() == 1);
    REQUIRE(constraints.forField("value") != nullptr);
    CHECK((*constraints.forField("value")->maximum).numerator == 10);
    CHECK(constraints.forField("absent") == nullptr);
    CHECK(!constraints.empty());
}

TEST_CASE("Decoration never mangles a schema it cannot read", "[forms][instance-constraints]") {
    const auto constraints = version(1, 10);
    CHECK(constraints.decorate("not json at all") == "not json at all");
    CHECK(constraints.decorate(R"({"type":"object"})") == R"({"type":"object"})");  // no `properties`

    // No declarations means the input is handed straight back.
    CHECK(InstanceConstraints{}.decorate(R"({"properties":{"value":{}}})") == R"({"properties":{"value":{}}})");
}

// ---------------------------------------------------------------------------
// The renderer half of the seam (morph#164).
//
// Serving an instance's keys and checking against them from one declaration is
// only worth anything if the shipped renderer honours what was served. It
// honoured `x-decimalPlaces` already; `x-minimum`/`x-maximum` it ignored
// outright, so an instance's specification range was a second opinion no
// renderer read -- the two-values-for-one-concept outcome the seam exists to
// remove, reintroduced inside the fix for it.
//
// `src/qt/forms/tests/data/instance_bounds.json` is one file with two readers:
// this suite, and `src/qt/forms/tests/tst_DynamicFormInstanceBounds.qml`, which
// drives every row through a real `DynamicForm`. The declaration itself lives
// in the file, so the schema the renderer is fed and the bound checked here are
// provably the same declaration rather than two hand-synced copies.
// ---------------------------------------------------------------------------

namespace {

/// @brief One row of the instance-bounds corpus.
struct BoundsCase {
    std::string id;
    std::string schema;  ///< `"decorated"` or `"compiled"`.
    std::string value;   ///< The text a user would have typed.
    bool ready = false;  ///< Whether the form should become submittable.
};

/// @brief The corpus file, parsed once.
struct BoundsCorpus {
    InstanceConstraints constraints;
    std::map<std::string, std::string, std::less<>> schemas;
    std::vector<BoundsCase> cases;
};

/// @brief Reads a `{"num","den","dp"}` node as an exact rational.
/// @param node The bound node.
/// @return The rational it spells.
[[nodiscard]] Rational boundFrom(const glz::generic_u64& node) {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM
    return Rational{Numerator{node["num"].as<std::int64_t>()}, Denominator{node["den"].as<std::int64_t>()},
                    DecimalPlaces{node["dp"].as<std::uint32_t>()}};
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

/// @brief Parses decimal entry text into the exact rational it denotes.
///
/// `"12.55"` is 1255/100 at two decimal places — the same value the renderer
/// builds from the same text, which is the point: both sides must be judging
/// one number.
/// @param text Decimal text, optionally signed, as typed.
/// @return The exact rational.
[[nodiscard]] Rational typedValue(const std::string& text) {
    const auto dot = text.find('.');
    const auto places = dot == std::string::npos ? 0U : static_cast<std::uint32_t>(text.size() - dot - 1);
    std::string digits = text;
    if (dot != std::string::npos) {
        digits.erase(dot, 1);
    }
    std::int64_t denominator = 1;
    for (std::uint32_t i = 0; i < places; ++i) {
        denominator *= 10;
    }
    return Rational{Numerator{std::stoll(digits)}, Denominator{denominator}, DecimalPlaces{places}};
}

/// @brief The instance-bounds corpus, read from the shared file.
[[nodiscard]] const BoundsCorpus& boundsCorpus() {
    static const BoundsCorpus parsed = [] {
        std::ifstream file{MORPH_FORMS_INSTANCE_BOUNDS};
        REQUIRE(file.is_open());
        std::ostringstream buffer;
        buffer << file.rdbuf();

        glz::generic_u64 dom{};
        REQUIRE_FALSE(glz::read_json(dom, buffer.str()));
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM
        REQUIRE(dom.contains("constraints"));
        REQUIRE(dom.contains("schemas"));
        REQUIRE(dom.contains("cases"));

        BoundsCorpus result{};
        const auto& declared = dom["constraints"];
        result.constraints.declare(FieldConstraint{.field = declared["field"].as<std::string>(),
                                                   .decimalPlaces = declared["decimalPlaces"].as<std::uint32_t>(),
                                                   .minimum = boundFrom(declared["minimum"]),
                                                   .maximum = boundFrom(declared["maximum"])});

        for (const auto& [name, schema] : dom["schemas"].get<glz::generic_u64::object_t>()) {
            result.schemas.emplace(name, schema.as<std::string>());
        }
        for (const auto& entry : dom["cases"].get<glz::generic_u64::array_t>()) {
            result.cases.push_back({.id = entry["id"].as<std::string>(),
                                    .schema = entry["schema"].as<std::string>(),
                                    .value = entry["state"]["value"].as<std::string>(),
                                    .ready = entry["ready"].as<bool>()});
        }
        // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        REQUIRE_FALSE(result.cases.empty());
        return result;
    }();
    return parsed;
}

}  // namespace

TEST_CASE("The corpus the renderer is fed is what this declaration emits", "[forms][instance-constraints]") {
    // Without this the QML half could go on rendering a schema no declaration
    // in this repository produces, and "the two agree" would be about a fossil.
    const auto& corpus = boundsCorpus();
    const auto decorated = corpus.schemas.find("decorated");
    const auto compiled = corpus.schemas.find("compiled");
    REQUIRE(decorated != corpus.schemas.end());
    REQUIRE(compiled != corpus.schemas.end());

    INFO("stale corpus schema; regenerate from:\n" << morph::forms::instanceSchemaJson<ICCapture>(corpus.constraints));
    CHECK(decorated->second == morph::forms::instanceSchemaJson<ICCapture>(corpus.constraints));
    INFO("stale corpus schema; regenerate from:\n" << morph::forms::schemaJson<ICCapture>());
    CHECK(compiled->second == morph::forms::schemaJson<ICCapture>());
}

TEST_CASE("Every corpus row's checked verdict is the one the corpus records", "[forms][instance-constraints]") {
    const auto& corpus = boundsCorpus();
    for (const auto& row : corpus.cases) {
        const auto value = typedValue(row.value);
        if (row.schema == "decorated") {
            INFO("row " << row.id << ": checkValue disagrees with the corpus");
            CHECK(corpus.constraints.checkValue("value", value).empty() == row.ready);
        } else {
            // The control rows: the same values, with no instance declaration
            // to break. Nothing in the compiled type rejects them, which is
            // precisely why the decorated rows above prove the decoration did
            // the work rather than some unrelated per-field check.
            INFO("row " << row.id << ": a control row must be accepted");
            CHECK(InstanceConstraints{}.checkValue("value", value).empty());
            CHECK(row.ready);
        }
    }
}

TEST_CASE("The corpus carries both verdicts", "[forms][instance-constraints]") {
    // A corpus whose every row said "accept" would pass against a renderer that
    // never blocks; one whose every row said "reject" would pass against a
    // renderer that blocks everything.
    std::size_t accepted = 0;
    for (const auto& row : boundsCorpus().cases) {
        accepted += row.ready ? 1U : 0U;
    }
    CHECK(accepted > 0);
    CHECK(accepted < boundsCorpus().cases.size());
}
