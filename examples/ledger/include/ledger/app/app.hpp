// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/qt/qt_executor.hpp>

/// @file
/// `ledger::app::App` -- rung 5's server-side bootstrap, and the home of the
/// one background job this rung has.
///
/// It exists because rung 5 had nowhere to put a background job and put it in
/// a model instead: `LedgerModel` owned a `ThreadPoolExecutor` and
/// `SubmitReport` posted the aggregation to it, making the one ladder model
/// that included `<morph/core/executor.hpp>` (morph#160). The layering that
/// resolves it is the same one `bookmarks::app::App` already demonstrates:
/// the App owns the worker pool and decides *when* work runs; the model still
/// owns *what* the work computes, and is re-entered as an ordinary client
/// dispatch (`RunReportJob`), on its own strand, where mutation is safe.
///
/// Mirrors `pastebin::app::App` (rung 1) closely and on purpose -- the same
/// timer-plus-internal-client shape, the same declaration-order-for-teardown
/// rule, the same in-flight settle seam -- minus the pieces this rung has no
/// equivalent for:
///
///  - **No `RemoteServer`, and therefore no `server()` accessor.** Not an
///    omission for brevity: this rung ships no `src/auth/`, no `AuthModel`
///    and no token secret, and `RemoteServer` *clears* the session principal
///    for any authorizer that does not authenticate
///    (`include/morph/core/remote.hpp`'s `dispatchExecute`, and
///    `docs/spec/security.md` on why passing an unverified claim through
///    would be worse). With the allow-all default that is all this rung
///    could install, every action arriving over a `RemoteServer` reaches the
///    model with an empty principal -- which every mutating `LedgerModel`
///    action, `RunReportJob` included, refuses. So the runner dispatches
///    over a `LocalBackend`, which is also exactly the backend this rung's
///    shipped client uses (`morph::ladder::gui::AppContext`'s `Local` mode).
///    Giving rung 5 a login, an authorizer and a server binary is its own
///    piece of work (morph#242); it is not a side effect of moving an
///    executor out of a model.
///  - **No `FileActionLog`.** `LedgerModel` attaches its own log explicitly
///    (`LedgerModel::attachActionLog`); installing a process-wide default
///    here would change what this rung journals, which is unrelated to why
///    this class exists.

namespace ledger::app {

/// @brief Owns rung 5's server side: the model worker pool, and the periodic
/// report runner that drains `ledger_report_jobs` by dispatching
/// `RunReportJob` at `LedgerModel`.
///
/// The runner dispatches through an **internal client** -- a `Bridge` over a
/// `LocalBackend` on this `App`'s own pool -- rather than calling
/// `LedgerModel::execute` directly. That is what makes a report run an
/// ordinary dispatch: authorized, keyed (so it lands on the strand for its
/// own ledger, serialised against that book's other actions) and journaled
/// exactly like a client-issued action, instead of a naked call from a
/// timer slot.
///
/// The bridge carries a default session naming `kReportRunnerPrincipal`,
/// which is the only principal `LedgerModel::execute(const RunReportJob&)`
/// accepts. It needs a bridge of its own for that and cannot borrow a
/// caller's: `Bridge::setDefaultSession` is bridge-wide and has no per-call
/// override, so a runner sharing a user's bridge would have to either
/// impersonate that user or overwrite their session.
///
/// @par What a restart does to a job, and why that is the improvement
/// Job state lives entirely in `ledger_report_jobs`, and a pass is just "find
/// the `Pending` rows and dispatch each". So a job accepted by a process that
/// then dies is not lost: it is still `Pending`, and the first pass of the
/// next process picks it up. Under the model-owned executor this replaced,
/// the same job was a lambda in a `ThreadPoolExecutor` queue -- recoverable
/// only for as long as that exact process lived, and invisible to anything
/// else. The cost of that recoverability is that a job whose aggregation
/// crashes the process is retried on restart rather than abandoned; a job
/// whose aggregation merely *throws* is recorded `Failed` and is not retried.
class App : public QObject {
    Q_OBJECT
public:
    /// @brief Wires up the server side and starts the report-runner timer.
    /// @param runInterval How often the report runner sweeps for `Pending`
    ///        jobs. Tests pass a long interval (effectively disabling the
    ///        timer) and call `runPendingReportsOnce()` directly instead, for
    ///        determinism.
    /// @param workers Size of the model worker pool.
    /// @param parent Optional `QObject` parent.
    explicit App(std::chrono::milliseconds runInterval = std::chrono::seconds{1}, std::size_t workers = 4,
                 QObject* parent = nullptr);

    /// @brief Stops the report-runner timer.
    ~App() override;

