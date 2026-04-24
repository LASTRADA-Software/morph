// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QWebSocket>
#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <morph/registry.hpp>
#include <morph/remote.hpp>
#include <atomic>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>


// ── Shared QCoreApplication ──────────────────────────────────────────────────
// QCoreApplication is owned by main() (below) and torn down before any global
// or static-local destructor runs, so Qt's QObject cleanup happens while the
// event loop machinery is still valid. ensureApp() exists only as a sanity
// check that main() ran first.
static QCoreApplication* ensureApp() {
    auto* app = QCoreApplication::instance();
    REQUIRE(app != nullptr);
    return app;
}

// ── Test models ──────────────────────────────────────────────────────────────
struct WsEchoAction {
    int value = 0;
};
struct WsEchoFail {};

struct WsEchoModel {
    int execute(WsEchoAction action) { return action.value; }
    int execute(WsEchoFail) { throw std::runtime_error("echo failed"); }
};

BRIDGE_REGISTER_MODEL(WsEchoModel, "WsEchoModel")
BRIDGE_REGISTER_ACTION(WsEchoModel, WsEchoAction, "WsEchoAction")
BRIDGE_REGISTER_ACTION(WsEchoModel, WsEchoFail, "WsEchoFail")

// Stateful counter — used to verify per-client isolation. Each client registers
// its own instance on the server, so increments on one client must not bleed
// into another client's counter.
struct WsAddAction {
    int by = 0;
};

struct WsCounterModel {
    int value = 0;
    int execute(WsAddAction action) {
        value += action.by;
        return value;
    }
};

BRIDGE_REGISTER_MODEL(WsCounterModel, "WsCounterModel")
BRIDGE_REGISTER_ACTION(WsCounterModel, WsAddAction, "WsAddAction")

// ── Poll helper — pumps Qt event loop while waiting ──────────────────────────
static void pumpUntil(const std::function<bool()>& done, int maxIterations = 50) {
    for (int idx = 0; idx < maxIterations && !done(); ++idx) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Drain any deferred deletes so Qt objects can be safely destroyed next.
    QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::ExcludeUserInputEvents);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// ── TLS config helpers ───────────────────────────────────────────────────────
static QSslConfiguration makeServerTlsConfig() {
    QFile certFile{QStringLiteral(TESTS_CERTS_DIR "/server.crt")};
    QFile keyFile{QStringLiteral(TESTS_CERTS_DIR "/server.key")};
    REQUIRE(certFile.open(QIODevice::ReadOnly));
    REQUIRE(keyFile.open(QIODevice::ReadOnly));

    QSslConfiguration cfg;
    cfg.setLocalCertificate(QSslCertificate{&certFile, QSsl::Pem});
    cfg.setPrivateKey(QSslKey{&keyFile, QSsl::Rsa, QSsl::Pem});
    return cfg;
}

static QSslConfiguration makeClientTlsConfig() {
    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
    return cfg;
}

// ── Plain WebSocket tests ────────────────────────────────────────────────────

TEST_CASE("morph::qt::QtWebSocketBackend: action result delivered via then", "[qt][ws]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    std::atomic<int> result{-1};  // declared BEFORE handler so it outlives it
    morph::bridge::BridgeHandler<WsEchoModel> handler{bridge, &qtExec};

    handler.execute(WsEchoAction{99}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {
    });

    pumpUntil([&] { return result.load() != -1; });
    REQUIRE(result.load() == 99);
}

TEST_CASE("morph::qt::QtWebSocketBackend: exception delivered via onError", "[qt][ws]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<WsEchoModel> handler{bridge, &qtExec};

    std::atomic<bool> errorFired{false};
    handler.execute(WsEchoFail{}).then([](int) {}).onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::runtime_error&) {
            errorFired.store(true);
        }
    });

    pumpUntil([&] { return errorFired.load(); });
    REQUIRE(errorFired.load());
}

