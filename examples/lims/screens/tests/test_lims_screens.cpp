// SPDX-License-Identifier: Apache-2.0
//
// lims' screens on the screen harness (spec 4 §7): every screen validates, the lifecycle screen follows its sample
// through the transitions its state allows, and the results screen opens on its sample. The models run in process
// over the rung's test database, as the presenter tests they replace did.

#include <app/app_source.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <morph/ui/value.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>

#include "lims/auth/lims_authorizer.hpp"
#include "lims/dto/analysis_dto.hpp"
#include "lims/dto/result_dto.hpp"
#include "lims/dto/sample_dto.hpp"
#include "lims/models/analysis_catalog_model.hpp"
#include "lims/models/auth_model.hpp"
#include "lims/models/sample_model.hpp"
#include "lims_screens.hpp"
#include "lims_test_support.hpp"

namespace {

using morph::examples::LocalApp;
using morph::examples::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ladder::testkit::DbFixture;
using morph::ui::ParamMap;
using morph::ui::Value;

LocalApp limsApp() { return LocalApp{.name = "lims", .bundle = &lims::screens::bundle, .setUp = {}}; }

/// A registered sample, through the model directly, as the bench would have created it.
std::int64_t registeredSample() {
    lims::test::ScopedPrincipal const principal{"tech"};
    lims::SampleModel model;
    auto const client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    auto const sample = model.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-1"});
    return *sample.id;
}

ParamMap sampleParams(std::int64_t sampleId) { return ParamMap{{"sampleId", Value{sampleId}}}; }

}  // namespace

TEST_CASE("lims screens: every screen validates, and the bundle builds", "[lims-screens]") {
    auto const bundle = lims::screens::bundle();
    INFO((bundle ? std::string{} : bundle.error().message()));
    REQUIRE(bundle.has_value());
}

// Mutations: show the sample before opening it (the header is empty); enable every transition whatever the state
// (start is enabled while the sample is only registered).
TEST_CASE("lims screens: the lifecycle screen opens its sample and allows the transition its state allows",
          "[lims-screens]") {
    DbFixture fixture;
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
    DbFixture fixture;
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
    DbFixture fixture;
    ScreenHarness harness{
        limsApp(), std::string{lims::screens::kRegister}, RigMode::Local, {}, LocalSetup{.db = {}, .user = "tech"}};
    harness.edit("clientName", "Waterworks Ltd");
    harness.click("registerClient");
    REQUIRE(harness.waitFor([&harness] {
        auto const text = harness.text("clientId");
        return text && text->starts_with("Client ");
    }));
    harness.edit("reference", "WW-7");
    harness.click("registerSample");
    REQUIRE(harness.waitFor([&harness] { return harness.mounted().size() == 2; }));
    CHECK(harness.mounted().back() == lims::screens::kLifecycle);
    REQUIRE(harness.waitFor([&harness] {
        auto const header = harness.text("header");
        return header && header->ends_with("Registered (v1)");
    }));
}

// Mutation: show nothing for a refused open (the error text stays empty).
TEST_CASE("lims screens: a model's refusal reaches the screen as its own message", "[lims-screens]") {
    DbFixture fixture;
    ScreenHarness harness{limsApp(), std::string{lims::screens::kLifecycle}, RigMode::Local, sampleParams(424242),
                          LocalSetup{.db = {}, .user = "tech"}};
    REQUIRE(harness.waitFor([&harness] {
        auto const error = harness.text("error");
        return error && !error->empty();
    }));
    CHECK(harness.text("error")->contains("424242"));
}

// Mutation: list a result's value as a double (12.5, not 12.500).
TEST_CASE("lims screens: a captured result is listed with its exact decimal", "[lims-screens]") {
    DbFixture fixture;
    std::int64_t sampleId = 0;
    {
        lims::test::ScopedPrincipal const principal{"tech"};
        lims::AnalysisCatalogModel catalog;
        auto const nitrate =
            catalog.execute(lims::DefineAnalysis{.name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3});
        lims::SampleModel model;
        auto const client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
        auto const sample = model.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-2"});
        model.execute(lims::ReceiveSample{});
        model.execute(lims::StartWork{});
        model.execute(lims::CaptureConcentration{
            .analysisVersionId = nitrate.versionId,
            .value = lims::Concentration{morph::math::Rational{morph::math::Numerator{25}, morph::math::Denominator{2},
                                                               morph::math::DecimalPlaces{3}}}});
        sampleId = *sample.id;
    }
    ScreenHarness harness{limsApp(), std::string{lims::screens::kResults}, RigMode::Local, sampleParams(sampleId),
                          LocalSetup{.db = {}, .user = "tech"}};
    bool const listed = harness.waitFor([&harness] {
        for (int const widget : harness.backend().all("Text")) {
            if (harness.backend().prop(widget, "text").starts_with("12.500")) {
                return true;
            }
        }
        return false;
    });
    if (!listed) {
        for (int const widget : harness.backend().all("Text")) {
            UNSCOPED_INFO("text: " << harness.backend().prop(widget, "text"));
        }
    }
    CHECK(listed);
}