    /// @brief Stops the periodic report runner, so nothing this `App` owns
    ///        can dispatch new work from now on.
    ///
    /// `~App` calls this too, so an owner that never calls it sees exactly
    /// the same behavior. It is public because a *shutting-down* owner has to
    /// call it earlier than that: the settle contract on `reportsInFlight()`
    /// below says "pump until it is `false`, then destroy", and pumping is
    /// precisely what lets the timer tick. A drain loop running with the
    /// timer still armed could dispatch a brand-new pass out of its own
    /// `processEvents()` call, re-raising `reportsInFlight()` after it had
    /// settled. Calling this first makes the drain monotonic: the outstanding
    /// set can only shrink.
    ///
    /// Idempotent (`QTimer::stop()` on a stopped timer is a no-op) and safe
    /// to call from the Qt thread at any point in the object's life.
    void stopBackgroundJobs();

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    /// @brief Runs one report-runner pass right now: finds every
    ///        `ReportStatus::Pending` job row, across every ledger, and
    ///        fire-and-forget dispatches `RunReportJob` for each through the
    ///        internal client.
    ///
    /// Does not block on the dispatched calls settling -- callers that need
    /// to observe completion (tests, shutdown) pump the Qt event loop
    /// afterward (`morph::ladder::testkit::pumpUntil`) on
    /// `reportsInFlight()`.
    ///
    /// Dispatching a job a previous pass is still working on is harmless and
    /// expected: both dispatches key on the same ledger and therefore land on
    /// one strand, and `execute(RunReportJob)` returns the already-terminal
    /// status without recomputing.
    ///
    /// The internal client used to issue this pass's dispatches stays alive
    /// until every dispatched `RunReportJob` has actually settled, success or
    /// failure -- see the implementation's own comment for why deregistering
    /// it any earlier would race the backend's still-pending dispatches and
    /// silently drop the pass.
    void runPendingReportsOnce();

    /// @brief Whether any `RunReportJob` dispatched by a previous
    ///        `runPendingReportsOnce()` has not settled yet.
    ///
    /// The settle seam a test -- or a shutting-down server -- needs before
    /// letting an `App` go, identical in contract to
    /// `bookmarks::app::App::fetchInFlight()` and
    /// `pastebin::app::App::sweepInFlight()`. Observing the *effect* of a
    /// pass (the job rows are `Done`) is not the same as the dispatches
    /// having settled: the aggregation happens on a worker thread while each
    /// call's completion callback is delivered later, on the Qt event loop.
    /// Destroying the `App` in that window leaves those callbacks queued
    /// against objects it owned. Pump on this until it is `false`, then
    /// destroy.
    ///
    /// A teardown that does *not* wait is still safe for the data -- the
    /// aggregation either committed or it did not, and an uncommitted job is
    /// simply still `Pending` for the next process to pick up. What it is not
    /// safe for is this object's own callbacks.
    /// @return `true` while at least one dispatched `RunReportJob` is
    ///         outstanding.
    [[nodiscard]] bool reportsInFlight() const noexcept { return _reportsInFlight->load() != 0; }

private:
    // Declaration order is load-bearing, and `_reportExecutor` comes first on
    // purpose -- the identical hazard pastebin::app::App documents at length.
    // Members are destroyed in reverse, so this is the *last* thing to go. A
    // pass's RunReportJob runs on `_pool`, and the worker thread that
    // finishes it resolves the completion by calling `post()` on the executor
    // the call was issued with. With the executor declared after the pool
    // (its natural reading order), `~App` would destroy it while pool threads
    // were still finishing dispatched work, and the next completion to
    // resolve would post through a dangling `IExecutor*`. Destroying `_pool`
    // (whose destructor joins its threads, so every in-flight completion has
    // resolved) before the executor closes that window. `QtExecutor` holds no
    // state and queues onto `QCoreApplication`, so callbacks it has already
    // posted stay safe after `App` is gone.
    ::morph::qt::QtExecutor _reportExecutor;
    /// Outstanding dispatches from `runPendingReportsOnce()`. A `shared_ptr`
    /// so the completion callbacks that decrement it hold it by value rather
    /// than through `this` -- a callback delivered after the `App` is gone
    /// (the very case `reportsInFlight()` exists to let callers avoid) must
    /// not touch a destroyed member.
    std::shared_ptr<std::atomic<int>> _reportsInFlight{std::make_shared<std::atomic<int>>(0)};
    ::morph::exec::ThreadPoolExecutor _pool;
    ::morph::bridge::Bridge _reportBridge;
    QTimer _reportTimer;
};

}  // namespace ledger::app
