// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QHostAddress>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <morph/core/connection_writer.hpp>
#include <morph/core/logger.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::qt {

namespace {

/// @brief Bytes a server-side `QWebSocket` puts on the socket for a text
///        message whose UTF-8 payload is @p payload bytes.
///
/// `QWebSocket` splits a message into frames of at most @p maxFrame payload
/// bytes, each with its own header; a server's frames are unmasked, so a
/// header is 2 bytes plus a 2- or 8-byte extended length.
/// @param payload  The message's UTF-8 length.
/// @param maxFrame The socket's `outgoingFrameSize()`.
/// @return The framed size.
qint64 framedSize(qint64 payload, quint64 maxFrame) {
    auto const header = [](qint64 size) -> qint64 {
        if (size < 126) {
            return 2;
        }
        return size <= 0xFFFF ? 4 : 10;
    };
    auto const cap = static_cast<qint64>(std::max<quint64>(maxFrame, 1));
    qint64 const whole = payload / cap;
    qint64 const rest = payload % cap;
    qint64 total = payload + (whole * header(cap));
    if (rest != 0 || whole == 0) {
        total += header(rest);
    }
    return total;
}

}  // namespace

QtWebSocketServer::QtWebSocketServer(::morph::backend::RemoteServer& server, quint16 port,
#ifndef QT_NO_SSL
                                     std::optional<QSslConfiguration> tls,
#endif
                                     QtWebSocketServerConfig cfg, QObject* parent)
    : QObject{parent},
      _server{server},
      _requestedPort{port},
      _cfg{std::move(cfg)},
#ifndef QT_NO_SSL
      _wsServer{QStringLiteral("morph"),
                tls.has_value() ? QWebSocketServer::SecureMode : QWebSocketServer::NonSecureMode, this},
#else
      _wsServer{QStringLiteral("morph"), QWebSocketServer::NonSecureMode, this},
#endif
      _housekeepingTimer{this} {
#ifndef QT_NO_SSL
    if (tls.has_value()) {
        _wsServer.setSslConfiguration(*tls);
    }
#endif
    connect(&_wsServer, &QWebSocketServer::newConnection, this, &QtWebSocketServer::onNewConnection);
    if (_cfg.idleTimeout.count() > 0) {
        connect(&_housekeepingTimer, &QTimer::timeout, this, &QtWebSocketServer::onHousekeepingTick);
        _housekeepingTimer.start(1000);  // 1s sweep granularity — see ClientState::lastActivity doc comment
    }
}

QtWebSocketServer::~QtWebSocketServer() { close(); }

bool QtWebSocketServer::listen() {
#ifndef QT_NO_SSL
    const bool hasTls = _wsServer.secureMode() == QWebSocketServer::SecureMode;
#else
    // QWebSocketServer::SecureMode doesn't exist on an SSL-less Qt build, and
    // the constructor above always constructs in NonSecureMode in that case.
    const bool hasTls = false;
#endif
    if (!_cfg.bindAddress.isLoopback() && !hasTls && !_cfg.allowPlaintextExposure) {
        ::morph::log::logError(
            "[qt_websocket_server] listen() refused: bindAddress=" + _cfg.bindAddress.toString().toStdString() +
            " is not loopback and no TLS configuration was supplied. Pass a QSslConfiguration, or set "
            "QtWebSocketServerConfig::allowPlaintextExposure = true to deliberately serve plaintext off-host.");
        return false;
    }
    return _wsServer.listen(_cfg.bindAddress, _requestedPort);
}

quint16 QtWebSocketServer::port() const { return _wsServer.serverPort(); }

void QtWebSocketServer::close() {
    _wsServer.close();
    _housekeepingTimer.stop();
    // Disconnect the onDisconnected slot first to prevent re-entrant modification of _clients
    // during the abort/deleteLater sequence below.
    for (auto& [socket, state] : _clients) {
        socket->disconnect(this);
        _server.closeConnection(state.cid);
        if (state.handshakeTimer != nullptr) {
            state.handshakeTimer->stop();
            state.handshakeTimer->deleteLater();
        }
        forgetConnection(state);
        socket->abort();
        socket->deleteLater();
    }
    _clients.clear();
}

