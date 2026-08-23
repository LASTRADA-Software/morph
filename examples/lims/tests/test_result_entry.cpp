// SPDX-License-Identifier: Apache-2.0
//
// Result entry with units (README build order §3): the multi-field encoding
// of `quantity | belowLOD | aboveUDL`, exact entry-unit conversion, and the
// D-test — all three "no number" meanings surviving the wire, the journal and
// an offline payload *distinguishably*.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/forms/forms.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/offline/offline_queue.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

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

/// @brief The `x-unitAlternatives` entry for @p wanted in @p alternatives.
/// @tparam Alternatives The span type `Quantity::unitAlternatives()` returns.
/// @param alternatives The field's advertised alternatives.
/// @param wanted The entry unit to look up.
/// @return The alternative, which the caller has already asserted exists.
template <typename Alternatives>
[[nodiscard]] auto alternativeFor(const Alternatives& alternatives, lims::LimsUnit wanted) {
    for (const auto& candidate : alternatives) {
        if (candidate.unit == wanted) {
            return candidate;
        }
    }
    FAIL("the unit system declares no direct relation to the requested entry unit");
    return alternatives.front();
}

/// @brief The four states the result encoding can be in — one reading and the
///        three distinct "no number" claims.
[[nodiscard]] std::array<lims::CaptureConcentration, 4> allFourStates(lims::AnalysisVersionId versionId) {
    return {
        lims::CaptureConcentration{.analysisVersionId = versionId, .value = lims::Concentration{exact(12, 5, 3)}},
        lims::CaptureConcentration{.analysisVersionId = versionId,
                                   .qualifier = lims::QualifierChoice{std::string{lims::kQualifierNotMeasured}}},
        lims::CaptureConcentration{.analysisVersionId = versionId,
                                   .qualifier = lims::QualifierChoice{std::string{lims::kQualifierBelowLod}}},
        lims::CaptureConcentration{.analysisVersionId = versionId,
                                   .qualifier = lims::QualifierChoice{std::string{lims::kQualifierAboveUdl}}},
    };
}

/// @brief Defines a nitrate analysis in mg/L at 3 dp.
/// @param catalog The catalogue to define it in.
/// @return The new analysis and its version 1.
lims::DefineAnalysisResult defineNitrate(lims::AnalysisCatalogModel& catalog) {
    return catalog.execute(lims::DefineAnalysis{.name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3});
}

/// @brief Registers a sample and walks it to `InProgress`, where results may
///        be captured.
/// @param model The sample model to work through.
/// @return The sample, at `InProgress`.
lims::SampleView sampleAtWork(lims::SampleModel& model) {
    const auto client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    const auto sample = model.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-1"});
    model.execute(lims::ReceiveSample{});
    return model.execute(lims::StartWork{});
}

}  // namespace

// ── The multi-field sum-type encoding ──────────────────────────────────────

TEST_CASE("Exactly one of value / qualifier may be engaged", "[lims][result][encoding]") {
    const lims::AnalysisVersionId version{1};

    // A reading.
    CHECK(lims::CaptureConcentration{.analysisVersionId = version, .value = lims::Concentration{exact(12, 5, 3)}}
              .validate());
    // A non-reading.
    CHECK(lims::CaptureConcentration{.analysisVersionId = version,
                                     .qualifier = lims::QualifierChoice{std::string{lims::kQualifierBelowLod}}}
              .validate());

    // Neither: no claim at all is not one of the three claims.
    CHECK_FALSE(lims::CaptureConcentration{.analysisVersionId = version}.validate());

    // Both: "0.5 mg/L, and also below the detection limit" is the state the
    // encoding exists to make unrepresentable.
    CHECK_FALSE(lims::CaptureConcentration{.analysisVersionId = version,
                                           .value = lims::Concentration{exact(1, 2, 3)},
                                           .qualifier = lims::QualifierChoice{std::string{lims::kQualifierBelowLod}}}
                    .validate());

    // The rule is not the only gate: the version still has to be named.
    CHECK_FALSE(lims::CaptureConcentration{.value = lims::Concentration{exact(12, 5, 3)}}.validate());
}

