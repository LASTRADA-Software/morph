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

    /// @brief Opt in to `registerModelAsync` (see its doc comment on `IBackend`).
    ///
    /// Defaults to `false`: `Bridge::registerHandler()` then falls back to the
    /// synchronous `registerModel`, exactly as before this feature existed —
    /// every existing embedder (a desktop Qt client, this backend's own test
    /// suite) keeps registering synchronously, immediately usable the line
    /// after `BridgeHandler`'s constructor returns.
    ///
    /// Set `true` only for a build where that synchronous guarantee cannot
    /// hold at all — a WASM main thread, where the nested `QEventLoop`
    /// `registerModel` relies on aborts the page outright. Doing so is a
    /// deliberate trade: the caller must then wait for registration to
    /// complete (e.g. gate the UI on it) before firing an action through that
    /// handler, since `executeVia` fails fast with "handler not bound" for an
    /// unbound binding rather than queuing or blocking.
    bool asyncRegistrationEnabled = false;
};

/// @brief `IBackend` implementation that communicates with a `RemoteServer` over WebSocket.
///
/// `registerModel()` is synchronous (blocks the calling thread via a nested
/// `QEventLoop` until the server replies) -- unusable on a WASM main thread,
/// which Qt refuses to spin a nested loop on at all. `registerModelAsync()`
/// is the non-blocking alternative `Bridge::registerHandler()` prefers when
/// available (see `IBackend::registerModelAsync`'s doc comment): it assigns a
/// call-id, sends the message, returns immediately, and invokes exactly one
/// of its `onRegistered`/`onError` callbacks once the matching reply arrives
/// -- the same call-id-matching mechanism `execute()` already uses.
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

    /// @brief Closes the socket and cleans up pending operations.
    ~QtWebSocketBackend() override;

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

    /// @brief Sends a `register` message and returns without blocking; the
    ///        reply is matched later, asynchronously, by `callId`.
    ///
    /// The non-blocking counterpart to `registerModel`/`registerModelWithContext`
    /// (see `IBackend::registerModelAsync`'s doc comment for why this exists):
    /// `registerModel` blocks the calling thread in a nested `QEventLoop` via
    /// `sendSync`, which a WASM main thread cannot do at all. This instead
    /// assigns a fresh `callId` (the same counter `execute()` uses), sends the
    /// `register` envelope, and returns `true` immediately; the reply is
    /// matched via `_pendingRegistrations` when `onTextMessage` sees it (no
    /// protocol change needed — the server already echoes `callId` on every
    /// reply, `register` included). Exactly one of @p onRegistered / @p onError
    /// fires, on the Qt event loop thread, once the reply arrives — or never,
    /// if the socket disconnects first without ever reconnecting and
    /// `cancelPending` is never called again for this id (a disconnect
    /// *before* a reconnect calls `cancelPending`, which does invoke @p onError
    /// — see `cancelPending`'s doc comment).
    ///
    /// If the socket has not finished connecting yet, the request is **queued**
    /// rather than failed: this is exactly the ordering a single-threaded WASM
    /// client must use, since it can never block waiting for the connection to
    /// settle (a `BridgeHandler` constructed the moment the backend is wired
    /// up, before the first `connected` signal). The queued request is sent —
    /// with a call-id assigned at that point, not now — the moment `connected`
    /// fires next (first connect included), in FIFO order. If the socket never
    /// connects at all and the backend is torn down first, the queued entry is
    /// still resolved: `~QtWebSocketBackend` calls `cancelPending`, which drains
    /// `_queuedRegistrations` too and invokes @p onError exactly once for each —
    /// no call-id was ever assigned, but the callback still fires, the same
    /// guarantee an already-sent (call-id-bearing) registration gets.
    ///
    /// @param typeId       String type-id of the model to instantiate.
    /// @param factory      Unused — this backend holds no local model to construct;
    ///                     the server instantiates the model from `typeId`.
    /// @param contextKey   Stable identity of the new instance; travels in the wire envelope.
    /// @param onRegistered Invoked with the server-assigned `ModelId` on success.
    /// @param onError      Invoked with a diagnostic message on failure or disconnect.
    /// @return `true` always — this backend has an async path (`false` is never returned).
    bool registerModelAsync(const std::string& typeId,
                            std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
                            std::string_view contextKey, std::function<void(::morph::exec::detail::ModelId)> onRegistered,
                            std::function<void(const std::string&)> onError) override;

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

    /// @brief Sends a shared (register-or-attach) `register` and, if async
    ///        registration is enabled, returns without blocking.
    ///
    /// The non-blocking counterpart to `registerModelShared`, matching
    /// `registerModelAsync`'s shape exactly (same `callId` counter, same
    /// `_pendingRegistrations` map, same verb-agnostic reply routing in
    /// `onTextMessage`). An empty `identity.primary` degrades to the private
    /// path, i.e. to `registerModelAsync`, mirroring the synchronous
    /// `registerModelShared`'s own degrade-to-private behaviour.
    ///
    /// @param typeId     String type-id of the model.
    /// @param factory    Ignored — model construction is delegated to the server.
    /// @param identity   Entity key for the action log plus the directory primary key.
    /// @param onRegistered Invoked with the assigned `ModelId` on success.
    /// @param onError    Invoked with a diagnostic message on failure.
    /// @return `true` if `asyncRegistrationEnabled` is set (see
    ///         `QtWebSocketBackendConfig`) and the request was sent;
    ///         `false` otherwise, falling back to the synchronous
    ///         `registerModelShared`.
    bool registerModelSharedAsync(const std::string& typeId,
                                  std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
                                  ::morph::backend::detail::InstanceIdentity identity,
                                  std::function<void(::morph::exec::detail::ModelId)> onRegistered,
                                  std::function<void(const std::string&)> onError) override;

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

    /// @brief Sends an `attach` and, if async registration is enabled,
    ///        returns without blocking.
    ///
    /// The non-blocking counterpart to `attachModel`; see
    /// `registerModelSharedAsync` immediately above for the shared shape. An
    /// empty `identity.primary` releases @p current and degrades to a private
    /// async registration, mirroring the synchronous `attachModel`'s own
    /// empty-primary branch.
    ///
    /// @param typeId     String type-id of the model.
    /// @param factory    Ignored — model construction is delegated to the server.
    /// @param identity   Entity key for the action log plus the directory primary key.
    /// @param current    Instance currently held, or `ModelId{0}` if none.
    /// @param onRegistered Invoked with the `ModelId` now attached to, on success.
    /// @param onError    Invoked with a diagnostic message on failure.
    /// @return `true` if `asyncRegistrationEnabled` is set and the request
    ///         was sent; `false` otherwise, falling back to the synchronous
    ///         `attachModel`.
    bool attachModelAsync(const std::string& typeId,
                          std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory,
                          ::morph::backend::detail::InstanceIdentity identity, ::morph::exec::detail::ModelId current,
                          std::function<void(::morph::exec::detail::ModelId)> onRegistered,
                          std::function<void(const std::string&)> onError) override;

    /// @brief Files a live server-side instance under @p primary.
    /// @param mid     Live instance to promote.
    /// @param typeId  Model type id.
    /// @param primary Canonical string encoding of the key to file it under.
    void assignPrimary(::morph::exec::detail::ModelId mid, const std::string& typeId,
                       std::string_view primary) override;

    /// @brief Sends an `assign` message and returns without blocking; the
    ///        reply is matched later, asynchronously, by `callId`.
    ///
    /// The non-blocking counterpart to `assignPrimary` (see
    /// `IBackend::assignPrimaryAsync`'s doc comment): `assignPrimary` blocks
    /// the calling thread in a nested `QEventLoop` via `sendSync`, the same
    /// shape `registerModelAsync` exists to let a caller avoid for the bind
    /// step. This instead assigns a fresh `callId` (the same counter
    /// `execute()`/`registerModelAsync()` use), sends the `assign` envelope,
    /// and returns `true` immediately; the reply is matched via
    /// `_pendingAssigns` when `onTextMessage` sees it. Exactly one of
    /// @p onRegistered / @p onError fires, on the Qt event loop thread, once
    /// the reply arrives.
    ///
    /// @param mid          Live instance to promote.
    /// @param typeId       Model type id.
    /// @param primary      Canonical string encoding of the key to file it under.
    /// @param onRegistered Invoked with @p mid on success (including the
    ///                     documented no-op cases -- see `IBackend::assignPrimaryAsync`).
    /// @param onError      Invoked with a diagnostic message on failure or disconnect.
    /// @return `true` always -- this backend has an async path (`false` is never returned).
    bool assignPrimaryAsync(::morph::exec::detail::ModelId mid, const std::string& typeId, std::string_view primary,
                            std::function<void(::morph::exec::detail::ModelId)> onRegistered,
                            std::function<void(const std::string&)> onError) override;

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
    /// `execute()`/`registerModelAsync()` use (see issue #65): `callId == 0`
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

    /// @brief Resolves every pending execute call's `Completion` with @p exc,
    ///        and fails every pending async registration's `onError`.
    ///
    /// Called by `Bridge::switchBackend()` on the outgoing backend, by `~Bridge`,
    /// and internally when the socket disconnects. Late replies arriving for
    /// already-cancelled call ids (execute or register) are dropped silently.
    ///
    /// @param exc Exception delivered to every pending completion's error sink;
    ///            `exc.what()`-equivalent text is delivered to every pending
    ///            `registerModelAsync` call's `onError`.
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

    /// @brief Schedules a reconnect attempt with exponential backoff.
    void scheduleReconnect();

    /// @brief Attempts to reopen the socket using the saved URL/TLS config.
    void attemptReconnect();

    /// @brief Assigns a call-id, records the pending registration, and sends
    ///        the `register` envelope. Shared by `registerModelAsync`'s
    ///        immediate path and the queued-request flush on `connected`.
    void sendRegisterAsync(const std::string& typeId, std::string_view contextKey,
                           std::function<void(::morph::exec::detail::ModelId)> onRegistered,
                           std::function<void(const std::string&)> onError);

    /// @brief Sends every request queued by `registerModelAsync` while the
    ///        socket was not yet connected, in FIFO order. Called from the
    ///        `connected` slot, before the reconnect handler fires.
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
        ::morph::exec::IExecutor* cbExec;
    };
    uint64_t _nextCallId{0};
    std::unordered_map<uint64_t, PendingExecute> _pending;
    std::mutex _pendingMtx;

    /// @brief One in-flight `registerModelAsync` call, keyed by `callId`.
    ///
    /// Kept separate from `PendingExecute`/`_pending` (a different `callId`
    /// namespace would be a protocol change; this shares the same namespace
    /// and counter, just a different local map) because a register reply's
    /// shape (`modelId`, no `deserialize` step) differs from an execute
    /// reply's.
    struct PendingRegistration {
        std::function<void(::morph::exec::detail::ModelId)> onRegistered;
        std::function<void(const std::string&)> onError;
    };
    std::unordered_map<uint64_t, PendingRegistration> _pendingRegistrations;

    /// @brief One `registerModelAsync` call made before the socket had
    ///        finished connecting. No call-id is assigned until the request
    ///        is actually sent (from `flushQueuedRegistrations`), so a queued
    ///        entry that never gets to fire (backend destroyed first) needs no
    ///        cancellation bookkeeping.
    struct QueuedRegistration {
        std::string typeId;
        std::string contextKey;
        std::function<void(::morph::exec::detail::ModelId)> onRegistered;
        std::function<void(const std::string&)> onError;
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

    /// @brief One in-flight `assignPrimaryAsync` call, keyed by `callId`.
    ///
    /// Mirrors `PendingRegistration`/`_pendingRegistrations`: same `callId`
    /// namespace and counter, separate map because an `assign` reply is
    /// matched the same way a `register`/`registerShared` reply is (a bare
    /// `modelId`, echoing back the instance that was promoted).
    struct PendingAssign {
        std::function<void(::morph::exec::detail::ModelId)> onRegistered;
        std::function<void(const std::string&)> onError;
    };
    std::unordered_map<uint64_t, PendingAssign> _pendingAssigns;
};

}  // namespace morph::qt
