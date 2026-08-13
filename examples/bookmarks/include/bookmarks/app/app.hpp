// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bookmarks/app/metadata_fetcher.hpp"

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
#include <string>

namespace bookmarks::app {

/// @brief Owns the server-side pieces every bookmarks deployment shares: the
/// worker pool, the `RemoteServer` with a real `auth::BookmarksAuthorizer`
/// installed, the durable `FileActionLog` (installed process-wide via
/// `morph::journal::setActionLog`), the process-global `TokenIssuer`
/// `AuthModel` mints from (`auth::setTokenIssuer`), the periodic
/// metadata-fetch worker, and the periodic outbox relay. Nothing here decides
/// deployment mode — that stays `examples/common/gui::AppContext`'s job on
/// the client side; this is exclusively the server side.
///
/// Mirrors `pastebin::app::App` (rung 1) closely and on purpose, including
/// its declaration-order-for-teardown-safety rule (see the private section)
/// and its internal-client pattern for background work: the metadata-fetch
/// worker dispatches `RecordMetadata` through a `Bridge` over
/// `SimulatedRemoteBackend{*server()}`, a first-class client of the same
/// `RemoteServer` a real socket client talks to
/// (`SimulatedRemoteBackend::execute()` calls `RemoteServer::handle()`, the
/// identical dispatch path), so every recorded fetch is authorized,
/// dispatched and journaled exactly like a client-issued action.
///
/// @par The service principal, and why the worker's own instance is enough
/// The worker's bridge carries a default session holding a token this `App`
/// minted for `auth::kMetadataFetcherPrincipal` with the *same secret* it
/// gave the authorizer, so it verifies exactly like a real user's. It runs on
/// its own `BridgeHandler<BookmarkModel>` — its own registered instance,
/// created by and attributed to itself — never on some user's instance, so
/// per-instance authorization has nothing to object to. What actually keeps
/// the worker's extra authority in bounds is
/// `BookmarkModel::execute(const RecordMetadata&)`'s own check that the
/// dispatching principal *is* the service principal, plus
/// `AuthModel`'s refusal to mint a token in the reserved `system:` namespace
/// on request. `authorizeInstance` could not have done that job here even
/// with the worker's instance recording a real owner (which register
/// envelopes now carry, unlike when this was first written): it compares
/// instance ownership, not row ownership, and the worker's own instance is
/// exactly what it is authorized to use — see
/// `bookmarks/auth/bookmarks_authorizer.hpp`'s `authorizeInstance` doc
/// comment.
class App : public QObject {
    Q_OBJECT
  public:
    /// @brief Wires up the whole server side and starts both periodic timers.
    /// @param actionLogPath Where `FileActionLog` persists entries.
    /// @param tokenSecret   Shared secret for the `auth::BookmarksAuthorizer`
    ///        this server installs, for the process-global `TokenIssuer`
    ///        `AuthModel` mints user tokens from, and for the
    ///        metadata-fetch worker's own service-principal token. All three
    ///        must be the same value, which is why there is one parameter:
    ///        a token minted by any of them has to verify against the
    ///        authorizer that checks every subsequent call.
    /// @param fetcher       Metadata fetch implementation; defaults to
    ///        `NullMetadataFetcher` (no network, no I/O at all).
    /// @param fetchInterval How often the metadata-fetch worker runs. Tests
    ///        pass a long interval (effectively disabling the timer) and call
    ///        `fetchMetadataOnce()` directly instead, for determinism.
    /// @param relayInterval How often the outbox relay runs. Same testing
    ///        convention as @p fetchInterval.
    /// @param workers       Size of the model worker pool.
    /// @param parent        Optional `QObject` parent.
    explicit App(std::filesystem::path actionLogPath, std::string tokenSecret,
                 std::shared_ptr<IBookmarkMetadataFetcher> fetcher = std::make_shared<NullMetadataFetcher>(),
                 std::chrono::milliseconds fetchInterval = std::chrono::seconds{5},
                 std::chrono::milliseconds relayInterval = std::chrono::seconds{2}, std::size_t workers = 4,
                 QObject* parent = nullptr);

    /// @brief Stops both timers and detaches the process-wide action log and
    ///        token issuer.
    ~App() override;

