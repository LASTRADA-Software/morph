// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QEventLoop>
#ifndef QT_NO_SSL
#include <QSslConfiguration>
#endif
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <chrono>
#include <functional>
#include <morph/core/backend.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/wire.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace morph::qt {

/// @brief Reconnect tuning for `QtWebSocketBackend`.
///
/// Declared outside `QtWebSocketBackend` so its default initialisers are fully
/// parsed before any constructor default argument that names `Config{}` is
/// evaluated (same rationale as `morph::offline::NetworkMonitorConfig`).
struct QtWebSocketBackendConfig {
    /// @brief Whether to attempt automatic reconnect after an unsolicited disconnect.
    bool reconnectEnabled = true;

    /// @brief Delay before the first reconnect attempt after a disconnect.
    std::chrono::milliseconds initialReconnectDelay = std::chrono::milliseconds{500};

    /// @brief Upper bound on the exponential backoff between reconnect attempts.
    std::chrono::milliseconds maxReconnectDelay = std::chrono::seconds{30};

    /// @brief Multiplier applied to the delay after each failed attempt.
    double backoffMultiplier = 2.0;

    /// @brief Whether `bindModel`/`promoteModel` may return before the reply.
    ///
    /// Defaults to `false`: both settle their `Completion` from inside the call,
    /// having blocked the Qt thread in a nested `QEventLoop` for the round trip
    /// — `IBackend`'s own default behaviour, and what every existing embedder
    /// (a desktop Qt client, this backend's own test suite) already relies on,
    /// since it makes a handler usable on the line after `BridgeHandler`'s
    /// constructor returns.
    ///
    /// Set `true` for a build where that blocking call cannot happen at all —
    /// a WASM main thread, where Qt refuses to spin a nested loop and the
    /// attempt aborts the page outright. `bindModel` then sends the request and
    /// returns an unsettled `Completion`, which the reply settles later. That is
    /// a deliberate trade, not a free improvement: the caller must wait for the
    /// continuation (e.g. gate the UI on `BridgeHandler::whenBound()`) before
    /// firing an action through that handler, since `executeVia` fails fast
    /// with "handler not bound" for an unbound binding rather than queuing or
    /// blocking.
    ///
    /// This flag chooses *whether the transport blocks*. It is no longer an
    /// opt-in to a second set of interface verbs: the continuation exists on
    /// both paths, because `bindModel` returns a `Completion` either way.
    bool asyncRegistrationEnabled = false;
};

/// @brief `IBackend` implementation that communicates with a `RemoteServer` over WebSocket.
///
/// Registration goes through the structural surface: `bindModel()` and
/// `promoteModel()` (`IBackend`, and `docs/spec/core/backend.md`'s "The
/// structural registration surface"). This is the one backend in the tree that
/// implements them *natively* rather than through the blocking defaults or
/// `SynchronousBackendAdapter` — with `Config::asyncRegistrationEnabled` set it
/// assigns a call-id, sends the message, and returns an unsettled `Completion`
/// that the matching reply settles, using the same call-id-matching mechanism
/// `execute()` already uses. Which thread the continuation then runs on is the
/// caller's choice, not this class's: `Completion` posts to the `IExecutor`
/// passed to `bindModel`/`promoteModel`, so a caller that needs the
/// continuation on its own thread names its own executor and gets it.
///
/// The synchronous verbs (`registerModel()`, `registerModelShared()`,
/// `attachModel()`, `assignPrimary()`) remain, and still block the calling
/// thread in a nested `QEventLoop` until the server replies — unusable on a
/// WASM main thread, which Qt refuses to spin a nested loop on at all. They are
/// what the default (`asyncRegistrationEnabled == false`) `bindModel` runs, and
/// what `Bridge::switchBackend()` re-registers through.
/// `deregisterModel()` is fire-and-forget (it sends the message without
/// waiting, avoiding a nested event loop during destruction). `execute()` is
/// asynchronous: it assigns a call-id, sends the message, and resolves the
/// returned `Completion` when the matching reply arrives.
///
/// @par TLS
/// Pass a `QSslConfiguration` to enable `wss://`. Build it with `tlsVerifyingConfig()`
/// (CA-verified — the recommended production default) or `tlsPinnedConfig()`
/// (pinned-certificate — the correct choice for a self-signed deployment), both in
/// `qt_tls.hpp`. `tlsInsecureNoVerify()` disables peer verification entirely and is
/// for local development and tests only — see security.md's "Transport security" section.
///
/// @par SSL-less Qt builds (`QT_NO_SSL`, including the standard Qt-for-WebAssembly build)
/// The constructor's `tls` parameter (and the `_tls` member) does not exist at all
/// when Qt itself was configured without SSL — `QSslConfiguration` isn't a type
/// Qt provides in that configuration, so there is no value to accept or ignore.
/// `wss://` still works on such a build regardless: in a WASM/browser
/// deployment the browser terminates TLS before Qt's `QWebSocket` ever sees
/// the connection, so the only thing genuinely unavailable is the ability to
/// *configure* TLS from C++ (client certificates, pinning, etc.) — plain
/// `wss://` and `ws://` both still connect normally.
///
/// @par Threading
/// Must be used from the Qt event loop thread. `execute()` and the internal
/// message handler are both called on that thread.
class QtWebSocketBackend : public ::morph::backend::detail::IBackend {
public:
    /// @brief Alias for the reconnect configuration struct.
    using Config = QtWebSocketBackendConfig;

