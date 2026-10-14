// SPDX-License-Identifier: Apache-2.0
//
// ledger's screen on the screen harness (spec 4 §7): the screen validates; a signed-in person creates a ledger and
// requests a statement; the job is polled until it is done, and until it fails, each outcome shown. The test runs the
// job as the server's report runner does, under the runner's principal.

#include <app/app_source.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <morph/session/session.hpp>
#include <string>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>

#include "ledger/core/types.hpp"
#include "ledger/dto/report_dto.hpp"
#include "ledger/models/ledger_model.hpp"
#include "ledger_screens.hpp"

namespace {

using morph::examples::LocalApp;
using morph::examples::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ladder::testkit::DbFixture;

LocalApp ledgerApp() {
    return LocalApp{.name = "ledger", .bundle = &ledger::screens::bundle, .setUp = [](LocalSetup const&) {
                        ledger::screens::prepareLocal();
                    }};
}

ScreenHarness open() {
    return ScreenHarness{
        ledgerApp(), std::string{ledger::screens::kLedger}, RigMode::Local, {}, LocalSetup{.db = {}, .user = {}}};
}

void reportProblems(ScreenHarness const& harness) {
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
}

/// The number after @p prefix in the text with @p testId.
std::int64_t numberIn(ScreenHarness& harness, std::string const& testId, std::string const& prefix) {
    std::string const text = harness.text(testId).value_or("");
    REQUIRE(text.starts_with(prefix));
    return std::stoll(text.substr(prefix.size()));
}

/// Waits for the job's status to end with @p suffix, advancing the clock so the status query polls.
bool waitForJob(ScreenHarness& harness, std::string const& suffix) {
    return harness.waitFor([&harness, &suffix] {
        harness.advance(std::chrono::milliseconds{200});
        return harness.text("jobStatus").value_or("").ends_with(suffix);
    });
}

/// Signs in when needed, creates a ledger named @p name, and answers its id.
std::int64_t createLedger(ScreenHarness& harness, std::string const& name) {
    if (harness.find("TextInput", "username")) {
        harness.edit("username", "alice");
        harness.click("signIn");
        // Typing into the ledger form before the sign-in lands would be lost with it.
        REQUIRE(harness.waitFor([&harness] {
            return harness.backend().prop(harness.find("Row", "signInRow").value(), "visible") == "false";
        }));
    }
    std::string const before = harness.text("ledger").value_or("");
    harness.editField("ledgerForm", "name", name);
    REQUIRE(harness.submitForm("ledgerForm"));
    REQUIRE(harness.waitFor([&harness, &before] {
        auto const shown = harness.text("ledger");
        return shown && shown->starts_with("Ledger ") && *shown != before;
    }));
    return numberIn(harness, "ledger", "Ledger ");
}

/// Runs a report job as the server's report runner does.
void runAsRunner(std::int64_t jobId, std::int64_t ledgerId) {
    morph::session::Context context;
    context.principal = std::string{ledger::kReportRunnerPrincipal};
    morph::session::detail::ScopedContext const scope{context};
    ledger::LedgerModel model;
    try {
        static_cast<void>(model.execute(
            ledger::RunReportJob{.jobId = ledger::ReportJobId{jobId}, .ledgerId = ledger::LedgerId{ledgerId}}));
    } catch (std::exception const& error) {
        // A refused run is settled Failed, which the screen is expected to show.
        UNSCOPED_INFO("the runner's run was refused: " << error.what());
    }
}

}  // namespace

TEST_CASE("ledger screens: the screen validates, and the bundle builds", "[ledger-screens]") {
    auto const bundle = ledger::screens::bundle();
    INFO((bundle ? std::string{} : bundle.error().message()));
    REQUIRE(bundle.has_value());
}

// Mutation: show the status once and never poll (it stays Pending).
TEST_CASE("ledger screens: the statement job is polled until it is done", "[ledger-screens]") {
    DbFixture fixture;
    ScreenHarness harness = open();
    std::int64_t const ledgerId = createLedger(harness, "Household");
    harness.click("requestStatement");
    REQUIRE(harness.waitFor([&harness] { return harness.text("jobStatus").value_or("").ends_with(": Pending"); }));
    runAsRunner(numberIn(harness, "jobStatus", "Statement "), ledgerId);
    bool const done = waitForJob(harness, ": Done");
    reportProblems(harness);
    // Polling stopped: the gate closed with the job.
    CHECK(harness.backend().prop(harness.find("Text", "polling").value(), "visible") == "false");
    CHECK(done);
    CHECK(harness.problems().empty());
}

// Mutation: stop polling on Done only (a failed job is polled forever, and the gate never closes).
TEST_CASE("ledger screens: a statement job run against another ledger is polled until it fails", "[ledger-screens]") {
    DbFixture fixture;
    ScreenHarness harness = open();
    std::int64_t const first = createLedger(harness, "First");
    harness.click("requestStatement");
    REQUIRE(harness.waitFor([&harness] { return harness.text("jobStatus").value_or("").ends_with(": Pending"); }));
    std::int64_t const second = createLedger(harness, "Second");
    REQUIRE(second != first);
    runAsRunner(numberIn(harness, "jobStatus", "Statement "), second);
    bool const failed = waitForJob(harness, ": Failed");
    reportProblems(harness);
    CHECK(harness.backend().prop(harness.find("Text", "polling").value(), "visible") == "false");
    CHECK(failed);
}
