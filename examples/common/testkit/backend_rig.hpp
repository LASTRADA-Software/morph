// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QUrl>
#include <chrono>
#include <functional>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <morph/session/session.hpp>
#include <optional>
#include <stdexcept>
#include <vector>

/// @file
/// The dual/triple-mode fixture (examples/TESTING.md, "The dual-mode
/// fixture"): one test body, parameterized by Catch2 GENERATE over Mode, runs
/// against every deployment shape the ladder ships.

namespace morph::ladder::testkit {

namespace detail {

/// @brief Wraps a `MainThreadExecutor` so every `post()` also schedules a
///        same-loop-iteration `runFor()` via a zero-delay `QTimer`.
///
/// `pump.hpp`'s `pumpUntil`/`awaitQt` only pump the Qt event loop
/// (`QCoreApplication::processEvents()`) — they never call
/// `MainThreadExecutor::runFor()`. A `BackendRig` in `Mode::LocalSingleThread`
/// posts model work onto a `MainThreadExecutor` (via `LocalBackend`'s strand);
/// without something draining that queue, a test doing
/// `awaitQt(handler.execute(...))` against that mode would hang forever, since
/// nothing ever runs the posted task. This adapter closes that gap: every
/// `post()` both enqueues the task on the wrapped `MainThreadExecutor` *and*
/// arranges for it (and anything it, in turn, posts — e.g. the `Completion`
/// callback delivered through this same executor) to drain the next time the
/// Qt event loop turns, which `pumpUntil`'s `processEvents()` loop already
/// does. This makes `LocalSingleThread` mode drain through the testkit's
/// existing pumping discipline instead of requiring a caller to manually call
/// `MainThreadExecutor::runFor()` the way bank's test harness does today
/// (`bank_test_support.hpp`'s `await()`/`waitUntil()`). It is also the closer
/// analogue to real WASM: under Emscripten the browser's own event loop drives
/// posted work, not a manually-polled loop.
class QtDrivenMainThreadExecutor : public ::morph::exec::IExecutor {
public:
    /// @brief Enqueues @p task and schedules a drain on the Qt event loop.
    ///
    /// The drain lambda holds a `weak_ptr` to `_liveness` and touches nothing
    /// else until it locks — never a bare `this`. A zero-delay
    /// `QTimer::singleShot` is a *posted Qt event*, and nothing cancels it
    /// when this executor dies: a `BackendRig` in `Mode::LocalSingleThread`
    /// is routinely destroyed with one still in flight (the last completion
    /// callback of a test case posts, the test body returns, the rig
    /// unwinds), and the event then fires the next time *anything* spins the
    /// Qt loop — the very next `BackendRig{Mode::Socket, ...}`'s
    /// `waitForConnected()`, or `~QtWebSocketBackend`'s own
    /// `processEvents()`, both of which happen inside a Catch2 `GENERATE`
    /// matrix's following iteration. Without the guard, that stale event
    /// reached `MainThreadExecutor::runFor()` on freed storage and threw
    /// `std::system_error{"mutex lock failed: Invalid argument"}` out of a Qt
    /// event handler, which Qt turns into an immediate `abort()` — surfacing
    /// as an intermittent "Subprocess aborted" attributed to whichever test
    /// case happened to be running, never to the one that left the event
    /// behind. Observed in practice on rung 1's QML-adapter suite, reliably
    /// under CPU load, roughly one run in fifty without it.
    /// Same `_liveness`/`weak_ptr` shape `morph::bridge::Bridge` uses for the
    /// identical hazard (`include/morph/core/bridge.hpp`).
    /// @param task Callable to execute on the next event-loop turn.
    void post(std::function<void()> task) override {
        _inner.post(std::move(task));
        QTimer::singleShot(0, [this, weakLiveness = std::weak_ptr<const void>{_liveness}] {
            if (weakLiveness.expired()) {
                return;  // This executor is gone; `this` is dangling.
            }
            _inner.runFor(kDrainBudget);
        });
    }

private:
    // A strictly-zero budget cannot pop anything: MainThreadExecutor::runFor()
    // computes `deadline = now() + timeout` once and loops `while (now() <
    // deadline)`; with `timeout == 0` that comparison is already false by the
    // time it is evaluated (two `steady_clock::now()` calls never return the
    // same instant on real hardware), so the task just posted would never run
    // and this adapter would hang exactly like the raw `MainThreadExecutor` it
    // replaces. A small positive budget gives the loop at least one chance to
    // observe the non-empty queue and drain it — and, transitively, anything a
    // drained task posts back onto this same executor (e.g. a `Completion`
    // resolving and posting its `.then()` callback), since that repost lands
    // in the same queue this call is still draining.
    static constexpr std::chrono::milliseconds kDrainBudget{5};

