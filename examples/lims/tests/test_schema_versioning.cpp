// SPDX-License-Identifier: Apache-2.0
//
// Schema versioning (README build order §4, review D4 — mandatory, no socket
// needed). The claim under test is a *split* one, and upstream issue #164
// moved where the split falls:
//
//   values are version-bound; structure is not.
//
// A client asking for version N's result-entry form gets version N's own
// precision and specification range **in the framework's own keys**
// (`x-decimalPlaces`, `x-minimum`, `x-maximum`), because the model declares
// them once as `morph::forms::InstanceConstraints` and that same declaration
// is what `SampleModel` checks the submitted reading against. There is no
// longer a second, app-private precision key contradicting the framework's.
//
// What is still compiled is the form's *shape*: which fields exist, the
// `required` array and the `x-rules` list all come from the
// `CaptureConcentration` struct and are byte-identical for every version. A
// version wanting a different shape cannot be served at all — which is the
// boundary rung 7's runtime custom fields will run into head-on.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
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
    CHECK(valuePropertyInt(schemaBefore.schemaJson, "x-decimalPlaces") == 3);
    CHECK(boundNumerator(schemaBefore.schemaJson, "x-maximum") == 50);

    const auto v2 = catalog.execute(nitrateV2(v1.analysisId));

    // v1's form is byte-for-byte what it was before the revision landed. This
    // is the ODK property: a result captured under v1 can still be rendered,
    // years later, against the definition it was captured under.
    const auto schemaAfter = catalog.execute(lims::GetAnalysisSchema{.versionId = v1.versionId});
    CHECK(schemaAfter.schemaJson == schemaBefore.schemaJson);

    // v2's form is a different one.
    const auto schemaV2 = catalog.execute(lims::GetAnalysisSchema{.versionId = v2.versionId});
    CHECK(schemaV2.version == 2);
    CHECK(valuePropertyInt(schemaV2.schemaJson, "x-decimalPlaces") == 1);
    CHECK(boundNumerator(schemaV2.schemaJson, "x-maximum") == 10);
    CHECK(schemaV2.schemaJson != schemaBefore.schemaJson);
}

TEST_CASE("Each version's form carries its own precision, in the framework's own key", "[lims][schema][versioning]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;

    const auto v1 = catalog.execute(nitrateV1());               // 3 decimal places
    const auto v2 = catalog.execute(nitrateV2(v1.analysisId));  // 1 decimal place

    const auto schemaV1 = catalog.execute(lims::GetAnalysisSchema{.versionId = v1.versionId});
    const auto schemaV2 = catalog.execute(lims::GetAnalysisSchema{.versionId = v2.versionId});

    // Review D4, restated as what it is now. `x-decimalPlaces` is the key the
    // renderer honours, and each version's form carries that version's own
    // number — the one the lab actually defined — rather than the compiled
    // `Quantity<mg_per_L, 3>` template parameter for both.
    CHECK(valuePropertyInt(schemaV1.schemaJson, "x-decimalPlaces") == 3);
    CHECK(valuePropertyInt(schemaV2.schemaJson, "x-decimalPlaces") == 1);

    // And there is exactly *one* precision key: the second, app-private
    // `x-versionDecimalPlaces` the rung had to serve beside it is gone. Two
    // keys for one concept, with no way for a renderer to know which to
    // believe, was worse than either alone.
    CHECK(schemaV1.schemaJson.find("x-versionDecimalPlaces") == std::string::npos);
    CHECK(schemaV2.schemaJson.find("x-versionDecimalPlaces") == std::string::npos);

    // Which keys came from the row rather than from the compiled type is
    // stated in the document, not left to be inferred.
    CHECK(schemaV2.schemaJson.find(R"("x-instanceConstraints":["value"])") != std::string::npos);

    // The *compiled* schema is untouched by any of this — it is memoised
    // per type and shared process-wide, and still answers 3 for everyone.
    // That is the surviving half of the finding: an instance varies the
    // *values* of keys, never the form's shape.
    const auto compiled = morph::forms::schemaJson<lims::CaptureConcentration>();
    CHECK(valuePropertyInt(compiled, "x-decimalPlaces") == 3);
    CHECK(compiled.find("x-instanceConstraints") == std::string::npos);

    // Structure is identical in both versions' forms: the rule list and the
    // required array come from the compiled struct.
    for (const auto& served : {schemaV1.schemaJson, schemaV2.schemaJson}) {
        CHECK(served.find(R"({"kind":"exactlyOneOf","fields":["value","qualifier"]})") != std::string::npos);
        CHECK(served.find(R"("required":["analysisVersionId"])") != std::string::npos);
    }
    CHECK(compiled.find(R"({"kind":"exactlyOneOf","fields":["value","qualifier"]})") != std::string::npos);
}