TEST_CASE("morph::qt::QtWebSocketBackend: multiple actions on same handler", "[qt][ws]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<WsEchoModel> handler{bridge, &qtExec};

    std::atomic<int> sum{0};
    std::atomic<int> count{0};
    constexpr int numActions = 5;

    for (int idx = 1; idx <= numActions; ++idx) {
        handler.execute(WsEchoAction{idx})
            .then([&](int val) {
                sum.fetch_add(val);
                count.fetch_add(1);
            })
            .onError([](const std::exception_ptr&) {});
    }

    pumpUntil([&] { return count.load() == numActions; }, 100);
    REQUIRE(count.load() == numActions);
    REQUIRE(sum.load() == 15);  // 1+2+3+4+5
}

// ── Multi-client tests ───────────────────────────────────────────────────────

TEST_CASE("Two QtWebSocketBackends share one server but have isolated model state", "[qt][ws][multi-client]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    auto backendA = std::make_unique<morph::qt::QtWebSocketBackend>(url);
    auto backendB = std::make_unique<morph::qt::QtWebSocketBackend>(url);
    REQUIRE(backendA->waitForConnected());
    REQUIRE(backendB->waitForConnected());

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridgeA{std::move(backendA)};
    morph::bridge::Bridge bridgeB{std::move(backendB)};
    morph::bridge::BridgeHandler<WsCounterModel> handlerA{bridgeA, &qtExec};
    morph::bridge::BridgeHandler<WsCounterModel> handlerB{bridgeB, &qtExec};

    std::atomic<int> lastA{-1};
    std::atomic<int> lastB{-1};

    // Client A increments by 10 three times → expect 10, 20, 30 with final 30.
    for (int idx = 0; idx < 3; ++idx) {
        handlerA.execute(WsAddAction{10})
            .then([&](int val) { lastA.store(val); })
            .onError([](const std::exception_ptr&) {});
    }
    // Client B increments by 1 twice → expect 1, 2 with final 2.
    for (int idx = 0; idx < 2; ++idx) {
        handlerB.execute(WsAddAction{1})
            .then([&](int val) { lastB.store(val); })
            .onError([](const std::exception_ptr&) {});
    }

    pumpUntil([&] { return lastA.load() == 30 && lastB.load() == 2; }, 200);
    REQUIRE(lastA.load() == 30);
    REQUIRE(lastB.load() == 2);
}

TEST_CASE("Many QtWebSocketBackends concurrently dispatch — every action resolves", "[qt][ws][multi-client]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};

    constexpr int numClients = 4;
    constexpr int perClient = 8;
    std::vector<std::unique_ptr<morph::qt::QtWebSocketBackend>> backends;
    backends.reserve(numClients);
    for (int idx = 0; idx < numClients; ++idx) {
        auto backend = std::make_unique<morph::qt::QtWebSocketBackend>(url);
        REQUIRE(backend->waitForConnected());
        backends.push_back(std::move(backend));
    }

    morph::qt::QtExecutor qtExec;
    std::vector<std::unique_ptr<morph::bridge::Bridge>> bridges;
    std::vector<std::unique_ptr<morph::bridge::BridgeHandler<WsCounterModel>>> handlers;
    bridges.reserve(numClients);
    handlers.reserve(numClients);
    for (auto& backend : backends) {
        auto bridge = std::make_unique<morph::bridge::Bridge>(std::move(backend));
        handlers.push_back(std::make_unique<morph::bridge::BridgeHandler<WsCounterModel>>(*bridge, &qtExec));
        bridges.push_back(std::move(bridge));
    }
    backends.clear();

    std::atomic<int> resolved{0};
    for (int clientIdx = 0; clientIdx < numClients; ++clientIdx) {
        for (int actionIdx = 1; actionIdx <= perClient; ++actionIdx) {
            handlers[static_cast<std::size_t>(clientIdx)]
                ->execute(WsAddAction{actionIdx})
                .then([&](int) { resolved.fetch_add(1); })
                .onError([](const std::exception_ptr&) {});
        }
    }

    constexpr int total = numClients * perClient;
    pumpUntil([&] { return resolved.load() == total; }, 500);
    REQUIRE(resolved.load() == total);
}

// ── Lifecycle tests ──────────────────────────────────────────────────────────

TEST_CASE("morph::qt::QtWebSocketBackend connecting to closed port fails to connect", "[qt][ws][lifecycle]") {
    ensureApp();

    // Port 1 is reserved (root-only) on Linux and not bound — connection refused.
    QUrl url{QString("ws://127.0.0.1:1")};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url);
    REQUIRE_FALSE(backendPtr->waitForConnected(200));
}