    ::morph::exec::MainThreadExecutor _inner;
    // Destroyed with this object; a still-pending drain lambda's weak_ptr
    // then expires and the lambda returns without touching `_inner`. Declared
    // last so it is destroyed *first* — before `_inner`, whose mutex is the
    // storage the stale lambda used to reach.
    std::shared_ptr<const void> _liveness{std::make_shared<char>()};
};

/// @brief Throws if `_wsServer->listen()` failed, otherwise a no-op.
///
/// Factored out of `Socket` mode's constructor branch so the decision is
/// directly testable with a plain `bool` — forcing a *real* ephemeral-port
/// `listen()` failure deterministically (without flakiness, and without
/// adding a test-only seam to `QtWebSocketServer` itself) isn't practically
/// achievable, so the throw logic is what gets tested instead of the real
/// I/O call. Called with the true result at the real call site, which is now
/// a trivial, branch-free line.
/// @param listenSucceeded The real `listen()` call's result.
/// @throws std::runtime_error if @p listenSucceeded is `false`.
inline void throwIfListenFailed(bool listenSucceeded) {
    if (!listenSucceeded) {
        throw std::runtime_error("BackendRig: QtWebSocketServer failed to listen");
    }
}

/// @brief Throws if a client's `waitForConnected()` failed, otherwise a no-op.
///
/// Same rationale as `throwIfListenFailed` — see its doc comment.
/// @param connected The real `waitForConnected()` call's result.
/// @throws std::runtime_error if @p connected is `false`.
inline void throwIfConnectFailed(bool connected) {
    if (!connected) {
        throw std::runtime_error("BackendRig: client failed to connect");
    }
}

}  // namespace detail

/// @brief Selects which of the three deployment shapes a `BackendRig` builds.
enum class Mode {
    /// One `ThreadPoolExecutor{4}`, one `Bridge{LocalBackend}` shared by every
    /// "client" — morph's in-process multi-handler semantics.
    Local,
    /// `LocalBackend` running models on the GUI executor itself: the WASM
    /// constraint-parity mode (single-threaded, matches bank's
    /// `__EMSCRIPTEN__` wiring).
    LocalSingleThread,
    /// `ThreadPoolExecutor{2-4}` -> `RemoteServer` -> `QtWebSocketServer` on
    /// an ephemeral port; each client is its own `QtWebSocketBackend` +
    /// `Bridge` over a real loopback socket.
    Socket,
};

/// @brief Owns the executors/backend/server for one test's worth of clients.
///
/// Teardown order: the test's own presenters/handlers go first (they are the
/// caller's locals, destroyed before this rig). Then `~BackendRig()` runs
/// `wsServer.closeGracefully(2s)` explicitly *before* any member is
/// destroyed, so the socket server stops accepting/serving while its clients
/// are still fully alive; member destruction then unwinds in reverse
/// declaration order (client bridges -> socket server -> `RemoteServer` ->
/// worker pool -> client executors). The executors going **last** is the
/// load-bearing part and the reason the members are not declared in reading
/// order: a pool thread resolves a caller's `Completion` by posting on the
/// client executor, so the pool — whose destructor joins its threads — has to
/// be gone before the executor it posts to is. See the member-declaration
/// comment below for the full rationale.
class BackendRig {
public:
    /// @brief Builds the fixture for @p mode with @p nClients clients.
    ///
    /// @param mode       Deployment shape to build.
    /// @param nClients   Number of clients `client<Model>()` will hand out.
    ///                   `Local`/`LocalSingleThread` ignore this beyond
    ///                   accepting it — every client shares the one `Bridge`
    ///                   built here, so there is nothing to construct per
    ///                   client. `Socket` builds exactly `nClients`
    ///                   independent sockets/bridges.
    /// @param authorizer Optional authorizer for `Mode::Socket`'s
    ///                   `RemoteServer`; ignored by the other two modes.
    /// @param serverConfig Per-connection resource limits for `Mode::Socket`'s
    ///                   `QtWebSocketServer` (frame-size cap, connection cap,
    ///                   rate limit, timeouts); ignored by the other two
    ///                   modes, which run no server. Defaults to
    ///                   `QtWebSocketServerConfig{}` — i.e. exactly the
    ///                   unconfigured server this rig has always built. A
    ///                   rung testing transport-enforced limits (pastebin's
    ///                   size-limit UX case, which needs a small
    ///                   `maxMessageBytes`) configures it here rather than
    ///                   standing up its own server alongside the rig.
    BackendRig(Mode mode, std::size_t nClients, std::shared_ptr<::morph::session::IAuthorizer> authorizer = nullptr,
               ::morph::qt::QtWebSocketServerConfig serverConfig = ::morph::qt::QtWebSocketServerConfig{})
        : _mode{mode} {
        switch (mode) {
            case Mode::Local: {
                _workerPool = std::make_unique<::morph::exec::ThreadPoolExecutor>(4);
                // The pool backs the *models* (LocalBackend's strands run
                // there); client-facing Completion callbacks must not. A
                // ThreadPoolExecutor here would deliver .then/.onError on a
                // pool thread, racing pump.hpp's pumpUntil/awaitQt (which
                // read the resolved state from the Qt thread with no
                // synchronization) and any Presenter built over this rig.
                // QtExecutor puts every callback back on the one Qt thread —
                // the same choice AppContext makes in both its modes, and
                // what examples/TESTING.md's "all clients on the one Qt main
                // thread" description of Local mode already claims.
                _qtExecutor = std::make_unique<::morph::qt::QtExecutor>();
                _clientExecutor = _qtExecutor.get();
                auto backend = std::make_unique<::morph::backend::LocalBackend>(*_workerPool);
                // All "clients" share one bridge in Local mode — there is
                // deliberately no per-client isolation here (see
                // examples/TESTING.md's convergence honesty note: Local mode
                // has no staleness to converge from). No construction loop is
                // needed: client<Model>(index) hands every index the same
                // Bridge built here regardless of nClients' value.
                _sharedLocalBridge = std::make_unique<::morph::bridge::Bridge>(std::move(backend));
                break;
            }
            case Mode::LocalSingleThread: {
                _mainThreadExecutor = std::make_unique<detail::QtDrivenMainThreadExecutor>();
                _clientExecutor = _mainThreadExecutor.get();
                auto backend = std::make_unique<::morph::backend::LocalBackend>(*_mainThreadExecutor);
                _sharedLocalBridge = std::make_unique<::morph::bridge::Bridge>(std::move(backend));
                break;
            }
            case Mode::Socket: {
                _workerPool = std::make_unique<::morph::exec::ThreadPoolExecutor>(4);
                if (authorizer) {
                    _server = std::make_shared<::morph::backend::RemoteServer>(*_workerPool, authorizer);
                } else {
                    _server = std::make_shared<::morph::backend::RemoteServer>(*_workerPool);
                }
#ifdef QT_NO_SSL
                _wsServer =
                    std::make_unique<::morph::qt::QtWebSocketServer>(*_server, quint16{0}, std::move(serverConfig));
#else
                _wsServer = std::make_unique<::morph::qt::QtWebSocketServer>(*_server, quint16{0}, std::nullopt,
                                                                             std::move(serverConfig));
#endif
                detail::throwIfListenFailed(_wsServer->listen());
                _qtExecutor = std::make_unique<::morph::qt::QtExecutor>();
                _clientExecutor = _qtExecutor.get();
                _url = QUrl{QString("ws://127.0.0.1:%1").arg(_wsServer->port())};
                for (std::size_t i = 0; i < nClients; ++i) {
                    auto backend = std::make_unique<::morph::qt::QtWebSocketBackend>(_url);
                    detail::throwIfConnectFailed(backend->waitForConnected());
                    // The Bridge below takes ownership; this non-owning
                    // pointer is what `socketBackend()` hands back, so a test
                    // can reach transport-level operations that have no
                    // Bridge-level equivalent (`negotiateProtocolVersion()`).
                    _socketBackends.push_back(backend.get());
                    _socketBridges.push_back(std::make_unique<::morph::bridge::Bridge>(std::move(backend)));
                }
                break;
            }
            default:
                // Every Mode enumerator has its own case above, so this is
                // unreachable in correct code — present only because
                // -Wswitch-default (unlike Clang's -Wcovered-switch-default,
                // suppressed project-wide for exactly this collision — see
                // cmake/compiler_options.cmake's own note) still demands an
                // explicit default even on a fully-covered switch. Throws
                // rather than silently doing nothing, so a future Mode value
                // reaching here from outside (a stray static_cast, memory
                // corruption) fails loudly instead of constructing a
                // half-initialized rig.
                throw std::logic_error{"BackendRig: unknown Mode"};
        }
    }

