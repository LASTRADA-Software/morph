// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/wire.hpp>

#include <QObject>
#include <QString>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

/// @file
/// The single highest-yield harness the ladder needs and the repo lacked
/// (examples/TESTING.md, "The fault-injection wire proxy"): an in-process
/// WebSocket relay between `QtWebSocketBackend` and `QtWebSocketServer` with
/// scriptable per-call rules — drop exactly the reply frame of call k, delay
/// it, duplicate it, or kill the connection mid-reply. Closes the fault-proxy
/// half of finding 004.

namespace morph::ladder::testkit {

namespace detail {

/// @brief Throws if `_listener->listen()` failed, otherwise a no-op.
///
/// Factored out of `start()` so the decision is directly unit-testable with
/// a plain `bool` — forcing a real ephemeral-port `listen()` failure
/// deterministically isn't practically achievable without flakiness or a
/// test-only seam on `QWebSocketServer` itself, so the throw logic is what
/// gets tested instead of the real I/O call (mirrors
/// `backend_rig.hpp`'s `throwIfListenFailed`, same rationale, different
/// error message). Named distinctly from that one (`...FaultProxy...` rather
/// than the same bare name) because both are `inline` free functions in this
/// same `detail` namespace: a translation unit that includes both headers —
/// as any offline-stack test wiring a `FaultProxy` in front of a
/// `BackendRig`-style server does — would otherwise hit a hard
/// redefinition error, not just an ODR risk (found while building Task 20's
/// offline DoD tests, the first file in the tree to include both headers
/// together).
/// @param listenSucceeded The real `listen()` call's result.
/// @throws std::runtime_error if @p listenSucceeded is `false`.
inline void throwIfFaultProxyListenFailed(bool listenSucceeded) {
    if (!listenSucceeded) {
        throw std::runtime_error("FaultProxy::start: failed to listen on an ephemeral loopback port");
    }
}

/// @brief Whether `nextPendingConnection()`'s result is real and should be
///        adopted as this proxy's client leg.
///
/// Factored out of `onClientConnection()` so the decision is directly
/// unit-testable by passing `nullptr` or a real pointer, without needing to
/// race a `QWebSocketServer` into returning a spent connection.
/// @param incoming The result of `_listener->nextPendingConnection()`.
/// @return `true` if @p incoming is non-null.
[[nodiscard]] inline bool isValidIncomingConnection(QWebSocket* incoming) noexcept {
    return incoming != nullptr;
}

/// @brief Decodes a wire frame's `callId`, or `0` if it doesn't decode.
///
/// Shared by `onClientTextMessage()` (request leg) and
/// `onUpstreamTextMessage()` (reply leg) — both need "the callId, or 0 for
/// an undecodable frame" and neither treats a decode failure as fatal (an
/// undecodable frame is forwarded unreported/unmatched rather than dropped).
/// Factoring the try/catch out here collapses both call sites down to a
/// single branch-free assignment, so this is what's unit-tested directly: a
/// real trusted server never emits an undecodable reply, so the
/// reply-side catch block is otherwise unreachable from an integration test.
/// @param message The raw text frame, as received from either socket.
/// @return The decoded `callId`, or `0` if @p message doesn't decode.
[[nodiscard]] inline std::uint64_t decodeCallIdOrZero(const QString& message) noexcept {
    try {
        return ::morph::wire::decode(message.toStdString()).callId;
    } catch (const std::exception&) {
        return 0;
    }
}

}  // namespace detail

/// @brief One client<->server relay leg with scriptable server->client reply
///        interception, keyed on the wire envelope's `callId`.
///
/// @par Wiring
/// Construct with the real `QtWebSocketServer`'s URL, call `start()`, and hand
/// the returned URL to a `QtWebSocketBackend` in place of the server's. Every
/// frame is forwarded verbatim in both directions except where a rule
/// registered for a reply's `callId` says otherwise.
///
/// @par Connection model
/// Exactly one client leg at a time (the testkit's clients are one socket per
/// `Bridge`; a rig needing N faulted clients builds N proxies). A second
/// incoming connection replaces the first, which matches what
/// `QtWebSocketBackend`'s automatic reconnect does after a `killAfter`. The
/// proxy opens its own upstream socket lazily, on the first client connection,
/// and buffers client frames until that upstream handshake completes — without
/// that buffer the very first frame a client sends (a synchronous `register`,
/// emitted the moment `waitForConnected()` returns) would be written to a
/// still-opening socket and silently lost.
///
/// @par Threading
/// A `QObject` living on the Qt event loop thread: every slot below runs
/// there, and so does `setRequestObserver`'s callback. The rule table is
/// nevertheless mutex-guarded so a rule may be armed from any thread.
class FaultProxy : public QObject {
    Q_OBJECT

  public:
    /// @brief Constructs a proxy that will relay to @p upstreamUrl.
    /// @param upstreamUrl The real `QtWebSocketServer`'s URL (e.g.
    ///        `ws://127.0.0.1:<wsServer.port()>`).
    /// @param parent Optional `QObject` parent.
    explicit FaultProxy(QUrl upstreamUrl, QObject* parent = nullptr);

    /// @brief Stops listening and tears both legs down.
    ~FaultProxy() override;

    FaultProxy(const FaultProxy&) = delete;
    FaultProxy& operator=(const FaultProxy&) = delete;
    FaultProxy(FaultProxy&&) = delete;
    FaultProxy& operator=(FaultProxy&&) = delete;