TEST_CASE("morph::qt::QtWebSocketBackend reconnects to a fresh server on the same port", "[qt][ws][lifecycle]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);

    quint16 port = 0;
    {
        morph::qt::QtWebSocketServer wsServer{*server, 0};
        REQUIRE(wsServer.listen());
        port = wsServer.port();

        QUrl url{QString("ws://127.0.0.1:%1").arg(port)};
        auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url);
        REQUIRE(backendPtr->waitForConnected());

        morph::qt::QtExecutor qtExec;
        morph::bridge::Bridge bridge{std::move(backendPtr)};
        morph::bridge::BridgeHandler<WsEchoModel> handler{bridge, &qtExec};
        std::atomic<int> result{-1};
        handler.execute(WsEchoAction{7}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {
        });
        pumpUntil([&] { return result.load() != -1; });
        REQUIRE(result.load() == 7);
    }
    // Server destroyed — give the OS a moment to release the port.
    pumpUntil([] { return false; }, 5);

    // Bring up a fresh server on the same port and reconnect.
    morph::qt::QtWebSocketServer wsServer{*server, port};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(port)};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected(2000));

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<WsEchoModel> handler{bridge, &qtExec};
    std::atomic<int> result{-1};
    handler.execute(WsEchoAction{42}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {
    });
    pumpUntil([&] { return result.load() != -1; });
    REQUIRE(result.load() == 42);
}

TEST_CASE("Server closing notifies morph::qt::QtWebSocketBackend disconnected signal", "[qt][ws][lifecycle]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    auto wsServer = std::make_unique<morph::qt::QtWebSocketServer>(*server, 0);
    REQUIRE(wsServer->listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer->port())};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<WsEchoModel> handler{bridge, &qtExec};

    // Sanity: round-trip works while the server is alive.
    std::atomic<int> warmup{-1};
    handler.execute(WsEchoAction{1}).then([&](int val) { warmup.store(val); }).onError([](const std::exception_ptr&) {
    });
    pumpUntil([&] { return warmup.load() == 1; });
    REQUIRE(warmup.load() == 1);

    // Close the server: client sockets are aborted, then deleteLater'd.
    wsServer->close();
    wsServer.reset();
    // Drain the close notifications. waitForConnected returns the *current*
    // _connected flag, which flips to false once the disconnected signal fires.
    pumpUntil([] { return false; }, 20);
    morph::qt::QtWebSocketBackend* rawBackend = nullptr;  // backend is owned by the bridge — no direct access needed
    (void)rawBackend;
}

// ── Malformed-protocol tests (raw QWebSocket) ────────────────────────────────
//
// These tests use a bare QWebSocket so they can send arbitrary garbage that the
// real backend would never produce. The server is expected to reply with
// `err|<msg>` for every malformed input rather than crash or hang.