    BackendRig(const BackendRig&) = delete;
    BackendRig& operator=(const BackendRig&) = delete;
    BackendRig(BackendRig&&) = delete;
    BackendRig& operator=(BackendRig&&) = delete;

    /// @brief Teardown order: gracefully close the socket server (if any),
    ///        join the worker pool early, drain the Qt event loop, then let
    ///        ordinary member destruction finish (client executors last).
    ///
    /// The member-declaration comment below documents why `_workerPool` (its
    /// destructor joins every worker thread) must be gone before the client
    /// executors (`_qtExecutor`/`_mainThreadExecutor`) are — reverse
    /// declaration order already guarantees that ordering. What plain member
    /// destruction cannot guarantee is that every task the pool ran had
    /// *finished being delivered* by the time the executor it posted to dies:
    /// `QtExecutor::post()` (`morph/qt/qt_executor.hpp`) uses
    /// `Qt::QueuedConnection` — it enqueues the callback on the Qt event loop
    /// and returns immediately, it does not run it. A pool thread's task can
    /// itself resolve a `Completion` whose continuation chains a *second*,
    /// nested `post()` (`morph::bridge::Bridge::executeVia`'s
    /// `.then`/`.onError` pair, `morph/core/bridge.hpp`, each wrapping a
    /// `morph::async::detail::CompletionState` that posts again from inside
    /// the first posted callback) — so "the pool has joined" only proves
    /// every `post()` call was *made*, not that every resulting event was
    /// *pumped*. A caller's own `pumpUntil` (tracking only its own top-level
    /// completions, e.g. test_kanban_stress.cpp's `outstanding` counter) has
    /// no visibility into that inner, nested completion, so it can observe
    /// "done" and let this rig tear down while a second post from that
    /// nested continuation is still sitting, undelivered, in the Qt event
    /// queue. When it finally runs, it touches an already-freed
    /// `QtExecutor`/`CompletionState` — a heap-use-after-free ThreadSanitizer
    /// catches reliably and an uninstrumented build sometimes just segfaults
    /// on instead. Resetting `_workerPool` explicitly here (rather than
    /// waiting for its turn in the member-destruction sequence) forces every
    /// pool-issued `post()` to have happened *before* the drain below runs,
    /// so the drain has something to actually flush; skipping the reset and
    /// draining first would race the still-running pool threads themselves.
    ~BackendRig() {
        if (_wsServer) {
            _wsServer->closeGracefully(std::chrono::milliseconds{2000});
        }
        // Join the pool now (not at its natural place in member-destruction
        // order) so every task it ran -- and every post() that task made,
        // including a nested one chained from inside another posted callback
        // -- has already happened by the time the drain below runs.
        _workerPool.reset();
        // No event-loop drain here any more. This used to spin
        // processEvents() for a fixed number of slices to flush posts queued
        // by those tasks before any client executor was freed -- a workaround
        // for morph#127, where a queued task delivered after its QtExecutor
        // died called post() on the freed executor. `QtExecutor` now drops a
        // task whose executor is already gone, so the hazard is closed in the
        // framework rather than worked around in this one fixture. A fixed
        // slice count was never a proof anyway, only a "probably enough".
    }