TEST_CASE("An unknown qualifier code decodes to nothing, never to a default", "[lims][result][encoding]") {
    CHECK(lims::qualifierFromCode("belowLOD") == lims::ResultQualifier::BelowLOD);
    CHECK(lims::qualifierFromCode("aboveUDL") == lims::ResultQualifier::AboveUDL);
    CHECK(lims::qualifierFromCode("notMeasured") == lims::ResultQualifier::NotMeasured);

    // Fail-closed. Resolving an unknown code to NotMeasured would turn "this
    // client speaks a dialect we do not know" into the lab asserting it never
    // looked — a fabricated claim, not a parse failure.
    CHECK_FALSE(lims::qualifierFromCode("").has_value());
    CHECK_FALSE(lims::qualifierFromCode("BELOWLOD").has_value());
    CHECK_FALSE(lims::qualifierFromCode("measured").has_value());
    CHECK_FALSE(lims::qualifierFromCode("belowDetectionLimit").has_value());
}

// ── The served schema ──────────────────────────────────────────────────────

TEST_CASE("The served schema carries the encoding, the precision and the entry units", "[lims][result][forms]") {
    const auto schema = morph::forms::schemaJson<lims::CaptureConcentration>();
    REQUIRE_FALSE(schema.empty());

    // The sum-type encoding travels to the client as data, so the renderer
    // enforces the same rule the server re-runs from the compiled rule list.
    CHECK(schema.find("\"x-rules\"") != std::string::npos);
    CHECK(schema.find("\"exactlyOneOf\"") != std::string::npos);
    CHECK(schema.find("\"value\"") != std::string::npos);
    CHECK(schema.find("\"qualifier\"") != std::string::npos);

    // The qualifier picklist is discovered, not hardcoded client-side.
    CHECK(schema.find("\"x-optionsAction\":\"ListResultQualifiers\"") != std::string::npos);

    // The reading's declared precision and its entry-unit alternatives.
    CHECK(schema.find("\"x-decimalPlaces\":3") != std::string::npos);
    CHECK(schema.find("\"x-unitAlternatives\"") != std::string::npos);
    CHECK(schema.find("\"ug_per_L\"") != std::string::npos);
    CHECK(schema.find("\"ng_per_L\"") != std::string::npos);
}

TEST_CASE("x-unitAlternatives lists direct relation edges only", "[lims][result][units]") {
    // The rung declares ng/L -> mg/L explicitly *as well as* ng/L -> µg/L ->
    // mg/L, and this is why: `Quantity::convert` composes a path through the
    // relation graph, but `unitAlternatives()` (what the schema publishes)
    // only ever reports direct neighbours. Drop the redundant-looking edge and
    // the C++ conversion still works while the generated form silently stops
    // offering ng/L as an entry unit.
    const auto alternatives = lims::Concentration::unitAlternatives();
    CHECK(alternatives.size() == 2);

    // µg/L is a neighbour of mg/L; ng/L is one only because the edge was
    // declared. Neither mA nor A is reachable at all — a different dimension.
    CHECK(alternativeFor(alternatives, lims::LimsUnit::ug_per_L).unit == lims::LimsUnit::ug_per_L);
    CHECK(alternativeFor(alternatives, lims::LimsUnit::ng_per_L).unit == lims::LimsUnit::ng_per_L);
    CHECK(lims::Current::unitAlternatives().size() == 1);
}

// ── Entry-unit conversion (the InvenTree flow) ─────────────────────────────

TEST_CASE("1500 mA against a template in A converts exactly to 1.5 A", "[lims][result][units]") {
    // The definition-of-done flow, checked from *both* ends so a client that
    // honours the schema and a server that runs the C++ converter cannot
    // disagree.
    //
    // (a) What the served schema advertises: value_in_A = value_in_mA * num/den.
    const auto milliamps = alternativeFor(lims::Current::unitAlternatives(), lims::LimsUnit::mA);
    const auto advertised = exact(milliamps.num, milliamps.den, 3);
    const auto fromSchema = exact(1500, 1, 0) * advertised;

    // (b) What the compiled converter does with the same reading.
    const lims::Current converted = lims::CurrentMilli{exact(1500, 1, 1)};

    REQUIRE(converted.hasValue());
    CHECK(*converted == fromSchema);
    // Exactly 3/2 — no double ever touched it.
    CHECK((*converted).numerator == 3);
    CHECK((*converted).denominator == 2);
}

