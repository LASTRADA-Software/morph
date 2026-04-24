// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QTimer>
#include <algorithm>
#include <morph/qt/qt_websocket_backend.hpp>
#include <cctype>
#include <stdexcept>

namespace morph::qt {

QtWebSocketBackend::QtWebSocketBackend(QUrl serverUrl, ::morph::model::detail::ActionDispatcher& /*dispatcher*/,
                                       ::morph::model::detail::ModelRegistryFactory& /*registry*/,
                                       std::optional<QSslConfiguration> tls) {
    if (tls.has_value()) {
        _socket.setSslConfiguration(*tls);
    }

    QObject::connect(&_socket, &QWebSocket::connected, [this]() {
        _connected = true;
        if (_syncLoop) {
            _syncLoop->quit();
        }
    });
    QObject::connect(&_socket, &QWebSocket::disconnected, [this]() { _connected = false; });
    QObject::connect(&_socket, &QWebSocket::textMessageReceived, [this](const QString& msg) { onTextMessage(msg); });

    _socket.open(serverUrl);
}

QtWebSocketBackend::~QtWebSocketBackend() {  // NOLINT(modernize-use-equals-default)
    // Disconnect all signals first so no slot tries to access our members after they destruct.
    _socket.disconnect();
    // Abort cleanly: sends TCP RST without attempting close handshake.
    _socket.abort();
    // Drain the event queue so Qt's internal WebSocket state machine fully settles
    // before _socket's QObject destructor runs its own cleanup.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents | QEventLoop::ExcludeSocketNotifiers);
}

bool QtWebSocketBackend::waitForConnected(int timeoutMs) {  // NOLINT(readability-make-member-function-const)
    if (_connected) {
        return true;
    }
    QEventLoop loop;
    _syncLoop = &loop;
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    _syncLoop = nullptr;
    return _connected;
}

std::string QtWebSocketBackend::sendSync(const std::string& msg) {
    _socket.sendTextMessage(QString::fromStdString(msg));
    QEventLoop loop;
    _syncLoop = &loop;
    loop.exec();
    _syncLoop = nullptr;
    return _pendingReply;
}

::morph::exec::detail::ModelId QtWebSocketBackend::registerModel(
    const std::string& typeId,
    std::function<std::unique_ptr<::morph::model::detail::IModelHolder>()> /*factory*/) {
    auto reply = sendSync("register|" + typeId);
    if (reply.starts_with("ok|")) {
        return ::morph::exec::detail::ModelId{std::stoull(reply.substr(3))};
    }
    throw std::runtime_error("register failed: " + reply);
}

void QtWebSocketBackend::deregisterModel(::morph::exec::detail::ModelId mid) {
    // Fire-and-forget — server cleans up remaining models when connection closes.
    // Avoids a nested QEventLoop during destructor which can trigger Qt asserts.
    if (_connected) {
        _socket.sendTextMessage(QString::fromStdString("deregister|" + std::to_string(mid.v)));
    }
}

::morph::async::Completion<std::shared_ptr<void>> QtWebSocketBackend::execute(
    ::morph::exec::detail::ModelId mid, ::morph::backend::detail::ActionCall call,
    ::morph::exec::IExecutor* cbExec) {
    auto compState = std::make_shared<::morph::async::detail::CompletionState<std::shared_ptr<void>>>();
    ::morph::async::Completion<std::shared_ptr<void>> comp{compState, cbExec};

    uint64_t callId = _nextCallId++;
    std::string body = call.serializeAction();
    std::string msg = "execute|" + std::to_string(callId) + "|" + std::to_string(mid.v) + "|" + call.modelTypeId +
                      "|" + call.actionTypeId + "|" + body;

    {
        std::scoped_lock lock{_pendingMtx};
        _pending[callId] = PendingExecute{compState, std::move(call.deserializeResult), cbExec};
    }

    _socket.sendTextMessage(QString::fromStdString(msg));
    return comp;
}

void QtWebSocketBackend::onTextMessage(const QString& message) {
    std::string msg = message.toStdString();

    auto firstPipe = msg.find('|');
    if (firstPipe == std::string::npos) {
        // bare "ok" — sync reply (deregister)
        _pendingReply = msg;
        if (_syncLoop) {
            _syncLoop->quit();
        }
        return;
    }

    auto secondPipe = msg.find('|', firstPipe + 1);
    if (secondPipe != std::string::npos) {
        std::string field1 = msg.substr(firstPipe + 1, secondPipe - firstPipe - 1);
        bool isNumeric = !field1.empty() && std::all_of(field1.begin(), field1.end(),
                                                        [](unsigned char chr) { return std::isdigit(chr) != 0; });

        if (isNumeric) {
            uint64_t callId = std::stoull(field1);
            PendingExecute pending;
            {
                std::scoped_lock lock{_pendingMtx};
                auto iter = _pending.find(callId);
                if (iter == _pending.end()) {
                    return;
                }
                pending = std::move(iter->second);
                _pending.erase(iter);
            }
            std::string_view prefix{msg.data(), firstPipe};
            std::string_view payload{msg.data() + secondPipe + 1, msg.size() - secondPipe - 1};
            if (prefix == "ok") {
                try {
                    pending.state->setValue(pending.deserialize(payload));
                } catch (...) {
                    pending.state->setException(std::current_exception());
                }
            } else {
                pending.state->setException(std::make_exception_ptr(std::runtime_error(std::string{payload})));
            }
            return;
        }
    }

    // Sync reply (register returns "ok|{mid}", errors return "err|...")
    _pendingReply = msg;
    if (_syncLoop) {
        _syncLoop->quit();
    }
}

}  // namespace morph::qt