    /// @brief Constructs the backend and opens a WebSocket connection to @p serverUrl.
    ///
    /// @param serverUrl   `ws://` or `wss://` URL of the remote `RemoteServer`.
    /// @param dispatcher  Action dispatcher (defaults to the process-level singleton).
    /// @param registry    Model registry (defaults to the process-level singleton).
#ifndef QT_NO_SSL
    /// @param tls         If non-null, enables TLS and applies this configuration. Not
    ///                    declared at all on an SSL-less Qt build (`QT_NO_SSL`) — see
    ///                    the class doc comment's "SSL-less Qt builds" section.
#endif
    /// @param cfg         Reconnect tuning. Default: enabled, 500ms initial / 30s cap, 2x backoff.
    explicit QtWebSocketBackend(
        QUrl serverUrl,
        ::morph::model::detail::ActionDispatcher& dispatcher = ::morph::model::detail::defaultDispatcher(),
        ::morph::model::detail::ModelRegistryFactory& registry = ::morph::model::detail::defaultRegistry(),
#ifndef QT_NO_SSL
        std::optional<QSslConfiguration> tls = std::nullopt,
#endif
        Config cfg = Config{});

    /// @brief Constructs the backend without naming the dispatcher/registry pair.
    ///
    /// `QtWebSocketBackend` never actually uses its `dispatcher`/`registry`
    /// constructor parameters (model construction is delegated to the server —
    /// see `registerModel()`'s doc comment); the main constructor still accepts
    /// them, positioned before `tls`/`cfg`, purely for API-shape parity with
    /// other backends. That forces a caller who only wants to set `cfg` (e.g.
    /// `Config::asyncRegistrationEnabled`) to spell out
    /// `morph::model::detail::defaultDispatcher()`/`defaultRegistry()` explicitly
    /// to reach the parameters after them — reaching into a `detail::` namespace
    /// for no functional reason. This overload skips straight to `tls`/`cfg`.
    /// @param serverUrl `ws://` or `wss://` URL of the remote `RemoteServer`.
    /// @param tls       If non-null, enables TLS and applies this configuration. Not
    ///                  declared at all on an SSL-less Qt build (`QT_NO_SSL`) — see
    ///                  the class doc comment's "SSL-less Qt builds" section.
    /// @param cfg       Reconnect tuning. Default: enabled, 500ms initial / 30s cap, 2x backoff.
#ifndef QT_NO_SSL
    explicit QtWebSocketBackend(QUrl serverUrl, std::optional<QSslConfiguration> tls, Config cfg = Config{})
        : QtWebSocketBackend(std::move(serverUrl), ::morph::model::detail::defaultDispatcher(),
                             ::morph::model::detail::defaultRegistry(), std::move(tls), cfg) {}
#endif

    /// @brief Constructs the backend without naming the dispatcher/registry pair or TLS.
    ///
    /// See the `(serverUrl, tls, cfg)` overload's doc comment for why this
    /// exists. Equivalent to that overload with `tls = std::nullopt` on an
    /// SSL-enabled Qt build, or to the main constructor's defaults on an
    /// SSL-less (`QT_NO_SSL`) build, where there is no `tls` parameter to skip.
    /// @param serverUrl `ws://` or `wss://` URL of the remote `RemoteServer`.
    /// @param cfg       Reconnect tuning. Default: enabled, 500ms initial / 30s cap, 2x backoff.
    explicit QtWebSocketBackend(QUrl serverUrl, Config cfg)
        : QtWebSocketBackend(std::move(serverUrl), ::morph::model::detail::defaultDispatcher(),
                             ::morph::model::detail::defaultRegistry(),
#ifndef QT_NO_SSL
                             std::nullopt,
#endif
                             cfg) {
    }

