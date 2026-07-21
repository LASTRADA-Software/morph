// SPDX-License-Identifier: Apache-2.0

// Adversarial cross-socket coverage for morph::qt::QtWebSocketServer /
// morph::backend::RemoteServer: a hostile client sends oversized frames, a
// rapid flood of messages, a duplicate-JSON-key envelope, and opens a
// connection it then never speaks on again. The server must stay up and keep
// serving honest clients throughout every scenario -- see
// docs/spec/testing_strategy.md and docs/spec/security.md.
//
// These tests intentionally run the hostile client and the honest client in
// the SAME process, both talking to the server over a real loopback TCP
// socket (like most of test_qt_websocket.cpp) rather than as two separate OS
// processes: the gap this fills is "a hostile peer over a real socket," not
// "OS-level process separation" (see test_qt_websocket.cpp's dedicated
// "Process separation: ..." cases for that).
//
// Current behavior note: `RemoteServer::LimitPolicy` and
// `QtWebSocketServerConfig` (maxConnections/maxMessageBytes/messagesPerSecond/
// handshakeTimeout/idleTimeout -- see docs/spec/core/backend.md,
// "LimitPolicy" and "Resource limits") are both implemented and each already
// has dedicated, precise unit coverage in tests/test_limit_policy.cpp and
// tests/qt/test_qt_websocket.cpp. Every server constructed below uses the
// *default*, unconfigured `QtWebSocketServerConfig`, under which
// `maxConnections`/`messagesPerSecond`/`handshakeTimeout`/`idleTimeout` are
// all still unbounded/disabled (`0`) -- so the flood and stalled-connection
// scenarios below are not rejected or capped -- but `maxMessageBytes`
// defaults to `wire::kMaxEnvelopeBytes`, not `0`, so the oversized-frame
// scenario IS actively rejected by default. Either way, the assertion this
// file cares about is the same: **the server keeps serving honest clients**
// through all four scenarios, not whether any individual hostile input is
// itself rejected outright.

#include <QCoreApplication>
#include <QUrl>
#include <QWebSocket>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <string>
#include <thread>

// ── Test model ────────────────────────────────────────────────────────────────

struct AdvEchoAction {
    int value = 0;
};
struct AdvEchoModel {
    int execute(const AdvEchoAction& act) { return act.value; }
};

BRIDGE_REGISTER_MODEL(AdvEchoModel, "Adv_EchoModel")
BRIDGE_REGISTER_ACTION(AdvEchoModel, AdvEchoAction, "Adv_EchoAction")

namespace {

// Pumps the Qt event loop until `done()` returns true or `maxIterations * 10ms`
// elapses. Local copy of test_qt_websocket.cpp's helper -- each Catch2
// translation unit in this target needs its own (the two files don't share
// non-exported statics).
void pumpUntil(const std::function<bool()>& done, int maxIterations = 100) {
    for (int idx = 0; idx < maxIterations && !done(); ++idx) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::ExcludeUserInputEvents);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// Confirms an honest client can still register a model and execute an action
// against `url`, end to end. Called after each hostile scenario below to
// prove the server is still serving normally.
void requireServerStillServesHonestClients(const QUrl& url) {
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());
    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<AdvEchoModel> handler{bridge, &qtExec};

    std::atomic<int> result{-1};
    handler.execute(AdvEchoAction{42})
        .then([&](int val) { result.store(val); })
        .onError([](const std::exception_ptr&) {});
    pumpUntil([&] { return result.load() != -1; });
    REQUIRE(result.load() == 42);
}

}  // namespace