    [[nodiscard]] Mode mode() const { return _mode; }

    /// @brief Returns the @p index'th client's `BridgeHandler<Model>`.
    ///
    /// `Local`/`LocalSingleThread`: every index shares the one `Bridge`
    /// (morph's in-process multi-handler semantics — the handler itself is
    /// still per-call, constructed fresh here). `Socket`: each index owns its
    /// own `Bridge` over its own socket.
    /// @tparam Model Concrete model type to bind the handler to.
    /// @param index  Client index in `[0, nClients)`.
    /// @return A fresh `BridgeHandler<Model>` bound to this client's bridge.
    template <typename Model>
    ::morph::bridge::BridgeHandler<Model> client(std::size_t index) {
        if (_mode == Mode::Socket) {
            if (index >= _socketBridges.size()) {
                throw std::out_of_range("BackendRig::client: index beyond nClients");
            }
            return ::morph::bridge::BridgeHandler<Model>{*_socketBridges[index], _clientExecutor};
        }
        return ::morph::bridge::BridgeHandler<Model>{*_sharedLocalBridge, _clientExecutor};
    }

    /// @brief Returns the @p index'th client's `Bridge`.
    ///
    /// The composability half of `client<Model>()`: a `Presenter` subclass
    /// takes `(Bridge&, IExecutor*)` and builds its own handlers, so a rung's
    /// presenter tests need the raw bridge, not a pre-bound handler. Mode
    /// dispatch mirrors `client<Model>()` exactly.
    ///
    /// @param index Client index in `[0, nClients)`; ignored in
    ///        `Local`/`LocalSingleThread`, where every client shares one
    ///        `Bridge`.
    /// @return Reference to that client's bridge, owned by this rig.
    /// @throws std::out_of_range in `Socket` mode if @p index >= nClients.
    [[nodiscard]] ::morph::bridge::Bridge& bridge(std::size_t index) {
        if (_mode == Mode::Socket) {
            if (index >= _socketBridges.size()) {
                throw std::out_of_range("BackendRig::bridge: index beyond nClients");
            }
            return *_socketBridges[index];
        }
        return *_sharedLocalBridge;
    }