bool QtWebSocketServer::closeGracefully(std::chrono::milliseconds deadline) {
    auto const absoluteDeadline = std::chrono::steady_clock::now() + deadline;

    // Step 1: stop taking new connections.
    _wsServer.pauseAccepting();
    // Step 2: new register/execute envelopes now fail fast on every existing
    // connection.
    _server.beginShutdown();

    // Step 3: wait for in-flight replies without blocking the event loop.
    // drainedWithin answers on the server's strand and delivers the answer on
    // the Qt thread, through `replyExec`; pumping processEvents until it
    // arrives also runs the reply callbacks RemoteServer already queued via
    // QMetaObject::invokeMethod (see onTextMessage), which is what actually
    // delivers those replies over the still-open sockets. `replyExec` posts to
    // the application object rather than to this server, and the handler
    // holds it, so an answer settled after this method stops waiting -- or
    // after this server is gone -- still posts to a live executor and target.
    struct DrainAnswer {
        std::atomic<bool> settled{false};
        std::atomic<bool> drained{false};
    };
    auto const answer = std::make_shared<DrainAnswer>();
    auto const budget = std::max(
        std::chrono::milliseconds{0},
        std::chrono::duration_cast<std::chrono::milliseconds>(absoluteDeadline - std::chrono::steady_clock::now()));
    auto const replyExec = std::make_shared<QtExecutor>(QCoreApplication::instance());
    _server.drainedWithin(budget, *replyExec).then([answer, replyExec](bool drainedNow) {
        answer->drained.store(drainedNow);
        answer->settled.store(true);
    });
    while (!answer->settled.load() && std::chrono::steady_clock::now() < absoluteDeadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    bool const drained = answer->drained.load();

    // The in-flight count reaches zero a hair before the model strand's reply
    // callback actually reaches the Qt event queue (the execute's `complete`
    // posts the decrement, then invokes `reply`), and actually
    // delivering that reply over the socket needs a couple more event-loop
    // round trips beyond that (this side's write, then the peer's read). Give
    // a reply that just landed a short, bounded window to flush before this
    // method starts sending close frames out from under it — bounded by
    // whatever is left of `deadline` so a caller's budget is never exceeded.
    auto const settleDeadline =
        std::min(absoluteDeadline, std::chrono::steady_clock::now() + std::chrono::milliseconds{50});
    while (std::chrono::steady_clock::now() < settleDeadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }

    // Step 4: whatever is still connected gets a proper close frame instead
    // of an abort, so the client can tell an orderly stop from a crash.
    // Snapshot the sockets first (same reason as onHousekeepingTick's toClose
    // vector below): closing an idle, already-primed loopback socket can
    // complete synchronously, reentrantly firing onDisconnected -- which
    // erases from _clients -- while this loop is still iterating it.
    std::vector<QWebSocket*> toClose;
    toClose.reserve(_clients.size());
    for (const auto& entry : _clients) {
        toClose.push_back(entry.first);
    }
    for (auto* socket : toClose) {
        socket->close(QWebSocketProtocol::CloseCodeGoingAway, QStringLiteral("server shutting down"));
    }

    // Let whatever remains of the deadline flush the close handshake over the
    // event loop before the hard stop below reclaims any stragglers. If the
    // drain step above already consumed the whole deadline, this loop body
    // never runs and close() below fires immediately.
    while (!_clients.empty() && std::chrono::steady_clock::now() < absoluteDeadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }

    // Step 5: hard stop for stragglers — identical to today's close() (also
    // reclaims each remaining client's connection scope).
    close();
    return drained;
}

void QtWebSocketServer::onNewConnection() {
    QWebSocket* socket = _wsServer.nextPendingConnection();
    if (socket == nullptr) {
        return;
    }
    if (_cfg.maxConnections != 0 && _clients.size() >= _cfg.maxConnections) {
        // To the client this looks exactly like the server being down -- the
        // one piece of information that would actually explain the symptom
        // (the configured cap was hit) otherwise never reaches an operator.
        ::morph::log::logWarn("[QtWebSocketServer] connection refused: at maxConnections ({}/{})", _clients.size(),
                              _cfg.maxConnections);
        socket->close();
        socket->deleteLater();
        return;
    }
    connect(socket, &QWebSocket::textMessageReceived, this, &QtWebSocketServer::onTextMessage);
    connect(socket, &QWebSocket::disconnected, this, &QtWebSocketServer::onDisconnected);
    connect(socket, &QWebSocket::bytesWritten, this, &QtWebSocketServer::onBytesWritten);

    ClientState state;
    state.socket = socket;
    state.tokens = static_cast<double>(_cfg.messagesPerSecond);  // full bucket: an immediate burst is allowed
    state.lastRefill = std::chrono::steady_clock::now();
    state.lastActivity = state.lastRefill;
    state.cid = _server.openConnection();

    // +1: this connection is admitted but not yet in `_clients` at this point.
    ::morph::log::logInfo("[QtWebSocketServer] connection {} accepted ({} live)", state.cid, _clients.size() + 1);

    if (_cfg.handshakeTimeout.count() > 0) {
        // Qt parent-child ownership: `this` owns the timer and deletes it, and every path that
        // drops the reference calls deleteLater() first (see the handshakeTimer handling below).
        // cppcoreguidelines-owning-memory has no model of that convention -- it flags any raw
        // `new` bound to a non-gsl::owner pointer. Suppressed here rather than for the
        // whole directory: this is the only such site in src/, and a directory-wide disable would
        // turn the check off for a future `new` that really is unowned.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) — QObject parent owns this
        auto* timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, socket] {
            if (_clients.contains(socket)) {
                socket->close();
            }
        });
        timer->start(static_cast<int>(_cfg.handshakeTimeout.count()));
        state.handshakeTimer = timer;
    }

    // The handshake response may still be on the socket; its bytes are
    // written ahead of any frame, so they start the count below zero.
    state.written = -socket->bytesToWrite();
    if (_cfg.sendTimeout.count() > 0) {
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) — QObject parent owns this
        auto* timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, socket] { onSendTimer(socket); });
        state.sendTimer = timer;
    }

    auto const cid = state.cid;
    auto const backlog = state.backlog;
    _clients.emplace(socket, std::move(state));

    // QWebSocketServer hands over a socket whose upgrade is already complete,
    // so a frame written from here on cannot precede the handshake response.
    QPointer<QtWebSocketServer> const self{this};
    QPointer<QWebSocket> const weakSocket{socket};
    _server.setConnectionWriter(
        cid, ::morph::backend::ConnectionWriter{
                 .write =
                     [self, weakSocket, backlog](std::string envelope) {
                         if (!queueEnvelope(self, weakSocket, backlog, std::move(envelope))) {
                             throw std::runtime_error("no Qt application to write the connection from");
                         }
                     },
                 .backlog = [backlog] { return backlog->load(); }});
}

