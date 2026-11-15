// SPDX-License-Identifier: Apache-2.0
//
// lims' screens on the screen harness (spec 4 §7): every screen validates, the lifecycle screen follows its sample
// through the transitions its state allows, and the results screen opens on its sample. The models run in process
// over the rung's test database, as the presenter tests they replace did.

#include <algorithm>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <morph/client/source.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/session/session_auth.hpp>
#include <morph/ui/value.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/bundle_warnings.hpp>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>
#include <testkit/storage_owner.hpp>
#include <vector>

#include "lims/app/lims_authorizer.hpp"
#include "lims/lab/dto/analysis_dto.hpp"
#include "lims/lab/dto/result_dto.hpp"
#include "lims/lab/dto/sample_dto.hpp"
#include "lims/lab/models/analysis_catalog_model.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/org/models/auth_model.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_screens.hpp"
#include "lims_test_support.hpp"

namespace {

using morph::client::LocalApp;
using morph::client::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ui::ParamMap;
using morph::ui::Value;

LocalApp limsApp() { return LocalApp{.name = "lims", .bundle = &lims::screens::bundle, .setUp = {}}; }

/// A registered sample, through the model directly, as the bench would have created it.
std::int64_t registeredSample() {
    lims::test::ScopedPrincipal const principal{"tech"};
    lims::SampleModel model;
    auto const client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    auto const sample = model.execute(
        lims::RegisterSample{.siteId = lims::test::seededSite(), .clientId = client.clientId, .reference = "WW-1"});
    return *sample.id;
}

ParamMap sampleParams(std::int64_t sampleId) { return ParamMap{{"sampleId", Value{sampleId}}}; }

}  // namespace

TEST_CASE("lims screens: every screen validates, and the bundle builds", "[lims-screens]") {
    auto const bundle = lims::screens::bundle();
    INFO((bundle ? std::string{} : bundle.error().message()));
    REQUIRE(bundle.has_value());
}

// Every document loads against the catalog the server serves with no refusal and no warning: each mutation's action
// declares its effects, or `none`.
TEST_CASE("lims_bundle_loads_without_warnings", "[lims-screens]") {
    auto const bundle = lims::screens::bundle();
    INFO((bundle ? std::string{} : bundle.error().message()));
    REQUIRE(bundle.has_value());
    std::vector<std::string> const findings = morph::examples::testing::bundleLoadFindings(**bundle);
    for (std::string const& finding : findings) {
        UNSCOPED_INFO(finding);
    }
    CHECK(findings.empty());
}

// Mutations: show the sample before opening it (the header is empty); enable every transition whatever the state
// (start is enabled while the sample is only registered).
TEST_CASE("lims screens: the lifecycle screen opens its sample and allows the transition its state allows",
          "[lims-screens]") {
    lims::test::LabFixture fixture;
    std::int64_t const sampleId = registeredSample();
    ScreenHarness harness{limsApp(), std::string{lims::screens::kLifecycle}, RigMode::Local, sampleParams(sampleId),
                          LocalSetup{.db = {}, .user = "tech"}};
    std::string const header = "sample " + std::to_string(sampleId);
    REQUIRE(harness.waitForText("header", header + " — Registered (v1)"));
    auto const enabled = [&harness](std::string_view testId) {
        return harness.backend().prop(*harness.backend().find("Button", "testId", testId), "enabled") != "false";
    };
    CHECK(enabled("receive"));
    CHECK_FALSE(enabled("start"));
    CHECK_FALSE(enabled("submit"));
    CHECK_FALSE(enabled("publish"));

    harness.click("receive");
    REQUIRE(harness.waitForText("header", header + " — Received (v2)"));
    CHECK_FALSE(enabled("receive"));
    CHECK(enabled("start"));

    harness.click("start");
    REQUIRE(harness.waitForText("header", header + " — InProgress (v3)"));
    CHECK(enabled("submit"));
    CHECK(harness.problems().empty());
}

TEST_CASE("lims screens: the results screen opens on its sample", "[lims-screens]") {
    lims::test::LabFixture fixture;
    std::int64_t const sampleId = registeredSample();
    ScreenHarness harness{limsApp(), std::string{lims::screens::kResults}, RigMode::Local, sampleParams(sampleId),
                          LocalSetup{.db = {}, .user = "tech"}};
    REQUIRE(harness.waitFor([&harness] { return harness.text("analyses").has_value(); }));
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
    CHECK(harness.problems().empty());
    CHECK(harness.waitForText("conflicts", "0 conflicts"));
}

// Mutation: navigate nowhere after a sample registers (the lifecycle screen never mounts).
TEST_CASE("lims screens: registration registers a client and a sample, then opens the sample's lifecycle",
          "[lims-screens]") {
    lims::test::LabFixture fixture;
    lims::test::grant("tech", lims::LimsRole::Analyst);
    ScreenHarness harness{
        limsApp(), std::string{lims::screens::kRegister}, RigMode::Local, {}, LocalSetup{.db = {}, .user = "tech"}};
    harness.edit("clientName", "Waterworks Ltd");
    harness.click("registerClient");
    REQUIRE(harness.waitFor([&harness] {
        auto const text = harness.text("clientId");
        return text && text->starts_with("Client ");
    }));
    int const sites = harness.find("Table", "sites").value();
    REQUIRE(harness.waitFor([&harness, sites] {
        auto const rows = harness.backend().children(sites);
        return std::ranges::any_of(rows, [&harness](int row) { return harness.backend().hasProp(row, "rowKey"); });
    }));
    // A client alone is not enough: the button waits for a site too.
    auto const registerEnabled = [&harness] {
        return harness.backend().prop(*harness.backend().find("Button", "testId", "registerSample"), "enabled") !=
               "false";
    };
    CHECK_FALSE(registerEnabled());
    harness.backend().selectRows(sites, {morph::ui::Key{std::int64_t{1}}});
    REQUIRE(harness.waitFor(registerEnabled));
    harness.edit("reference", "WW-7");
    harness.click("registerSample");
    REQUIRE(harness.waitFor([&harness] { return harness.mounted().size() == 2; }));
    CHECK(harness.mounted().back() == lims::screens::kLifecycle);
    REQUIRE(harness.waitFor([&harness] {
        auto const header = harness.text("header");
        return header && header->ends_with("Registered (v1)");
    }));
}
