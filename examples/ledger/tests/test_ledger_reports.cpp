// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/session/session.hpp>
#include <thread>
#include <vector>

#include "ledger/core/errors.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/ledger_model.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/step_executor.hpp"

namespace {

/// @brief A `Context` carrying only @p principal -- same helper
///        `test_ledger_import.cpp`/`test_ledger_model.cpp` use, kept local
///        to this file rather than shared, matching this codebase's existing
///        per-test-file duplication of this exact helper.
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

using morph::ladder::testkit::StepExecutor;

/// @brief Polls @p model's `GetReportStatus` for @p jobId until it leaves
///        `Pending`, or until the hard iteration cap is reached.
///
/// A bounded retry loop with a hard cap (100 x 10ms = 1s). Used by exactly
/// ONE test case below -- the one that deliberately keeps a real
/// `ThreadPoolExecutor` underneath the model (see its own comment). Every
/// other case in this file injects a `StepExecutor` and drives the worker by
/// hand, so it neither sleeps nor guesses at a budget. Do not reach for this
/// helper for a new case without the same explicit justification.
/// @param model The model to poll.
/// @param jobId The submitted job.
/// @return The last status observed -- still `Pending` only if the cap was hit.
[[nodiscard]] ledger::GetReportStatusResult pollUntilSettled(ledger::LedgerModel& model,
                                                             const ledger::ReportJobId& jobId) {
    ledger::GetReportStatusResult status;
    for (int i = 0; i < 100; ++i) {
        status = model.execute(ledger::GetReportStatus{.jobId = jobId});
        if (status.status != ledger::ReportStatus::Pending) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return status;
}

/// @brief Runs the one report job @p worker is holding, asserting on the way
///        through that it really was holding exactly one and that the job
///        posted no follow-up work of its own.
///
/// The `pending() == 1` on entry is the assertion a real pool cannot make:
/// against a `ThreadPoolExecutor` the worker may already have finished by the
/// time the test looks, so "submitted but not yet run" can only be sampled.
/// @param worker The executor injected into the model under test.
void runReportJob(StepExecutor& worker) {
    REQUIRE(worker.pending() == 1);
    REQUIRE(worker.runOne());
    CHECK_FALSE(worker.runOne());
}

}  // namespace

// The one case in this file that deliberately keeps a REAL ThreadPoolExecutor,
// and the reason not everything here was converted to `StepExecutor`. It is
// the only test that exercises the production shape end to end:
//
//   * the default-constructed `LedgerModel` -- the constructor the bridge
//     registry actually uses, and the one that owns the pool. Every converted
//     case below goes through the injecting constructor instead, so without
//     this case nothing covers the default wiring at all.
//   * the worker running on a genuinely different thread. `execute(SubmitReport)`
//     is written on the premise that nothing from the caller's stack frame
//     survives into the task -- not its `DataMapper`, and in particular not
//     `morph::session::current()`, a thread-local. Under `StepExecutor` the
//     task runs inline on the test's thread, where the `ScopedPrincipal` above
//     is still installed: a worker that wrongly reached for the caller's
//     session would pass every converted case and fail only here.
//
// The cost is the retry loop `pollUntilSettled` still carries. Paying it once,
// for the properties only a real thread can show, beats paying it five times.
TEST_CASE("SubmitReport returns immediately; GetReportStatus transitions Pending to Done", "[ledger][reports]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Groceries",
                                      .kind = ledger::AccountKind::Expense,
                                      .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    auto checkingId = ledgerState.accounts[0].id;
    auto groceriesId = ledgerState.accounts[1].id;

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    model.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Weekly shop",
        .date = morph::time::Timestamp::now(),
        .legs = {ledger::TransactionLeg{
                     .accountId = checkingId,
                     .amount = morph::math::Rational{Numerator{-5000}, Denominator{1}, DecimalPlaces{2}}},
                 ledger::TransactionLeg{
                     .accountId = groceriesId,
                     .amount = morph::math::Rational{Numerator{5000}, Denominator{1}, DecimalPlaces{2}}}}});

    auto jobId = model.execute(
        ledger::SubmitReport{.ledgerId = ledgerId, .kind = ledger::ReportKind::MonthlyStatement, .params = "{}"});
    REQUIRE(jobId.hasValue());

    const auto status = pollUntilSettled(model, jobId);
    REQUIRE(status.status == ledger::ReportStatus::Done);
    REQUIRE(status.result.has_value());
    // The body is a real aggregation, not an empty placeholder: the two USD
    // accounts' balances (-50.00 and +50.00) net to exactly zero, carried as
    // a Rational triple (numerator 0), never a float.
    std::vector<ledger::ReportLine> lines;
    REQUIRE(!glz::read_json(lines, *status.result));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].currency == "USD");
    CHECK(lines[0].numerator == 0);
    CHECK(lines[0].decimalPlaces == 2);
}