    /// @brief Returns the @p index'th client's raw `QtWebSocketBackend`.
    ///
    /// Deliberately narrow: `Bridge` is the ordinary seam, and every test that
    /// only dispatches actions should use `client<Model>()`/`bridge()`
    /// instead. A handful of transport-level operations have no Bridge-level
    /// equivalent at all — `negotiateProtocolVersion()` (the `hello`
    /// handshake, which pastebin's protocol-negotiation case exercises) is the
    /// motivating one — and reaching them otherwise would mean a test
    /// standing up a second socket alongside the rig's own, testing a
    /// connection the rig never built.
    ///
    /// @param index Client index in `[0, nClients)`.
    /// @return Reference to that client's backend, owned by the corresponding
    ///         `Bridge` (which is owned by this rig).
    /// @throws std::logic_error in `Local`/`LocalSingleThread` — those modes
    ///         run no socket and have no such backend.
    /// @throws std::out_of_range in `Socket` mode if @p index >= nClients.
    [[nodiscard]] ::morph::qt::QtWebSocketBackend& socketBackend(std::size_t index) {
        if (_mode != Mode::Socket) {
            throw std::logic_error(
                "BackendRig::socketBackend: only Mode::Socket runs over a socket; there is no backend in this mode");
        }
        if (index >= _socketBackends.size()) {
            throw std::out_of_range("BackendRig::socketBackend: index beyond nClients");
        }
        return *_socketBackends[index];
    }

