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
    /// @param tls         If non-null, enables TLS and applies this configuration. Not
    ///                    declared at all on an SSL-less Qt build (`QT_NO_SSL`) — see
    ///                    the class doc comment's "SSL-less Qt builds" section.
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

private:
    /// @brief Sends @p msg synchronously by blocking the Qt thread via a nested event loop.
    std::string sendSync(const std::string& msg);

    /// @brief Slot called by `QWebSocket` when a text frame arrives.
    void onTextMessage(const QString& message);

    /// @brief Schedules a reconnect attempt with exponential backoff.
    void scheduleReconnect();

    /// @brief Attempts to reopen the socket using the saved URL/TLS config.
    void attemptReconnect();

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
};

}  // namespace morph::qt