bool QtWebSocketServer::queueEnvelope(QPointer<QtWebSocketServer> const& server, QPointer<QWebSocket> const& socket,
                                      std::shared_ptr<std::atomic<std::size_t>> const& backlog,
                                      std::string envelope) {
    auto* app = QCoreApplication::instance();
    if (app == nullptr) {
        return false;
    }
    std::size_t const counted = envelope.size();
    // Before the queueing, so a backlog read on the server strand already
    // covers a frame still on its way to the Qt thread.
    backlog->fetch_add(counted);
    bool const queued = QMetaObject::invokeMethod(
        app,
        [server, socket, backlog, counted, envelope = std::move(envelope)]() {
            if (server && socket) {
                auto iter = server->_clients.find(socket.data());
                if (iter != server->_clients.end()) {
                    server->writeCounted(socket.data(), iter->second, envelope);
                    return;
                }
            }
            backlog->fetch_sub(counted);
        },
        Qt::QueuedConnection);
    if (!queued) {
        backlog->fetch_sub(counted);
    }
    return queued;
}

void QtWebSocketServer::writeNow(QWebSocket* socket, ClientState& state, std::string const& envelope) {
    state.backlog->fetch_add(envelope.size());
    writeCounted(socket, state, envelope);
}

void QtWebSocketServer::writeCounted(QWebSocket* socket, ClientState& state, std::string const& envelope) {
    qint64 const payload = socket->sendTextMessage(QString::fromStdString(envelope));
    if (payload <= 0) {
        // Not connected any more: nothing reached the socket.
        releaseBacklog(state, envelope.size());
        return;
    }
    // The frame ends where the socket's written count will stand once its
    // last byte is out: after the frame ahead of it, or after whatever the
    // socket has written so far. `bytesWritten` reports framed bytes, so the
    // position is framed too; only the envelope's size is counted.
    qint64 const base = state.unwritten.empty() ? state.written : state.unwritten.back().end;
    state.unwritten.push_back(UnwrittenFrame{.end = base + framedSize(payload, socket->outgoingFrameSize()),
                                             .counted = envelope.size(),
                                             .queuedAt = std::chrono::steady_clock::now()});
    if (state.unwritten.size() == 1) {
        armSendTimer(state);
    }
}

