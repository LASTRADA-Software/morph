// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/session/session.hpp>
#include <string>
#include <vector>

#include "ledger/core/errors.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/ledger_model.hpp"
#include "testkit/db_fixture.hpp"

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

/// @brief Runs the report job @p jobId, on @p ledgerId, the way
///        `ledger::app::App`'s runner does -- under the runner principal,
///        as an ordinary `RunReportJob` dispatch.
///
/// This file used to poll `GetReportStatus` in a bounded retry loop with
/// `std::this_thread::sleep_for` between iterations, because
/// `execute(SubmitReport)` posted the aggregation to a real
/// `ThreadPoolExecutor` the model itself owned and there was no way to
/// observe -- let alone control -- when the worker ran. There is no loop, no
/// sleep and no cap any more, and not because a test double was introduced:
/// the aggregation is now an ordinary synchronous action
/// (morph#160), so "the report has been computed" is simply what this call
/// returning means. `examples/TESTING.md`'s ban on `sleep_for` outside
/// `pump.hpp` is satisfied by construction here rather than by budget.
///
/// The principal scope is installed and dropped inside this helper, so a
/// caller's own `ScopedPrincipal` (a *user*, which `RunReportJob` refuses)
/// is restored by the time it returns.
/// @param model The model to run the job on.
/// @param jobId The submitted job.
/// @param ledgerId The job's ledger -- what the action keys on.
/// @return The job's terminal status, as the run itself reported it.
[[nodiscard]] ledger::RunReportJobResult runReportJob(ledger::LedgerModel& model, const ledger::ReportJobId& jobId,
                                                      const ledger::LedgerId& ledgerId) {
    const ScopedPrincipal runner{std::string{ledger::kReportRunnerPrincipal}};
    return model.execute(ledger::RunReportJob{.jobId = jobId, .ledgerId = ledgerId});
}

/// @brief Runs @p jobId and returns what a client polling it would then see.
/// @param model The model to run and poll.
/// @param jobId The submitted job.
/// @param ledgerId The job's ledger.
/// @return The `GetReportStatus` answer after the run.
[[nodiscard]] ledger::GetReportStatusResult runAndPoll(ledger::LedgerModel& model, const ledger::ReportJobId& jobId,
                                                       const ledger::LedgerId& ledgerId) {
    static_cast<void>(runReportJob(model, jobId, ledgerId));
    return model.execute(ledger::GetReportStatus{.jobId = jobId});
}

}  // namespace

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

    // Submit is now provably only a row write: the job is Pending, with no
    // body, and stays that way for as long as nothing runs it. Against the
    // model-owned thread pool this replaced, the same two polls raced the
    // worker and proved nothing -- a SubmitReport that computed the report
    // inline would have passed them.
    const auto beforeRun = model.execute(ledger::GetReportStatus{.jobId = jobId});
    CHECK(beforeRun.status == ledger::ReportStatus::Pending);
    CHECK_FALSE(beforeRun.result.has_value());

    CHECK(runReportJob(model, jobId, ledgerId).status == ledger::ReportStatus::Done);
    const auto status = model.execute(ledger::GetReportStatus{.jobId = jobId});
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
        const auto status = runAndPoll(model, jobId, ledgerId);
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

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});

    auto jobId = model.execute(
        ledger::SubmitReport{.ledgerId = ledgerId, .kind = ledger::ReportKind::MonthlyStatement, .params = "{}"});

    const auto status = runAndPoll(model, jobId, ledgerId);
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

TEST_CASE("A submitted report stays Pending for as long as nothing runs it", "[ledger][reports]") {
    // The property that says the executor really did leave the model
    // (morph#160), and the one the old thread-pool version could not assert
    // at all: with no runner anywhere in the process, a submitted job is
    // stable at Pending rather than merely "not done yet". This is also
    // exactly what a job outliving the process that accepted it looks like --
    // the row waits for whichever runner comes along next.
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

    const auto jobId = model.execute(
        ledger::SubmitReport{.ledgerId = ledgerId, .kind = ledger::ReportKind::MonthlyStatement, .params = "{}"});
    REQUIRE(jobId.hasValue());

    for (int poll = 0; poll < 3; ++poll) {
        const auto pending = model.execute(ledger::GetReportStatus{.jobId = jobId});
        CHECK(pending.status == ledger::ReportStatus::Pending);
        CHECK_FALSE(pending.result.has_value());
    }
}