TEST_CASE("Concentration entry units convert exactly, including the 10^-6 edge", "[lims][result][units]") {
    // 1500 µg/L is 1.5 mg/L.
    const lims::Concentration fromMicro = lims::ConcentrationMicro{exact(1500, 1, 3)};
    REQUIRE(fromMicro.hasValue());
    CHECK((*fromMicro).numerator == 3);
    CHECK((*fromMicro).denominator == 2);

    // 2500 ng/L is 0.0025 mg/L = 1/400 — the trace-concentration range the
    // README flags, where a float round-trip would already have lost digits.
    const lims::Concentration fromNano = lims::ConcentrationNano{exact(2500, 1, 3)};
    REQUIRE(fromNano.hasValue());
    CHECK((*fromNano).numerator == 1);
    CHECK((*fromNano).denominator == 400);

    // And the schema advertises that same 10^-6 ratio for ng/L.
    const auto nano = alternativeFor(lims::Concentration::unitAlternatives(), lims::LimsUnit::ng_per_L);
    CHECK(exact(2500, 1, 0) * exact(nano.num, nano.den, 6) == *fromNano);

    // Chaining agrees with the declared shortcut: ng/L -> µg/L -> mg/L is the
    // same exact value as the direct ng/L -> mg/L edge.
    const lims::ConcentrationMicro viaMicro = lims::ConcentrationNano{exact(2500, 1, 3)};
    const lims::Concentration chained = viaMicro;
    CHECK(*chained == *fromNano);
}

// ── The D-test: three "no number" meanings, distinguishable everywhere ─────

TEST_CASE("The four result states are pairwise distinct on the wire and decode back", "[lims][result][wire]") {
    using Traits = morph::model::ActionTraits<lims::CaptureConcentration>;
    const auto states = allFourStates(lims::AnalysisVersionId{7});

    std::vector<std::string> encoded;
    for (const auto& state : states) {
        encoded.push_back(Traits::toJson(state));
    }

    // Pairwise distinct: "empty quantity", "below LOD" and "above UDL" are
    // three different scientific claims, and a wire form that collapsed any
    // two of them would make the fourth column of a report a lie.
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        for (std::size_t j = i + 1; j < encoded.size(); ++j) {
            INFO("state " << i << " vs " << j << ": " << encoded[i] << " / " << encoded[j]);
            CHECK(encoded[i] != encoded[j]);
        }
    }

    // And each survives the round trip as itself, not as a neighbour.
    for (std::size_t i = 0; i < states.size(); ++i) {
        const auto decoded = Traits::fromJson(encoded[i]);
        INFO("state " << i << ": " << encoded[i]);
        CHECK(decoded.value == states[i].value);
        CHECK(decoded.qualifier == states[i].qualifier);
        CHECK(decoded.analysisVersionId == states[i].analysisVersionId);
        CHECK(decoded.validate());
    }

    // The measured state really did carry an exact value across.
    const auto measured = Traits::fromJson(encoded[0]);
    REQUIRE(measured.value.hasValue());
    CHECK((*measured.value).numerator == 12);
    CHECK((*measured.value).denominator == 5);
}

TEST_CASE("The four result states survive a journal line round trip", "[lims][result][journal]") {
    using Traits = morph::model::ActionTraits<lims::CaptureConcentration>;
    const auto states = allFourStates(lims::AnalysisVersionId{7});

    morph::journal::InMemoryActionLog log;
    for (const auto& state : states) {
        morph::journal::LogEntry entry;
        entry.modelType = "SampleModel";
        entry.entityKey = "1";
        entry.actionType = std::string{Traits::typeId()};
        entry.payload = Traits::toJson(state);
        log.append(entry);
    }

    const auto entries = log.entries("1");
    REQUIRE(entries.size() == 4);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        // Through the journal's own line codec, not just through the vector:
        // a journal is only an audit trail if it survives being written out
        // and read back.
        const auto line = morph::journal::toJson(entries[i]);
        const auto reread = morph::journal::fromJson(line);
        const auto decoded = Traits::fromJson(reread.payload);
        INFO("state " << i << " line: " << line);
        CHECK(decoded.value == states[i].value);
        CHECK(decoded.qualifier == states[i].qualifier);
    }
}

