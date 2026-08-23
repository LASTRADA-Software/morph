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
#include <memory>

#include <morph/journal/action_log.hpp>
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

TEST_CASE("Revising and fetching refuse malformed requests", "[lims][catalog]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel model;

    const auto v1 = model.execute(nitrate());
    // A zero-denominator bound on a revision is as unusable as on a
    // definition, and is refused by the same `validate()`.
    CHECK_THROWS_AS(model.execute(lims::ReviseAnalysis{
                        .analysisId = v1.analysisId,
                        .canonicalUnit = "mg_per_L",
                        .decimalPlaces = 3,
                        .specHigh = lims::AnalysisBound{.numerator = 10, .denominator = 0}}),
                    lims::ValidationError);
    CHECK_THROWS_AS(model.execute(lims::GetAnalysisVersion{}), lims::ValidationError);
}

TEST_CASE("The served schema carries the version's detection limits too", "[lims][catalog][schema]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel model;

    // nitrate() declares both an LOD and a UDL, so both must reach the form —
    // a renderer that only ever saw the spec range could not tell an operator
    // where the instrument's floor and ceiling are.
    const auto defined = model.execute(nitrate());
    const auto served = model.execute(lims::GetAnalysisSchema{.versionId = defined.versionId});
    CHECK(served.schemaJson.find("x-limitOfDetection") != std::string::npos);
    CHECK(served.schemaJson.find("x-upperDetectionLimit") != std::string::npos);
    CHECK(served.schemaJson.find("x-specLow") != std::string::npos);
    CHECK(served.schemaJson.find("x-specHigh") != std::string::npos);
}

TEST_CASE("The catalogue journals its edits against the attached identity", "[lims][catalog][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    lims::AnalysisCatalogModel model;
    model.attachActionLog(log, std::string{"catalogue"});

    const auto v1 = model.execute(nitrate());
    model.execute(lims::ReviseAnalysis{.analysisId = v1.analysisId,
                                       .canonicalUnit = "mg_per_L",
                                       .decimalPlaces = 3,
                                       .specHigh = lims::AnalysisBound{.numerator = 10, .denominator = 1}});

    // The catalogue is lab-wide, so its entries carry the identity given at
    // attach time rather than a per-entity one. Editing a definition is an
    // auditable act: an analysis whose bounds moved without a recorded author
    // is exactly what a regulator asks about.
    const auto entries = log->entries("catalogue");
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].actionType == "DefineAnalysis");
    CHECK(entries[1].actionType == "ReviseAnalysis");
    for (const auto& entry : entries) {
        CHECK(entry.modelType == "AnalysisCatalogModel");
        CHECK(entry.principal == "alice");
        CHECK(entry.outcome == morph::journal::Outcome::Succeeded);
    }
}

TEST_CASE("An analysis with no versions is skipped rather than listed half-formed",
          "[lims][catalog]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel model;

    model.execute(nitrate());

    // An identity row with no version row cannot arise through the model
    // (DefineAnalysis writes both in one transaction), so it is written
    // directly. It is still worth handling: the two tables are separate, and
    // a listing that dereferenced a missing version would crash rather than
    // omit.
    {
        Lightweight::DataMapper mapper;
        lims::db::AnalysisRecord orphan;
        orphan.name = Lightweight::SqlAnsiString<128>{std::string{"Orphan"}};
        mapper.Create(orphan);
    }

    const auto listed = model.execute(lims::ListAnalyses{});
    REQUIRE(listed.analyses.size() == 1);
    CHECK(listed.analyses.front().name == "Nitrate");
}