TEST_CASE("A reading outside the served spec range is stored, and flagged", "[lims][schema][versioning]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel model;

    const auto v1 = catalog.execute(nitrateV1());               // spec 0..50
    const auto v2 = catalog.execute(nitrateV2(v1.analysisId));  // spec 0..10
    sampleAtWork(model);

    // v2's served form says the value may not exceed 10 mg/L — in `x-maximum`,
    // a key the framework has a vocabulary for, not an app-private one.
    const auto schemaV2 = catalog.execute(lims::GetAnalysisSchema{.versionId = v2.versionId});
    REQUIRE(boundNumerator(schemaV2.schemaJson, "x-maximum") == 10);

    // 40 mg/L is outside it. `validate()` still accepts it, and still cannot
    // see why: `allRulesSatisfied` evaluates the *compiled* rule list, whose
    // vocabulary has no notion of a bound living in a database row. That has
    // not changed and is not what the fix is about.
    const lims::CaptureConcentration overSpec{.analysisVersionId = v2.versionId,
                                              .value = lims::Concentration{exact(40, 1, 1)}};
    CHECK(overSpec.validate());

    // What changed is what happens next. The model stores it — an
    // out-of-specification result is a result to *flag*, not to refuse — and
    // now it flags it, against the identical constraint declaration the served
    // form was decorated from. Previously the bound round-tripped to the
    // client and back and was acted on nowhere.
    const auto stored = model.execute(overSpec);
    REQUIRE(stored.value.hasValue());
    CHECK((*stored.value).numerator == 40);
    CHECK(stored.outOfSpec);

    // The same payload against v1, whose spec range *does* contain it, is
    // stored unflagged — the two are no longer indistinguishable.
    const lims::CaptureConcentration inSpec{.analysisVersionId = v1.versionId,
                                            .value = lims::Concentration{exact(40, 1, 1)}};
    const auto within = model.execute(inSpec);
    CHECK((*within.value).numerator == 40);
    CHECK(!within.outOfSpec);

    // The flag survives the round trip through storage, so a report built from
    // the results list sees it too, not just the caller who captured it.
    const auto listed = model.execute(lims::ListResults{});
    REQUIRE(listed.results.size() == 2);
    std::size_t flagged = 0;
    for (const auto& row : listed.results) {
        flagged += row.outOfSpec ? 1U : 0U;
    }
    CHECK(flagged == 1);
}

TEST_CASE("A qualifier makes no numeric claim, so it is never out of specification", "[lims][schema][versioning]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel model;

    const auto v1 = catalog.execute(nitrateV1());
    catalog.execute(nitrateV2(v1.analysisId));
    sampleAtWork(model);

    const auto stored = model.execute(lims::CaptureConcentration{
        .analysisVersionId = v1.versionId, .qualifier = lims::QualifierChoice{std::string{lims::kQualifierBelowLod}}});
    CHECK(!stored.value.hasValue());
    CHECK(!stored.outOfSpec);
}

TEST_CASE("Per-version precision is refused, from the same declaration that served it", "[lims][schema][versioning]") {
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

    // ...and refused under v2 (which declares 1). Refused rather than rounded:
    // a reading finer than the method supports is a claim about the
    // instrument. What is new is that the served v2 form's `x-decimalPlaces`
    // now *says* 1, so a renderer honouring the framework-enforced key would
    // never have submitted this payload in the first place — the client-side
    // gate and the server-side one finally read the same number.
    REQUIRE(valuePropertyInt(catalog.execute(lims::GetAnalysisSchema{.versionId = v2.versionId}).schemaJson,
                             "x-decimalPlaces") == 1);
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