    /// @brief Closes the socket and cleans up pending operations.
    ~QtWebSocketBackend() override;

    // Neither copyable nor movable: the backend owns a QWebSocket bound to this
    // object's address through Qt's signal/slot connections, and its pending-call
    // maps are keyed to callbacks that capture `this`.
    QtWebSocketBackend(const QtWebSocketBackend&) = delete;
    QtWebSocketBackend& operator=(const QtWebSocketBackend&) = delete;
    QtWebSocketBackend(QtWebSocketBackend&&) = delete;
    QtWebSocketBackend& operator=(QtWebSocketBackend&&) = delete;

    /// @brief Pumps the Qt event loop until the socket is connected or @p timeoutMs elapses.
    ///
    /// Must be called on the Qt event loop thread after construction.
    ///
    /// @param timeoutMs Maximum time to wait in milliseconds.
    /// @return `true` if connected before the timeout, `false` otherwise.
    bool waitForConnected(int timeoutMs = 5000);

    /// @brief Sends a `"hello"` envelope to the server and classifies its reply.
    ///
    /// Synchronous, like `registerModel` — blocks the calling (Qt event loop)
    /// thread via a nested `QEventLoop` until the reply arrives. Intended to be
    /// called once, after `waitForConnected()` returns `true` and before any
    /// `registerModel`/`execute` call; nothing enforces that ordering.
    ///
    /// @return `Negotiated` if the server accepted `kProtocolVersion`;
    ///         `LegacyPeer` if the server does not understand `"hello"`.
    /// @throws std::runtime_error if the server explicitly rejects the version,
    ///         or if the socket is not connected (or disconnects mid-call).
    ::morph::wire::ProtocolNegotiationResult negotiateProtocolVersion();

    /// @brief Sends a `register` message to the server and blocks until the reply arrives.
    ///
    /// @param typeId  String type-id of the model to register.
    /// @param factory Ignored — model construction is delegated to the server.
    /// @return `ModelId` assigned by the server.
    /// @throws std::runtime_error if the server replies with an error or the socket is not connected.
    ::morph::exec::detail::ModelId registerModel(
        const std::string& typeId,
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory) override;

    /// @brief Sends a `register` carrying @p contextKey and blocks until the reply arrives.
    ///
    /// `IBackend::registerModelWithContext`'s default drops @p contextKey, which
    /// is right for `LocalBackend` — the caller's own factory closure already
    /// captures the identity — but wrong for a backend whose instances live on
    /// the far side of a wire protocol: the server constructs the holder itself,
    /// so `contextKey` is the *only* channel by which the instance's identity
    /// reaches it. `RemoteServer::attachLogIfConfigured` returns without
    /// consulting its `LogProvider` at all when the envelope's `contextKey` is
    /// empty, so dropping it here did not merely lose an entity key — it left
    /// the instance **unjournalled** (morph#594). `SimulatedRemoteBackend` and
    /// `morph::net::SocketBackend` (morph#587) override this for the same
    /// reason; backends documented as interchangeable must not disagree about
    /// whether a private registration is audited.
    ///
    /// This is also the verb the *blocking* `bindModel` path reaches for an
    /// empty-`primary`, zero-`current` request, and the one
    /// `Bridge::switchBackend` calls directly when it re-registers a handler
    /// after a reconnect — so before morph#594 the key was dropped whatever
    /// `Config::asyncRegistrationEnabled` was set to on a backend swap, and
    /// dropped on every private registration when it was unset. `bindModel`'s
    /// own non-blocking path already carried it.
    ///
    /// `registerModel` forwards here with an empty key, so there is one place
    /// that builds this envelope rather than two that can drift apart.
    ///
    /// @param typeId     String type-id of the model to register.
    /// @param factory    Ignored — model construction is delegated to the server.
    /// @param contextKey Stable identity of the new instance; empty if none.
    /// @return `ModelId` assigned by the server.
    /// @throws std::runtime_error if the server replies with an error or the socket is not connected.
    ::morph::exec::detail::ModelId registerModelWithContext(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        std::string_view contextKey) override;

