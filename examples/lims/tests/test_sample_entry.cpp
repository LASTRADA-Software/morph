// SPDX-License-Identifier: Apache-2.0
//
// Sample entry: a private working copy edited section by section, committed only by a save; the save's typed
// refusals; the dilution series the model computes; the project lookup; and deleting a registration.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/action_guard.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/table/query.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "lims/lab/db/lims_entity.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/org/core/errors.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/storage_owner.hpp"

using lims::test::ScopedPrincipal;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

/// @brief An exact concentration of @p num / @p den mg/L.
/// @param num The numerator.
/// @param den The denominator.
/// @return The concentration.
lims::Concentration mgL(std::int64_t num, std::int64_t den = 1) {
    return lims::Concentration{Rational{Numerator{num}, Denominator{den}, DecimalPlaces{3}}};
}

/// @brief An exact recovery of @p num / @p den percent.
/// @param num The numerator.
/// @param den The denominator.
/// @return The recovery.
Rational pct(std::int64_t num, std::int64_t den = 1) {
    return Rational{Numerator{num}, Denominator{den}, DecimalPlaces{1}};
}

/// @brief Starts a working copy with a complete header for a new client, filed under project 2.
/// @param model The model.
void completeCopy(lims::SampleModel& model) {
    auto const client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    model.execute(lims::NewSample{});
    model.execute(lims::HeaderSection{.clientId = client.clientId,
                                      .reference = "WW-0001",
                                      .receivedOn = 20'000,
                                      .remarks = "cold chain",
                                      .siteId = lims::test::seededSite()});
    model.execute(lims::SelectProject{.projectId = 2});
}

}  // namespace