// Mutation: keep the login screen non-public (nothing is listed before sign-in), or skip the token (registration's
// calls are refused).
TEST_CASE("lims screens: signing in to a server opens sample registration", "[lims-screens]") {
    DbFixture fixture;
    LocalApp app = limsApp();
    app.setUp = [](LocalSetup const&) { lims::screens::prepareLocal(); };
    // The secret prepareLocal's issuer signs with, which the simulated server verifies, with lims' own role lookup.
    app.serverAuthorizer = [] {
        return std::make_shared<lims::auth::LimsAuthorizer>(
            "lims-local-development", morph::session::hmacSha256,
            [](std::string_view principal) { return lims::SampleModel::rolesOf(principal); });
    };
    ScreenHarness harness{app, std::string{lims::screens::kLogin}, RigMode::Simulated, {}, LocalSetup{}};
    harness.edit("username", "tech");
    harness.click("signIn");
    bool const opened = harness.waitFor(
        [&harness] { return !harness.mounted().empty() && harness.mounted().back() == lims::screens::kRegister; });
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
    INFO("error: " << harness.text("error").value_or(""));
    CHECK(opened);
    CHECK(harness.problems().empty());
}

// Mutations: keep the model's product out of the computed field (the preview stays empty), or ignore the dilution in
// it (the diluted reading previews undiluted).
TEST_CASE("lims screens: the results screen previews the reported concentration while it is typed", "[lims-screens]") {
    DbFixture fixture;
    std::int64_t sampleId = 0;
    {
        lims::test::ScopedPrincipal const principal{"tech"};
        lims::SampleModel model;
        auto const client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
        auto const sample = model.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-3"});
        model.execute(lims::ReceiveSample{});
        model.execute(lims::StartWork{});
        sampleId = *sample.id;
    }
    ScreenHarness harness{limsApp(), std::string{lims::screens::kResults}, RigMode::Local, sampleParams(sampleId),
                          LocalSetup{.db = {}, .user = "tech"}};
    REQUIRE(harness.waitFor([&harness] { return harness.text("analyses").has_value(); }));
    // The preview waits out its debounce on the harness's manual clock, so the clock moves while it is awaited.
    auto const previews = [&harness](std::string_view expected) {
        return harness.waitFor([&harness, expected] {
            harness.advance(std::chrono::milliseconds{300});
            return harness.text("reported") == expected;
        });
    };
    harness.editField("capture", "value", "1.25");
    bool const neat = previews("Reported 1.250 mg/L");
    INFO("reported: " << harness.text("reported").value_or("<none>"));
    CHECK(neat);

    auto const dilution = harness.findIn("capture", "Select", "field_dilution");
    REQUIRE(dilution);
    REQUIRE(harness.waitFor(
        [&harness, &dilution] { return harness.backend().prop(*dilution, "options").contains("diluted"); }));
    harness.chooseField("capture", "dilution", R"("diluted")");
    // The factor is shown only once the choice reaches the form, and a hidden field takes no typing.
    auto const factor = harness.findIn("capture", "TextInput", "field_dilutionFactor");
    auto const form = harness.find("Column", "capture");
    REQUIRE(factor);
    REQUIRE(form);
    REQUIRE(harness.waitFor([&] { return harness.shownUnder(*form, *factor); }));
    harness.editField("capture", "dilutionFactor", "4");
    bool const diluted = previews("Reported 5.000 mg/L");
    INFO("reported: " << harness.text("reported").value_or("<none>"));
    CHECK(diluted);
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
    CHECK(harness.problems().empty());
}