    /// @brief Starts listening on an ephemeral loopback port.
    /// @return This proxy's own URL, to hand to a `QtWebSocketBackend` in place
    ///         of the real server's.
    /// @throws std::runtime_error if the listening socket cannot be bound.
    [[nodiscard]] QUrl start();

    /// @brief This proxy's own URL.
    /// @return The URL `start()` returned, or an empty `QUrl` before `start()`.
    [[nodiscard]] QUrl url() const { return _url; }

    /// @brief How many server->client frames this proxy has written to the
    ///        client leg so far.
    ///
    /// Counts frames on the wire, not calls: a `duplicateReply`'d call
    /// contributes two, a `dropReply`'d or `killAfter`'d one contributes none.
    /// This is what lets a test tell "the client's `Completion` ignored the
    /// second copy" apart from "no second copy was ever sent" — the difference
    /// between a real idempotency guarantee and a vacuous assertion.
    ///
    /// @return The running count. Read it from the Qt event loop thread.
    [[nodiscard]] std::uint64_t repliesForwarded() const { return _repliesForwarded; }

    /// @brief The reply whose envelope has this `callId` is silently dropped
    ///        (never forwarded to the client) — simulates a lost reply frame
    ///        after the server already committed the effect.
    /// @param callId Wire `callId` of the reply to drop.
    void dropReply(std::uint64_t callId);

    /// @brief The reply for @p callId is held for @p delay before forwarding.
    /// @param callId Wire `callId` of the reply to hold.
    /// @param delay  How long to hold it.
    void delayReply(std::uint64_t callId, std::chrono::milliseconds delay);

    /// @brief The reply for @p callId is forwarded twice (simulates a
    ///        duplicate delivery, the inverse fault to `dropReply`).
    /// @param callId Wire `callId` of the reply to duplicate.
    void duplicateReply(std::uint64_t callId);

    /// @brief The client<->proxy connection is aborted the instant the
    ///        reply for @p callId would otherwise be forwarded (simulates a
    ///        crash/kill mid-reply, before the client observes it).
    /// @param callId Wire `callId` of the reply to die on.
    void killAfter(std::uint64_t callId);

    /// @brief Registers a callback invoked synchronously from the
    ///        client->server forwarding path, after decoding a request's
    ///        `callId` but before that request is forwarded upstream.
    ///
    /// This is how a test arms a rule for a *specific upcoming* call
    /// race-free. `BridgeHandler::execute()` returns a bare `Completion` and
    /// never exposes the `callId` the backend assigned it, so a test cannot
    /// name call k from the outside. The observer supplies it at the only
    /// moment where naming it is still safe: the request is sitting in this
    /// proxy, not yet forwarded, so a rule registered from inside the callback
    /// is guaranteed installed before the request — and therefore before any
    /// possible reply to it — ever reaches the upstream server.
    ///
    /// Only requests carrying a non-zero `callId` are reported: `callId == 0`
    /// is the wire's marker for a synchronous control call
    /// (`register`/`deregister`/`hello`), which has no asynchronous reply to
    /// fault. A request this proxy cannot decode is forwarded unreported.
    ///
    /// @param observer Callback receiving the forwarded request's `callId` and
    ///        this proxy (so it can call `dropReply`/`delayReply`/
    ///        `duplicateReply`/`killAfter` on it directly). Pass `nullptr` to
    ///        clear.
    void setRequestObserver(std::function<void(std::uint64_t callId, FaultProxy& self)> observer);

  private slots:
    /// @brief Accepts the pending client connection and opens the upstream leg.
    void onClientConnection();

    /// @brief Forwards one client->server frame, reporting it to the observer first.
    /// @param message The raw frame text.
    void onClientTextMessage(const QString& message);

    /// @brief Flushes frames buffered while the upstream handshake was in flight.
    void onUpstreamConnected();

    /// @brief Applies this reply's rule (if any) and forwards it to the client.
    /// @param message The raw frame text.
    void onUpstreamTextMessage(const QString& message);

  private:
    /// @brief The scripted faults armed for one `callId`.
    struct Rule {
        /// @brief Never forward the reply.
        bool drop = false;
        /// @brief Forward the reply twice.
        bool duplicate = false;
        /// @brief Abort the client leg instead of forwarding.
        bool kill = false;
        /// @brief Hold the reply this long before forwarding.
        std::optional<std::chrono::milliseconds> delay;
    };

    /// @brief Looks up the rule armed for @p callId.
    /// @param callId Wire `callId` to look up.
    /// @return The armed rule, or a default (fault-free) one.
    [[nodiscard]] Rule ruleFor(std::uint64_t callId);

    /// @brief Sends @p message to the client leg if one is connected.
    /// @param message The raw frame text.
    void sendToClient(const QString& message);

    QUrl _upstreamUrl;
    QUrl _url;
    std::unique_ptr<QWebSocketServer> _listener;
    QWebSocket* _clientSocket{nullptr};    // the test's QtWebSocketBackend connects here
    QWebSocket* _upstreamSocket{nullptr};  // the proxy's own connection to the real server
    bool _upstreamConnected{false};
    std::uint64_t _repliesForwarded{0};
    std::vector<QString> _upstreamBacklog;  // client frames awaiting the upstream handshake

    std::mutex _rulesMtx;
    std::unordered_map<std::uint64_t, Rule> _rules;
    std::function<void(std::uint64_t, FaultProxy&)> _requestObserver;
};

}  // namespace morph::ladder::testkit
