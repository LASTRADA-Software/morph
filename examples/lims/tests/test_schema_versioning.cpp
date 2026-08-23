// SPDX-License-Identifier: Apache-2.0
//
// Schema versioning (README build order §4, review D4 — mandatory, no socket
// needed). The claim under test is deliberately a *split* one:
//
//   rendering is version-bound; validation is not.
//
// A client asking for version N's result-entry form gets version N's own
// precision, spec range and detection limits, however many revisions have
// landed since. But everything `morph::forms` itself enforces — the `required`
// array, the `x-rules` list, the `x-decimalPlaces` the framework checks on
// dispatch — comes from the compiled `CaptureConcentration` struct and is
// byte-identical for every version. Every version-specific rule this rung
// actually enforces had to be re-implemented as a hand-written model check
// reading the version row.

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/rational.hpp>
#include <string>

#include "lims/core/errors.hpp"
#include "lims/models/analysis_catalog_model.hpp"
#include "lims/models/sample_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

/// @brief An exact rational at @p places decimal places.
/// @param num Numerator.
/// @param den Denominator.
/// @param places Decimal-precision tag.
/// @return The canonical rational.
[[nodiscard]] Rational exact(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return Rational{Numerator{num}, Denominator{den}, DecimalPlaces{places}};
}

/// @brief Nitrate at 3 decimal places, spec 0..50 mg/L.
/// @return The definition action.
[[nodiscard]] lims::DefineAnalysis nitrateV1() {
    return lims::DefineAnalysis{
        .name = "Nitrate",
        .canonicalUnit = "mg_per_L",
        .decimalPlaces = 3,
        .specLow = lims::AnalysisBound{.numerator = 0, .denominator = 1},
        .specHigh = lims::AnalysisBound{.numerator = 50, .denominator = 1},
    };
}

/// @brief The same analysis narrowed: 1 decimal place, spec 0..10 mg/L.
/// @param analysisId The analysis to revise.
/// @return The revision action.
[[nodiscard]] lims::ReviseAnalysis nitrateV2(lims::AnalysisId analysisId) {
    return lims::ReviseAnalysis{
        .analysisId = analysisId,
        .canonicalUnit = "mg_per_L",
        .decimalPlaces = 1,
        .specLow = lims::AnalysisBound{.numerator = 0, .denominator = 1},
        .specHigh = lims::AnalysisBound{.numerator = 10, .denominator = 1},
    };
}

/// @brief Registers a sample and walks it to `InProgress`.
/// @param model The sample model to work through.
/// @return The sample, at `InProgress`.
lims::SampleView sampleAtWork(lims::SampleModel& model) {
    const auto client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    model.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-1"});
    model.execute(lims::ReceiveSample{});
    return model.execute(lims::StartWork{});
}

/// @brief Reads one integer key off the `value` property of a served schema.
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
    // `as<>`, not `get<>`: generic_u64 stores a positive integer as uint64_t
    // after a write/read round trip, so a `get<int64_t>` would throw.
    return value[key].as<std::int64_t>();
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

/// @brief Reads a `{"num":..,"den":..}` node's numerator off a served schema.
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

}  // namespace

TEST_CASE("Revising an analysis does not change the form its old version serves", "[lims][schema][versioning]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;

    const auto v1 = catalog.execute(nitrateV1());
    const auto schemaBefore = catalog.execute(lims::GetAnalysisSchema{.versionId = v1.versionId});
    CHECK(schemaBefore.version == 1);
    CHECK(valuePropertyInt(schemaBefore.schemaJson, "x-versionDecimalPlaces") == 3);
    CHECK(boundNumerator(schemaBefore.schemaJson, "x-specHigh") == 50);

    const auto v2 = catalog.execute(nitrateV2(v1.analysisId));

    // v1's form is byte-for-byte what it was before the revision landed. This
    // is the ODK property: a result captured under v1 can still be rendered,
    // years later, against the definition it was captured under.
    const auto schemaAfter = catalog.execute(lims::GetAnalysisSchema{.versionId = v1.versionId});
    CHECK(schemaAfter.schemaJson == schemaBefore.schemaJson);

    // v2's form is a different one.
    const auto schemaV2 = catalog.execute(lims::GetAnalysisSchema{.versionId = v2.versionId});
    CHECK(schemaV2.version == 2);
    CHECK(valuePropertyInt(schemaV2.schemaJson, "x-versionDecimalPlaces") == 1);
    CHECK(boundNumerator(schemaV2.schemaJson, "x-specHigh") == 10);
    CHECK(schemaV2.schemaJson != schemaBefore.schemaJson);
}

