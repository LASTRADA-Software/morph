// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QEventLoop>
#include <QSslConfiguration>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <morph/core/backend.hpp>
#include <morph/core/registry.hpp>
#include <chrono>
#include <functional>
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
};

/// @brief `IBackend` implementation that communicates with a `RemoteServer` over WebSocket.
///
/// `registerModel()` is synchronous (blocks the calling thread via a nested
/// `QEventLoop` until the server replies). `deregisterModel()` is fire-and-forget
/// (it sends the message without waiting, avoiding a nested event loop during
/// destruction). `execute()` is asynchronous: it assigns a call-id, sends the
/// message, and resolves the returned `Completion` when the matching reply arrives.
///
/// @par TLS
/// Pass a `QSslConfiguration` to enable `wss://`. For self-signed certificates
/// set `QSslSocket::VerifyNone` on the configuration before passing it in.
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
    /// @param tls         If non-null, enables TLS and applies this configuration.
    /// @param cfg         Reconnect tuning. Default: enabled, 500ms initial / 30s cap, 2x backoff.
    explicit QtWebSocketBackend(
        QUrl serverUrl,
        ::morph::model::detail::ActionDispatcher& dispatcher = ::morph::model::detail::defaultDispatcher(),
        ::morph::model::detail::ModelRegistryFactory& registry = ::morph::model::detail::defaultRegistry(),
        std::optional<QSslConfiguration> tls = std::nullopt,
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

    /// @brief Sends a `register` message to the server and blocks until the reply arrives.
    ///
    /// @param typeId  String type-id of the model to register.
    /// @param factory Ignored — model construction is delegated to the server.
    /// @return `ModelId` assigned by the server.
    /// @throws std::runtime_error if the server replies with an error or the socket is not connected.
    ::morph::exec::detail::ModelId registerModel(
        const std::string& typeId,
        std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> factory) override;

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

    /// @brief Resolves every pending execute call's `Completion` with @p exc.
    ///
    /// Called by `Bridge::switchBackend()` on the outgoing backend, by `~Bridge`,
    /// and internally when the socket disconnects. Late replies arriving for
    /// already-cancelled call ids are dropped silently.
    ///
    /// @param exc Exception delivered to every pending completion's error sink.
    void cancelPending(const std::exception_ptr& exc) override;

    /// @brief Installs the handler `Bridge` uses to re-register handlers after a reconnect.
    /// @param handler Callable invoked on the Qt thread after every successful reconnect.
    ///                Pass `nullptr` to clear.
    void setReconnectHandler(const std::function<void()>& handler) override;

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
    std::optional<QSslConfiguration> _tls;
    Config _cfg;
    QWebSocket _socket;
    QTimer _reconnectTimer;
    std::chrono::milliseconds _currentReconnectDelay;
    bool _connected{false};
    bool _everConnected{false};
    bool _shuttingDown{false};
    std::function<void()> _reconnectHandler;

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
};

}  // namespace morph::qt
