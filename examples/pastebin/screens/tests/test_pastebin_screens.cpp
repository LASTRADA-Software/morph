// SPDX-License-Identifier: Apache-2.0
//
// pastebin's screen on the screen harness (spec 4 §7): the screen validates; a created paste is listed; opening it
// shows its content and consumes one read, and a second open while one is in flight is refused rather than
// consuming another; deleting it removes it from the list. The model runs in process over the rung's test database.

#include <app/app_source.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>

#include "pastebin_screens.hpp"

namespace {

using morph::examples::LocalApp;
using morph::examples::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ladder::testkit::DbFixture;

LocalApp pastebinApp() { return LocalApp{.name = "pastebin", .bundle = &pastebin::screens::bundle, .setUp = {}}; }

ScreenHarness open() {
    return ScreenHarness{pastebinApp(),
                         std::string{pastebin::screens::kPastes},
                         RigMode::Local,
                         {},
                         LocalSetup{.db = {}, .user = "alice"}};
}

void reportProblems(ScreenHarness const& harness) {
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
}

/// Creates a paste through the form and waits for the list to hold one.
void createPaste(ScreenHarness& harness) {
    REQUIRE(harness.waitFor(
        [&harness] { return harness.findIn("createForm", "TextInput", "field_content").has_value(); }));
    harness.editField("createForm", "content", "hello paste");
    harness.editField("createForm", "syntax", "plaintext");
    REQUIRE(harness.submitForm("createForm"));
    REQUIRE(harness.waitForText("count", "1 pastes"));
}

/// The first row's button labelled @p label.
int rowButton(ScreenHarness& harness, std::string const& label) {
    auto const button = harness.backend().find("Button", "label", label);
    REQUIRE(button.has_value());
    return *button;
}

}  // namespace

TEST_CASE("pastebin screens: the screen validates, and the bundle builds", "[pastebin-screens]") {
    auto const bundle = pastebin::screens::bundle();
    INFO((bundle ? std::string{} : bundle.error().message()));
    REQUIRE(bundle.has_value());
}

// Mutation: let a second open run while one is in flight (two reads are consumed).
TEST_CASE("pastebin screens: opening a paste shows it and consumes one read; a second open in flight is refused",
          "[pastebin-screens]") {
    DbFixture fixture;
    ScreenHarness harness = open();
    createPaste(harness);
    int const button = rowButton(harness, "Open");
    harness.backend().click(button);
    harness.backend().click(button);  // in flight: refused
    REQUIRE(harness.waitForText("content", "hello paste"));
    REQUIRE(harness.waitForText("reads", "plaintext, read 1 times"));
    harness.backend().click(button);
    bool const again = harness.waitForText("reads", "plaintext, read 2 times");
    reportProblems(harness);
    CHECK(again);
    CHECK(harness.problems().empty());
}

// Mutation: delete without invalidating the list (the count stays at one).
TEST_CASE("pastebin screens: deleting a paste removes it from the list", "[pastebin-screens]") {
    DbFixture fixture;
    ScreenHarness harness = open();
    createPaste(harness);
    harness.backend().click(rowButton(harness, "Delete"));
    bool const removed = harness.waitForText("count", "0 pastes");
    reportProblems(harness);
    CHECK(removed);
    CHECK(harness.problems().empty());
}