    /// @brief Acquires a model instance over the wire, natively non-blocking
    ///        when `Config::asyncRegistrationEnabled` is set.
    ///
    /// The structural registration surface (`IBackend::bindModel`), implemented
    /// here rather than inherited: this is the one backend in the tree with a
    /// genuinely non-blocking acquire path, so it settles the returned
    /// `Completion` when the server's reply arrives instead of blocking a
    /// thread until then.
    ///
    /// With `Config::asyncRegistrationEnabled` unset (the default) this defers
    /// to `IBackend::bindModel`, which runs the synchronous verb the request's
    /// shape names and **blocks the Qt thread** in a nested `QEventLoop` for the
    /// round trip. That is the desktop behaviour every existing embedder
    /// relies on; see `QtWebSocketBackendConfig::asyncRegistrationEnabled`.
    ///
    /// With it set, the request's shape selects the envelope, mirroring
    /// `IBackend::bindModelBlocking`'s routing of the same three shapes:
    ///
    /// | `primary` | `current` | Envelope sent |
    /// |---|---|---|
    /// | empty | `0` | `register` (private) |
    /// | empty | non-zero | `deregister` of `current`, then `register` |
    /// | non-empty | `0` | `register` with `shared` (register-or-attach) |
    /// | non-empty | non-zero | `attach`, naming `current` |
    ///
    /// Each carries a fresh `callId` from the counter `execute()` uses, and the
    /// reply is matched via `_pendingRegistrations` when `onTextMessage` sees
    /// it — no protocol change, since the server already echoes `callId` on
    /// every reply.
    ///
    /// A **private** registration issued before the socket has finished
    /// connecting is queued rather than failed: exactly the ordering a
    /// single-threaded WASM client must use, since it can never block waiting
    /// for the connection to settle (a `BridgeHandler` constructed the moment
    /// the backend is wired up, before the first `connected` signal). The queued
    /// request is sent — with a call-id assigned at that point, not now — the
    /// moment `connected` fires next, first connect included, in FIFO order. If
    /// the socket never connects and the backend is torn down first, the queued
    /// entry is still settled: `~QtWebSocketBackend` calls `cancelPending`,
    /// which drains `_queuedRegistrations` too and rejects each exactly once.
    /// A keyed bind on a disconnected socket rejects immediately instead.
    ///
    /// @param request Owning bind request; moved from. `request.factory` is
    ///                unused — this backend holds no local model to construct;
    ///                the server instantiates the model from `request.typeId`.
    /// @param cbExec  Executor the continuation is delivered on. Borrowed: it
    ///                must outlive the returned `Completion`.
    /// @return A `Completion` resolved with the bound `ModelId`, or rejected
    ///         with the failure. Already settled on the blocking path.
    ::morph::async::Completion<::morph::exec::detail::ModelId> bindModel(::morph::backend::detail::BindRequest request,
                                                                         ::morph::exec::IExecutor& cbExec) override;

    /// @brief Whether a caller may block waiting for this backend's
    ///        `bindModel`/`promoteModel` completions.
    ///
    /// `kCallerMustNotBlock` exactly when `Config::asyncRegistrationEnabled` is
    /// set, because that is exactly when a completion is settled by
    /// `onTextMessage` — a Qt slot, delivered by the event loop of the thread
    /// that called `bindModel`. A caller blocked in a wait is not running that
    /// event loop, so the reply it is waiting for can never arrive: the
    /// deadlock morph#568 exists to remove, which on a WASM main thread aborts
    /// the page outright.
    ///
    /// With the flag unset this backend's `bindModel` is `IBackend`'s default,
    /// which settles inside the call, so `kCallerMayBlock` is both true and
    /// free: the caller's wait finds the outcome already parked and returns
    /// without sleeping.
    ///
    /// Note which way round this reads. It does not say "registration is
    /// asynchronous" — `SocketBackend`'s is too, and it answers
    /// `kCallerMayBlock` because a separate I/O thread settles its completions.
    /// It says only that *this* thread must not stop and wait. See morph#593.
    ///
    /// @return `kCallerMustNotBlock` when `Config::asyncRegistrationEnabled` is
    ///         set, `kCallerMayBlock` otherwise.
    [[nodiscard]] ::morph::backend::detail::BindWait bindWaitPolicy() const noexcept override {
        return _cfg.asyncRegistrationEnabled ? ::morph::backend::detail::BindWait::kCallerMustNotBlock
                                             : ::morph::backend::detail::BindWait::kCallerMayBlock;
    }

