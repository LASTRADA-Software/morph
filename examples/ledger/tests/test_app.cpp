// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <morph/session/session.hpp>
#include <string>

#include "ledger/app/app.hpp"
#include "ledger/core/types.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/ledger_model.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::pumpUntil;

namespace {

/// @brief A `Context` carrying only @p principal.
///
/// Built field-by-field rather than with a designated initializer on purpose:
/// `-Weverything` includes `-Wmissing-designated-field-initializers`, which
/// fires on a partial designated-initializer list, and `ladder_ledger_tests`
/// is built with `apply_warnings()` (so `-Werror` under
/// `MORPH_ENABLE_STRICT_COMPILATION`, CI's default).
[[nodiscard]] morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

class ScopedPrincipal {
public:
    explicit ScopedPrincipal(std::string principal) : _ctx{contextFor(std::move(principal))}, _scope{_ctx} {}

private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

/// @brief An hour-long runner interval: the timer is effectively disabled and
///        the pass is driven by hand, so nothing here depends on wall-clock
///        timing.
constexpr std::chrono::hours kTimerOff{1};

/// @brief A runner interval short enough that a handful of pumped event-loop
///        slices are certain to contain several ticks of it.
///
/// Used only by the two `stopBackgroundJobs()` cases. The pair is
/// deliberately asymmetric: the *control* case waits for a tick to arrive
/// (bounded by `pumpUntil`'s own `MORPH_LADDER_DEADLINE_MS`-scaled deadline,
/// so a slow runner cannot fail it), while the case under test waits for one
/// that must never arrive.
constexpr std::chrono::milliseconds kFastRunInterval{20};

/// @brief Creates a ledger with one account and returns its id.
/// @param mapper The mapper to create the ledger row through.
/// @param model The model to open the account on.
/// @return The new ledger's id.
[[nodiscard]] ledger::LedgerId makeLedger(Lightweight::DataMapper& mapper, ledger::LedgerModel& model) {
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};
    const ScopedPrincipal alice{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});
    return ledgerId;
}

/// @brief Submits a report for @p ledgerId as an ordinary user would.
/// @param model The model to submit through.
/// @param ledgerId The ledger to report on.
/// @return The new job's id.
[[nodiscard]] ledger::ReportJobId submitReport(ledger::LedgerModel& model, const ledger::LedgerId& ledgerId) {
    const ScopedPrincipal alice{"alice"};
    return model.execute(
        ledger::SubmitReport{.ledgerId = ledgerId, .kind = ledger::ReportKind::MonthlyStatement, .params = "{}"});
}

}  // namespace

TEST_CASE("App::runPendingReportsOnce settles a job the model only wrote a row for", "[ledger][app]") {
    // The whole of morph#160 in one case: the model accepts the submission
    // and does nothing else -- no executor, no thread -- and the App layer is
    // what turns the Pending row into a computed report, by dispatching
    // RunReportJob back at the model on its own strand.
    //
    // `App` reaches the same database this test does because both go through
    // Lightweight's process-global default connection string, which
    // `DbFixture` (constructed first) already set.
    DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::LedgerModel model;
    const auto ledgerId = makeLedger(mapper, model);
    const auto jobId = submitReport(model, ledgerId);
    REQUIRE(jobId.hasValue());
    REQUIRE(model.execute(ledger::GetReportStatus{.jobId = jobId}).status == ledger::ReportStatus::Pending);

    {
        ledger::app::App app{kTimerOff};
        app.runPendingReportsOnce();
        REQUIRE(pumpUntil([&app] { return !app.reportsInFlight(); }));

        const auto status = model.execute(ledger::GetReportStatus{.jobId = jobId});
        CHECK(status.status == ledger::ReportStatus::Done);
        CHECK(status.result.has_value());
    }
}

TEST_CASE("A job submitted before the App existed is picked up by its first pass", "[ledger][app]") {
    // What a restart looks like from the job's point of view. The process
    // that accepted the submission is gone (here: there never was one -- the
    // App is constructed only afterwards), and the job is still there,
    // because the job *is* the row. Under the model-owned ThreadPoolExecutor
    // this replaced, the same job was a lambda in a queue that died with its
    // process and could not be recovered by anything.
    DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::LedgerModel model;
    const auto ledgerId = makeLedger(mapper, model);

    ledger::ReportJobId first;
    ledger::ReportJobId second;
    {
        // A first "process": submits two jobs, runs neither, and goes away.
        ledger::app::App accepting{kTimerOff};
        first = submitReport(model, ledgerId);
        second = submitReport(model, ledgerId);
        REQUIRE(first.hasValue());
        REQUIRE(second.hasValue());
        REQUIRE(*first != *second);
    }
    CHECK(model.execute(ledger::GetReportStatus{.jobId = first}).status == ledger::ReportStatus::Pending);
    CHECK(model.execute(ledger::GetReportStatus{.jobId = second}).status == ledger::ReportStatus::Pending);

    {
        // A second "process": one pass, and both survivors settle.
        ledger::app::App resuming{kTimerOff};
        resuming.runPendingReportsOnce();
        REQUIRE(pumpUntil([&resuming] { return !resuming.reportsInFlight(); }));
    }
    CHECK(model.execute(ledger::GetReportStatus{.jobId = first}).status == ledger::ReportStatus::Done);
    CHECK(model.execute(ledger::GetReportStatus{.jobId = second}).status == ledger::ReportStatus::Done);
}

