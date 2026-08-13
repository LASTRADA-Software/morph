// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/journal/file_action_log.hpp>
#include <morph/qt/qt_executor.hpp>

#include <QObject>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>

namespace pastebin::app {

/// @brief Owns the server-side pieces every pastebin deployment shares: the
/// worker pool, the `RemoteServer`, the durable `FileActionLog` (installed
/// process-wide via `morph::journal::setActionLog`, so every `PasteModel`
/// instance auto-attaches — see its own doc comment), and the periodic
/// expiry sweep. Nothing here decides deployment mode (`Local`/`Remote`) —
/// that stays `examples/common/gui::AppContext`'s job on the client side;
/// this is exclusively the server side.
///
/// The expiry sweep dispatches `ExpirePaste{id}` through an **internal
/// client** — a `Bridge` over `SimulatedRemoteBackend{*server()}` — a
/// first-class client of the same `RemoteServer` a real socket client
/// talks to (`SimulatedRemoteBackend::execute()` calls
/// `RemoteServer::handle()`, the identical dispatch path), so every swept
/// expiry is authorized, dispatched, and auto-journaled exactly like a
/// client-issued action. See `examples/pastebin/README.md`'s "How does
/// expiry replay?" for the full rationale, including why sweep *timing*
/// does not affect correctness (`PasteModel::execute(GetPaste)`'s own
/// atomic update already excludes an expired row on its own).
class App : public QObject {
    Q_OBJECT
  public:
    /// @param actionLogPath Where `FileActionLog` persists entries.
    /// @param sweepInterval How often the expiry sweep runs. Tests pass a
    ///        long interval (effectively disabling the timer) and call
    ///        `sweepExpiredOnce()` directly instead, for determinism.
    /// @param workers        Size of the model worker pool.
    /// @param parent         Optional `QObject` parent.
    explicit App(std::filesystem::path actionLogPath, std::chrono::milliseconds sweepInterval = std::chrono::seconds{5},
                 std::size_t workers = 4, QObject* parent = nullptr);

    /// @brief Detaches the process-wide default action log.
    ~App() override;

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    /// @brief The server every transport (a `QtWebSocketServer`, a test's
    ///        `BackendRig`) wraps or dispatches against.
    [[nodiscard]] std::shared_ptr<::morph::backend::RemoteServer> server() const noexcept { return _server; }

    /// @brief Runs one expiry sweep pass right now: finds every paste whose
    ///        `expires_at_ms` has passed and fire-and-forget dispatches
    ///        `ExpirePaste` for each through the internal client. Does not
    ///        block on the dispatched calls settling — callers that need
    ///        to observe completion (tests) pump the Qt event loop
    ///        afterward (`morph::ladder::testkit::pumpUntil`).
    ///
    /// The internal client used to issue this pass's dispatches stays alive
    /// (via a lifetime extended past this call) until every dispatched
    /// `ExpirePaste` has actually settled, success or failure — see the
    /// implementation's doc comment for why deregistering it any earlier
    /// would race `RemoteServer`'s still-pending dispatch and silently drop
    /// the reclaim for this pass.
    void sweepExpiredOnce();

    /// @brief Whether any `ExpirePaste` dispatched by a previous
    ///        `sweepExpiredOnce()` has not settled yet.
    ///
    /// The settle seam a test needs before letting an `App` go, mirroring
    /// `Presenter::busy()`. Observing the *effect* of a sweep (the rows are
    /// gone) is not the same as the dispatches having settled: the reclaim
    /// happens on a worker thread, while each call's completion callback is
    /// delivered later, on the Qt event loop. Destroying the `App` in that
    /// window leaves those callbacks queued against objects it owned, and
    /// they detonate whenever some later `processEvents()` gets to them —
    /// which is nowhere near the code that caused it. Pump on this until it
    /// is `false`, then destroy.
    /// @return `true` while at least one dispatched `ExpirePaste` is
    ///         outstanding.
    [[nodiscard]] bool sweepInFlight() const noexcept { return _sweepInFlight->load() != 0; }

  private:
    // Declaration order is load-bearing, and `_sweepExecutor` comes first on
    // purpose: members are destroyed in reverse, so this is the *last* thing
    // to go. A sweep's `ExpirePaste` runs on `_pool`, and the worker thread
    // that finishes it resolves the completion by calling `post()` on the
    // executor the call was issued with. With the executor declared after the
    // pool (its natural reading order), `~App` destroyed it while pool
    // threads were still finishing dispatched sweeps, and the next completion
    // to resolve posted through a dangling `IExecutor*` — an intermittent
    // segfault, reproduced by this rung's sweep tests, in whichever test
    // happened to be running when the late completion landed. Destroying
    // `_pool` (which joins its threads, so every in-flight completion has
    // resolved) before the executor closes that window. `QtExecutor` itself
    // holds no state and queues onto `QCoreApplication`, so the callbacks it
    // has already posted stay safe after `App` is gone.
    ::morph::qt::QtExecutor _sweepExecutor;
    /// Outstanding dispatches from `sweepExpiredOnce()`. A `shared_ptr` so the
    /// completion callbacks that decrement it hold it by value rather than
    /// through `this` — a callback delivered after the `App` is gone (the very
    /// case `sweepInFlight()` exists to let callers avoid) must not touch a
    /// destroyed member.
    std::shared_ptr<std::atomic<int>> _sweepInFlight{std::make_shared<std::atomic<int>>(0)};
    std::shared_ptr<::morph::journal::FileActionLog> _actionLog;
    ::morph::exec::ThreadPoolExecutor _pool;
    std::shared_ptr<::morph::backend::RemoteServer> _server;
    ::morph::bridge::Bridge _sweepBridge;
    QTimer _sweepTimer;
};

}  // namespace pastebin::app