    /// @brief The executor every client's callbacks are delivered on.
    ///
    /// The second half of a presenter's `(Bridge&, IExecutor*)` pair. A
    /// `QtExecutor` in `Local`/`Socket`, the Qt-driven `MainThreadExecutor`
    /// adapter in `LocalSingleThread` — all three deliver on the Qt thread,
    /// which is what makes `pump.hpp`'s wait primitives sound.
    /// @return Non-owning pointer to the rig's client-facing executor.
    [[nodiscard]] ::morph::exec::IExecutor* executor() const { return _clientExecutor; }

    /// @brief The loopback URL clients connect to, for building an extra
    ///        client (e.g. an `AppContext{Remote{rig.url()}}`) against this
    ///        rig's server.
    /// @return `ws://127.0.0.1:<ephemeral port>`.
    /// @throws std::logic_error in `Local`/`LocalSingleThread` — those modes
    ///         run no server and have no URL to hand out.
    [[nodiscard]] QUrl url() const {
        if (_mode != Mode::Socket) {
            throw std::logic_error("BackendRig::url: only Mode::Socket runs a server; there is no URL in this mode");
        }
        return _url;
    }

private:
    Mode _mode;
    ::morph::exec::IExecutor* _clientExecutor{nullptr};

    // Declared in reverse teardown order, and the client-facing executors
    // come first on purpose: members are destroyed in reverse, so they are
    // the *last* things to go.
    //
    // In `Mode::Local` a model runs on `_workerPool`, and the pool thread
    // that finishes it resolves the caller's `Completion` by calling `post()`
    // on `_clientExecutor`. With the executor declared before the pool (its
    // natural reading order), `~BackendRig` destroyed it while pool threads
    // were still finishing dispatched work, and the next completion to
    // resolve posted through a dangling `IExecutor*`. That crashes nowhere
    // near the rig — the stale callback lands on the Qt event loop and
    // detonates inside whatever later test happens to pump it, which is
    // exactly how it presented (intermittent SIGSEGVs scattered across
    // pastebin's socket cases). Destroying `_workerPool` — which joins its
    // threads, so every in-flight completion has resolved — before the
    // executors closes that window. `QtExecutor` is stateless and queues onto
    // `QCoreApplication`, so callbacks it has already posted stay safe after
    // the rig is gone.
    std::unique_ptr<::morph::qt::QtExecutor> _qtExecutor;                     // Local / Socket
    std::unique_ptr<detail::QtDrivenMainThreadExecutor> _mainThreadExecutor;  // LocalSingleThread
    std::unique_ptr<::morph::exec::ThreadPoolExecutor> _workerPool;           // Local / Socket
    std::shared_ptr<::morph::backend::RemoteServer> _server;                  // Socket
    std::unique_ptr<::morph::qt::QtWebSocketServer> _wsServer;                // Socket
    std::unique_ptr<::morph::bridge::Bridge> _sharedLocalBridge;              // Local / LocalSingleThread
    std::vector<std::unique_ptr<::morph::bridge::Bridge>> _socketBridges;     // Socket
    // Non-owning, parallel to _socketBridges: each entry is the backend the
    // bridge at the same index owns. Declared *after* _socketBridges so it is
    // destroyed first — it must never outlive the objects it points at.
    std::vector<::morph::qt::QtWebSocketBackend*> _socketBackends;  // Socket
    QUrl _url;                                                      // Socket
};

}  // namespace morph::ladder::testkit
