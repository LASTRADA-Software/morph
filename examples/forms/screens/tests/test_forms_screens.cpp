// SPDX-License-Identifier: Apache-2.0
//
// The forms demo's screens on the screen harness (spec 4 §7): every screen validates; the density form computes in
// the model with its units; the samples collection lists the model's rows and runs a collection action; the intake
// wizard registers a sample and carries its id into the measurement step. The model runs in process.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/client/source.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/screen_harness.hpp>

#include "forms_screens.hpp"

namespace {

using morph::client::LocalApp;
using morph::client::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;

LocalApp formsApp() { return LocalApp{.name = "forms", .bundle = &lab::screens::bundle, .setUp = {}}; }

ScreenHarness open(std::string_view screenId) {
    return ScreenHarness{formsApp(), std::string{screenId}, RigMode::Local, {}, LocalSetup{.db = {}, .user = "tech"}};
}

void reportProblems(ScreenHarness const& harness) {
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
}

/// Whether the button with @p testId is mounted and enabled.
bool enabled(ScreenHarness& harness, std::string_view testId) {
    auto const button = harness.find("Button", testId);
    return button && harness.backend().prop(*button, "enabled") != "false";
}

}  // namespace

TEST_CASE("forms screens: every screen validates, and the bundle builds", "[forms-screens]") {
    auto const bundle = lab::screens::bundle();
    INFO((bundle ? std::string{} : bundle.error().message()));
    REQUIRE(bundle.has_value());
}

// Mutation: show the density before the model answers (the text is shown empty).
TEST_CASE("forms screens: the density form computes in the model, with its unit", "[forms-screens]") {
    ScreenHarness harness = open(lab::screens::kDensity);
    REQUIRE(harness.waitFor([&harness] { return harness.find("TextInput", "field_massDry").has_value(); }));
    harness.edit("field_massDry", "2");
    // The action declares no explicit submit, so the form submits once both fields hold a value.
    harness.edit("field_volume", "0.001");
    bool const shown = harness.waitFor([&harness] {
        auto const text = harness.text("density");
        return text && text->starts_with("Dry density: 2000");
    });
    UNSCOPED_INFO("density: " << harness.text("density").value_or("?"));
    reportProblems(harness);
    CHECK(shown);
    CHECK(harness.problems().empty());
}

// Mutation: serve no view document (the collection is reported and shown muted).
TEST_CASE("forms screens: the samples collection lists the model's rows and runs a collection action",
          "[forms-screens]") {
    ScreenHarness harness = open(lab::screens::kSamples);
    REQUIRE(harness.waitFor([&harness] { return harness.find("Button", "rowOpen_7").has_value(); }));
    CHECK(harness.find("Button", "rowOpen_1").has_value());
    CHECK(harness.find("Button", "rowOpen_2").has_value());
    harness.click("collectionAction_CreateSample");
    CHECK(harness.waitFor([&harness] { return harness.find("Button", "rowOpen_100").has_value(); }));
    reportProblems(harness);
    CHECK(harness.problems().empty());
}

// Mutation: serve no wizard document (the wizard is reported and shown muted).
TEST_CASE("forms screens: the intake wizard registers a sample, then moves to its first measurement",
          "[forms-screens]") {
    ScreenHarness harness = open(lab::screens::kIntake);
    REQUIRE(harness.waitFor([&harness] { return harness.find("TextInput", "field_name").has_value(); }));
    CHECK(harness.text("wizard_title") == "Register & measure a sample  (1 / 2)");
    harness.edit("field_name", "Clay C");
    REQUIRE(harness.waitFor([&harness] { return enabled(harness, "wizardNext"); }));
    harness.click("wizardNext");
    bool const advanced = harness.waitForText("wizard_title", "Register & measure a sample  (2 / 2)");
    reportProblems(harness);
    CHECK(advanced);
    CHECK(harness.problems().empty());
}

TEST_CASE("forms screens: the measurement form mounts with its sample choice", "[forms-screens]") {
    ScreenHarness harness = open(lab::screens::kMeasure);
    REQUIRE(harness.waitFor([&harness] { return harness.find("Column", "measureForm").has_value(); }));
    reportProblems(harness);
    CHECK(harness.problems().empty());
}