void QtWebSocketServer::releaseBacklog(ClientState const& state, std::size_t bytes) {
    if (bytes == 0) {
        return;
    }
    // Judged from the value this subtraction replaced, so of several releases
    // that pass the bound together exactly one reports it. A second report,
    // after other threads lift the count again, is harmless: the server sends
    // nothing when no resync is owed.
    std::size_t const before = state.backlog->fetch_sub(bytes);
    bool const crossed =
        before >= ::morph::backend::kEventBacklogLow && before - bytes < ::morph::backend::kEventBacklogLow;
    if (crossed && state.socket != nullptr && state.socket->state() == QAbstractSocket::ConnectedState) {
        _server.connectionDrained(state.cid);
    }
}

void QtWebSocketServer::onBytesWritten(qint64 bytes) {
    auto* socket = qobject_cast<QWebSocket*>(sender());
    if (socket == nullptr) {
        return;
    }
    auto iter = _clients.find(socket);
    if (iter == _clients.end()) {
        return;
    }
    ClientState& state = iter->second;
    state.written += bytes;
    // An empty socket buffer means every frame handed to it is out, whatever
    // the positions say: a frame whose framed size was misjudged is still
    // released, rather than holding its bytes and tripping `sendTimeout`.
    bool const allWritten = socket->bytesToWrite() == 0;
    bool released = false;
    while (!state.unwritten.empty() && (allWritten || state.unwritten.front().end <= state.written)) {
        std::size_t const counted = state.unwritten.front().counted;
        state.unwritten.pop_front();
        releaseBacklog(state, counted);
        released = true;
    }
    if (released) {
        armSendTimer(state);
    }
}

void QtWebSocketServer::armSendTimer(ClientState& state) const {
    if (state.sendTimer == nullptr) {
        return;
    }
    if (state.unwritten.empty()) {
        state.sendTimer->stop();
        return;
    }
    auto const due = state.unwritten.front().queuedAt + _cfg.sendTimeout;
    auto const remaining = std::chrono::ceil<std::chrono::milliseconds>(due - std::chrono::steady_clock::now());
    state.sendTimer->start(std::max(std::chrono::milliseconds{0}, remaining));
}

void QtWebSocketServer::onSendTimer(QWebSocket* socket) {
    auto iter = _clients.find(socket);
    if (iter == _clients.end() || iter->second.unwritten.empty()) {
        return;
    }
    ClientState& state = iter->second;
    if (std::chrono::steady_clock::now() - state.unwritten.front().queuedAt < _cfg.sendTimeout) {
        armSendTimer(state);
        return;
    }
    ::morph::log::logWarn("[QtWebSocketServer] connection {} closed: send timeout, {} bytes unwritten after {} ms",
                          state.cid, state.backlog->load(), _cfg.sendTimeout.count());
    // An abort, not a close: a close frame would queue behind what the peer
    // is not reading. It may disconnect synchronously, erasing `state`.
    socket->abort();
}

void QtWebSocketServer::forgetConnection(ClientState& state) {
    if (state.sendTimer != nullptr) {
        state.sendTimer->stop();
        state.sendTimer->deleteLater();
        state.sendTimer = nullptr;
    }
    std::size_t dropped = 0;
    for (auto const& frame : state.unwritten) {
        dropped += frame.counted;
    }
    state.unwritten.clear();
    state.backlog->fetch_sub(dropped);
}

bool QtWebSocketServer::consumeToken(ClientState& state) const {
    if (_cfg.messagesPerSecond == 0) {
        return true;  // unbounded (today's behavior)
    }
    auto const now = std::chrono::steady_clock::now();
    double const elapsedSeconds = std::chrono::duration<double>(now - state.lastRefill).count();
    state.lastRefill = now;
    auto const capacity = static_cast<double>(_cfg.messagesPerSecond);
    state.tokens = std::min(capacity, state.tokens + (elapsedSeconds * capacity));
    if (state.tokens < 1.0) {
        return false;
    }
    state.tokens -= 1.0;
    return true;
}