TEST_CASE("Running one report job settles that job and no other", "[ledger][reports]") {
    // Two jobs outstanding at once, settled one at a time -- a completion
    // that wrote the wrong row, or every row, would show up here and nowhere
    // else. The property predates morph#160 as a goal but could not be
    // asserted while the aggregation was a lambda on a real pool: both jobs
    // finished before either could be observed, and "job B is Done" looked
    // the same whether B ran or A settled it. `RunReportJob` names the job it
    // settles, so the second one staying Pending is now an ordinary CHECK.
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    ledger::ReportJobId first;
    ledger::ReportJobId second;
    {
        const ScopedPrincipal alice{"alice"};
        model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                          .name = "Checking",
                                          .kind = ledger::AccountKind::Asset,
                                          .currency = ledger::Currency::USD});
        first = model.execute(
            ledger::SubmitReport{.ledgerId = ledgerId, .kind = ledger::ReportKind::MonthlyStatement, .params = "{}"});
        second = model.execute(
            ledger::SubmitReport{.ledgerId = ledgerId, .kind = ledger::ReportKind::MonthlyStatement, .params = "{}"});
    }
    REQUIRE(first.hasValue());
    REQUIRE(second.hasValue());
    REQUIRE(*first != *second);

    CHECK(runReportJob(model, first, ledgerId).status == ledger::ReportStatus::Done);
    CHECK(model.execute(ledger::GetReportStatus{.jobId = first}).status == ledger::ReportStatus::Done);
    CHECK(model.execute(ledger::GetReportStatus{.jobId = second}).status == ledger::ReportStatus::Pending);

    CHECK(runReportJob(model, second, ledgerId).status == ledger::ReportStatus::Done);
    CHECK(model.execute(ledger::GetReportStatus{.jobId = second}).status == ledger::ReportStatus::Done);
}

TEST_CASE("SubmitReport stores its params on the job row, and RunReportJob reads them back", "[ledger][reports]") {
    // The params used to be decoded on SubmitReport's own thread and captured
    // into the posted lambda, so they never had to persist. Now the run
    // happens later -- possibly in another process -- and the row is the only
    // record of what was asked for. Proven through behavior rather than by
    // reading the column: a January statement and a February one over the
    // same ledger must disagree, which they can only do if each job's own
    // params survived the round trip through the database.
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
    const auto checkingId = ledgerState.accounts[0].id;
    const auto groceriesId = ledgerState.accounts[1].id;

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    const auto instant = morph::time::DateTime::fromIso8601("2026-02-01T04:30:00Z");
    REQUIRE(instant.has_value());
    model.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "late on the 31st, local",
        .date = morph::time::Timestamp{*instant},
        .legs = {ledger::TransactionLeg{
                     .accountId = checkingId,
                     .amount = morph::math::Rational{Numerator{-5000}, Denominator{1}, DecimalPlaces{2}}},
                 ledger::TransactionLeg{
                     .accountId = groceriesId,
                     .amount = morph::math::Rational{Numerator{5000}, Denominator{1}, DecimalPlaces{2}}}}});

    // BOTH jobs are submitted before EITHER is run: whatever distinguishes
    // them at run time cannot have come from the submitting call frame.
    const auto submitFor = [&](int year, unsigned month) {
        const ledger::MonthlyStatementParams params{.year = year, .month = month, .timezoneOffsetMinutes = -300};
        std::string paramsJson;
        REQUIRE(!glz::write_json(params, paramsJson));
        return model.execute(ledger::SubmitReport{
            .ledgerId = ledgerId, .kind = ledger::ReportKind::MonthlyStatement, .params = paramsJson});
    };
    const auto januaryJob = submitFor(2026, 1);
    const auto februaryJob = submitFor(2026, 2);

    const auto countOf = [&](const ledger::GetReportStatusResult& status) {
        REQUIRE(status.status == ledger::ReportStatus::Done);
        REQUIRE(status.result.has_value());
        std::vector<ledger::ReportLine> lines;
        REQUIRE(!glz::read_json(lines, *status.result));
        REQUIRE(lines.size() == 1);
        return lines[0].transactionCount;
    };
    CHECK(countOf(runAndPoll(model, januaryJob, ledgerId)) == 1);
    CHECK(countOf(runAndPoll(model, februaryJob, ledgerId)) == 0);
}

