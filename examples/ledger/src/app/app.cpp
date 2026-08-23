// SPDX-License-Identifier: Apache-2.0
#include "ledger/app/app.hpp"

#include "ledger/core/types.hpp"
#include "ledger/dto/report_dto.hpp"
// Every model this rung hosts is included here, not only the one the report
// runner dispatches against. `BRIDGE_REGISTER_MODEL`/`BRIDGE_REGISTER_ACTION`
// place their registrars in the *header*, so a translation unit that includes
// the header both registers the type with the process-wide
// registry/dispatcher and emits a reference to that model's `execute` bodies
// -- which is what pulls each model's object file out of the static library
// for a binary (a future server `main()`) whose own code names nothing but
// `App`. Without this, such a binary would either fail to link or come up
// serving no models at all. Same reason, same comment, as bookmarks' own
// app.cpp.
#include <Lightweight/SqlStatement.hpp>
#include <cstdint>
#include <exception>
#include <morph/core/logger.hpp>
#include <morph/session/session.hpp>
#include <string>
#include <utility>
#include <vector>

#include "ledger/models/budget_model.hpp"
#include "ledger/models/ledger_model.hpp"
#include "ledger/models/rule_model.hpp"

namespace ledger::app {

namespace {

/// @brief Renders @p error's message, whatever its type.
/// @param error The captured exception, never null on an `onError` path.
/// @return The exception's `what()`, or a placeholder for a non-`std`
///         exception.
[[nodiscard]] std::string describe(const std::exception_ptr& error) {
    if (!error) {
        return "no exception";
    }
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& exc) {
        return exc.what();
    } catch (...) {
        return "unknown exception";
    }
}

}  // namespace

App::App(std::chrono::milliseconds runInterval, std::size_t workers, QObject* parent)
    // Initialiser order follows the declaration order in app.hpp, which is
    // itself chosen for teardown safety -- see that header's comment.
    : QObject{parent}, _pool{workers}, _reportBridge{std::make_unique<::morph::backend::LocalBackend>(_pool)} {
    // The runner's own session. No token and no issuer: this rung installs no
    // authorizer (see app.hpp for why there is no RemoteServer here either),
    // so there is nothing to verify a token against and minting one would be
    // theatre. The principal is what
    // `LedgerModel::execute(const RunReportJob&)` checks, and
    // `kReportRunnerPrincipal`'s own doc comment is explicit that without a
    // verifying authorizer this is a layering gate rather than an
    // authorization boundary.
    ::morph::session::Context session;
    session.principal = std::string{kReportRunnerPrincipal};
    _reportBridge.setDefaultSession(session);

    // The timer slot is wrapped rather than connected to the method directly.
    // An exception escaping a Qt slot is unsupported -- Qt's event dispatcher
    // propagates it out of `exec()` at best and calls `std::terminate` at
    // worst -- so a pass that throws would take the whole server process down
    // with it, for a failure that only ever concerns one pass.
    // `runPendingReportsOnce()` is not exception-free: it queries the
    // database, and it constructs a `BridgeHandler`, which throws if the
    // register is refused. Logging and dropping the pass is the right
    // response: the next tick simply retries, since a dropped pass consumes
    // nothing -- the jobs it failed to dispatch are still `Pending`. The
    // public method itself keeps throwing, so a test that calls it directly
    // still sees the failure.
    connect(&_reportTimer, &QTimer::timeout, this, [this] {
        try {
            runPendingReportsOnce();
        } catch (const std::exception& e) {
            ::morph::log::logError(std::string{"[ledger::App] report-runner pass threw, pass abandoned: "} + e.what());
        } catch (...) {
            ::morph::log::logError("[ledger::App] report-runner pass threw a non-std exception, pass abandoned");
        }
    });
    _reportTimer.start(runInterval);
}

void App::stopBackgroundJobs() { _reportTimer.stop(); }

App::~App() {
    // Stop first: a tick landing while the members below are being torn down
    // would dispatch a pass into a half-destroyed App. A shutting-down owner
    // will normally have called stopBackgroundJobs() already, before its own
    // drain loop started pumping (see that method's doc comment); calling it
    // again here is a no-op, and keeps this destructor correct for every
    // owner that does not.
    stopBackgroundJobs();
}