TEST_CASE("App::runPendingReportsOnce with nothing pending dispatches nothing", "[ledger][app]") {
    // The negative half: a pass over an empty queue must not raise the
    // in-flight counter at all, or every drain would wait on work that does
    // not exist. Asserted synchronously, before any pumping, so a dispatch
    // that *was* issued cannot have settled first and hidden itself.
    DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::LedgerModel model;
    const auto ledgerId = makeLedger(mapper, model);
    const auto jobId = submitReport(model, ledgerId);

    ledger::app::App app{kTimerOff};
    app.runPendingReportsOnce();
    REQUIRE(app.reportsInFlight());
    REQUIRE(pumpUntil([&app] { return !app.reportsInFlight(); }));
    REQUIRE(model.execute(ledger::GetReportStatus{.jobId = jobId}).status == ledger::ReportStatus::Done);

    // Nothing is Pending any more, so this pass finds nothing to do.
    app.runPendingReportsOnce();
    CHECK_FALSE(app.reportsInFlight());
}

TEST_CASE("The App's report timer settles a job with no pass driven by hand", "[ledger][app]") {
    // The control for the stopBackgroundJobs() case below: with a live timer
    // and nobody calling runPendingReportsOnce(), the job still settles --
    // which is what makes "it did not settle" mean something down there.
    DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::LedgerModel model;
    const auto ledgerId = makeLedger(mapper, model);
    const auto jobId = submitReport(model, ledgerId);

    ledger::app::App app{kFastRunInterval};
    REQUIRE(pumpUntil(
        [&] { return model.execute(ledger::GetReportStatus{.jobId = jobId}).status == ledger::ReportStatus::Done; }));
    app.stopBackgroundJobs();
    REQUIRE(pumpUntil([&app] { return !app.reportsInFlight(); }));
}

TEST_CASE("stopBackgroundJobs stops the report runner for good", "[ledger][app]") {
    // Why this matters beyond tidiness: the settle contract on
    // reportsInFlight() is "pump until false, then destroy", and pumping is
    // exactly what lets the timer tick. A shutdown that drained without
    // stopping first could dispatch a brand-new pass out of its own
    // processEvents() call and destroy the App with that pass outstanding.
    DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::LedgerModel model;
    const auto ledgerId = makeLedger(mapper, model);

    ledger::app::App app{kFastRunInterval};
    app.stopBackgroundJobs();

    // Submitted only after the timer is stopped, so the job is one no pass
    // has ever seen.
    const auto jobId = submitReport(model, ledgerId);
    // A deliberately long wait for a tick that must never come. Inverted:
    // pumpUntil returning false is the pass.
    CHECK_FALSE(pumpUntil(
        [&] { return model.execute(ledger::GetReportStatus{.jobId = jobId}).status != ledger::ReportStatus::Pending; },
        std::chrono::milliseconds{400}));
    CHECK_FALSE(app.reportsInFlight());

    // Still runnable by hand -- the timer stopped, not the runner.
    app.runPendingReportsOnce();
    REQUIRE(pumpUntil([&app] { return !app.reportsInFlight(); }));
    CHECK(model.execute(ledger::GetReportStatus{.jobId = jobId}).status == ledger::ReportStatus::Done);
}

TEST_CASE("The App runs a job for every ledger, not only the first", "[ledger][app]") {
    // RunReportJob keys on its ledgerId, so two ledgers' jobs run on two
    // different strands. A pass that dispatched only one of them, or keyed
    // them onto one instance, would show up here and nowhere else.
    DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::LedgerModel model;
    const auto firstLedger = makeLedger(mapper, model);
    const auto secondLedger = makeLedger(mapper, model);
    REQUIRE(*firstLedger != *secondLedger);
    const auto firstJob = submitReport(model, firstLedger);
    const auto secondJob = submitReport(model, secondLedger);

    ledger::app::App app{kTimerOff};
    app.runPendingReportsOnce();
    REQUIRE(pumpUntil([&app] { return !app.reportsInFlight(); }));

    CHECK(model.execute(ledger::GetReportStatus{.jobId = firstJob}).status == ledger::ReportStatus::Done);
    CHECK(model.execute(ledger::GetReportStatus{.jobId = secondJob}).status == ledger::ReportStatus::Done);
}