// Mutation: store the working copy on every section action (the second model loads the edited reference).
TEST_CASE("sample entry: sections edit the working copy and only a save stores it", "[lims][sample-entry]") {
    lims::test::LabFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::SampleModel model;
    completeCopy(model);
    model.execute(
        lims::TurbiditySection{.ntu = lims::Turbidity{Rational{Numerator{3}, Denominator{2}, DecimalPlaces{2}}}});

    auto const saved = model.execute(lims::SaveSample{});
    REQUIRE(saved.error == lims::SaveError::none);
    REQUIRE(saved.sampleId.hasValue());
    CHECK(model.execute(lims::GetHeader{}).sampleNumber == "S-" + std::to_string(*saved.sampleId));

    // An edit after the save stays in this instance's working copy.
    auto const header = model.execute(lims::GetHeader{});
    model.execute(
        lims::HeaderSection{.clientId = header.clientId, .reference = "WW-0002", .receivedOn = 20'000, .remarks = {}});
    CHECK(model.execute(lims::GetHeader{}).reference == "WW-0002");

    lims::SampleModel other;
    auto const loaded = other.execute(lims::LoadSample{.id = saved.sampleId});
    CHECK(loaded.reference == "WW-0001");
    CHECK(loaded.remarks == "cold chain");
    CHECK(loaded.receivedOn == 20'000);
    CHECK(loaded.projectId == 2);
    CHECK(loaded.projectName == "River monitoring");
    auto const measurements = other.execute(lims::GetMeasurements{});
    CHECK(measurements.hasTurbidity);
    CHECK_FALSE(measurements.hasNitrate);
    REQUIRE(measurements.ntu.hasValue());
    CHECK(*measurements.ntu == Rational{Numerator{3}, Denominator{2}, DecimalPlaces{2}});
}

// Mutation: accept a copy without a project (the last refusal reads `none` and a sample is stored).
TEST_CASE("sample entry: a save refuses an incomplete copy with a typed reason and stores nothing",
          "[lims][sample-entry]") {
    lims::test::LabFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::SampleModel model;
    CHECK(model.execute(lims::SaveSample{}).error == lims::SaveError::noWorkingCopy);

    auto const client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    model.execute(lims::NewSample{});
    CHECK(model.execute(lims::SaveSample{}).error == lims::SaveError::noClient);
    model.execute(lims::HeaderSection{.clientId = client.clientId, .reference = {}, .receivedOn = 0, .remarks = {}});
    CHECK(model.execute(lims::SaveSample{}).error == lims::SaveError::noReference);
    model.execute(
        lims::HeaderSection{.clientId = client.clientId, .reference = "WW-0001", .receivedOn = 0, .remarks = {}});
    auto const refused = model.execute(lims::SaveSample{});
    CHECK(refused.error == lims::SaveError::noProject);
    CHECK_FALSE(refused.sampleId.hasValue());
    CHECK_FALSE(model.execute(lims::GetHeader{}).sampleId.hasValue());

    lims::SampleModel other;
    CHECK_THROWS_AS(other.execute(lims::LoadSample{.id = lims::SampleId{1}}), lims::NotFound);
}

// Mutation: compute the nominal as spike × factor (every nominal after the first is wrong).
TEST_CASE("sample entry: the model computes the dilution series and its limits exactly", "[lims][sample-entry]") {
    lims::test::LabFixture fixture;
    lims::SampleModel model;
    model.execute(lims::NewSample{});
    CHECK(model.execute(lims::GetDilutionSeries{}).rows.empty());

    model.execute(lims::SamplingSection{
        .spike = mgL(10), .measured1 = mgL(10), .measured2 = mgL(11, 2), .measured3 = mgL(2), .measured4 = {}});
    auto const series = model.execute(lims::GetDilutionSeries{});
    REQUIRE(series.rows.size() == 4);
    std::vector<Rational> nominal;
    nominal.reserve(series.rows.size());
    for (auto const& row : series.rows) {
        nominal.push_back(*row.nominal);
    }
    CHECK(nominal == std::vector<Rational>{*mgL(10), *mgL(5), *mgL(5, 2), *mgL(5, 4)});
    CHECK(*series.rows.at(0).recoveryPct == pct(100));
    CHECK(*series.rows.at(1).recoveryPct == pct(110));
    CHECK(*series.rows.at(2).recoveryPct == pct(80));
    CHECK_FALSE(series.rows.at(0).outsideLimits);
    CHECK_FALSE(series.rows.at(2).outsideLimits);  // the lower limit is inside
    CHECK_FALSE(series.rows.at(3).recoveryPct.hasValue());
    CHECK_FALSE(series.rows.at(3).outsideLimits);  // not measured is not outside

    model.execute(lims::SamplingSection{
        .spike = mgL(10), .measured1 = mgL(121, 10), .measured2 = {}, .measured3 = {}, .measured4 = mgL(9, 10)});
    auto const outside = model.execute(lims::GetDilutionSeries{});
    CHECK(*outside.rows.at(0).recoveryPct == pct(121));
    CHECK(outside.rows.at(0).outsideLimits);
    CHECK(*outside.rows.at(3).recoveryPct == pct(72));
    CHECK(outside.rows.at(3).outsideLimits);
}

// Mutation: clear both readings for either name (the nitrate reading is gone after clearing turbidity).
TEST_CASE("sample entry: clearing an optional measurement removes only its reading", "[lims][sample-entry]") {
    lims::test::LabFixture fixture;
    lims::SampleModel model;
    model.execute(lims::NewSample{});
    model.execute(lims::TurbiditySection{.ntu = lims::Turbidity{Rational{2, DecimalPlaces{2}}}});
    model.execute(lims::NitrateSection{.nitrate = mgL(12)});
    auto const cleared = model.execute(lims::ClearOptionalMeasurement{.which = "turbidity"});
    CHECK_FALSE(cleared.hasTurbidity);
    CHECK(cleared.hasNitrate);
    CHECK_THROWS_AS(model.execute(lims::ClearOptionalMeasurement{.which = "colour"}), lims::ValidationError);
}

TEST_CASE("sample entry: a section without a working copy is refused", "[lims][sample-entry]") {
    lims::test::LabFixture fixture;
    lims::SampleModel model;
    CHECK_THROWS_AS(model.execute(lims::GetHeader{}), lims::NotFound);
    CHECK_THROWS_AS(model.execute(lims::NitrateSection{.nitrate = mgL(1)}), lims::NotFound);
    model.execute(lims::NewSample{});
    CHECK_THROWS_AS(model.execute(lims::SelectProject{.projectId = 99}), lims::NotFound);
}

// Mutation: ignore the table query (the filter returns every project).
TEST_CASE("sample entry: the project lookup is sorted, filtered and paged by the table engine",
          "[lims][sample-entry]") {
    lims::test::LabFixture fixture;
    lims::SampleModel model;
    lims::ListProjects request;
    request.query.sort.push_back(
        morph::table::SortKey{.column = "code", .dir = morph::table::SortDirection::Descending});
    auto const sorted = model.execute(request);
    REQUIRE(sorted.rows.size() == 5);
    CHECK(sorted.rows.front().code == "WAT-24");

    lims::ListProjects filtered;
    filtered.query.filters.columns["name"].include.push_back(
        morph::table::FilterEntry{.startsWith = std::string{"Ri"}});
    auto const page = model.execute(filtered);
    REQUIRE(page.rows.size() == 1);
    CHECK(page.rows.front().id == 2);
}

// Mutation: delete without the state check (the received sample is deleted).
TEST_CASE("sample entry: deleting withdraws a registration and refuses a received sample", "[lims][sample-entry]") {
    lims::test::LabFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::SampleModel model;
    completeCopy(model);
    auto const first = model.execute(lims::SaveSample{});
    REQUIRE(first.error == lims::SaveError::none);
    CHECK(model.execute(lims::DeleteSample{}).sampleId == first.sampleId);
    CHECK_THROWS_AS(model.execute(lims::GetHeader{}), lims::NotFound);
    lims::SampleModel reader;
    CHECK_THROWS_AS(reader.execute(lims::LoadSample{.id = first.sampleId}), lims::NotFound);

    completeCopy(model);
    auto const second = model.execute(lims::SaveSample{});
    REQUIRE(second.error == lims::SaveError::none);
    model.execute(lims::ReceiveSample{});
    CHECK_THROWS_AS(::morph::model::executeChecked(model, lims::DeleteSample{}), lims::IllegalTransition);
    CHECK(reader.execute(lims::LoadSample{.id = second.sampleId}).reference == "WW-0001");
}

// Mutations: drop the id check from LoadSample; let DeleteSample run on a working copy that was never saved.
TEST_CASE("sample entry: loading names a sample, and a never-saved working copy cannot be deleted",
          "[lims][sample-entry]") {
    lims::test::LabFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Analyst);
    ScopedPrincipal const alice{"alice"};
    lims::SampleModel model;
    CHECK_THROWS_AS(model.execute(lims::LoadSample{}), lims::ValidationError);
    CHECK_THROWS_AS(::morph::model::executeChecked(model, lims::DeleteSample{}), lims::NotFound);
}

// Mutations: skip the client lookup in SaveSample (a header naming a client that does not exist is stored); clear
// turbidity whichever optional measurement is named.
TEST_CASE("sample entry: a save names a client that exists, and each optional measurement clears on its own",
          "[lims][sample-entry]") {
    lims::test::LabFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Analyst);
    ScopedPrincipal const alice{"alice"};
    lims::SampleModel model;
    completeCopy(model);
    model.execute(lims::HeaderSection{.clientId = lims::ClientId{9999},
                                      .reference = "WW-0001",
                                      .receivedOn = 20'000,
                                      .remarks = {},
                                      .siteId = lims::test::seededSite()});
    CHECK_THROWS_AS(::morph::model::executeChecked(model, lims::SaveSample{}), lims::NotFound);

    model.execute(
        lims::TurbiditySection{.ntu = lims::Turbidity{Rational{Numerator{3}, Denominator{2}, DecimalPlaces{2}}}});
    model.execute(lims::NitrateSection{
        .nitrate = lims::Concentration{Rational{Numerator{3}, Denominator{2}, DecimalPlaces{2}}}});
    auto const cleared = model.execute(lims::ClearOptionalMeasurement{.which = std::string{lims::kNitrate}});
    CHECK_FALSE(cleared.hasNitrate);
    CHECK(cleared.hasTurbidity);
}

// Mutation: DeleteSample reads the stored sample without checking that the working copy was ever saved.
TEST_CASE("sample entry: the delete handler refuses a never-saved working copy on its own", "[lims][sample-entry]") {
    lims::test::LabFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::SampleModel model;
    completeCopy(model);
    CHECK_THROWS_AS(model.execute(lims::DeleteSample{}), lims::NotFound);
    // The draft survives the refusal.
    CHECK(model.execute(lims::GetHeader{}).reference == "WW-0001");
}

// Mutation: let the entry scopes of a handler with no working copy name a site.
TEST_CASE("sample entry: a handler with no working copy writes to no scope", "[lims][sample-entry]") {
    lims::SampleModel model;
    CHECK(model.entryScopes().empty());
}

// Mutation: ignore a stored entry that does not decode (the sample loads with an empty working copy).
TEST_CASE("sample entry: a stored entry that does not decode is refused on load", "[lims][sample-entry]") {
    lims::test::LabFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::SampleModel model;
    completeCopy(model);
    auto const saved = model.execute(lims::SaveSample{});
    REQUIRE(saved.error == lims::SaveError::none);
    {
        Lightweight::DataMapper mapper;
        auto entries = mapper.Query<lims::db::SampleEntryRecord>().All();
        REQUIRE(entries.size() == 1U);
        entries.front().payload = Lightweight::SqlDynamicAnsiString<4096>{std::string{"not json"}};
        mapper.Update(entries.front());
    }
    lims::SampleModel other;
    CHECK_THROWS_AS(other.execute(lims::LoadSample{.id = saved.sampleId}), lims::ValidationError);
}

// Mutation: journal a refused save before checking that the working copy belongs to a stored sample (a never-saved
// draft gains a trail), or never journal it (a stored sample's trail lacks the refusal).
TEST_CASE("sample entry: a refused rewrite of a stored sample is journaled in its trail, a never-saved draft's is not",
          "[lims][sample-entry]") {
    lims::test::LabFixture fixture;
    ScopedPrincipal const alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>(morph::ladder::testkit::storageOwner());
    lims::SampleModel model;
    model.attachActionLog(log, std::string{});

    // A never-saved draft naming a client that does not exist: refused, and no trail to put it in.
    completeCopy(model);
    model.execute(lims::HeaderSection{.clientId = lims::ClientId{9999},
                                      .reference = "WW-0001",
                                      .receivedOn = 20'000,
                                      .remarks = {},
                                      .siteId = lims::test::seededSite()});
    auto const before = log->entries().size();
    CHECK_THROWS_AS(model.execute(lims::SaveSample{}), lims::NotFound);
    CHECK(log->entries().size() == before);

    // The same refusal on a stored sample is its trail's last step.
    auto const client = model.execute(lims::RegisterClient{.name = "Second Ltd"});
    model.execute(lims::HeaderSection{.clientId = client.clientId,
                                      .reference = "WW-0002",
                                      .receivedOn = 20'000,
                                      .remarks = {},
                                      .siteId = lims::test::seededSite()});
    auto const saved = model.execute(lims::SaveSample{});
    REQUIRE(saved.error == lims::SaveError::none);
    auto const key = std::to_string(*saved.sampleId);
    model.execute(lims::HeaderSection{.clientId = lims::ClientId{9999},
                                      .reference = "WW-0002",
                                      .receivedOn = 20'000,
                                      .remarks = {},
                                      .siteId = lims::test::seededSite()});
    auto const trailBefore = log->entries(key).size();
    CHECK_THROWS_AS(model.execute(lims::SaveSample{}), lims::NotFound);
    auto const trail = log->entries(key);
    REQUIRE(trail.size() == trailBefore + 1U);
    CHECK(trail.back().outcome == morph::journal::Outcome::Failed);
    CHECK(trail.back().actionType == "SaveSample");
}