void QtWebSocketServer::onTextMessage(const QString& message) {
    auto* socket = qobject_cast<QWebSocket*>(sender());
    if (socket == nullptr) {
        return;
    }
    auto iter = _clients.find(socket);
    if (iter == _clients.end()) {
        return;  // disconnected/untracked socket; drop
    }
    ClientState& state = iter->second;
    state.lastActivity = std::chrono::steady_clock::now();
    if (state.handshakeTimer != nullptr) {
        state.handshakeTimer->stop();
        state.handshakeTimer->deleteLater();
        state.handshakeTimer = nullptr;
    }

    // Hoisted: both rejection branches below address their reply to the call it
    // answers, and neither decodes the frame to do it.
    const auto utf8 = message.toUtf8();
    const auto peekedCallId = [&utf8] {
        // Address the rejection to the call it answers. The frame is never
        // decoded (that is the point of the cap), so the id is recovered by a
        // bounded prefix scan. Replying with a zeroed callId would be worse
        // than useless: `callId == 0` is the client's synchronous-reply
        // discriminator, so the error would resume some unrelated parked
        // register/deregister with a reply meant for an execute, while the
        // execute that triggered it still never resolves.
        return ::morph::wire::detail::peekCallId(
            std::string_view{utf8.constData(), static_cast<std::size_t>(utf8.size())});
    };

    if (std::cmp_greater(utf8.size(), _cfg.maxMessageBytes)) {
        writeNow(socket, state,
                 ::morph::wire::encode(::morph::wire::makeErr("message exceeds maxMessageBytes", peekedCallId())));
        return;
    }

    if (!consumeToken(state)) {
        // Over the per-connection rate limit. The frame is not executed, but it
        // *is* answered: dropping it silently left a pending client Completion
        // for an `execute` with nothing to resolve it, so the caller hung
        // unless it had armed `LimitPolicy::executeTimeout` (off by default).
        // A reply costs nothing at the protocol level and turns that hang into
        // an ordinary error the caller's `.onError(...)` already handles --
        // the same reply-without-full-decode pattern the maxMessageBytes
        // branch above uses. The connection stays open: rate limiting throttles
        // a client, it does not evict one.
        writeNow(socket, state, ::morph::wire::encode(::morph::wire::makeErr("rate limited", peekedCallId())));
        return;
    }

    // A reply counts toward the connection's backlog like an event, and is
    // never held back.
    QPointer<QtWebSocketServer> const self{this};
    QPointer<QWebSocket> const weakSocket{socket};
    _server.handle(
        message.toStdString(),
        [self, weakSocket, backlog = state.backlog](const std::string& reply) {
            static_cast<void>(queueEnvelope(self, weakSocket, backlog, reply));
        },
        state.cid);
}

void QtWebSocketServer::onDisconnected() {
    auto* socket = qobject_cast<QWebSocket*>(sender());
    if (socket == nullptr) {
        return;
    }
    auto iter = _clients.find(socket);
    if (iter != _clients.end()) {
        // Close code and reason are only available on the socket while it is
        // still around; capture them before erase()/deleteLater() below --
        // they are the useful part of "why did that one drop?", the question
        // an operator otherwise has no way to answer from the server side.
        ::morph::log::logInfo("[QtWebSocketServer] connection {} disconnected ({} live), closeCode={} reason={}",
                              iter->second.cid, _clients.size() - 1, static_cast<int>(socket->closeCode()),
                              socket->closeReason().toStdString());
        _server.closeConnection(iter->second.cid);
        if (iter->second.handshakeTimer != nullptr) {
            iter->second.handshakeTimer->stop();
            iter->second.handshakeTimer->deleteLater();
        }
        forgetConnection(iter->second);
        _clients.erase(iter);
        socket->deleteLater();
    }
}

void QtWebSocketServer::onHousekeepingTick() {
    if (_cfg.idleTimeout.count() <= 0) {
        return;
    }
    auto const now = std::chrono::steady_clock::now();
    std::vector<QWebSocket*> toClose;
    for (auto& [socket, state] : _clients) {
        if (now - state.lastActivity > _cfg.idleTimeout) {
            toClose.push_back(socket);
        }
    }
    for (auto* socket : toClose) {
        socket->close();
    }
}

}  // namespace morph::qt
