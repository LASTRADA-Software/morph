// SPDX-License-Identifier: Apache-2.0

#pragma once
#ifndef QT_NO_SSL
#include <QSslConfiguration>
#endif
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <chrono>
#include <functional>
#include <morph/attributes.hpp>
#include <morph/core/backend.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/wire.hpp>
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
};

/// @brief `IBackend` implementation that communicates with a `RemoteServer` over WebSocket.
///
/// Every request is asynchronous and nothing blocks the Qt thread: a request
/// carries a fresh non-zero `callId`, its pending entry is filed under it, and
/// the matching reply settles that entry from the `textMessageReceived` slot.
/// The caller's `Completion` is delivered on the executor the caller names.
/// The request verbs are `execute()`, `bindModel()`/`promoteModel()` (the
/// structural registration surface, `docs/spec/core/backend.md`),
/// `instances()` and `negotiateProtocolVersion()`. A bind issued before the
/// socket is up leaves its handler unbound; a call made through it is held by
/// the `Bridge` and dispatched when the bind settles.
///
/// The synchronous `IBackend` verbs that would have to wait for a reply —
/// `registerModel()`, `assignPrimary()`, `listInstances()` — throw
/// `std::logic_error` naming their completion form: waiting would mean
/// spinning a nested event loop on the Qt thread, which a WASM main thread
/// cannot do at all.
///
/// Everything this backend keeps — the socket, the pending tables — belongs to
/// the thread the socket lives on, as Qt's own objects do: every verb is called
/// there (the `Bridge`'s owner is that thread) and every reply slot runs there,
/// so none of it takes a lock. A debug build asserts it at each site.
/// `deregisterModel()` is fire-and-forget: it sends the message and files only
/// its `callId`, so the reply can be recognised and dropped.
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
/// Must be used from the Qt event loop thread. Every verb and the internal
/// message handler are called on that thread.
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
    /// `Config::reconnectEnabled`) to spell out
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
    /// Must be called on the Qt event loop thread after construction. It spins
    /// a local `QEventLoop`, which a WASM main thread cannot do: a WASM client
    /// binds without waiting (a bind issued before the socket is up is queued)
    /// or reacts to `setConnectHandler`.
    ///
    /// @param timeoutMs Maximum time to wait in milliseconds.
    /// @return `true` if connected before the timeout, `false` otherwise.
    bool waitForConnected(int timeoutMs = 5000);

    /// @brief Sends a `"hello"` envelope to the server and classifies its reply.
    ///
    /// Intended to be sent once, after the socket connects and before any
    /// `bindModel`/`execute`; nothing enforces that ordering.
    ///
    /// @param replyExec Executor the answer is delivered on. Borrowed: it must
    ///        outlive the returned `Completion`.
    /// @return A `Completion` resolved with `Negotiated` if the server accepted
    ///         `kProtocolVersion`, or `LegacyPeer` if it does not understand
    ///         `"hello"`; rejected with a `std::runtime_error` if the server
    ///         rejects the version or the socket is not connected, and with
    ///         `backend::DisconnectedError` if it drops before the reply.
    [[nodiscard]] ::morph::async::Completion<::morph::wire::ProtocolNegotiationResult> negotiateProtocolVersion(
        ::morph::exec::IExecutor& replyExec MORPH_LIFETIMEBOUND);

    /// @brief Refuses: this backend registers only through `bindModel`.
    ///
    /// A registration is a round trip, and answering it synchronously would
    /// mean blocking the Qt thread until the reply. `bindModel` with an empty
    /// `primary` sends the same `register` envelope and settles a `Completion`.
    /// The model is constructed by the server from its own registry, so this
    /// backend never uses a factory.
    /// @param typeId  Unused.
    /// @param factory Unused.
    /// @return Never returns.
    /// @throws std::logic_error always.
    ::morph::exec::detail::ModelId registerModel(
        const std::string& typeId,
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory) override;

    /// @brief Acquires a model instance over the wire, settled by the server's reply.
    ///
    /// The structural registration surface (`IBackend::bindModel`). The
    /// request's shape selects the envelope:
    ///
    /// | `primary` | `current` | Envelope sent |
    /// |---|---|---|
    /// | empty | `0` | `register` (private) |
    /// | empty | non-zero | `deregister` of `current`, then `register` |
    /// | non-empty | `0` | `register` with `shared` (register-or-attach) |
    /// | non-empty | non-zero | `attach`, naming `current` |
    ///
    /// Each carries a fresh `callId` from the counter `execute()` uses and the
    /// installed session; `request.contextKey` rides on every shape, since the
    /// server constructs the holder itself and the key is the only channel by
    /// which the instance's identity (and so its action log) reaches it.
    ///
    /// A **private** registration issued before the socket has finished
    /// connecting is queued rather than failed: exactly the ordering a
    /// single-threaded WASM client must use, since it can never wait for the
    /// connection to settle. The queued request is sent — with a call-id
    /// assigned at that point — the moment `connected` fires next, first
    /// connect included, in FIFO order. If the backend is torn down first,
    /// `cancelPending` rejects each queued entry exactly once. A keyed bind on
    /// a disconnected socket rejects immediately instead: it may re-point a
    /// live instance, and replaying that against a later connection would
    /// attach from a `current` that connection never issued.
    ///
    /// @param request Owning bind request; moved from. `request.factory` is
    ///                unused — the server instantiates the model from
    ///                `request.typeId`.
    /// @param cbExec  Executor the continuation is delivered on. Borrowed: it
    ///                must outlive the returned `Completion`.
    /// @return A `Completion` resolved with the bound `ModelId`, or rejected
    ///         with the failure.
    ::morph::async::Completion<::morph::exec::detail::ModelId> bindModel(::morph::backend::detail::BindRequest request,
                                                                         ::morph::exec::IExecutor& cbExec) override;

    /// @brief Refuses: this backend promotes only through `promoteModel`.
    ///
    /// Same reason as `registerModel`: an `assign` is a round trip.
    /// @param mid     Unused.
    /// @param typeId  Unused.
    /// @param primary Unused.
    /// @throws std::logic_error always.
    void assignPrimary(::morph::exec::detail::ModelId mid, const std::string& typeId,
                       std::string_view primary) override;

    /// @brief Files a live server-side instance under a key, settled by the server's reply.
    ///
    /// The structural counterpart of `assignPrimary` (`IBackend::promoteModel`):
    /// sends the `assign` envelope with a fresh `callId` and returns an
    /// unsettled `Completion`. The documented no-op cases (empty `primary`,
    /// zero `mid`) resolve with @p request's `mid` without sending anything,
    /// matching `IBackend::promoteModel`. A disconnected socket rejects.
    ///
    /// @param request Owning promote request; moved from.
    /// @param cbExec  Executor the continuation is delivered on. Borrowed: it
    ///                must outlive the returned `Completion`.
    /// @return A `Completion` resolved with the promoted `ModelId`, or rejected
    ///         with the failure.
    ::morph::async::Completion<::morph::exec::detail::ModelId> promoteModel(
        ::morph::backend::detail::PromoteRequest request, ::morph::exec::IExecutor& cbExec) override;

    /// @brief Refuses: this backend lists instances only through `instances`.
    ///
    /// Same reason as `registerModel`: an `instances` request is a round trip.
    /// @param typeId Unused.
    /// @return Never returns.
    /// @throws std::logic_error always.
    std::vector<std::string> listInstances(const std::string& typeId) override;

    /// @brief Asks the server for the live shared primary keys of @p typeId.
    ///
    /// Sends an `instances` envelope with a fresh `callId`; the reply settles
    /// the returned `Completion`. A disconnected socket rejects.
    /// @param typeId String type-id to enumerate.
    /// @param cbExec Executor the answer is delivered on. Borrowed: it must
    ///        outlive the returned `Completion`.
    /// @return A `Completion` resolved with the canonical key strings of the
    ///         live shared instances, or rejected with the server's error, a
    ///         decode failure, or `backend::DisconnectedError`.
    ::morph::async::Completion<std::vector<std::string>> instances(const std::string& typeId,
                                                                   ::morph::exec::IExecutor& cbExec) override;

    /// @brief Sends a `deregister` message fire-and-forget (does not wait for a reply).
    ///
    /// Nothing waits for the acknowledgement, so it is safe from a destructor.
    /// The server performs no connection-scoped cleanup: an undelivered or lost
    /// `deregister` leaves the model registered on the server indefinitely.
    ///
    /// The request carries a real, non-zero `callId` from the counter every
    /// other request uses, filed in `_pendingDeregisters` only so its reply is
    /// recognised and dropped.
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

    /// @brief Rejects every in-flight request's `Completion` with @p exc —
    ///        executes, binds (queued ones included), promotes, instance
    ///        listings and the `hello` alike.
    ///
    /// Called by `Bridge::switchBackend()` on the outgoing backend, by `~Bridge`,
    /// and internally when the socket disconnects. Late replies arriving for
    /// already-cancelled call ids are dropped silently.
    ///
    /// @param exc Exception delivered to every pending completion's error sink.
    void cancelPending(const std::exception_ptr& exc) override;

    /// @brief Installs the handler `Bridge` uses to re-register handlers after a reconnect.
    /// @param handler Callable posted to @p exec after every successful
    ///                reconnect. Pass `nullptr` to clear.
    /// @param exec    Executor the handler runs on. Borrowed: it must outlive
    ///                the installation.
    void setReconnectHandler(std::function<void()> handler, ::morph::exec::IExecutor* exec) override;

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
    /// @brief A request waiting for its reply: what the reply settles, and
    ///        what a cancel rejects. Exactly one of the two is called.
    struct PendingControl {
        /// Settles the caller's `Completion` from the matching reply.
        std::function<void(const ::morph::wire::Envelope&)> reply;
        /// Rejects it, when the request is cancelled or never sent.
        std::function<void(const std::exception_ptr&)> fail;
    };

    /// @brief A `PendingControl` whose reply is a bare `modelId`
    ///        (`register`, `registerShared`, `attach`, `assign`).
    /// @param promise The caller's promise.
    /// @return The pending entry that settles @p promise.
    static PendingControl modelIdReply(::morph::async::Completion<::morph::exec::detail::ModelId>::Promise promise);

    /// @brief Slot called by `QWebSocket` when a text frame arrives.
    void onTextMessage(const QString& message);

    /// @brief Dispatches a reply to whichever pending table holds its `callId`,
    ///        and drops it when none does.
    /// @param env Decoded reply envelope.
    void routeReply(const ::morph::wire::Envelope& env);

    /// @brief Settles the execute filed under `env.callId`, if one is pending.
    /// @param env Decoded reply envelope.
    /// @return `true` if an execute was found and settled.
    bool tryRouteExecuteReply(const ::morph::wire::Envelope& env);

    /// @brief Settles the request filed under `env.callId` in `_pendingControl`, if any.
    /// @param env Decoded reply envelope.
    /// @return `true` if a pending request was found and settled.
    bool tryRouteControlReply(const ::morph::wire::Envelope& env);

    /// @brief Drops the reply to a fire-and-forget deregister.
    /// @param env Decoded reply envelope.
    /// @return `true` if the id belonged to a pending deregister.
    bool tryRouteDeregisterReply(const ::morph::wire::Envelope& env);

    /// @brief Schedules a reconnect attempt with exponential backoff.
    void scheduleReconnect();

    /// @brief Attempts to reopen the socket using the saved URL/TLS config.
    void attemptReconnect();

    /// @brief Assigns a call-id, files @p pending under it, and sends @p env.
    ///
    /// The one send path every request but `execute` takes. It encodes before
    /// filing: a throwing `wire::encode()` after filing would leave an entry
    /// waiting for a reply to a message that was never sent, so a throw
    /// rejects through @p pending instead.
    ///
    /// @param env     Request envelope to send; its `callId`/`session` are
    ///                stamped here.
    /// @param pending Settled when the matching reply arrives, or rejected by
    ///                `cancelPending`.
    void sendControl(::morph::wire::Envelope env, PendingControl pending);

    /// @brief Sends every private bind queued while the socket was not yet
    ///        connected, in FIFO order. Called from the `connected` slot,
    ///        before the reconnect handler fires.
    void flushQueuedRegistrations();

    /// @brief Asserts, in a debug build, that @p site runs on the socket's
    ///        thread — where every table this backend keeps belongs.
    /// @param site Name of the calling body.
    void checkThread(char const* site) const noexcept;

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
    ::morph::exec::IExecutor* _reconnectExec{nullptr};
    std::function<void()> _connectHandler;
    std::function<void()> _disconnectHandler;
    ::morph::session::Context _session;

    struct PendingExecute {
        std::shared_ptr<::morph::async::detail::CompletionState<std::shared_ptr<void>>> state;
        std::function<std::shared_ptr<void>(std::string_view)> deserialize;
        ::morph::exec::IExecutor* cbExec{nullptr};
    };
    uint64_t _nextCallId{0};
    // Every table below is touched only on the socket's thread: by the verbs,
    // called there, and by the reply slot, which runs there. All of them share
    // the one `callId` counter, so an id names one request across them.
    std::unordered_map<uint64_t, PendingExecute> _pending;

    /// @brief In-flight requests other than `execute` — `register`,
    ///        `registerShared`, `attach`, `assign`, `instances`, `hello` —
    ///        keyed by `callId`.
    ///
    /// Apart from `_pending` because an execute reply is deserialised into the
    /// action's result, while each of these settles its own value type.
    std::unordered_map<uint64_t, PendingControl> _pendingControl;

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
    ///        reply, so the reply is recognised and dropped. A late reply for
    ///        an id no longer here (the backend was cancelled) is dropped too.
    std::unordered_set<uint64_t> _pendingDeregisters;
};

}  // namespace morph::qt