    /// @brief Sends a shared (register-or-attach) `register` and blocks for the reply.
    ///
    /// An empty primary degrades to the private path.
    /// @param typeId   String type-id of the model.
    /// @param factory  Ignored — model construction is delegated to the server.
    /// @param identity Entity key for the action log plus the directory primary key.
    /// @return `ModelId` of the shared (or newly created) instance.
    /// @throws std::runtime_error if the server errors or the socket is not connected.
    ::morph::exec::detail::ModelId registerModelShared(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        ::morph::backend::detail::InstanceIdentity identity) override;

    /// @brief Sends an `attach` and blocks for the reply, re-pointing from @p current.
    /// @param typeId   String type-id of the model.
    /// @param factory  Ignored — model construction is delegated to the server.
    /// @param identity Entity key for the action log plus the directory primary key.
    /// @param current  Instance currently held, or `ModelId{0}` if none.
    /// @return `ModelId` of the instance now attached to.
    /// @throws std::runtime_error if the server errors or the socket is not connected.
    ::morph::exec::detail::ModelId attachModel(
        const std::string& typeId, std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
        ::morph::backend::detail::InstanceIdentity identity, ::morph::exec::detail::ModelId current) override;

    /// @brief Files a live server-side instance under @p primary.
    /// @param mid     Live instance to promote.
    /// @param typeId  Model type id.
    /// @param primary Canonical string encoding of the key to file it under.
    void assignPrimary(::morph::exec::detail::ModelId mid, const std::string& typeId,
                       std::string_view primary) override;

    /// @brief Files a live server-side instance under a key, without blocking.
    ///
    /// The structural counterpart of `assignPrimary` (`IBackend::promoteModel`),
    /// implemented natively for the same reason `bindModel` above is:
    /// `assignPrimary` blocks the Qt thread in a nested `QEventLoop` via
    /// `sendSync`, which a WASM main thread cannot do. This assigns a fresh
    /// `callId` from the counter `execute()`/`bindModel()` use, sends the
    /// `assign` envelope, and returns an unsettled `Completion`; the reply is
    /// matched via `_pendingAssigns` when `onTextMessage` sees it.
    ///
    /// Unlike `bindModel`, this does **not** consult
    /// `Config::asyncRegistrationEnabled`: promotion happens from inside the
    /// result `Completion`'s callback chain, where no caller is left blocked
    /// waiting for it either way, so there is no synchronous guarantee to
    /// preserve — which is why `assignPrimaryAsync`, the verb this replaces,
    /// had no opt-in gate either.
    ///
    /// The documented no-op cases (empty `primary`, zero `mid`) resolve with
    /// @p request's `mid` without sending anything, matching
    /// `IBackend::promoteModel`. A disconnected socket rejects.
    ///
    /// @param request Owning promote request; moved from.
    /// @param cbExec  Executor the continuation is delivered on. Borrowed: it
    ///                must outlive the returned `Completion`.
    /// @return A `Completion` resolved with the promoted `ModelId`, or rejected
    ///         with the failure.
    ::morph::async::Completion<::morph::exec::detail::ModelId> promoteModel(
        ::morph::backend::detail::PromoteRequest request, ::morph::exec::IExecutor& cbExec) override;

    /// @brief Asks the server for the live shared primary keys of @p typeId.
    /// @param typeId String type-id to enumerate.
    /// @return Canonical key strings of the live shared instances.
    /// @throws std::runtime_error if the server errors or the socket is not connected.
    std::vector<std::string> listInstances(const std::string& typeId) override;

    /// @brief Sends a `deregister` message fire-and-forget (does not wait for a reply).
    ///
    /// No acknowledgement is awaited, which avoids a nested `QEventLoop` during
    /// destruction (that can trip Qt asserts). Note the server performs no
    /// connection-scoped cleanup: an undelivered or lost `deregister` leaves the
    /// model registered on the server indefinitely.
    ///
    /// Assigned a real, non-zero `callId` from the same counter/namespace
    /// `execute()`/`bindModel()` use (see issue #65): `callId == 0`
    /// is reserved for a parked synchronous control call's reply, and a
    /// fire-and-forget `deregister` sharing that sentinel could otherwise have
    /// its own stray "ok" reply handed to an unrelated `registerModel`'s
    /// parked `sendSync` loop if the two land back to back on the same
    /// connection. The reply is tracked in `_pendingDeregisters` purely so it
    /// can be recognised and dropped in `onTextMessage`; nothing observes it.
    ///
    /// @param mid Id of the model to remove on the server.
    void deregisterModel(::morph::exec::detail::ModelId mid) override;

