// SPDX-License-Identifier: Apache-2.0
//
// lims' sample-entry screen on the screen harness: a new sample is filled section by section, refused with a typed
// reason until it is complete, and saved; a stored one opens by id with the model's dilution series; an optional
// measurement is added and removed; a registration is deleted. The recording backend loads no host component, so the
// series shows through its fallbacks.

#include <app/app_source.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/ui/value.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>
#include <vector>

#include "lims/dto/sample_dto.hpp"
#include "lims/dto/sample_entry_dto.hpp"
#include "lims/models/sample_model.hpp"
#include "lims_screens.hpp"
#include "lims_test_support.hpp"

namespace {

using morph::examples::LocalApp;
using morph::examples::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;
using morph::ui::ParamMap;
using morph::ui::Value;

LocalApp limsApp() { return LocalApp{.name = "lims", .bundle = &lims::screens::bundle, .setUp = {}}; }

lims::Concentration mgL(std::int64_t num, std::int64_t den = 1) {
    return lims::Concentration{Rational{Numerator{num}, Denominator{den}, DecimalPlaces{3}}};
}

/// The widget of @p kind with @p testId under @p root: a screen with several forms repeats their field test ids.
std::optional<int> within(ScreenHarness& harness, int root, std::string_view kind, std::string_view testId) {
    auto const& backend = harness.backend();
    std::vector<int> pending{root};
    while (!pending.empty()) {
        int const widget = pending.back();
        pending.pop_back();
        if (backend.kindOf(widget) == kind && backend.prop(widget, "testId") == testId) {
            return widget;
        }
        for (int const child : backend.children(widget)) {
            pending.push_back(child);
        }
    }
    return std::nullopt;
}

/// The form container with @p testId.
int formOf(ScreenHarness& harness, std::string_view testId) {
    auto const form = harness.find("Column", testId);
    REQUIRE(form.has_value());
    return *form;
}

/// A field of one form on the screen.
struct FormField {
    /// The form's test id.
    std::string_view form;
    /// The field's name.
    std::string name;
};

/// Types @p value into @p field.
void editField(ScreenHarness& harness, FormField const& field, std::string value) {
    auto const input = within(harness, formOf(harness, field.form), "TextInput", "field_" + field.name);
    INFO("form " << field.form << ", field " << field.name);
    REQUIRE(input.has_value());
    harness.backend().edit(*input, std::move(value));
}

/// Submits the form with @p formId.
void submitForm(ScreenHarness& harness, std::string_view formId) {
    auto const submit = within(harness, formOf(harness, formId), "Button", "form_submit");
    INFO("form " << formId);
    REQUIRE(submit.has_value());
    // The button enables once the edits settle into the form.
    REQUIRE(harness.waitFor([&harness, &submit] { return harness.backend().prop(*submit, "enabled") != "false"; }));
    harness.backend().click(*submit);
}

/// Whether the widget with @p testId is mounted and not hidden.
bool shown(ScreenHarness& harness, std::string_view kind, std::string_view testId) {
    auto const widget = harness.find(kind, testId);
    return widget && harness.backend().prop(*widget, "visible") != "false";
}

void reportProblems(ScreenHarness const& harness) {
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
}

struct Stored {
    std::int64_t sampleId = 0;
    std::int64_t clientId = 0;
};

/// A sample saved through the model, with a dilution series whose last step is outside the limits.
Stored storedSample() {
    lims::test::ScopedPrincipal const principal{"tech"};
    lims::SampleModel model;
    auto const client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    model.execute(lims::NewSample{});
    model.execute(
        lims::HeaderSection{.clientId = client.clientId, .reference = "WW-3", .receivedOn = 0, .remarks = {}});
    model.execute(lims::SamplingSection{
        .spike = mgL(10), .measured1 = mgL(10), .measured2 = mgL(5), .measured3 = {}, .measured4 = mgL(9, 10)});
    model.execute(lims::SelectProject{.projectId = 1});
    auto const saved = model.execute(lims::SaveSample{});
    REQUIRE(saved.error == lims::SaveError::none);
    return {.sampleId = *saved.sampleId, .clientId = *client.clientId};
}

std::int64_t registeredClient() {
    lims::test::ScopedPrincipal const principal{"tech"};
    lims::SampleModel model;
    return *model.execute(lims::RegisterClient{.name = "Waterworks Ltd"}).clientId;
}

}  // namespace

