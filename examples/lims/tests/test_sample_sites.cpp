// SPDX-License-Identifier: Apache-2.0
//
// Every sample is registered at a site (lims/lab over lims/org), and every sample that existed before sites did is
// at the seeded one.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <string_view>

#include "lims/lab/db/lims_entity.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/db/org_entity.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "migration_probe.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

// A named namespace: Lightweight reflects records, which needs types with linkage.
namespace lims_site_migration_fixtures {

/// @brief `lims_clients` as the last migration before org wrote it.
struct LegacyClientRecord {
    static constexpr std::string_view TableName =
        "lims_clients";  // NOLINT(readability-identifier-naming): the ORM reads this spelling
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    Light::Field<Light::SqlAnsiString<128>, Light::SqlRealName{"name"}> name;
};

/// @brief `lims_samples` as the last migration before org wrote it: no `site_id`.
struct LegacySampleRecord {
    static constexpr std::string_view TableName =
        "lims_samples";  // NOLINT(readability-identifier-naming): the ORM reads this spelling
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    Light::Field<std::int64_t, Light::SqlRealName{"client_id"}> clientId{0};
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"reference"}> reference;
    Light::Field<int, Light::SqlRealName{"state"}> state{0};
    Light::Field<std::int32_t, Light::SqlRealName{"version"}> version{1};
    Light::Field<std::int64_t, Light::SqlRealName{"registered_at"}> registeredAt{0};
};

}  // namespace lims_site_migration_fixtures

// Mutation: store no site (the view reads back an empty site).
TEST_CASE("lab: a sample is registered at a site, and its view names the site", "[lims][org][sites]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    auto const fieldLab = org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"});

    lims::SampleModel bench;
    auto const client = bench.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    auto const sample =
        bench.execute(lims::RegisterSample{.siteId = fieldLab.id, .clientId = client.clientId, .reference = "WW-1"});
    CHECK(sample.siteId == fieldLab.id);

    lims::SampleModel reader;
    CHECK(reader.execute(lims::OpenSample{.sampleId = sample.id}).siteId == fieldLab.id);
}

// Mutation: drop the site's requirement (a sample is registered at site 999, where nobody holds a role).
TEST_CASE("lab: registering at no site, or at one that does not exist, is refused", "[lims][org][sites]") {
    lims::test::LabFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::SampleModel bench;
    auto const client = bench.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    CHECK_THROWS_AS(
        bench.execute(lims::RegisterSample{.siteId = {}, .clientId = client.clientId, .reference = "WW-1"}),
        lims::ValidationError);
    CHECK_THROWS_AS(bench.execute(lims::RegisterSample{
                        .siteId = lims::SiteId{999}, .clientId = client.clientId, .reference = "WW-1"}),
                    lims::Forbidden);
}

// Mutation: skip the noSite check (the save without a site stores a sample).
TEST_CASE("lab: sample entry stores the header's site and refuses a header without one", "[lims][org][sites]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::SampleModel model;
    auto const client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    model.execute(lims::NewSample{});
    model.execute(
        lims::HeaderSection{.clientId = client.clientId, .reference = "WW-1", .receivedOn = 0, .remarks = {}});
    model.execute(lims::SelectProject{.projectId = 2});
    auto const refused = model.execute(lims::SaveSample{});
    CHECK(refused.error == lims::SaveError::noSite);
    CHECK_FALSE(refused.sampleId.hasValue());

    model.execute(lims::HeaderSection{.clientId = client.clientId,
                                      .reference = "WW-1",
                                      .receivedOn = 0,
                                      .remarks = {},
                                      .siteId = lims::test::seededSite()});
    auto const saved = model.execute(lims::SaveSample{});
    REQUIRE(saved.error == lims::SaveError::none);
    lims::SampleModel other;
    CHECK(other.execute(lims::LoadSample{.id = saved.sampleId}).siteId == lims::test::seededSite());
    CHECK(other.execute(lims::GetSample{}).siteId == lims::test::seededSite());
}

// Mutation: drop the backfill from the lab migration (the legacy sample's site reads back empty).
TEST_CASE("lab: samples registered before sites existed migrate to the seeded site", "[lims][org][sites][migration]") {
    using lims_site_migration_fixtures::LegacyClientRecord;
    using lims_site_migration_fixtures::LegacySampleRecord;
    DbFixture fixture;
    lims::test::MigrationProbe probe{20261107000001};
    {
        Lightweight::DataMapper mapper;
        // The probe really is the old schema: the org tables do not exist yet.
        CHECK_THROWS(mapper.Query<lims::db::CompanyRecord>().All());
        LegacyClientRecord client;
        client.name = Lightweight::SqlAnsiString<128>{"Waterworks Ltd"};
        mapper.Create(client);
        LegacySampleRecord sample;
        sample.clientId = static_cast<std::int64_t>(client.id.Value());
        sample.reference = Lightweight::SqlAnsiString<64>{"WW-OLD"};
        mapper.Create(sample);
    }
    probe.applyRest();
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<lims::db::SampleRecord>().All();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().siteId.Value() == std::optional<std::int64_t>{lims::db::kSeededSiteId});
}

// Mutation: delete the site lookup from SaveSample (a header naming site 999 stores a sample).
TEST_CASE("lab: sample entry refuses a header naming a site that does not exist", "[lims][org][sites]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::SampleModel model;
    auto const client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    model.execute(lims::NewSample{});
    model.execute(lims::HeaderSection{.clientId = client.clientId,
                                      .reference = "WW-1",
                                      .receivedOn = 0,
                                      .remarks = {},
                                      .siteId = lims::SiteId{999}});
    model.execute(lims::SelectProject{.projectId = 2});
    CHECK_THROWS_AS(model.execute(lims::SaveSample{}), lims::NotFound);
}

// Mutation: drop the site assignment from SaveSample's update branch (the moved sample reads back its old site).
TEST_CASE("lab: a saved sample can be moved to another site", "[lims][org][sites]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    auto const fieldLab = org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"});

    lims::SampleModel model;
    auto const client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    model.execute(lims::NewSample{});
    model.execute(lims::HeaderSection{.clientId = client.clientId,
                                      .reference = "WW-1",
                                      .receivedOn = 0,
                                      .remarks = {},
                                      .siteId = lims::test::seededSite()});
    model.execute(lims::SelectProject{.projectId = 2});
    auto const first = model.execute(lims::SaveSample{});
    REQUIRE(first.error == lims::SaveError::none);

    lims::SampleModel editor;
    auto const loaded = editor.execute(lims::LoadSample{.id = first.sampleId});
    REQUIRE(loaded.siteId == lims::test::seededSite());
    editor.execute(lims::HeaderSection{.clientId = loaded.clientId,
                                       .reference = loaded.reference,
                                       .receivedOn = loaded.receivedOn,
                                       .remarks = loaded.remarks,
                                       .siteId = fieldLab.id});
    auto const moved = editor.execute(lims::SaveSample{});
    REQUIRE(moved.error == lims::SaveError::none);
    CHECK(moved.sampleId == first.sampleId);

    lims::SampleModel reader;
    CHECK(reader.execute(lims::LoadSample{.id = first.sampleId}).siteId == fieldLab.id);
}