TEST_CASE("A 23:30-local transaction is reported in its local month, not its UTC one", "[ledger][reports][time]") {
    // Design spec §9's own stated assertion. At UTC-5:
    //   2026-02-01T04:30Z == 2026-01-31T23:30 local -> belongs to JANUARY
    //   2026-02-01T05:01Z == 2026-02-01T00:01 local -> belongs to FEBRUARY
    // Both are already February by the UTC date stored in the column, so a
    // report that compared stored dates against a UTC month would put both in
    // February and neither in January.
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    auto worker = std::make_shared<StepExecutor>();
    ledger::LedgerModel model{worker};
    const ScopedPrincipal principal{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Groceries",
                                      .kind = ledger::AccountKind::Expense,
                                      .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    const auto checkingId = ledgerState.accounts[0].id;
    const auto groceriesId = ledgerState.accounts[1].id;

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    const auto storeAt = [&](std::string_view iso, std::string_view description) {
        const auto instant = morph::time::DateTime::fromIso8601(iso);
        REQUIRE(instant.has_value());
        model.execute(ledger::StoreTransaction{
            .ledgerId = ledgerId,
            .description = std::string{description},
            .date = morph::time::Timestamp{*instant},
            .legs = {ledger::TransactionLeg{
                         .accountId = checkingId,
                         .amount = morph::math::Rational{Numerator{-5000}, Denominator{1}, DecimalPlaces{2}}},
                     ledger::TransactionLeg{
                         .accountId = groceriesId,
                         .amount = morph::math::Rational{Numerator{5000}, Denominator{1}, DecimalPlaces{2}}}}});
    };
    storeAt("2026-02-01T04:30:00Z", "late on the 31st, local");
    storeAt("2026-02-01T05:01:00Z", "just after midnight, local");

    const auto countFor = [&](int year, unsigned month) {
        const ledger::MonthlyStatementParams params{.year = year, .month = month, .timezoneOffsetMinutes = -300};
        std::string paramsJson;
        REQUIRE(!glz::write_json(params, paramsJson));
        const auto jobId = model.execute(ledger::SubmitReport{
            .ledgerId = ledgerId, .kind = ledger::ReportKind::MonthlyStatement, .params = paramsJson});
        REQUIRE(jobId.hasValue());
        // Three report jobs run in this test case; each is submitted, asserted
        // still Pending, then run by hand. Previously this was three passes of
        // a 10ms-granularity poll loop, and the bulk of this file's runtime.
        CHECK(model.execute(ledger::GetReportStatus{.jobId = jobId}).status == ledger::ReportStatus::Pending);
        runReportJob(*worker);
        const auto status = model.execute(ledger::GetReportStatus{.jobId = jobId});
        REQUIRE(status.status == ledger::ReportStatus::Done);
        REQUIRE(status.result.has_value());
        std::vector<ledger::ReportLine> lines;
        REQUIRE(!glz::read_json(lines, *status.result));
        REQUIRE(lines.size() == 1);
        CHECK(lines[0].currency == "USD");
        // Zero-sum per currency holds whatever the period -- which is exactly
        // why the count, not the total, is what distinguishes the months.
        CHECK(lines[0].numerator == 0);
        return lines[0].transactionCount;
    };

    CHECK(countFor(2026, 1) == 1);  // the 23:30-local transaction, and only it
    CHECK(countFor(2026, 2) == 1);  // the 00:01-local one, and only it

    // A month with neither transaction reports zero rather than everything --
    // proves the filter is applied at all, not merely computed and dropped.
    CHECK(countFor(2026, 3) == 0);
}

TEST_CASE("Re-polling the same completed job returns byte-identical results", "[ledger][reports]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    auto worker = std::make_shared<StepExecutor>();
    ledger::LedgerModel model{worker};
    const ScopedPrincipal principal{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});

    auto jobId = model.execute(
        ledger::SubmitReport{.ledgerId = ledgerId, .kind = ledger::ReportKind::MonthlyStatement, .params = "{}"});

    runReportJob(*worker);
    const auto status = model.execute(ledger::GetReportStatus{.jobId = jobId});
    REQUIRE(status.status == ledger::ReportStatus::Done);

    // Two more polls of the SAME completed job -- byte-identical results
    // (design spec §9's DoD bullet, scoped to one job's own idempotent
    // retrieval; a fresh SubmitReport for the same period is explicitly
    // allowed to differ, per that same section -- not tested here).
    auto secondPoll = model.execute(ledger::GetReportStatus{.jobId = jobId});
    auto thirdPoll = model.execute(ledger::GetReportStatus{.jobId = jobId});
    CHECK(secondPoll.result == thirdPoll.result);
    CHECK(secondPoll.status == thirdPoll.status);
}