    /// @brief Stops both periodic timers, so nothing this `App` owns can
    ///        dispatch new work from now on.
    ///
    /// `~App` calls this too, so an owner that never calls it sees exactly the
    /// previous behavior. It is public because a *shutting-down* owner has to
    /// call it earlier than that: the settle contract on `fetchInFlight()`
    /// below says "pump until it is `false`, then destroy", and pumping is
    /// precisely what lets `_fetchTimer` tick. A drain loop that ran with the
    /// timer still armed could therefore dispatch a brand-new `RecordMetadata`
    /// pass out of its own `processEvents()` call, re-raising `fetchInFlight()`
    /// after it had settled — and if that late pass is still outstanding when
    /// the drain's budget expires, `~App` runs with a dispatch in flight, which
    /// is the exact window the drain exists to close. Calling this first makes
    /// the drain monotonic: the outstanding set can only shrink.
    ///
    /// Idempotent (`QTimer::stop()` on a stopped timer is a no-op) and safe to
    /// call from the Qt thread at any point in the object's life.
    void stopBackgroundJobs();

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    /// @brief The server every transport (a `QtWebSocketServer`, a test's
    ///        `BackendRig`) wraps or dispatches against.
    /// @return The shared `RemoteServer`; never null.
    [[nodiscard]] std::shared_ptr<::morph::backend::RemoteServer> server() const noexcept { return _server; }

    /// @brief Runs one metadata-fetch pass right now: finds every bookmark
    ///        (across every owner) whose title is still empty, calls the
    ///        injected fetcher for each, and fire-and-forget dispatches
    ///        `RecordMetadata` through the internal client.
    ///
    /// Does not block on the dispatched calls settling — callers that need to
    /// observe completion (tests, shutdown) pump the Qt event loop afterward
    /// (`morph::ladder::testkit::pumpUntil`) on `fetchInFlight()`.
    ///
    /// The internal client used to issue this pass's dispatches stays alive
    /// until every dispatched `RecordMetadata` has actually settled, success
    /// or failure — see the implementation's own comment for why
    /// deregistering it any earlier would race `RemoteServer`'s still-pending
    /// dispatch and silently drop the pass.
    void fetchMetadataOnce();

    /// @brief Whether any `RecordMetadata` dispatched by a previous
    ///        `fetchMetadataOnce()` has not settled yet.
    ///
    /// The settle seam a test needs before letting an `App` go, identical in
    /// contract to `pastebin::app::App::sweepInFlight()`: observing the
    /// *effect* of a pass (the titles are set) is not the same as the
    /// dispatches having settled, because the update happens on a worker
    /// thread while each call's completion callback is delivered later, on
    /// the Qt event loop. Destroying the `App` in that window leaves those
    /// callbacks queued against objects it owned. Pump on this until it is
    /// `false`, then destroy.
    /// @return `true` while at least one dispatched `RecordMetadata` is outstanding.
    [[nodiscard]] bool fetchInFlight() const noexcept { return _fetchInFlight->load() != 0; }

    /// @brief Drains `bookmark_outbox` into the durable action log via
    ///        `journal::OutboxRelay`, once, right now.
    ///
    /// Synchronous, so it needs no in-flight seam of its own: it touches the
    /// database and the log directly rather than dispatching through the
    /// server. Both `BookmarkModel::execute(const BulkEdit&)` and
    /// `TagModel`'s `RenameTag`/`MergeTags` write into that one table, so one
    /// relay covers both models.
    /// @return The number of outbox rows relayed in this pass.
    std::size_t relayOutboxOnce();

  private:
    // Declaration order is load-bearing, and `_fetchExecutor` comes first on
    // purpose — the identical hazard pastebin::app::App documents at length.
    // Members are destroyed in reverse, so this is the *last* thing to go. A
    // pass's RecordMetadata runs on `_pool`, and the worker thread that
    // finishes it resolves the completion by calling `post()` on the executor
    // the call was issued with. With the executor declared after the pool
    // (its natural reading order), `~App` would destroy it while pool threads
    // were still finishing dispatched work, and the next completion to
    // resolve would post through a dangling `IExecutor*`. Destroying `_pool`
    // (whose destructor joins its threads, so every in-flight completion has
    // resolved) before the executor closes that window. `QtExecutor` holds no
    // state and queues onto `QCoreApplication`, so callbacks it has already
    // posted stay safe after `App` is gone.
    ::morph::qt::QtExecutor _fetchExecutor;
    /// Outstanding dispatches from `fetchMetadataOnce()`. A `shared_ptr` so
    /// the completion callbacks that decrement it hold it by value rather
    /// than through `this` — a callback delivered after the `App` is gone
    /// (the very case `fetchInFlight()` exists to let callers avoid) must not
    /// touch a destroyed member.
    std::shared_ptr<std::atomic<int>> _fetchInFlight{std::make_shared<std::atomic<int>>(0)};
    std::shared_ptr<::morph::journal::FileActionLog> _actionLog;
    ::morph::exec::ThreadPoolExecutor _pool;
    std::shared_ptr<::morph::backend::RemoteServer> _server;
    ::morph::bridge::Bridge _fetchBridge;
    std::shared_ptr<IBookmarkMetadataFetcher> _fetcher;
    QTimer _fetchTimer;
    QTimer _relayTimer;
};

}  // namespace bookmarks::app