TEST_CASE("The four result states survive the offline queue's opaque payload", "[lims][result][offline]") {
    using Traits = morph::model::ActionTraits<lims::CaptureConcentration>;
    const auto states = allFourStates(lims::AnalysisVersionId{7});

    // `QueueItem::payload` is an opaque `std::string` the queue never
    // interprets — the same contract `SqliteOfflineQueue` implements durably
    // (which needs -DMORPH_BUILD_OFFLINE_SQLITE, off in this build). What is
    // under test here is the payload contract, not the storage medium.
    morph::offline::InMemoryOfflineQueue queue;
    for (const auto& state : states) {
        static_cast<void>(queue.enqueue(Traits::toJson(state)));
    }

    const auto items = queue.drain();
    REQUIRE(items.size() == 4);
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto decoded = Traits::fromJson(items[i].payload);
        INFO("state " << i << " payload: " << items[i].payload);
        CHECK(decoded.value == states[i].value);
        CHECK(decoded.qualifier == states[i].qualifier);
    }
}

// ── Capture through the model ──────────────────────────────────────────────

TEST_CASE("A measured result stores its exact value against its analysis version", "[lims][result]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel model;

    const auto nitrate = defineNitrate(catalog);
    const auto sample = sampleAtWork(model);

    const auto stored = model.execute(lims::CaptureConcentration{.analysisVersionId = nitrate.versionId,
                                                                 .value = lims::Concentration{exact(12, 5, 3)}});

    CHECK(stored.qualifier == lims::ResultQualifier::Measured);
    CHECK(stored.analysisVersionId == nitrate.versionId);
    CHECK(stored.sampleId == sample.id);
    CHECK(stored.capturedBy == "alice");
    REQUIRE(stored.value.hasValue());
    CHECK((*stored.value).numerator == 12);
    CHECK((*stored.value).denominator == 5);

    const auto listed = model.execute(lims::ListResults{});
    REQUIRE(listed.results.size() == 1);
    CHECK(listed.results[0].value == stored.value);
    CHECK(listed.results[0].qualifier == lims::ResultQualifier::Measured);
}

TEST_CASE("The three no-number results are stored and read back distinguishably", "[lims][result]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel model;

    // Three analyses, so all three claims coexist on one sample.
    const auto lead =
        catalog.execute(lims::DefineAnalysis{.name = "Lead", .canonicalUnit = "mg_per_L", .decimalPlaces = 3});
    const auto mercury =
        catalog.execute(lims::DefineAnalysis{.name = "Mercury", .canonicalUnit = "mg_per_L", .decimalPlaces = 3});
    const auto cadmium =
        catalog.execute(lims::DefineAnalysis{.name = "Cadmium", .canonicalUnit = "mg_per_L", .decimalPlaces = 3});
    sampleAtWork(model);

    model.execute(
        lims::CaptureConcentration{.analysisVersionId = lead.versionId,
                                   .qualifier = lims::QualifierChoice{std::string{lims::kQualifierNotMeasured}}});
    model.execute(
        lims::CaptureConcentration{.analysisVersionId = mercury.versionId,
                                   .qualifier = lims::QualifierChoice{std::string{lims::kQualifierBelowLod}}});
    model.execute(
        lims::CaptureConcentration{.analysisVersionId = cadmium.versionId,
                                   .qualifier = lims::QualifierChoice{std::string{lims::kQualifierAboveUdl}}});

    const auto listed = model.execute(lims::ListResults{});
    REQUIRE(listed.results.size() == 3);

    std::vector<lims::ResultQualifier> qualifiers;
    for (const auto& row : listed.results) {
        CHECK_FALSE(row.value.hasValue());  // none of the three carries a number
        qualifiers.push_back(row.qualifier);
    }
    CHECK(std::count(qualifiers.begin(), qualifiers.end(), lims::ResultQualifier::NotMeasured) == 1);
    CHECK(std::count(qualifiers.begin(), qualifiers.end(), lims::ResultQualifier::BelowLOD) == 1);
    CHECK(std::count(qualifiers.begin(), qualifiers.end(), lims::ResultQualifier::AboveUDL) == 1);
}

