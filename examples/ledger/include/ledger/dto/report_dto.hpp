// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "ledger/core/types.hpp"

namespace ledger {

/// @brief Enqueues a report computation for `ledgerId` (design spec §9) and
///        returns immediately with the freshly created job's
///        `ReportJobId` -- the submit half of the submit->poll pair.
///        `params` is opaque to the model: a JSON object carrying whatever
///        the named `kind` needs (period bounds, the client's timezone
///        offset, ...), stored with the job rather than interpreted here.
///
///        "Enqueues" means exactly one thing: a `Pending` row is written,
///        carrying `kind` and `params` verbatim. Nothing is scheduled and no
///        thread is started -- the row *is* the queue, and
///        `ledger::app::App`'s report runner is what drains it, by
///        dispatching `RunReportJob` back at this model (morph#160). A job
///        submitted while no runner is up therefore stays `Pending` and is
///        picked up by the first pass of the next one, which is what makes a
///        report survive a server restart instead of dying with the process
///        that accepted it.
struct SubmitReport {
    LedgerId ledgerId;
    ReportKind kind;
    std::string params;  // JSON-encoded, report-specific (design spec §9)

    /// @brief Whether this action carries the fields its execution needs.
    /// @return `true` if `ledgerId` is engaged.
    [[nodiscard]] bool validate() const noexcept { return ledgerId.hasValue(); }
};

/// @brief `SubmitReport::params` as a `ReportKind::MonthlyStatement` reads
///        it: the local calendar month to report on, plus the client's
///        offset from UTC at the time it asked.
///
///        `params` is opaque to the model in general (see `SubmitReport`);
///        this is the one kind that interprets it. Declared here rather than
///        privately inside `ledger_model.cpp` for exactly the reason
///        `ReportLine` below is -- a client, or a test, encodes `params`
///        from the same type the model decodes it into, instead of both
///        sides hand-writing the same JSON object shape and drifting. It
///        also has to have external linkage for glaze's reflection to see
///        it at all, which a `.cpp`-private anonymous-namespace type does
///        not.
///
///        A fixed offset rather than a zone name: exact for any period with
///        no DST transition inside it, which is what this rung claims.
struct MonthlyStatementParams {
    int year{};
    unsigned month{};
    int timezoneOffsetMinutes{};
};

/// @brief Computes the report the job named by `jobId` was submitted for and
///        writes its terminal state onto that job's row -- the *run* half of
///        the submit->run->poll triple.
///
///        Dispatched by `ledger::app::App`'s report runner, never by a user
///        client: `LedgerModel::execute(const RunReportJob&)` refuses any
///        principal but `kReportRunnerPrincipal`. It exists as an action, and
///        not as a lambda posted to an executor the model owns, because the
///        aggregation is business logic and business logic lives in a model
///        (`examples/IMPLEMENTATION.md` rule 1) -- while deciding *when* it
///        runs is orchestration, which does not (morph#160). Re-entering the
///        model as an ordinary dispatch is the same shape
///        `bookmarks::RecordMetadata` has for that rung's metadata worker.
///
///        Idempotent by construction: a job that has already reached a
///        terminal status is left exactly as it is and its current status
///        returned, so a second dispatch for the same job -- which the
///        runner will issue whenever a pass ticks while a previous pass's
///        dispatch is still outstanding -- recomputes nothing and overwrites
///        nothing.
///
///        Carries `ledgerId` alongside `jobId` even though the job row
///        records its own ledger: `ActionKeyTraits<RunReportJob>` keys on it,
///        so the run lands on the *same* strand as every other action against
///        that ledger. Resolving it by reading the job row inside `key()`
///        would repeat the DB-lookup-inside-key() pattern this rung already
///        rejected once (see `ActionKeyTraits<GetReportStatus>`'s comment).
struct RunReportJob {
    ReportJobId jobId;
    LedgerId ledgerId;

    /// @brief Whether this action carries the fields its execution needs.
    /// @return `true` if both `jobId` and `ledgerId` are engaged.
    [[nodiscard]] bool validate() const noexcept { return jobId.hasValue() && ledgerId.hasValue(); }
};

/// @brief What the job's row says after `RunReportJob` returns: `Done` when
///        this call computed the body (or found it already computed),
///        `Failed` when the aggregation threw and the failure was recorded.
///
///        Never `Pending`: `RunReportJob` either settles the job or reports
///        that it was already settled, so a runner seeing `Pending` come back
///        would be looking at a bug rather than at work still to do.
struct RunReportJobResult {
    ReportStatus status{ReportStatus::Pending};
};

/// @brief Polls the job `SubmitReport` returned. A pure read with no
///        session-scoped side effect (hence no empty-principal gate and
///        `Loggable::No` on its registration, matching `GetLedger`).
struct GetReportStatus {
    ReportJobId jobId;

    /// @brief Whether this action carries the fields its execution needs.
    /// @return `true` if `jobId` is engaged.
    [[nodiscard]] bool validate() const noexcept { return jobId.hasValue(); }
};

/// @brief One poll's answer: the job's current status plus, once it has
///        reached `ReportStatus::Done`, the computed report body.
struct GetReportStatusResult {
    ReportStatus status{ReportStatus::Pending};
    std::optional<std::string> result;  // engaged only once status == Done
};

/// @brief One line of a computed report body: a currency and that currency's
///        total across every account in the ledger, carried as its exact
///        `morph::math::Rational` triple rather than a lossy decimal string
///        (design spec §7's no-float rule applies to a report body exactly
///        as it does to a leg amount).
///
///        `GetReportStatusResult::result` holds a JSON array of these. It
///        stays a serialized string on the DTO rather than a typed vector:
///        the column it round-trips through is opaque text, and a future
///        `ReportKind` is free to shape its own body differently without
///        this result type changing. Declared here (not privately inside
///        `ledger_model.cpp`) so a client -- or a test -- can decode a
///        report body against the same type the model encoded it from.
struct ReportLine {
    std::string currency;
    std::int64_t numerator{0};
    std::int64_t denominator{1};
    std::uint32_t decimalPlaces{0};

    /// @brief How many transactions this line's total was computed over.
    ///
    ///        Carried because the balance alone cannot answer the question a
    ///        monthly statement exists to answer. `StoreTransaction` enforces
    ///        a per-currency zero-sum (design spec §1), so *any* whole set of
    ///        transactions nets to exactly zero per currency -- which makes
    ///        the total identical whether the report covers one month or all
    ///        time, and leaves "did my 23:30-on-the-31st transaction land in
    ///        the right month?" (design spec §9's own stated assertion)
    ///        unanswerable from the body.
    ///
    ///        The count does vary with the period, so it is what makes the
    ///        local-month boundary observable rather than merely computed.
    std::int64_t transactionCount{0};
};

}  // namespace ledger