    /// @brief Sends an `execute` message and returns a `Completion` that resolves on reply.
    ///
    /// Assigns a monotonically increasing call-id so that concurrent calls can be
    /// matched to their replies. The `Completion` callbacks are posted via @p cbExec.
    ///
    /// @param mid    Target model id on the server.
    /// @param call   Bundled action; `serializeAction` and `deserializeResult` are used.
    /// @param cbExec Executor for delivering the completion callbacks.
    /// @return Completion resolved asynchronously when the server reply arrives.
    ::morph::async::Completion<std::shared_ptr<void>> execute(::morph::exec::detail::ModelId mid,
                                                              ::morph::backend::detail::ActionCall call,
                                                              ::morph::exec::IExecutor* cbExec) override;

    /// @brief No-op — this backend holds no local model objects.
    void notifyBackendChanged() override {}

    /// @brief Rejects every in-flight call's `Completion` with @p exc —
    ///        executes, binds (queued ones included) and promotes alike.
    ///
    /// Called by `Bridge::switchBackend()` on the outgoing backend, by `~Bridge`,
    /// and internally when the socket disconnects. Late replies arriving for
    /// already-cancelled call ids (execute, bind or promote) are dropped silently.
    ///
    /// @param exc Exception delivered to every pending completion's error sink.
    void cancelPending(const std::exception_ptr& exc) override;

    /// @brief Installs the handler `Bridge` uses to re-register handlers after a reconnect.
    /// @param handler Callable invoked on the Qt thread after every successful reconnect.
    ///                Pass `nullptr` to clear.
    void setReconnectHandler(const std::function<void()>& handler) override;

    /// @brief Installs a handler invoked on every successful connect, including the first.
    /// @param handler Callable invoked on the Qt thread after every successful connect
    ///                (first and subsequent). Pass `nullptr` to clear.
    void setConnectHandler(const std::function<void()>& handler) override;

    /// @brief Installs a handler invoked whenever the socket drops.
    ///
    /// Fires before reconnect scheduling, so an observer sees the disconnected
    /// state even when a retry follows immediately.
    /// @param handler Callable invoked on the Qt thread whenever the connection
    ///                drops. Pass `nullptr` to clear.
    void setDisconnectHandler(const std::function<void()>& handler) override;

    /// @brief Installs the session stamped onto every control envelope this
    ///        backend subsequently builds (`register`, `registerShared`,
    ///        `attach`, `assign`, `deregister`). See `IBackend::setSession`.
    /// @param session Session to stamp; typically pushed by `Bridge::setDefaultSession()`.
    void setSession(::morph::session::Context session) override;

private:
    /// @brief Sends @p msg synchronously by blocking the Qt thread via a nested event loop.
    std::string sendSync(const std::string& msg);

    /// @brief Slot called by `QWebSocket` when a text frame arrives.
    void onTextMessage(const QString& message);

    /// @brief Handles a frame that is not a decodable envelope.
    ///
    /// Hands @p msg to a parked `sendSync` waiter if there is one; otherwise
    /// fails every in-flight call, because an undecodable frame means the
    /// peer's framing can no longer be trusted.
    /// @param msg Raw frame text, as received.
    void onUndecodableMessage(std::string msg);

    /// @brief Dispatches a reply carrying a non-zero `callId` to whichever
    ///        pending-call map holds that id, if any.
    /// @param env Decoded reply envelope.
    void routeKeyedReply(const ::morph::wire::Envelope& env);

    /// @brief Settles the execute filed under `env.callId`, if one is pending.
    /// @param env Decoded reply envelope.
    /// @return `true` if an execute was found and settled.
    bool tryRouteExecuteReply(const ::morph::wire::Envelope& env);

    /// @brief Settles the bind/promote filed under `env.callId`, if one is pending.
    /// @param env Decoded reply envelope.
    /// @return `true` if a pending control call was found and settled.
    bool tryRouteControlReply(const ::morph::wire::Envelope& env);

