// SPDX-License-Identifier: Apache-2.0
//
// polls' screens on the screen harness (spec 4 §7): every screen validates; the vote screen opens its poll by id,
// shows its options, and sends `SubmitVotes` for the first vote and `UpdateVotes` after it, with the activity feed
// following. The model runs in process over the rung's test database.

#include <app/app_source.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/ui/value.hpp>
#include <string>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>

#include "polls/dto/poll_dto.hpp"
#include "polls/models/poll_model.hpp"
#include "polls_screens.hpp"

namespace {

using morph::examples::LocalApp;
using morph::examples::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ladder::testkit::DbFixture;
using morph::ui::ParamMap;
using morph::ui::Value;

LocalApp pollsApp() { return LocalApp{.name = "polls", .bundle = &polls::screens::bundle, .setUp = {}}; }

void reportProblems(ScreenHarness const& harness) {
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
}

/// Whether some `Text` on the screen contains @p needle.
bool shows(ScreenHarness& harness, std::string const& needle) {
    for (int const widget : harness.backend().all("Text")) {
        if (harness.backend().prop(widget, "text").contains(needle)) {
            return true;
        }
    }
    return false;
}

/// Whether the form container with @p testId is mounted and not hidden.
bool formShown(ScreenHarness& harness, std::string const& testId) {
    auto const form = harness.find("Column", testId);
    return form && harness.backend().prop(*form, "visible") != "false";
}

}  // namespace

TEST_CASE("polls screens: every screen validates, and the bundle builds", "[polls-screens]") {
    auto const bundle = polls::screens::bundle();
    INFO((bundle ? std::string{} : bundle.error().message()));
    REQUIRE(bundle.has_value());
}

// Mutation: keep sending SubmitVotes after a vote succeeded (the update form never shows).
TEST_CASE("polls screens: the first vote is SubmitVotes and the next is UpdateVotes, and the feed follows",
          "[polls-screens]") {
    DbFixture fixture;
    polls::PollModel model;
    auto const created = model.execute(polls::CreatePoll{.title = "Lunch", .options = {{"Pizza"}, {"Sushi"}}});
    ScreenHarness harness{pollsApp(), std::string{polls::screens::kVote}, RigMode::Local,
                          ParamMap{{"pollId", Value{created.pollId}}}, LocalSetup{.db = {}, .user = "alice"}};
    REQUIRE(harness.waitForText("title", "Lunch"));
    REQUIRE(harness.waitFor([&harness] { return shows(harness, "Sushi"); }));
    CHECK(formShown(harness, "voteForm"));
    CHECK_FALSE(formShown(harness, "updateForm"));

    harness.edit("participant", "alice");
    REQUIRE(harness.submitForm("voteForm"));
    bool const updated = harness.waitFor([&harness] { return formShown(harness, "updateForm"); });
    reportProblems(harness);
    CHECK(updated);
    CHECK_FALSE(formShown(harness, "voteForm"));
    CHECK(harness.waitFor([&harness] { return shows(harness, "alice"); }));

    REQUIRE(harness.submitForm("updateForm"));
    CHECK(harness.waitFor([&harness] { return harness.text("error").value_or("x").empty(); }));
    reportProblems(harness);
    CHECK(harness.problems().empty());
}
