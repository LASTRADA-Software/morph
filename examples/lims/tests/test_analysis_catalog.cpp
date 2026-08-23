// SPDX-License-Identifier: Apache-2.0
//
// The analysis catalogue: definitions, and the append-only versioning that
// keeps an old result bound to the definition it was captured under.

#include "lims/core/errors.hpp"
#include "lims/db/lims_entity.hpp"
#include "lims/models/analysis_catalog_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/session/session.hpp>

#include <string>

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

namespace {

[[nodiscard]] lims::DefineAnalysis nitrate() {
    return lims::DefineAnalysis{
        .name = "Nitrate",
        .canonicalUnit = "mg_per_L",
        .decimalPlaces = 3,
        .specLow = lims::AnalysisBound{.numerator = 0, .denominator = 1},
        .specHigh = lims::AnalysisBound{.numerator = 50, .denominator = 1},
        .limitOfDetection = lims::AnalysisBound{.numerator = 1, .denominator = 100},
        .upperDetectionLimit = lims::AnalysisBound{.numerator = 200, .denominator = 1},
    };
}

}  // namespace

TEST_CASE("DefineAnalysis creates the analysis and its version 1", "[lims][catalog]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel model;

    const auto created = model.execute(nitrate());
    REQUIRE(created.analysisId.hasValue());
    REQUIRE(created.versionId.hasValue());
    CHECK(created.version == 1);

    const auto view = model.execute(lims::GetAnalysisVersion{.versionId = created.versionId});
    CHECK(view.name == "Nitrate");
    CHECK(view.canonicalUnit == "mg_per_L");
    CHECK(view.decimalPlaces == 3);
    REQUIRE(view.specHigh.has_value());
    CHECK(view.specHigh->numerator == 50);
    REQUIRE(view.limitOfDetection.has_value());
    CHECK(view.limitOfDetection->numerator == 1);
    CHECK(view.limitOfDetection->denominator == 100);
}

TEST_CASE("ReviseAnalysis appends a version and leaves the previous one intact",
          "[lims][catalog][versioning]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel model;

    const auto v1 = model.execute(nitrate());

    // Narrow the spec range, exactly the edit the README's stale-schema
    // scenario is about.
    const auto v2 = model.execute(lims::ReviseAnalysis{
        .analysisId = v1.analysisId,
        .canonicalUnit = "mg_per_L",
        .decimalPlaces = 3,
        .specLow = lims::AnalysisBound{.numerator = 0, .denominator = 1},
        .specHigh = lims::AnalysisBound{.numerator = 10, .denominator = 1},
        .limitOfDetection = lims::AnalysisBound{.numerator = 1, .denominator = 100},
        .upperDetectionLimit = lims::AnalysisBound{.numerator = 200, .denominator = 1},
    });
    CHECK(v2.version == 2);
    CHECK(v2.analysisId == v1.analysisId);
    CHECK(v2.versionId != v1.versionId);

    // Version 1 still says 50, not 10. This is the property the whole
    // versioning design exists for: a result captured under v1 must still be
    // renderable against v1's own bounds.
    const auto stillV1 = model.execute(lims::GetAnalysisVersion{.versionId = v1.versionId});
    CHECK(stillV1.version == 1);
    REQUIRE(stillV1.specHigh.has_value());
    CHECK(stillV1.specHigh->numerator == 50);

    const auto asV2 = model.execute(lims::GetAnalysisVersion{.versionId = v2.versionId});
    REQUIRE(asV2.specHigh.has_value());
    CHECK(asV2.specHigh->numerator == 10);
}

TEST_CASE("ListAnalyses returns each analysis's current version", "[lims][catalog]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel model;

    const auto v1 = model.execute(nitrate());
    model.execute(lims::ReviseAnalysis{.analysisId = v1.analysisId,
                                       .canonicalUnit = "mg_per_L",
                                       .decimalPlaces = 3,
                                       .specHigh = lims::AnalysisBound{.numerator = 10, .denominator = 1}});
    model.execute(lims::DefineAnalysis{.name = "pH", .canonicalUnit = "scalar", .decimalPlaces = 2});

    const auto listed = model.execute(lims::ListAnalyses{});
    REQUIRE(listed.analyses.size() == 2);

    const auto* nitrateView = &listed.analyses[0];
    for (const auto& candidate : listed.analyses) {
        if (candidate.name == "Nitrate") {
            nitrateView = &candidate;
        }
    }
    CHECK(nitrateView->name == "Nitrate");
    // Current, not original.
    CHECK(nitrateView->version == 2);
}

TEST_CASE("A definition with a zero-denominator bound is rejected", "[lims][catalog]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel model;

    auto bad = nitrate();
    bad.specHigh = lims::AnalysisBound{.numerator = 50, .denominator = 0};
    CHECK_THROWS_AS(model.execute(bad), lims::ValidationError);
}

TEST_CASE("Defining an analysis with no principal is refused", "[lims][catalog][audit]") {
    DbFixture fixture;
    lims::AnalysisCatalogModel model;

    // No ScopedPrincipal: a catalogue edit with no author would produce an
    // audit entry naming nobody.
    CHECK_THROWS_AS(model.execute(nitrate()), lims::EmptyPrincipalError);
}

TEST_CASE("Revising an unknown analysis is NotFound, not a silent insert", "[lims][catalog]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel model;

    CHECK_THROWS_AS(model.execute(lims::ReviseAnalysis{.analysisId = lims::AnalysisId{999999},
                                                        .canonicalUnit = "mg_per_L",
                                                        .decimalPlaces = 3}),
                    lims::NotFound);
}