TEST_CASE("A submitted report stays Pending until its worker actually runs", "[ledger][reports]") {
    // The assertion a real thread pool cannot support. Against a
    // `ThreadPoolExecutor` the worker may already have finished by the time
    // the test looks, so "submitted, and the aggregation has NOT happened yet"
    // can only be sampled and hoped for -- which means a `SubmitReport` that
    // quietly computed the report inline, on the caller's thread, would pass
    // every previous test in this file. Here it is an ordinary CHECK.
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    auto worker = std::make_shared<StepExecutor>();
    ledger::LedgerModel model{worker};
    const ScopedPrincipal principal{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});

    const auto jobId = model.execute(
        ledger::SubmitReport{.ledgerId = ledgerId, .kind = ledger::ReportKind::MonthlyStatement, .params = "{}"});
    REQUIRE(jobId.hasValue());

    // Submitted: the aggregation is queued and provably has not run.
    CHECK(worker->pending() == 1);
    // Stable, not merely "not yet": polled repeatedly, it stays Pending with
    // no result body for as long as the worker is not run. On a real pool the
    // same three polls race the job and prove nothing.
    for (int poll = 0; poll < 3; ++poll) {
        const auto pending = model.execute(ledger::GetReportStatus{.jobId = jobId});
        CHECK(pending.status == ledger::ReportStatus::Pending);
        CHECK_FALSE(pending.result.has_value());
    }
    CHECK(worker->pending() == 1);

    REQUIRE(worker->runOne());

    const auto done = model.execute(ledger::GetReportStatus{.jobId = jobId});
    CHECK(done.status == ledger::ReportStatus::Done);
    CHECK(done.result.has_value());
    // The job is one task, not a chain: it left nothing queued behind it.
    CHECK_FALSE(worker->runOne());
}

TEST_CASE("Running one report job settles that job and no other", "[ledger][reports]") {
    // Two jobs outstanding at once, settled one at a time. Only a hand-driven
    // worker can hold a second job at Pending while the first completes, so a
    // completion that wrote the wrong row -- or every row -- was previously
    // untestable here: with a real pool both jobs finish before either can be
    // observed, and "job B is Done" looks the same either way.
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    auto worker = std::make_shared<StepExecutor>();
    ledger::LedgerModel model{worker};
    const ScopedPrincipal principal{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});

    const auto first = model.execute(
        ledger::SubmitReport{.ledgerId = ledgerId, .kind = ledger::ReportKind::MonthlyStatement, .params = "{}"});
    const auto second = model.execute(
        ledger::SubmitReport{.ledgerId = ledgerId, .kind = ledger::ReportKind::MonthlyStatement, .params = "{}"});
    REQUIRE(first.hasValue());
    REQUIRE(second.hasValue());
    REQUIRE(*first != *second);
    REQUIRE(worker->pending() == 2);

    // FIFO: the first submitted is the first queued, so this settles `first`.
    REQUIRE(worker->runOne());
    CHECK(model.execute(ledger::GetReportStatus{.jobId = first}).status == ledger::ReportStatus::Done);
    CHECK(model.execute(ledger::GetReportStatus{.jobId = second}).status == ledger::ReportStatus::Pending);

    REQUIRE(worker->runOne());
    CHECK(model.execute(ledger::GetReportStatus{.jobId = second}).status == ledger::ReportStatus::Done);
    CHECK_FALSE(worker->runOne());
}

TEST_CASE("SubmitReport rejects a disengaged ledgerId and an unknown ledger", "[ledger][reports]") {
    morph::ladder::testkit::DbFixture fixture;

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    CHECK_THROWS_AS(model.execute(ledger::SubmitReport{
                        .ledgerId = ledger::LedgerId{}, .kind = ledger::ReportKind::MonthlyStatement, .params = "{}"}),
                    ledger::ValidationError);
    CHECK_THROWS_AS(
        model.execute(ledger::SubmitReport{
            .ledgerId = ledger::LedgerId{9999}, .kind = ledger::ReportKind::MonthlyStatement, .params = "{}"}),
        ledger::NotFound);
}

TEST_CASE("GetReportStatus rejects a disengaged jobId and an unknown job", "[ledger][reports]") {
    morph::ladder::testkit::DbFixture fixture;

    ledger::LedgerModel model;
    CHECK_THROWS_AS(model.execute(ledger::GetReportStatus{.jobId = ledger::ReportJobId{}}), ledger::ValidationError);
    CHECK_THROWS_AS(model.execute(ledger::GetReportStatus{.jobId = ledger::ReportJobId{9999}}), ledger::NotFound);
}