TEST_CASE("Only the version's *data* varies; the framework-enforced parts are identical",
          "[lims][schema][versioning][finding]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;

    const auto v1 = catalog.execute(nitrateV1());               // 3 decimal places
    const auto v2 = catalog.execute(nitrateV2(v1.analysisId));  // 1 decimal place

    const auto schemaV1 = catalog.execute(lims::GetAnalysisSchema{.versionId = v1.versionId});
    const auto schemaV2 = catalog.execute(lims::GetAnalysisSchema{.versionId = v2.versionId});

    // This is review D4, stated as an assertion. `x-decimalPlaces` is the key
    // the framework itself enforces on dispatch ("Advertised precision is
    // enforced on dispatch", docs/spec/forms/forms.md) and it is emitted from
    // `Quantity<mg_per_L, 3>::declaredDecimals` — a *template parameter*. So
    // both versions advertise 3, including the one whose definition says 1.
    CHECK(valuePropertyInt(schemaV1.schemaJson, "x-decimalPlaces") == 3);
    CHECK(valuePropertyInt(schemaV2.schemaJson, "x-decimalPlaces") == 3);
    CHECK(valuePropertyInt(schemaV2.schemaJson, "x-versionDecimalPlaces") == 1);

    // The two keys disagree, and that disagreement is the finding: a lab that
    // defines an analysis at 1 dp gets a form whose framework-enforced
    // precision is 3. Served side by side deliberately — overwriting
    // `x-decimalPlaces` would advertise a promise no code keeps.
    CHECK(valuePropertyInt(schemaV2.schemaJson, "x-decimalPlaces") !=
          valuePropertyInt(schemaV2.schemaJson, "x-versionDecimalPlaces"));

    // Everything else morph::forms derives is compiled, so it is the same text
    // in both versions' forms: the rule list and the required array.
    const auto compiled = morph::forms::schemaJson<lims::CaptureConcentration>();
    for (const auto& served : {schemaV1.schemaJson, schemaV2.schemaJson}) {
        CHECK(served.find(R"("x-rules":[{"kind":"exactlyOneOf","fields":["value","qualifier"]}])") !=
              std::string::npos);
        CHECK(served.find(R"("required":["analysisVersionId"])") != std::string::npos);
    }
    CHECK(compiled.find(R"("x-rules":[{"kind":"exactlyOneOf","fields":["value","qualifier"]}])") != std::string::npos);
}

TEST_CASE("The spec range a served schema advertises is enforced by nobody",
          "[lims][schema][versioning][finding]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel model;

    const auto v1 = catalog.execute(nitrateV1());               // spec 0..50
    const auto v2 = catalog.execute(nitrateV2(v1.analysisId));  // spec 0..10
    sampleAtWork(model);

    // v2's served form says the value may not exceed 10 mg/L.
    const auto schemaV2 = catalog.execute(lims::GetAnalysisSchema{.versionId = v2.versionId});
    REQUIRE(boundNumerator(schemaV2.schemaJson, "x-specHigh") == 10);

    // 40 mg/L is outside it. The framework's own gate — `validate()`, which
    // every dispatch path runs before `execute` — accepts it anyway, because
    // `allRulesSatisfied` evaluates the *compiled* rule list, and that
    // vocabulary has no notion of a bound living in a database row.
    const lims::CaptureConcentration overSpec{.analysisVersionId = v2.versionId,
                                              .value = lims::Concentration{exact(40, 1, 1)}};
    CHECK(overSpec.validate());

    // And the model stores it: an out-of-specification result is a result to
    // *flag*, not one to refuse (SENAITE's own model), so refusing here would
    // be wrong. But nothing computes that flag either — the bound round-trips
    // to the client and back and is acted on nowhere. This assertion is what
    // will fail the day someone adds enforcement or flagging, which is the
    // point of pinning it.
    const auto stored = model.execute(overSpec);
    REQUIRE(stored.value.hasValue());
    CHECK((*stored.value).numerator == 40);
    CHECK((*stored.value).denominator == 1);

    // The same payload against v1, whose spec range *does* contain it, is
    // indistinguishable at every layer that runs: same acceptance, same store.
    const lims::CaptureConcentration inSpec{.analysisVersionId = v1.versionId,
                                            .value = lims::Concentration{exact(40, 1, 1)}};
    CHECK(inSpec.validate());
    CHECK((*model.execute(inSpec).value).numerator == 40);
}

TEST_CASE("Per-version precision is enforced only because the model re-implements it by hand",
          "[lims][schema][versioning]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel model;

    const auto v1 = catalog.execute(nitrateV1());               // 3 decimal places
    const auto v2 = catalog.execute(nitrateV2(v1.analysisId));  // 1 decimal place
    sampleAtWork(model);

    // 1.234 = 617/500, exact at three decimals.
    const auto threeDecimals = lims::Concentration{exact(617, 500, 3)};

    // Accepted under v1 (which declares 3)...
    const auto stored =
        model.execute(lims::CaptureConcentration{.analysisVersionId = v1.versionId, .value = threeDecimals});
    CHECK((*stored.value).denominator == 500);

    // ...and refused under v2 (which declares 1) — by `SampleModel`'s own
    // check against the version row, not by anything morph::forms did. The
    // served v2 form's `x-decimalPlaces` still says 3, so a renderer honouring
    // only the framework-enforced key would happily submit this payload and
    // have it rejected. That gap is the reason `x-versionDecimalPlaces` is
    // served beside it.
    CHECK_THROWS_AS(
        model.execute(lims::CaptureConcentration{.analysisVersionId = v2.versionId, .value = threeDecimals}),
        lims::ValidationError);
}

TEST_CASE("A version with no compiled result-entry action is refused, not guessed at",
          "[lims][schema][versioning][finding]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;

    // There is one compiled action per *unit family*, not per analysis, so an
    // analysis denominated in amps has no form to serve. Refusing is the
    // honest answer; returning the mg/L form would be a plausible-looking
    // wrong one.
    const auto current =
        catalog.execute(lims::DefineAnalysis{.name = "Probe current", .canonicalUnit = "A", .decimalPlaces = 3});
    CHECK_THROWS_AS(catalog.execute(lims::GetAnalysisSchema{.versionId = current.versionId}), lims::ValidationError);
}

TEST_CASE("Asking for a schema needs a real version", "[lims][schema][versioning]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;

    CHECK_THROWS_AS(catalog.execute(lims::GetAnalysisSchema{}), lims::ValidationError);
    CHECK_THROWS_AS(catalog.execute(lims::GetAnalysisSchema{.versionId = lims::AnalysisVersionId{424242}}),
                    lims::NotFound);
}