void App::runPendingReportsOnce() {
    // (job row id, that job's ledger id). Both are read here rather than
    // resolved later because `RunReportJob` carries both: the ledger id is
    // what `ActionKeyTraits<RunReportJob>` keys on, so the run lands on the
    // same strand as every other action against that book.
    std::vector<std::pair<std::int64_t, std::int64_t>> pending;
    {
        ::Lightweight::SqlStatement stmt;
        stmt.Prepare("SELECT id, ledger_id FROM ledger_report_jobs WHERE status = ?");
        auto cursor = stmt.Execute(static_cast<int>(ReportStatus::Pending));
        while (cursor.FetchRow()) {
            pending.emplace_back(cursor.GetColumn<std::int64_t>(1), cursor.GetColumn<std::int64_t>(2));
        }
    }
    if (pending.empty()) {
        return;
    }

    // `handler` is kept alive by every dispatched call's own completion, not
    // by this function's stack frame -- the identical pattern (and identical
    // race) pastebin::app::App::sweepExpiredOnce() documents at length.
    // `BridgeHandler::execute()` posts to the worker pool and returns
    // immediately, so this loop routinely returns before the backend has so
    // much as looked up the model instance for the first dispatch. A
    // `handler` destroyed synchronously here would deregister its instance (a
    // synchronous "deregister" in ~BridgeHandler) and race those pending
    // dispatches, which would then find the instance missing and fail instead
    // of ever running RunReportJob -- silently dropping the pass. Capturing `handler` in every completion below closes
    // that window: the instance is released only once every dispatch this pass issued has settled, whichever of
    // .then()/.onError() that turns out to be for each.
    //
    // Constructing it per pass rather than once in the constructor also keeps
    // an idle server from holding a live model instance between passes.
    auto handler = std::make_shared<::morph::bridge::BridgeHandler<LedgerModel>>(_reportBridge, &_reportExecutor);
    // Captured by value, never through `this`: the callbacks below can
    // outlive this App (see reportsInFlight()'s doc comment), and a late one
    // must still be able to decrement the counter safely.
    auto inFlight = _reportsInFlight;
    for (const auto& [jobId, ledgerId] : pending) {
        // The raise has to precede the dispatch -- a completion delivered
        // from a worker thread could otherwise lower a count this loop had
        // not raised yet -- which leaves a window the `catch` below closes.
        inFlight->fetch_add(1);
        try {
            handler->execute(RunReportJob{.jobId = ReportJobId{jobId}, .ledgerId = LedgerId{ledgerId}})
                .then([handler, inFlight, jobId](RunReportJobResult result) {
                    inFlight->fetch_sub(1);
                    if (result.status == ReportStatus::Failed) {
                        // The aggregation threw and recorded itself Failed.
                        // Not an error of the dispatch -- which is why it
                        // arrives here and not in onError -- but still the
                        // only place an operator would see it.
                        ::morph::log::logError("[ledger::App] report job " + std::to_string(jobId) + " failed");
                    }
                })
                .onError([handler, inFlight, jobId](const std::exception_ptr& error) {
                    inFlight->fetch_sub(1);
                    // The dispatch itself failed, so the row is untouched and
                    // still Pending: the next pass retries it. Logged rather
                    // than swallowed because a pass that can never dispatch
                    // (a refused register, a vanished model) would otherwise
                    // retry silently forever -- and logged *with the reason*,
                    // because "dispatch failed" alone cannot distinguish a
                    // refused register from a rejected principal from a
                    // vanished row, which are three different operator
                    // actions.
                    ::morph::log::logError("[ledger::App] RunReportJob dispatch failed for job " +
                                           std::to_string(jobId) + ": " + describe(error));
                });
        } catch (const std::exception& e) {
            // `execute()` threw instead of returning a `Completion`, so
            // neither callback above was ever attached and nothing else will
            // ever lower the count the line above raised. Leaving it raised
            // wedges `reportsInFlight()` at `true` permanently, and with it
            // every consumer that drains on it.
            inFlight->fetch_sub(1);
            ::morph::log::logError("[ledger::App] RunReportJob dispatch for job " + std::to_string(jobId) +
                                   " threw: " + e.what());
        }
    }
}

}  // namespace ledger::app