    /// @brief Drops the reply to a fire-and-forget deregister (issue #65).
    /// @param env Decoded reply envelope.
    /// @return `true` if the id belonged to a pending deregister.
    bool tryRouteDeregisterReply(const ::morph::wire::Envelope& env);

    /// @brief Schedules a reconnect attempt with exponential backoff.
    void scheduleReconnect();

    /// @brief Attempts to reopen the socket using the saved URL/TLS config.
    void attemptReconnect();

    /// @brief Assigns a call-id, records @p promise, and sends @p env.
    ///
    /// The one send path every non-blocking control call takes, so the ordering
    /// invariant it enforces — encode before the map insertion, because a
    /// throwing `wire::encode()` after inserting would park a promise for a
    /// reply to a message that was never sent — is stated and tested once
    /// rather than four times.
    ///
    /// @param env     Control envelope to send; its `callId`/`session` are
    ///                stamped here.
    /// @param promise Settled when the matching reply arrives, or by
    ///                `cancelPending`.
    void sendControl(::morph::wire::Envelope env,
                     ::morph::async::Completion<::morph::exec::detail::ModelId>::Promise promise);

    /// @brief Sends every private bind queued while the socket was not yet
    ///        connected, in FIFO order. Called from the `connected` slot,
    ///        before the reconnect handler fires.
    void flushQueuedRegistrations();

    QUrl _serverUrl;
#ifndef QT_NO_SSL
    std::optional<QSslConfiguration> _tls;
#endif
    Config _cfg;
    QWebSocket _socket;
    QTimer _reconnectTimer;
    std::chrono::milliseconds _currentReconnectDelay;
    bool _connected{false};
    bool _everConnected{false};
    bool _shuttingDown{false};
    std::function<void()> _reconnectHandler;
    std::function<void()> _connectHandler;
    std::function<void()> _disconnectHandler;
    ::morph::session::Context _session;

    std::string _pendingReply;
    QEventLoop* _syncLoop{nullptr};

    struct PendingExecute {
        std::shared_ptr<::morph::async::detail::CompletionState<std::shared_ptr<void>>> state;
        std::function<std::shared_ptr<void>(std::string_view)> deserialize;
        ::morph::exec::IExecutor* cbExec{nullptr};
    };
    uint64_t _nextCallId{0};
    std::unordered_map<uint64_t, PendingExecute> _pending;
    std::mutex _pendingMtx;

    /// @brief In-flight `bindModel`/`promoteModel` calls, keyed by `callId`.
    ///
    /// Kept separate from `PendingExecute`/`_pending` (a different `callId`
    /// namespace would be a protocol change; this shares the same namespace
    /// and counter, just a different local map) because a control reply's shape
    /// (`modelId`, no `deserialize` step) differs from an execute reply's.
    ///
    /// One map for both verbs, not two: `register`, `registerShared`, `attach`
    /// and `assign` replies are all matched identically — a bare `modelId`
    /// echoed against the `callId` — so the split the four `*Async` verbs used
    /// to justify has nothing left to represent.
    std::unordered_map<uint64_t, ::morph::async::Completion<::morph::exec::detail::ModelId>::Promise>
        _pendingRegistrations;

    /// @brief One private `bindModel` issued before the socket had finished
    ///        connecting. No call-id is assigned until the request is actually
    ///        sent (from `flushQueuedRegistrations`), so a queued entry that
    ///        never gets to fire (backend destroyed first) is drained by
    ///        `cancelPending` from here rather than by call-id.
    struct QueuedRegistration {
        std::string typeId;
        std::string contextKey;
        ::morph::async::Completion<::morph::exec::detail::ModelId>::Promise promise;
    };
    std::vector<QueuedRegistration> _queuedRegistrations;

    /// @brief Call-ids of `deregister` envelopes still awaiting their (unused)
    ///        reply (see issue #65).
    ///
    /// `deregisterModel` is fire-and-forget: nobody observes the reply, but it
    /// still needs a real, non-zero `callId` so `onTextMessage` can recognise
    /// and drop it explicitly, rather than letting it fall through to the
    /// `callId == 0` branch and collide with a parked `sendSync` waiter. A
    /// late reply for an id no longer in this set (already dropped, or the
    /// backend was cancelled/destroyed) is simply ignored — nothing to clean
    /// up either way.
    std::unordered_set<uint64_t> _pendingDeregisters;
};

}  // namespace morph::qt
