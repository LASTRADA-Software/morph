// SPDX-License-Identifier: Apache-2.0
#include "ledger/core/errors.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/ledger_model.hpp"
#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/session/session.hpp>

#include <glaze/glaze.hpp>

#include <chrono>
#include <thread>
#include <vector>

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

/// @brief Polls @p model's `GetReportStatus` for @p jobId until it leaves
///        `Pending`, or until the hard iteration cap is reached.
///
/// A bounded retry loop with a hard cap (100 x 10ms = 1s), matching the only
/// precedent for testing an async job in this codebase
/// (`examples/bookmarks/tests/test_app.cpp`,
/// `examples/pastebin/tests/test_paste_model.cpp`): no deferred-executor test
/// double exists for the worker-pool side of a job, so this genuinely spins
/// the real `ThreadPoolExecutor` and sleeps between polls. A single report
/// job over a tiny test ledger completes far inside that budget; exhausting
/// the cap means a real stall (e.g. a SQLite lock held by another
/// connection), not a slow machine.
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
    model.execute(ledger::OpenAccount{
        .ledgerId = ledgerId, .name = "Checking", .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});
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
        .legs = {ledger::TransactionLeg{.accountId = checkingId,
                                        .amount = morph::math::Rational{Numerator{-5000}, Denominator{1},
                                                                        DecimalPlaces{2}}},
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

TEST_CASE("A 23:30-local transaction is reported in its local month, not its UTC one",
          "[ledger][reports][time]") {
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
            .legs = {ledger::TransactionLeg{.accountId = checkingId,
                                            .amount = morph::math::Rational{Numerator{-5000}, Denominator{1},
                                                                            DecimalPlaces{2}}},
                     ledger::TransactionLeg{.accountId = groceriesId,
                                            .amount = morph::math::Rational{Numerator{5000}, Denominator{1},
                                                                            DecimalPlaces{2}}}}});
    };
    storeAt("2026-02-01T04:30:00Z", "late on the 31st, local");
    storeAt("2026-02-01T05:01:00Z", "just after midnight, local");

    const auto countFor = [&](int year, unsigned month) {
        const ledger::MonthlyStatementParams params{
            .year = year, .month = month, .timezoneOffsetMinutes = -300};
        std::string paramsJson;
        REQUIRE(!glz::write_json(params, paramsJson));
        const auto jobId = model.execute(ledger::SubmitReport{
            .ledgerId = ledgerId, .kind = ledger::ReportKind::MonthlyStatement, .params = paramsJson});
        REQUIRE(jobId.hasValue());
        const auto status = pollUntilSettled(model, jobId);
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
    model.execute(ledger::OpenAccount{
        .ledgerId = ledgerId, .name = "Checking", .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});

    auto jobId = model.execute(
        ledger::SubmitReport{.ledgerId = ledgerId, .kind = ledger::ReportKind::MonthlyStatement, .params = "{}"});

    const auto status = pollUntilSettled(model, jobId);
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
    CHECK_THROWS_AS(model.execute(ledger::SubmitReport{.ledgerId = ledger::LedgerId{},
                                                       .kind = ledger::ReportKind::MonthlyStatement,
                                                       .params = "{}"}),
                    ledger::ValidationError);
    CHECK_THROWS_AS(model.execute(ledger::SubmitReport{.ledgerId = ledger::LedgerId{9999},
                                                       .kind = ledger::ReportKind::MonthlyStatement,
                                                       .params = "{}"}),
                    ledger::NotFound);
}

TEST_CASE("GetReportStatus rejects a disengaged jobId and an unknown job", "[ledger][reports]") {
    morph::ladder::testkit::DbFixture fixture;

    ledger::LedgerModel model;
    CHECK_THROWS_AS(model.execute(ledger::GetReportStatus{.jobId = ledger::ReportJobId{}}), ledger::ValidationError);
    CHECK_THROWS_AS(model.execute(ledger::GetReportStatus{.jobId = ledger::ReportJobId{9999}}), ledger::NotFound);
}