namespace {

// Helper: sends `request`, returns the next text frame the server replies with.
// Pumps the Qt loop while waiting; fails the REQUIRE on timeout.
std::string sendRawAndAwaitReply(QWebSocket& sock, const QString& request) {
    QString reply;
    bool got = false;
    auto conn = QObject::connect(&sock, &QWebSocket::textMessageReceived, [&](const QString& msg) {
        reply = msg;
        got = true;
    });
    sock.sendTextMessage(request);
    pumpUntil([&] { return got; }, 100);
    QObject::disconnect(conn);
    REQUIRE(got);
    return reply.toStdString();
}

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("Server rejects malformed protocol messages", "[qt][ws][protocol]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    QWebSocket sock;
    sock.open(url);
    pumpUntil([&] { return sock.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(sock.state() == QAbstractSocket::ConnectedState);

    SECTION("bare unknown message type") {
        auto reply = sendRawAndAwaitReply(sock, "hello");
        REQUIRE(reply == "err|bad msg type");
    }

    SECTION("register without typeId") {
        auto reply = sendRawAndAwaitReply(sock, "register");
        REQUIRE(reply.starts_with("err|"));
    }

    SECTION("register with unknown typeId") {
        auto reply = sendRawAndAwaitReply(sock, "register|NoSuchModel");
        REQUIRE(reply.starts_with("err|"));
        REQUIRE(reply.contains("unknown model type"));
    }

    SECTION("deregister without modelId") {
        auto reply = sendRawAndAwaitReply(sock, "deregister");
        REQUIRE(reply.starts_with("err|"));
    }

    SECTION("execute with too few parts") {
        auto reply = sendRawAndAwaitReply(sock, "execute|1|2");
        REQUIRE(reply.starts_with("err|"));
    }

    SECTION("execute with non-numeric callId") {
        auto reply = sendRawAndAwaitReply(sock, "execute|notnum|1|WsEchoModel|WsEchoAction|{\"value\":1}");
        REQUIRE(reply.starts_with("err|"));
    }

    SECTION("execute against unknown modelId") {
        auto reply = sendRawAndAwaitReply(sock, "execute|7|999|WsEchoModel|WsEchoAction|{\"value\":1}");
        // Format: err|<callId>|model not found
        REQUIRE(reply.starts_with("err|7|"));
        REQUIRE(reply.contains("model not found"));
    }

    SECTION("execute with malformed JSON body") {
        // Register a model first so dispatch reaches fromJson.
        auto regReply = sendRawAndAwaitReply(sock, "register|WsEchoModel");
        REQUIRE(regReply.starts_with("ok|"));
        auto mid = regReply.substr(3);

        auto reply = sendRawAndAwaitReply(sock, QString("execute|9|%1|WsEchoModel|WsEchoAction|{not json")
                                                    .arg(QString::fromStdString(mid)));
        REQUIRE(reply.starts_with("err|9|"));
    }

    SECTION("execute against unknown actionTypeId") {
        auto regReply = sendRawAndAwaitReply(sock, "register|WsEchoModel");
        REQUIRE(regReply.starts_with("ok|"));
        auto mid = regReply.substr(3);

        auto reply =
            sendRawAndAwaitReply(sock, QString("execute|10|%1|WsEchoModel|NoSuchAction|{}").arg(QString::fromStdString(mid)));
        REQUIRE(reply.starts_with("err|10|"));
        REQUIRE(reply.contains("unknown action"));
    }

    sock.close();
    pumpUntil([&] { return sock.state() == QAbstractSocket::UnconnectedState; }, 50);
}

TEST_CASE("Server keeps serving good clients after a malformed message", "[qt][ws][protocol]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};

    // Misbehaving raw client.
    QWebSocket badSock;
    badSock.open(url);
    pumpUntil([&] { return badSock.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(badSock.state() == QAbstractSocket::ConnectedState);
    auto badReply = sendRawAndAwaitReply(badSock, "garbage|garbage|garbage");
    REQUIRE(badReply.starts_with("err|"));

    // Well-behaved backend on the same server should still work end-to-end.
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());
    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<WsEchoModel> handler{bridge, &qtExec};

    std::atomic<int> result{-1};
    handler.execute(WsEchoAction{77}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {
    });
    pumpUntil([&] { return result.load() != -1; });
    REQUIRE(result.load() == 77);

    badSock.close();
    pumpUntil([&] { return badSock.state() == QAbstractSocket::UnconnectedState; }, 50);
}

// ── TLS WebSocket tests ──────────────────────────────────────────────────────

TEST_CASE("morph::qt::QtWebSocketBackend TLS: action result delivered via then", "[qt][wss]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0, makeServerTlsConfig()};
    REQUIRE(wsServer.listen());

    QUrl url{QString("wss://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr =
        std::make_unique<morph::qt::QtWebSocketBackend>(url, morph::model::detail::defaultDispatcher(), morph::model::detail::defaultRegistry(), makeClientTlsConfig());
    REQUIRE(backendPtr->waitForConnected());

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<WsEchoModel> handler{bridge, &qtExec};

    std::atomic<int> result{-1};
    handler.execute(WsEchoAction{99}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {
    });

    pumpUntil([&] { return result.load() != -1; });
    REQUIRE(result.load() == 99);
}

TEST_CASE("morph::qt::QtWebSocketBackend TLS: exception delivered via onError", "[qt][wss]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0, makeServerTlsConfig()};
    REQUIRE(wsServer.listen());

    QUrl url{QString("wss://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr =
        std::make_unique<morph::qt::QtWebSocketBackend>(url, morph::model::detail::defaultDispatcher(), morph::model::detail::defaultRegistry(), makeClientTlsConfig());
    REQUIRE(backendPtr->waitForConnected());

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<WsEchoModel> handler{bridge, &qtExec};

    std::atomic<bool> errorFired{false};
    handler.execute(WsEchoFail{}).then([](int) {}).onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::runtime_error&) {
            errorFired.store(true);
        }
    });

    pumpUntil([&] { return errorFired.load(); });
    REQUIRE(errorFired.load());
}

TEST_CASE("morph::qt::QtWebSocketBackend TLS: connection refused when client lacks TLS", "[qt][wss][lifecycle]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0, makeServerTlsConfig()};
    REQUIRE(wsServer.listen());

    // Plain `ws://` against a `wss://` server — should not establish.
    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url);
    REQUIRE_FALSE(backendPtr->waitForConnected(500));
}

// ── Process-separation tests ─────────────────────────────────────────────────
//
// QT_TEST_SERVER_BIN and QT_TEST_CLIENT_BIN are absolute paths to the helper
// binaries, baked in at compile time by CMakeLists.txt. The test launches the
// real server binary, parses its READY|<port> line, then runs the real client
// binary against it. This exercises the actual TCP/WebSocket code path between
// two processes — the only test that does, since every other test runs both
// sides inside a single QCoreApplication.

namespace {

struct ServerProcess {
    QProcess proc;
    quint16 port{0};

    bool start(const QStringList& extraArgs = {}) {
        proc.setProgram(QStringLiteral(QT_TEST_SERVER_BIN));
        proc.setArguments(extraArgs);
        proc.setProcessChannelMode(QProcess::SeparateChannels);
        proc.start();
        if (!proc.waitForStarted(2000)) {
            return false;
        }
        // Read the READY|<port> line from the child's stdout.
        QByteArray accumulated;
        for (int idx = 0; idx < 50; ++idx) {
            if (proc.waitForReadyRead(100)) {
                accumulated += proc.readAllStandardOutput();
                qsizetype newline = accumulated.indexOf('\n');
                if (newline != -1) {
                    QByteArray line = accumulated.left(newline);
                    if (line.startsWith("READY|")) {
                        port = static_cast<quint16>(line.mid(6).toUInt());
                        return port != 0;
                    }
                    return false;
                }
            }
        }
        return false;
    }

    void stop() {
        if (proc.state() == QProcess::Running) {
            proc.write("quit\n");
            proc.closeWriteChannel();
            if (!proc.waitForFinished(2000)) {
                proc.kill();
                proc.waitForFinished(1000);
            }
        }
    }

    ~ServerProcess() { stop(); }
};

int runClient(const QString& url, const QStringList& extraArgs = {}) {
    QProcess client;
    client.setProgram(QStringLiteral(QT_TEST_CLIENT_BIN));
    QStringList args{url};
    args.append(extraArgs);
    client.setArguments(args);
    client.start();
    if (!client.waitForStarted(2000)) {
        return -100;
    }
    if (!client.waitForFinished(5000)) {
        client.kill();
        client.waitForFinished(1000);
        return -101;
    }
    if (client.exitStatus() != QProcess::NormalExit) {
        return -102;
    }
    return client.exitCode();
}

}  // namespace

TEST_CASE("Process separation: client binary talks to server binary over real socket", "[qt][ws][process]") {
    REQUIRE(QFileInfo::exists(QStringLiteral(QT_TEST_SERVER_BIN)));
    REQUIRE(QFileInfo::exists(QStringLiteral(QT_TEST_CLIENT_BIN)));

    ServerProcess server;
    REQUIRE(server.start());
    REQUIRE(server.port != 0);

    QString url = QStringLiteral("ws://127.0.0.1:%1").arg(server.port);

    SECTION("happy path returns the doubled value") { REQUIRE(runClient(url) == 0); }

    SECTION("server-side exception surfaces as onError in the client") {
        REQUIRE(runClient(url, {QStringLiteral("--fail")}) == 0);
    }

    SECTION("two clients in succession against the same server both succeed") {
        REQUIRE(runClient(url) == 0);
        REQUIRE(runClient(url) == 0);
    }
}

TEST_CASE("Process separation: many client processes hit one server concurrently", "[qt][ws][process][multi-client]") {
    REQUIRE(QFileInfo::exists(QStringLiteral(QT_TEST_SERVER_BIN)));
    REQUIRE(QFileInfo::exists(QStringLiteral(QT_TEST_CLIENT_BIN)));

    ServerProcess server;
    REQUIRE(server.start());
    REQUIRE(server.port != 0);

    QString url = QStringLiteral("ws://127.0.0.1:%1").arg(server.port);

    // Spin up clients in parallel WITHOUT waiting for any to finish, so the
    // server is genuinely handling overlapping connections — not a sequence of
    // independent connect/disconnect cycles. The mix of happy-path and --fail
    // clients also exercises concurrent error replies on the server side.
    constexpr int numClients = 6;
    std::vector<std::unique_ptr<QProcess>> clients;
    std::vector<bool> shouldFail;
    clients.reserve(numClients);
    shouldFail.reserve(numClients);
    for (int idx = 0; idx < numClients; ++idx) {
        bool fail = (idx % 2) == 1;  // every other client drives EchoFailAction
        auto proc = std::make_unique<QProcess>();
        proc->setProgram(QStringLiteral(QT_TEST_CLIENT_BIN));
        QStringList args{url};
        if (fail) {
            args << QStringLiteral("--fail");
        }
        proc->setArguments(args);
        proc->start();
        REQUIRE(proc->waitForStarted(2000));
        clients.push_back(std::move(proc));
        shouldFail.push_back(fail);
    }

    // Now wait for each — they were all already running.
    for (std::size_t idx = 0; idx < clients.size(); ++idx) {
        auto& proc = clients[idx];
        if (!proc->waitForFinished(10000)) {
            proc->kill();
            proc->waitForFinished(1000);
            FAIL("client " << idx << " timed out");
        }
        REQUIRE(proc->exitStatus() == QProcess::NormalExit);
        // Both modes return 0 on success: happy clients verify result == 42,
        // --fail clients verify onError fires.
        INFO("client idx=" << idx << " fail=" << shouldFail[idx]);
        REQUIRE(proc->exitCode() == 0);
    }
}

TEST_CASE("Process separation: client fails fast against a non-listening URL", "[qt][ws][process]") {
    REQUIRE(QFileInfo::exists(QStringLiteral(QT_TEST_CLIENT_BIN)));

    // Port 1 is reserved on Linux and never listening — connection refused.
    int code = runClient(QStringLiteral("ws://127.0.0.1:1"));
    REQUIRE(code == 10);  // qt_test_client's "connect failed" exit code
}

TEST_CASE("Process separation: TLS handshake works across processes", "[qt][wss][process]") {
    REQUIRE(QFileInfo::exists(QStringLiteral(QT_TEST_SERVER_BIN)));
    REQUIRE(QFileInfo::exists(QStringLiteral(QT_TEST_CLIENT_BIN)));

    ServerProcess server;
    REQUIRE(server.start({QStringLiteral("--tls"), QStringLiteral(TESTS_CERTS_DIR "/server.crt"),
                          QStringLiteral(TESTS_CERTS_DIR "/server.key")}));
    REQUIRE(server.port != 0);

    QString url = QStringLiteral("wss://127.0.0.1:%1").arg(server.port);
    REQUIRE(runClient(url, {QStringLiteral("--tls")}) == 0);
}

// ── Custom main: own the QCoreApplication explicitly ─────────────────────────
//
// Without this, `QCoreApplication` was a static local in `ensureApp()` and so
// got destroyed at process exit, AFTER any global QObject pulled in by Qt
// internals. That ordering produced "corrupted size vs. prev_size while
// consolidating" on shutdown. By owning the app here we destroy it before
// returning from main() and before the C++ runtime tears down statics.
int main(int argc, char* argv[]) {
    QCoreApplication app{argc, argv};
    int result = Catch::Session().run(argc, argv);
    // Drain any Qt-deferred deletes one last time so destructors run with a
    // valid event loop instead of during static teardown.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    return result;
}