TEST_CASE("RunReportJob refuses any principal but the report runner's", "[ledger][reports]") {
    // The layering gate: RunReportJob is the App layer's action, not a
    // client's. An ordinary user dispatching it must not be able to settle a
    // job -- and, just as importantly, must not be able to do so *silently*.
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    ledger::ReportJobId jobId;
    {
        const ScopedPrincipal alice{"alice"};
        jobId = model.execute(
            ledger::SubmitReport{.ledgerId = ledgerId, .kind = ledger::ReportKind::MonthlyStatement, .params = "{}"});
        REQUIRE(jobId.hasValue());
        CHECK_THROWS_AS(model.execute(ledger::RunReportJob{.jobId = jobId, .ledgerId = ledgerId}), ledger::Forbidden);
    }
    // Refused with no session at all, too -- not merely with the wrong one.
    CHECK_THROWS_AS(model.execute(ledger::RunReportJob{.jobId = jobId, .ledgerId = ledgerId}), ledger::Forbidden);

    // And the refusal left the job alone rather than half-settling it.
    CHECK(model.execute(ledger::GetReportStatus{.jobId = jobId}).status == ledger::ReportStatus::Pending);
}

TEST_CASE("Running an already-settled job recomputes nothing", "[ledger][reports]") {
    // The runner re-dispatches a job whenever a pass ticks while an earlier
    // pass's dispatch for the same job is still outstanding, so this is the
    // ordinary case, not an exotic one. Both dispatches land on the ledger's
    // one strand; the second must find the job terminal and leave it exactly
    // as it is.
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    ledger::ReportJobId jobId;
    {
        const ScopedPrincipal alice{"alice"};
        model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                          .name = "Checking",
                                          .kind = ledger::AccountKind::Asset,
                                          .currency = ledger::Currency::USD});
        jobId = model.execute(
            ledger::SubmitReport{.ledgerId = ledgerId, .kind = ledger::ReportKind::MonthlyStatement, .params = "{}"});
    }
    const auto first = runAndPoll(model, jobId, ledgerId);
    REQUIRE(first.status == ledger::ReportStatus::Done);

    // The second run reports Done without touching the row: byte-identical,
    // which is what makes the re-run safe rather than merely tolerated.
    CHECK(runReportJob(model, jobId, ledgerId).status == ledger::ReportStatus::Done);
    const auto second = model.execute(ledger::GetReportStatus{.jobId = jobId});
    CHECK(second.status == first.status);
    CHECK(second.result == first.result);
}

TEST_CASE("RunReportJob rejects unengaged ids and an unknown job", "[ledger][reports]") {
    morph::ladder::testkit::DbFixture fixture;
    ledger::LedgerModel model;
    const ScopedPrincipal runner{std::string{ledger::kReportRunnerPrincipal}};

    CHECK_THROWS_AS(
        model.execute(ledger::RunReportJob{.jobId = ledger::ReportJobId{}, .ledgerId = ledger::LedgerId{1}}),
        ledger::ValidationError);
    CHECK_THROWS_AS(
        model.execute(ledger::RunReportJob{.jobId = ledger::ReportJobId{1}, .ledgerId = ledger::LedgerId{}}),
        ledger::ValidationError);
    CHECK_THROWS_AS(
        model.execute(ledger::RunReportJob{.jobId = ledger::ReportJobId{9999}, .ledgerId = ledger::LedgerId{9999}}),
        ledger::NotFound);
}