TEST_CASE("An over-precise reading is rejected, not silently retagged", "[lims][result][precision]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel model;

    const auto nitrate = defineNitrate(catalog);  // 3 decimal places
    sampleAtWork(model);

    // 1.23456 needs five decimals. morph's own `x-decimalPlaces` enforcement
    // *retags* the precision tag without changing the value (issue #159), so
    // accepting this would store 1.23456 while every display of it read
    // 1.235. Storage disagreeing with display is disqualifying in a LIMS, so
    // this rung rejects the payload instead.
    CHECK_THROWS_AS(model.execute(lims::CaptureConcentration{.analysisVersionId = nitrate.versionId,
                                                             .value = lims::Concentration{exact(123456, 100000, 5)}}),
                    lims::ValidationError);
    CHECK(model.execute(lims::ListResults{}).results.empty());

    // A value that *is* exact at three decimals goes through: 1.234 = 617/500.
    const auto ok = model.execute(lims::CaptureConcentration{.analysisVersionId = nitrate.versionId,
                                                             .value = lims::Concentration{exact(617, 500, 3)}});
    REQUIRE(ok.value.hasValue());
    CHECK((*ok.value).numerator == 617);
    CHECK((*ok.value).denominator == 500);
}

TEST_CASE("An unknown qualifier code is refused by the model too", "[lims][result]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel model;

    const auto nitrate = defineNitrate(catalog);
    sampleAtWork(model);

    CHECK_THROWS_AS(model.execute(lims::CaptureConcentration{
                        .analysisVersionId = nitrate.versionId,
                        .qualifier = lims::QualifierChoice{std::string{"belowDetectionLimit"}}}),
                    lims::ValidationError);
    CHECK(model.execute(lims::ListResults{}).results.empty());
}

TEST_CASE("A concentration action cannot be captured against a non-concentration analysis", "[lims][result][units]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel model;

    // The action's unit is a compile-time template parameter, so the only way
    // it can be wrong is if the *definition* is denominated in something else.
    const auto current =
        catalog.execute(lims::DefineAnalysis{.name = "Probe current", .canonicalUnit = "A", .decimalPlaces = 3});
    sampleAtWork(model);

    CHECK_THROWS_AS(model.execute(lims::CaptureConcentration{.analysisVersionId = current.versionId,
                                                             .value = lims::Concentration{exact(3, 2, 3)}}),
                    lims::ValidationError);
    CHECK(model.execute(lims::ListResults{}).results.empty());
}

TEST_CASE("Results can only be captured while a sample is in progress", "[lims][result][lifecycle]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel model;

    const auto nitrate = defineNitrate(catalog);
    const auto client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    model.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-2"});

    const lims::CaptureConcentration capture{.analysisVersionId = nitrate.versionId,
                                             .value = lims::Concentration{exact(12, 5, 3)}};

    // Registered: not received yet, so there is nothing to measure.
    CHECK_THROWS_AS(model.execute(capture), lims::IllegalTransition);

    model.execute(lims::ReceiveSample{});
    model.execute(lims::StartWork{});
    model.execute(capture);
    model.execute(lims::SubmitForVerification{});

    // Awaiting verification: the numbers under review must not move.
    CHECK_THROWS_AS(model.execute(capture), lims::IllegalTransition);

    model.execute(lims::ReturnForRework{.reason = "recount"});
    model.execute(capture);
    model.execute(lims::SubmitForVerification{});
    model.execute(lims::PublishSample{});

    // Published: a released report that quietly accepted a new result is the
    // regulatory failure this whole rung is about.
    CHECK_THROWS_AS(model.execute(capture), lims::IllegalTransition);
}

TEST_CASE("Capturing a result moves the sample's base version", "[lims][result][offline]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel model;

    const auto nitrate = defineNitrate(catalog);
    const auto atWork = sampleAtWork(model);
    const auto before = *atWork.version;

    model.execute(lims::CaptureConcentration{.analysisVersionId = nitrate.versionId,
                                             .value = lims::Concentration{exact(12, 5, 3)}});

    // A result changes what the sample says, so an offline update prepared
    // against the older version is stale — the same reason a transition bumps
    // it.
    CHECK(*model.execute(lims::GetSample{}).version == before + 1);
}