// Mutations: treat a refused save as saved (no refusal text); file the sample under no project on activation (the
// save is refused again).
TEST_CASE("lims sample screen: a new sample is refused until complete, then saved", "[lims-screens][sample]") {
    DbFixture fixture;
    std::int64_t const clientId = registeredClient();
    ScreenHarness harness{
        limsApp(), std::string{lims::screens::kSample}, RigMode::Local, {}, LocalSetup{.db = {}, .user = "tech"}};
    REQUIRE(harness.waitForText("title", "New sample"));
    // The header form is prefilled from the working copy: typing before it loads would be overwritten.
    REQUIRE(harness.waitFor([&harness] { return shown(harness, "Text", "client"); }));

    editField(harness, {.form = "headerForm", .name = "clientId"}, std::to_string(clientId));
    editField(harness, {.form = "headerForm", .name = "reference"}, "WW-9");
    submitForm(harness, "headerForm");
    // A section edits the working copy; the header shows it once the submit is answered.
    REQUIRE(harness.waitForText("client", "Client " + std::to_string(clientId)));
    harness.click("save");
    CHECK(harness.waitForText("saveRefusal", "Choose a project before saving"));
    CHECK(shown(harness, "Text", "saveRefusal"));

    harness.click("chooseProject");
    REQUIRE(harness.waitFor([&harness] { return harness.find("Table", "projects").has_value(); }));
    harness.backend().activateRow(*harness.find("Table", "projects"), morph::ui::Key{std::int64_t{2}});
    REQUIRE(harness.waitForText("projectName", "Project: River monitoring"));

    harness.click("save");
    bool const saved = harness.waitFor([&harness] {
        auto const title = harness.text("title");
        return title && title->starts_with("S-");
    });
    reportProblems(harness);
    CHECK(saved);
    CHECK_FALSE(shown(harness, "Text", "saveRefusal"));
    CHECK(harness.problems().empty());
}

// Mutation: count every row as outside the limits (the pill reads 4).
TEST_CASE("lims sample screen: a stored sample opens by id with the model's dilution series",
          "[lims-screens][sample]") {
    DbFixture fixture;
    Stored const stored = storedSample();
    ScreenHarness harness{limsApp(), std::string{lims::screens::kSample}, RigMode::Local,
                          ParamMap{{"sampleId", Value{stored.sampleId}}}, LocalSetup{.db = {}, .user = "tech"}};
    REQUIRE(harness.waitForText("title", "S-" + std::to_string(stored.sampleId)));
    CHECK(harness.waitForText("projectName", "Project: Drinking water 2024"));
    CHECK(harness.waitForText("outside", "1 outside limits"));
    CHECK(harness.waitForText("curveFallback", "Recovery curve over 4 steps"));
    CHECK(harness.find("Table", "seriesFallback").has_value());
    reportProblems(harness);
    CHECK(harness.problems().empty());
}

// Mutation: hide a removed measurement without clearing it (the stored reading keeps the form shown).
TEST_CASE("lims sample screen: an optional measurement is added, entered and removed", "[lims-screens][sample]") {
    DbFixture fixture;
    ScreenHarness harness{
        limsApp(), std::string{lims::screens::kSample}, RigMode::Local, {}, LocalSetup{.db = {}, .user = "tech"}};
    REQUIRE(harness.waitForText("title", "New sample"));
    CHECK_FALSE(shown(harness, "Column", "turbidityForm"));

    harness.click("add-turbidity");
    REQUIRE(harness.waitFor([&harness] { return shown(harness, "Column", "turbidityForm"); }));
    editField(harness, {.form = "turbidityForm", .name = "ntu"}, "1.5");
    submitForm(harness, "turbidityForm");
    REQUIRE(harness.waitFor([&harness] { return shown(harness, "Button", "remove-turbidity"); }));

    harness.click("remove-turbidity");
    CHECK(harness.waitFor([&harness] { return !shown(harness, "Column", "turbidityForm"); }));
    CHECK(shown(harness, "Button", "add-turbidity"));
    reportProblems(harness);
    CHECK(harness.problems().empty());
}

// Mutation: leave the working copy loaded after a delete (the title keeps the sample's number).
TEST_CASE("lims sample screen: deleting a registration starts a new sample", "[lims-screens][sample]") {
    DbFixture fixture;
    Stored const stored = storedSample();
    ScreenHarness harness{limsApp(), std::string{lims::screens::kSample}, RigMode::Local,
                          ParamMap{{"sampleId", Value{stored.sampleId}}}, LocalSetup{.db = {}, .user = "tech"}};
    REQUIRE(harness.waitForText("title", "S-" + std::to_string(stored.sampleId)));
    harness.click("delete");
    CHECK(harness.waitForText("title", "New sample"));
    reportProblems(harness);
    CHECK(harness.problems().empty());
}
