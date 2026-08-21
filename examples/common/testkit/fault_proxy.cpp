// SPDX-License-Identifier: Apache-2.0
#include "testkit/fault_proxy.hpp"

#include <QHostAddress>
#include <QTimer>

#include <utility>

namespace morph::ladder::testkit {

FaultProxy::FaultProxy(QUrl upstreamUrl, QObject* parent) : QObject{parent}, _upstreamUrl{std::move(upstreamUrl)} {}

FaultProxy::~FaultProxy() {
    if (_listener) {
        _listener->close();
    }
    if (_clientSocket != nullptr) {
        _clientSocket->disconnect();
        _clientSocket->abort();
    }
    if (_upstreamSocket != nullptr) {
        _upstreamSocket->disconnect();
        _upstreamSocket->abort();
    }
}

QUrl FaultProxy::start() {
    _listener = std::make_unique<QWebSocketServer>(QStringLiteral("morph-ladder-fault-proxy"),
                                                   QWebSocketServer::NonSecureMode);
    connect(_listener.get(), &QWebSocketServer::newConnection, this, &FaultProxy::onClientConnection);
    detail::throwIfFaultProxyListenFailed(_listener->listen(QHostAddress::LocalHost, 0));
    _url = QUrl{QString("ws://127.0.0.1:%1").arg(_listener->serverPort())};
    return _url;
}

void FaultProxy::dropReply(std::uint64_t callId) {
    std::lock_guard lock{_rulesMtx};
    _rules[callId].drop = true;
}

void FaultProxy::delayReply(std::uint64_t callId, std::chrono::milliseconds delay) {
    std::lock_guard lock{_rulesMtx};
    _rules[callId].delay = delay;
}

void FaultProxy::duplicateReply(std::uint64_t callId) {
    std::lock_guard lock{_rulesMtx};
    _rules[callId].duplicate = true;
}

void FaultProxy::killAfter(std::uint64_t callId) {
    std::lock_guard lock{_rulesMtx};
    _rules[callId].kill = true;
}

void FaultProxy::setRequestObserver(std::function<void(std::uint64_t, FaultProxy&)> observer) {
    _requestObserver = std::move(observer);
}

FaultProxy::Rule FaultProxy::ruleFor(std::uint64_t callId) {
    std::lock_guard lock{_rulesMtx};
    auto iter = _rules.find(callId);
    return iter == _rules.end() ? Rule{} : iter->second;
}

void FaultProxy::onClientConnection() {
    auto* incoming = _listener->nextPendingConnection();
    if (!detail::isValidIncomingConnection(incoming)) {
        return;
    }
    // One client leg at a time (see the class doc comment). A reconnect after
    // killAfter arrives here as a fresh connection replacing the aborted one.
    if (_clientSocket != nullptr) {
        _clientSocket->disconnect();
        _clientSocket->abort();
        _clientSocket->deleteLater();
    }
    _clientSocket = incoming;
    connect(_clientSocket, &QWebSocket::textMessageReceived, this, &FaultProxy::onClientTextMessage);
    connect(_clientSocket, &QWebSocket::disconnected, this, [this] { _clientSocket = nullptr; });

    if (_upstreamSocket == nullptr) {
        _upstreamSocket = new QWebSocket{QString{}, QWebSocketProtocol::VersionLatest, this};
        connect(_upstreamSocket, &QWebSocket::connected, this, &FaultProxy::onUpstreamConnected);
        connect(_upstreamSocket, &QWebSocket::textMessageReceived, this, &FaultProxy::onUpstreamTextMessage);
        _upstreamSocket->open(_upstreamUrl);
    }
}

void FaultProxy::onClientTextMessage(const QString& message) {
    // Report the request before forwarding it. This runs while the frame is
    // still in this proxy, so a rule armed from the observer is installed
    // strictly before the upstream server can produce a reply for it — the
    // race-free way to name "call k" from outside the wire layer (see
    // setRequestObserver).
    if (_requestObserver) {
        const std::uint64_t callId = detail::decodeCallIdOrZero(message);
        if (callId != 0) {
            _requestObserver(callId, *this);
        }
    }

    // Client -> server direction is forwarded verbatim; every rule this proxy
    // supports targets the reply (server -> client) leg, matching
    // TESTING.md's "drop exactly the reply frame of call k".
    if (_upstreamSocket != nullptr && _upstreamConnected) {
        _upstreamSocket->sendTextMessage(message);
    } else {
        // The upstream handshake is still in flight; a write now would be
        // dropped on the floor. Buffer instead — the very first client frame
        // (a synchronous `register`) reliably lands in this window.
        _upstreamBacklog.push_back(message);
    }
}

void FaultProxy::onUpstreamConnected() {
    _upstreamConnected = true;
    auto backlog = std::move(_upstreamBacklog);
    _upstreamBacklog.clear();
    for (const auto& message : backlog) {
        _upstreamSocket->sendTextMessage(message);
    }
}

void FaultProxy::sendToClient(const QString& message) {
    if (_clientSocket != nullptr) {
        _clientSocket->sendTextMessage(message);
        ++_repliesForwarded;
    }
}

void FaultProxy::onUpstreamTextMessage(const QString& message) {
    const std::uint64_t callId = detail::decodeCallIdOrZero(message);
    const Rule rule = ruleFor(callId);

    if (rule.drop) {
        return;
    }
    if (rule.kill) {
        if (_clientSocket != nullptr) {
            // Detach the dying socket's signals before aborting: a queued
            // `disconnected` from it, delivered after the client's automatic
            // reconnect has already installed a fresh leg, would otherwise
            // null out that new leg.
            _clientSocket->disconnect();
            _clientSocket->abort();
            _clientSocket = nullptr;
        }
        return;
    }

    const int copies = rule.duplicate ? 2 : 1;
    for (int i = 0; i < copies; ++i) {
        if (rule.delay) {
            QTimer::singleShot(*rule.delay, this, [this, message] { sendToClient(message); });
        } else {
            sendToClient(message);
        }
    }
}

}  // namespace morph::ladder::testkit