TEST_CASE("Re-capturing one analysis version replaces its answer, and the journal keeps both",
          "[lims][result][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel model;
    model.attachActionLog(log, std::string{});

    const auto nitrate = defineNitrate(catalog);
    const auto sample = sampleAtWork(model);

    model.execute(lims::CaptureConcentration{.analysisVersionId = nitrate.versionId,
                                             .value = lims::Concentration{exact(12, 5, 3)}});
    model.execute(lims::CaptureConcentration{.analysisVersionId = nitrate.versionId,
                                             .value = lims::Concentration{exact(13, 5, 3)}});

    const auto listed = model.execute(lims::ListResults{});
    REQUIRE(listed.results.size() == 1);
    CHECK((*listed.results[0].value).numerator == 13);

    // The superseded reading is not gone; it is in the journal, which is where
    // a 21 CFR-style trail expects to find it.
    std::vector<std::string> captures;
    for (const auto& entry : log->entries(std::to_string(*sample.id))) {
        if (entry.actionType == "CaptureConcentration") {
            captures.push_back(entry.payload);
        }
    }
    REQUIRE(captures.size() == 2);
    CHECK(captures[0] != captures[1]);
    CHECK(captures[0].find("\"num\":12") != std::string::npos);
    CHECK(captures[1].find("\"num\":13") != std::string::npos);
}

TEST_CASE("A result stays bound to the version it was captured under", "[lims][result][versioning]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel model;

    const auto v1 = defineNitrate(catalog);
    sampleAtWork(model);
    model.execute(
        lims::CaptureConcentration{.analysisVersionId = v1.versionId, .value = lims::Concentration{exact(12, 5, 3)}});

    // The definition moves on — narrowed spec range, same unit.
    const auto v2 = catalog.execute(lims::ReviseAnalysis{
        .analysisId = v1.analysisId,
        .canonicalUnit = "mg_per_L",
        .decimalPlaces = 3,
        .specHigh = lims::AnalysisBound{.numerator = 10, .denominator = 1},
    });
    REQUIRE(v2.versionId != v1.versionId);

    // The stored result still names v1, so it can still be rendered against
    // the bounds it was captured under.
    const auto listed = model.execute(lims::ListResults{});
    REQUIRE(listed.results.size() == 1);
    CHECK(listed.results[0].analysisVersionId == v1.versionId);

    const auto asCaptured = catalog.execute(lims::GetAnalysisVersion{.versionId = v1.versionId});
    CHECK(asCaptured.version == 1);
    CHECK_FALSE(asCaptured.specHigh.has_value());
}

TEST_CASE("Capturing with no principal is refused and journals nothing", "[lims][result][audit]") {
    DbFixture fixture;
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel model;

    lims::AnalysisVersionId versionId;
    lims::SampleId sampleId;
    {
        const ScopedPrincipal alice{"alice"};
        versionId = defineNitrate(catalog).versionId;
        sampleId = sampleAtWork(model).id;
    }

    lims::SampleModel anonymous;
    anonymous.attachActionLog(log, std::string{});
    anonymous.execute(lims::OpenSample{.sampleId = sampleId});
    CHECK_THROWS_AS(anonymous.execute(lims::CaptureConcentration{.analysisVersionId = versionId,
                                                                 .value = lims::Concentration{exact(12, 5, 3)}}),
                    lims::EmptyPrincipalError);
    CHECK(log->entries().empty());
}

TEST_CASE("The qualifier picklist is served, and matches the codes the model accepts", "[lims][result][forms]") {
    DbFixture fixture;
    lims::SampleModel model;

    const auto options = model.execute(lims::ListResultQualifiers{});
    REQUIRE(options.qualifiers.size() == 3);
    for (const auto& option : options.qualifiers) {
        INFO("served option " << option.id);
        // Every served code decodes; nothing the renderer can offer is a code
        // the server would then reject.
        CHECK(lims::qualifierFromCode(option.id).has_value());
        CHECK_FALSE(option.name.empty());
    }
}

TEST_CASE("Listing or capturing against an unattached handler is NotFound", "[lims][result]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel model;

    const auto nitrate = defineNitrate(catalog);
    CHECK_THROWS_AS(model.execute(lims::ListResults{}), lims::NotFound);
    CHECK_THROWS_AS(model.execute(lims::CaptureConcentration{.analysisVersionId = nitrate.versionId,
                                                             .value = lims::Concentration{exact(12, 5, 3)}}),
                    lims::NotFound);
}

TEST_CASE("Capturing against an analysis version that does not exist is NotFound", "[lims][result]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::SampleModel model;

    sampleAtWork(model);
    CHECK_THROWS_AS(model.execute(lims::CaptureConcentration{.analysisVersionId = lims::AnalysisVersionId{424242},
                                                             .value = lims::Concentration{exact(12, 5, 3)}}),
                    lims::NotFound);
}
