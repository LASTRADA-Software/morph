// SPDX-License-Identifier: Apache-2.0
//
// ReportJobPoller's own suite. The `Dispatch` closure is injected, so these
// cases drive the poller's state machine directly with no bridge, no
// database, and no real report job -- which is the point of the closure
// shape it borrowed from EventPoller.

#include "report_job_poller.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;
using namespace std::chrono_literals;

}  // namespace

TEST_CASE("ReportJobPoller reports Done exactly once and then stops polling", "[ledger][gui][poller]") {
    BackendRig rig{Mode::Local, 1};

    int dispatches = 0;
    int doneCalls = 0;
    int failedCalls = 0;
    std::string body;

    // Pending twice, then Done: proves the poller keeps ticking through
    // Pending and settles on the first terminal answer.
    ledger::gui::ReportJobPoller poller{
        rig.bridge(0), ledger::ReportJobId{1},
        [&](ledger::ReportJobId, ledger::gui::ReportJobPoller::OnSuccess onSuccess,
            ledger::gui::ReportJobPoller::OnError) {
            ++dispatches;
            ledger::GetReportStatusResult result;
            if (dispatches < 3) {
                result.status = ledger::ReportStatus::Pending;
            } else {
                result.status = ledger::ReportStatus::Done;
                result.result = std::string{R"([{"currency":"USD"}])"};
            }
            onSuccess(std::move(result));
        },
        [&](std::string resultJson) {
            ++doneCalls;
            body = std::move(resultJson);
        },
        [&](const QString&) { ++failedCalls; }, /*interval=*/10ms};

    REQUIRE(pumpUntil([&] { return doneCalls > 0; }));
    CHECK(doneCalls == 1);
    CHECK(failedCalls == 0);
    CHECK(body == R"([{"currency":"USD"}])");
    CHECK(poller.finished());

    // Keep pumping well past several intervals: a disarmed poller must not
    // dispatch again, and must not deliver a second terminal callback.
    const auto dispatchesAtDone = dispatches;
    REQUIRE_FALSE(pumpUntil([] { return false; }, 120ms));
    CHECK(dispatches == dispatchesAtDone);
    CHECK(doneCalls == 1);
}

TEST_CASE("ReportJobPoller reports Failed once and disarms", "[ledger][gui][poller]") {
    BackendRig rig{Mode::Local, 1};

    int doneCalls = 0;
    int failedCalls = 0;
    ledger::gui::ReportJobPoller poller{
        rig.bridge(0), ledger::ReportJobId{2},
        [&](ledger::ReportJobId, ledger::gui::ReportJobPoller::OnSuccess onSuccess,
            ledger::gui::ReportJobPoller::OnError) {
            ledger::GetReportStatusResult result;
            result.status = ledger::ReportStatus::Failed;
            onSuccess(std::move(result));
        },
        [&](std::string) { ++doneCalls; }, [&](const QString&) { ++failedCalls; }, /*interval=*/10ms};

    REQUIRE(pumpUntil([&] { return failedCalls > 0; }));
    CHECK(failedCalls == 1);
    CHECK(doneCalls == 0);
    CHECK(poller.finished());

    REQUIRE_FALSE(pumpUntil([] { return false; }, 120ms));
    CHECK(failedCalls == 1);
}

TEST_CASE("ReportJobPoller treats a dispatch error as terminal, not a retry loop",
          "[ledger][gui][poller]") {
    // Retrying a failing call forever is how a "stuck at Pending" bug hides:
    // the UI spins and nothing ever surfaces. One error ends the poll and the
    // message reaches the caller, which can then decide to resubmit.
    BackendRig rig{Mode::Local, 1};

    int dispatches = 0;
    int failedCalls = 0;
    QString message;
    ledger::gui::ReportJobPoller poller{
        rig.bridge(0), ledger::ReportJobId{3},
        [&](ledger::ReportJobId, ledger::gui::ReportJobPoller::OnSuccess,
            ledger::gui::ReportJobPoller::OnError onError) {
            ++dispatches;
            onError(std::make_exception_ptr(std::runtime_error{"transport is down"}));
        },
        [&](std::string) {}, [&](const QString& msg) {
            ++failedCalls;
            message = msg;
        },
        /*interval=*/10ms};

    REQUIRE(pumpUntil([&] { return failedCalls > 0; }));
    CHECK(failedCalls == 1);
    CHECK(message.contains("transport is down"));
    CHECK(poller.finished());

    const auto dispatchesAtFailure = dispatches;
    REQUIRE_FALSE(pumpUntil([] { return false; }, 120ms));
    CHECK(dispatches == dispatchesAtFailure);
}