TEST_CASE("Adversarial: oversized frame does not take down the server", "[qt][ws][adversarial]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());
    const QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};

    QWebSocket hostile;
    hostile.open(url);
    pumpUntil([&] { return hostile.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(hostile.state() == QAbstractSocket::ConnectedState);

    // Build an execute envelope whose body alone exceeds kMaxEnvelopeBytes.
    // The server's default QtWebSocketServerConfig::maxMessageBytes ==
    // wire::kMaxEnvelopeBytes, so this is rejected before it ever reaches
    // RemoteServer::handle() (see qt_websocket_server.cpp) -- reliably, not
    // just "maybe": this is what tests/qt/test_qt_websocket.cpp's dedicated
    // "maxMessageBytes rejects an oversized frame before dispatch" case pins
    // with a small custom cap; here the same enforcement fires against the
    // real 8 MiB default cap with a frame 1 MiB over it.
    morph::wire::Envelope req;
    req.kind = "execute";
    req.modelType = "Adv_EchoModel";
    req.actionType = "Adv_EchoAction";
    req.body = std::string(morph::wire::kMaxEnvelopeBytes + (1U << 20), 'a');  // +1 MiB over the cap

    QString replyMsg;
    bool gotReply = false;
    auto conn = QObject::connect(&hostile, &QWebSocket::textMessageReceived, [&](const QString& msg) {
        replyMsg = msg;
        gotReply = true;
    });
    hostile.sendTextMessage(QString::fromStdString(morph::wire::encode(req)));
    pumpUntil([&] { return gotReply; }, 200);
    QObject::disconnect(conn);

    REQUIRE(gotReply);
    auto replyEnv = morph::wire::decode(replyMsg.toStdString());
    CHECK(replyEnv.kind == "err");
    CHECK(replyEnv.message.find("maxMessageBytes") != std::string::npos);

    hostile.close();
    pumpUntil([&] { return hostile.state() == QAbstractSocket::UnconnectedState; }, 50);
    requireServerStillServesHonestClients(url);
}

TEST_CASE("Adversarial: a rapid flood of messages does not crash the server", "[qt][ws][adversarial]") {
    morph::exec::ThreadPoolExecutor serverPool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());
    const QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};

    QWebSocket hostile;
    hostile.open(url);
    pumpUntil([&] { return hostile.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(hostile.state() == QAbstractSocket::ConnectedState);

    // Fire a burst of tiny execute-against-unknown-model messages without
    // waiting for any reply. This server's QtWebSocketServerConfig::
    // messagesPerSecond is left at its default (0 == unbounded, see
    // qt_websocket_server.hpp) -- test_qt_websocket.cpp's dedicated
    // "messagesPerSecond throttles a burst" case covers the *configured*
    // enforcement path directly -- so here the server must simply not crash
    // or wedge under an uncapped burst.
    constexpr int floodSize = 5000;
    for (int i = 0; i < floodSize; ++i) {
        morph::wire::Envelope req;
        req.kind = "execute";
        req.callId = static_cast<uint64_t>(i);
        req.modelId = 999999;
        req.modelType = "Adv_EchoModel";
        req.actionType = "Adv_EchoAction";
        req.body = R"({"value":1})";
        hostile.sendTextMessage(QString::fromStdString(morph::wire::encode(req)));
    }
    // Drain whatever replies do come back so the socket's send/receive buffers
    // don't back up indefinitely, then give the pool time to work through the
    // backlog.
    pumpUntil([] { return false; }, 300);

    hostile.close();
    pumpUntil([&] { return hostile.state() == QAbstractSocket::UnconnectedState; }, 50);
    requireServerStillServesHonestClients(url);
}

TEST_CASE("Adversarial: duplicate-JSON-key envelope does not crash the server", "[qt][ws][adversarial]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());
    const QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};

    QWebSocket hostile;
    hostile.open(url);
    pumpUntil([&] { return hostile.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(hostile.state() == QAbstractSocket::ConnectedState);

    // The wire-layer smuggling primitive from docs/spec/core/wire.md: glaze
    // accepts a duplicated top-level key (last-wins) rather than rejecting
    // it. Confirm this holds over a real socket too, and does not crash or
    // wedge the server.
    const QString dup =
        QStringLiteral(R"({"kind":"register","kind":"deregister","typeId":"Adv_EchoModel","modelId":999999})");
    QString reply;
    bool got = false;
    auto conn = QObject::connect(&hostile, &QWebSocket::textMessageReceived, [&](const QString& msg) {
        reply = msg;
        got = true;
    });
    hostile.sendTextMessage(dup);
    pumpUntil([&] { return got; }, 100);
    QObject::disconnect(conn);
    REQUIRE(got);
    // Last-wins: "kind" resolves to "deregister" (an unknown modelId, so it's
    // simply a no-op "ok" per RemoteServer::dispatchMessage's deregister
    // branch -- see include/morph/core/remote.hpp). The point here is that it
    // decodes and replies at all, not which branch it takes.
    REQUIRE_NOTHROW(morph::wire::decode(reply.toStdString()));

    hostile.close();
    pumpUntil([&] { return hostile.state() == QAbstractSocket::UnconnectedState; }, 50);
    requireServerStillServesHonestClients(url);
}

TEST_CASE("Adversarial: a client that opens then stalls does not block other clients", "[qt][ws][adversarial]") {
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());
    const QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};

    // Opens a connection and never sends a single frame on it. This server's
    // QtWebSocketServerConfig::handshakeTimeout/idleTimeout are left at their
    // defaults (0 == disabled, see qt_websocket_server.hpp) -- test_qt_websocket.cpp's
    // dedicated "handshakeTimeout closes a silent connection" / "idleTimeout
    // closes a connection that goes silent after activity" cases cover the
    // *configured* enforcement path directly -- so here the connection is
    // simply expected to sit open without affecting anyone else.
    QWebSocket stalled;
    stalled.open(url);
    pumpUntil([&] { return stalled.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(stalled.state() == QAbstractSocket::ConnectedState);

    requireServerStillServesHonestClients(url);
    requireServerStillServesHonestClients(url);  // twice, to show it's not a one-shot fluke

    REQUIRE(stalled.state() == QAbstractSocket::ConnectedState);  // still open, untouched
    stalled.close();
    pumpUntil([&] { return stalled.state() == QAbstractSocket::UnconnectedState; }, 50);
}
