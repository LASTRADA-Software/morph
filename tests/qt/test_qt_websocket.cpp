// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QThread>
#include <QWebSocket>
#include <atomic>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_tls.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

// Sleeps for a caller-specified duration — used to exercise
// RemoteServer::LimitPolicy::executeTimeout over a real WebSocket connection.
struct WsSlowAction {
    int ms = 0;
};
struct WsSlowModel {
    int execute(WsSlowAction action) {
        std::this_thread::sleep_for(std::chrono::milliseconds(action.ms));
        return action.ms;
    }
};

BRIDGE_REGISTER_MODEL(WsSlowModel, "WsSlowModel")
BRIDGE_REGISTER_ACTION(WsSlowModel, WsSlowAction, "WsSlowAction")

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

static QSslCertificate loadCertificate(const QString& path) {
    QFile file{path};
    REQUIRE(file.open(QIODevice::ReadOnly));
    return QSslCertificate{&file, QSsl::Pem};
}

static QSslConfiguration makeMitmServerTlsConfig() {
    QFile certFile{QStringLiteral(TESTS_CERTS_DIR "/mitm.crt")};
    QFile keyFile{QStringLiteral(TESTS_CERTS_DIR "/mitm.key")};
    REQUIRE(certFile.open(QIODevice::ReadOnly));
    REQUIRE(keyFile.open(QIODevice::ReadOnly));

    QSslConfiguration cfg;
    cfg.setLocalCertificate(QSslCertificate{&certFile, QSsl::Pem});
    cfg.setPrivateKey(QSslKey{&keyFile, QSsl::Rsa, QSsl::Pem});
    return cfg;
}

// ── QtExecutor tests ─────────────────────────────────────────────────────────

TEST_CASE("morph::qt::QtExecutor: default context posts to QCoreApplication::instance()", "[qt][executor]") {
    auto* app = ensureApp();
    morph::qt::QtExecutor exec;
    std::atomic<QThread*> ranOnThread{nullptr};
    std::atomic<bool> ran{false};
    exec.post([&] {
        ranOnThread = QThread::currentThread();
        ran = true;
    });
    pumpUntil([&] { return ran.load(); });
    REQUIRE(ran.load());
    REQUIRE(ranOnThread.load() == app->thread());
}

TEST_CASE("morph::qt::QtExecutor: explicit context posts to that object's thread, not the caller's",
          "[qt][executor]") {
    ensureApp();

    // Worker thread with its own event loop and a QObject living in it — the
    // "non-main thread event loop" scenario from the issue.
    QThread worker;
    worker.start();
    QObject context;
    context.moveToThread(&worker);

    morph::qt::QtExecutor exec{&context};
    std::atomic<QThread*> ranOnThread{nullptr};
    std::atomic<bool> ran{false};
    exec.post([&] {
        ranOnThread = QThread::currentThread();
        ran = true;
    });

    for (int idx = 0; idx < 200 && !ran.load(); ++idx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(ran.load());
    REQUIRE(ranOnThread.load() == &worker);
    REQUIRE(ranOnThread.load() != QThread::currentThread());

    worker.quit();
    worker.wait();
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

TEST_CASE(
    "morph::qt::QtWebSocketBackend: registerModelAsync (opt-in via Config::asyncRegistrationEnabled) registers "
    "without blocking",
    "[qt][ws][issue26]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(
        url, morph::model::detail::defaultDispatcher(), morph::model::detail::defaultRegistry(), std::nullopt,
        morph::qt::QtWebSocketBackend::Config{.asyncRegistrationEnabled = true});
    REQUIRE(backendPtr->waitForConnected());

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "WsEchoModel";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<WsEchoModel>(); };
    bridge.registerHandler(binding);

    // Registration does not block: registerHandler() already returned above,
    // yet the binding is still unbound -- this is the whole point of the
    // async path (see IBackend::registerModelAsync's doc comment). A real
    // WASM caller would gate its UI on this instead of firing an action
    // immediately, since executeVia fails fast on an unbound binding.
    CHECK(binding->currentId.load() == 0U);

    pumpUntil([&] { return binding->currentId.load() != 0U; });
    REQUIRE(binding->currentId.load() != 0U);

    morph::bridge::BridgeHandler<WsEchoModel> handler{bridge, &qtExec, binding};
    std::atomic<int> result{-1};
    handler.execute(WsEchoAction{99}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {
    });
    pumpUntil([&] { return result.load() != -1; });
    REQUIRE(result.load() == 99);
}

TEST_CASE(
    "morph::qt::QtWebSocketBackend: registerModelAsync's pending registration is cancelled when the connection "
    "drops before a reply arrives",
    "[qt][ws][issue26]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    auto wsServer = std::make_unique<morph::qt::QtWebSocketServer>(*server, quint16{0});
    REQUIRE(wsServer->listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer->port())};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(
        url, morph::model::detail::defaultDispatcher(), morph::model::detail::defaultRegistry(), std::nullopt,
        morph::qt::QtWebSocketBackend::Config{.asyncRegistrationEnabled = true});
    REQUIRE(backendPtr->waitForConnected());

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "WsEchoModel";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<WsEchoModel>(); };
    bridge.registerHandler(binding);
    CHECK(binding->currentId.load() == 0U);  // reply has not arrived yet

    // Close the server before it can reply: cancelPending() must drain and
    // fail the pending registration (onError), not leave it dangling forever.
    wsServer->close();
    wsServer.reset();
    pumpUntil([] { return false; }, 20);  // drain the disconnected signal

    CHECK(binding->currentId.load() == 0U);  // onRegistered never fired; still safely unbound
}

TEST_CASE(
    "morph::qt::QtWebSocketBackend: registerModelAsync called before the socket connects queues and retries once "
    "connected fires",
    "[qt][ws][issue54]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    // Deliberately do NOT call waitForConnected() before registering -- this is
    // exactly the ordering a single-threaded WASM client must use, since it can
    // never block waiting for the connection to settle (see issue #54).
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(
        url, morph::model::detail::defaultDispatcher(), morph::model::detail::defaultRegistry(), std::nullopt,
        morph::qt::QtWebSocketBackend::Config{.asyncRegistrationEnabled = true});

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};

    auto binding = std::make_shared<morph::bridge::detail::HandlerBinding>();
    binding->typeId = "WsEchoModel";
    binding->modelFactory = [] { return morph::model::detail::ModelFactory::create<WsEchoModel>(); };
    // registerModelAsync is called here, before the socket has finished its
    // handshake. Previously this called onError("disconnected") immediately
    // and never retried -- currentId would stay 0 forever even once the
    // socket connects moments later. It must instead queue the attempt and
    // retry once `connected` fires.
    bridge.registerHandler(binding);
    CHECK(binding->currentId.load() == 0U);  // not connected yet, nothing sent

    pumpUntil([&] { return binding->currentId.load() != 0U; }, 200);
    REQUIRE(binding->currentId.load() != 0U);

    morph::bridge::BridgeHandler<WsEchoModel> handler{bridge, &qtExec, binding};
    std::atomic<int> result{-1};
    handler.execute(WsEchoAction{42}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {
    });
    pumpUntil([&] { return result.load() != -1; });
    REQUIRE(result.load() == 42);
}

TEST_CASE(
    "morph::qt::QtWebSocketBackend: a fire-and-forget deregister's reply cannot be misrouted to a following "
    "synchronous register",
    "[qt][ws][issue65]") {
    // Reproduces issue #65: deregisterModel() is fire-and-forget with callId
    // 0, and registerModel()'s sendSync path also parks its nested event loop
    // waiting for a callId==0 reply. Back to back on the same connection,
    // whichever callId==0 reply lands first used to be handed to the parked
    // sync loop -- if it was the deregister's stray "ok" (no modelId), the
    // new registration's currentId came back 0 (or garbage) and the real
    // register reply was dropped as unmatched. Repeated many times to make a
    // pre-fix race land reliably (the issue itself measured ~1-in-8 in
    // isolated runs); every iteration must produce a real, non-zero,
    // *distinct* modelId with no dropped/garbled reply.
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    morph::qt::QtWebSocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    // First instance to be deregistered fire-and-forget.
    auto firstId = backend.registerModel("WsEchoModel", [] { return morph::model::detail::ModelFactory::create<WsEchoModel>(); });
    REQUIRE(firstId.v != 0U);

    constexpr int iterations = 40;
    for (int i = 0; i < iterations; ++i) {
        backend.deregisterModel(firstId);  // fire-and-forget, callId == 0
        // Immediately issue a synchronous register on the same connection --
        // the exact adjacency the issue describes. Its reply must be the
        // real "ok" with a fresh modelId, never the deregister's stray ack.
        auto newId =
            backend.registerModel("WsEchoModel", [] { return morph::model::detail::ModelFactory::create<WsEchoModel>(); });
        REQUIRE(newId.v != 0U);
        firstId = newId;
    }
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

TEST_CASE(
    "morph::qt::QtWebSocketBackend: setConnectHandler fires on the first connect (unlike setReconnectHandler)",
    "[qt][ws][issue29]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    morph::qt::QtWebSocketBackend backend{url};

    std::atomic<int> connectCount{0};
    std::atomic<int> reconnectCount{0};
    backend.setConnectHandler([&] { connectCount.fetch_add(1); });
    backend.setReconnectHandler([&] { reconnectCount.fetch_add(1); });

    REQUIRE(backend.waitForConnected());
    pumpUntil([&] { return connectCount.load() >= 1; });
    CHECK(connectCount.load() == 1);
    CHECK(reconnectCount.load() == 0);  // must not fire on the first connect
}

TEST_CASE("morph::qt::QtWebSocketBackend: setDisconnectHandler fires when the socket drops, before reconnect",
         "[qt][ws][issue29]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);

    std::atomic<bool> disconnected{false};
    // Reconnect disabled: isolates the disconnect notification from any
    // automatic reconnect attempt racing the assertions below.
    std::unique_ptr<morph::qt::QtWebSocketBackend> backendPtr;
    {
        morph::qt::QtWebSocketServer wsServer{*server, 0};
        REQUIRE(wsServer.listen());

        QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
        backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(
            url, morph::model::detail::defaultDispatcher(), morph::model::detail::defaultRegistry(), std::nullopt,
            morph::qt::QtWebSocketBackend::Config{.reconnectEnabled = false});
        REQUIRE(backendPtr->waitForConnected());

        backendPtr->setDisconnectHandler([&] { disconnected.store(true); });
        CHECK_FALSE(disconnected.load());

        // wsServer goes out of scope here -- the client socket drops, while
        // backendPtr (declared in the outer scope) survives to observe it.
    }
    pumpUntil([&] { return disconnected.load(); });
    CHECK(disconnected.load());
}

TEST_CASE(
    "morph::qt::QtWebSocketBackend: setDisconnectHandler fires strictly before any reconnect activity",
    "[qt][ws][issue29]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);

    quint16 port = 0;
    std::unique_ptr<morph::qt::QtWebSocketBackend> backendPtr;
    std::atomic<int> reconnectCount{0};
    std::atomic<bool> disconnectSawNoReconnectYet{false};
    std::atomic<bool> disconnectFired{false};
    {
        morph::qt::QtWebSocketServer wsServer{*server, 0};
        REQUIRE(wsServer.listen());
        port = wsServer.port();

        backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(
            QUrl{QString("ws://127.0.0.1:%1").arg(port)}, morph::model::detail::defaultDispatcher(),
            morph::model::detail::defaultRegistry(), std::nullopt,
            morph::qt::QtWebSocketBackend::Config{.initialReconnectDelay = std::chrono::milliseconds{10}});
        REQUIRE(backendPtr->waitForConnected());

        backendPtr->setReconnectHandler([&] { reconnectCount.fetch_add(1); });
        backendPtr->setDisconnectHandler([&] {
            // The doc-comment-claimed ordering: disconnect must observe zero
            // reconnect activity, since scheduleReconnect() -- let alone a
            // completed reconnect -- has not run yet at this point.
            disconnectSawNoReconnectYet.store(reconnectCount.load() == 0);
            disconnectFired.store(true);
        });

        // wsServer goes out of scope here -- the client socket drops.
    }
    pumpUntil([&] { return disconnectFired.load(); });
    CHECK(disconnectFired.load());
    CHECK(disconnectSawNoReconnectYet.load());

    // Bring a fresh server up on the same port so the backend's own automatic
    // reconnect (Config::reconnectEnabled defaults to true) succeeds, proving
    // the reconnect handler does eventually fire (just strictly after disconnect).
    morph::qt::QtWebSocketServer wsServer{*server, port};
    REQUIRE(wsServer.listen());
    // waitForConnected pumps the event loop until the automatic reconnect lands.
    REQUIRE(backendPtr->waitForConnected(2000));
}

TEST_CASE(
    "morph::qt::QtWebSocketBackend: passing nullptr to setConnectHandler/setDisconnectHandler/setReconnectHandler "
    "clears them",
    "[qt][ws][issue29]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);

    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());
    quint16 const port = wsServer.port();

    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(
        QUrl{QString("ws://127.0.0.1:%1").arg(port)}, morph::model::detail::defaultDispatcher(),
        morph::model::detail::defaultRegistry(), std::nullopt,
        morph::qt::QtWebSocketBackend::Config{.initialReconnectDelay = std::chrono::milliseconds{10}});
    REQUIRE(backendPtr->waitForConnected());

    std::atomic<int> connectCount{0};
    std::atomic<int> disconnectCount{0};
    std::atomic<int> reconnectCount{0};
    backendPtr->setConnectHandler([&] { connectCount.fetch_add(1); });
    backendPtr->setDisconnectHandler([&] { disconnectCount.fetch_add(1); });
    backendPtr->setReconnectHandler([&] { reconnectCount.fetch_add(1); });

    // First cycle: drop the connection (fires disconnect), then have the
    // backend auto-reconnect to a fresh server on the same port (fires
    // reconnect, which itself fires connect too).
    wsServer.close();
    pumpUntil([&] { return disconnectCount.load() >= 1; });
    REQUIRE(disconnectCount.load() == 1);

    morph::qt::QtWebSocketServer wsServer2{*server, port};
    REQUIRE(wsServer2.listen());
    REQUIRE(backendPtr->waitForConnected(2000));
    pumpUntil([&] { return reconnectCount.load() >= 1; });
    // The initial connect fired before setConnectHandler was installed above,
    // so only the reconnect's connect signal is observed here.
    REQUIRE(connectCount.load() == 1);
    REQUIRE(reconnectCount.load() == 1);

    // Clear all three handlers, then force a second disconnect/reconnect cycle.
    backendPtr->setConnectHandler(nullptr);
    backendPtr->setDisconnectHandler(nullptr);
    backendPtr->setReconnectHandler(nullptr);

    int const connectCountBefore = connectCount.load();
    int const disconnectCountBefore = disconnectCount.load();
    int const reconnectCountBefore = reconnectCount.load();

    wsServer2.close();
    pumpUntil([] { return false; }, 10);  // give the drop time to reach the client

    morph::qt::QtWebSocketServer wsServer3{*server, port};
    REQUIRE(wsServer3.listen());
    REQUIRE(backendPtr->waitForConnected(2000));
    pumpUntil([] { return false; }, 10);  // settle: nothing more should fire

    CHECK(connectCount.load() == connectCountBefore);
    CHECK(disconnectCount.load() == disconnectCountBefore);
    CHECK(reconnectCount.load() == reconnectCountBefore);
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
        handler.execute(WsEchoAction{7})
            .then([&](int val) { result.store(val); })
            .onError([](const std::exception_ptr&) {});
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

// FIX 3 — a register whose reply never arrives (socket never connected or
// dropped) must fail fast with a "register failed: disconnected" error instead
// of parking the nested QEventLoop in sendSync forever. registerModel() is what
// Bridge/BridgeHandler construction calls, so a hang here would freeze the whole
// Qt thread the moment a handler is created against a dead connection.

TEST_CASE("morph::qt::QtWebSocketBackend: registerModel on a never-connected socket throws, does not hang",
          "[qt][ws][lifecycle][disconnect]") {
    ensureApp();
    // Port 1 is reserved and never listening — the socket never reaches Connected.
    QUrl url{QString("ws://127.0.0.1:1")};
    morph::qt::QtWebSocketBackend backend{url};
    REQUIRE_FALSE(backend.waitForConnected(200));

    bool threw = false;
    std::string what;
    try {
        (void)backend.registerModel("WsEchoModel", nullptr);
    } catch (const std::exception& exc) {
        threw = true;
        what = exc.what();
    }
    REQUIRE(threw);
    REQUIRE(what.find("register failed") != std::string::npos);
    REQUIRE(what.find("disconnected") != std::string::npos);
}

TEST_CASE("morph::qt::QtWebSocketBackend: register after the server closes fails instead of hanging",
          "[qt][ws][lifecycle][disconnect]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    auto wsServer = std::make_unique<morph::qt::QtWebSocketServer>(*server, quint16{0});
    REQUIRE(wsServer->listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer->port())};
    morph::qt::QtWebSocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    // One good register while connected.
    auto mid = backend.registerModel("WsEchoModel", nullptr);
    REQUIRE(mid.v != 0U);

    // Close the server and drain the disconnect notification so the backend's
    // _connected flag flips to false.
    wsServer->close();
    wsServer.reset();
    pumpUntil([] { return false; }, 20);

    // A register now must fail fast (the up-front connected check in sendSync
    // trips) rather than park a nested event loop that never resolves.
    bool threw = false;
    std::string what;
    try {
        (void)backend.registerModel("WsEchoModel", nullptr);
    } catch (const std::exception& exc) {
        threw = true;
        what = exc.what();
    }
    REQUIRE(threw);
    REQUIRE(what.find("register failed") != std::string::npos);
}

TEST_CASE("morph::qt::QtWebSocketBackend: negotiateProtocolVersion succeeds against a real RemoteServer",
          "[qt][ws][protocol]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    morph::qt::QtWebSocketBackend backend{url};
    REQUIRE(backend.waitForConnected());

    auto result = backend.negotiateProtocolVersion();
    REQUIRE(result == morph::wire::ProtocolNegotiationResult::Negotiated);
}

TEST_CASE("morph::qt::QtWebSocketBackend: negotiateProtocolVersion throws when never connected",
          "[qt][ws][protocol]") {
    ensureApp();
    QUrl url{QString("ws://127.0.0.1:1")};  // reserved port, never listening
    morph::qt::QtWebSocketBackend backend{url};
    REQUIRE_FALSE(backend.waitForConnected(200));

    REQUIRE_THROWS_AS(backend.negotiateProtocolVersion(), std::runtime_error);
}

TEST_CASE("Server closing notifies morph::qt::QtWebSocketBackend disconnected signal", "[qt][ws][lifecycle]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    auto wsServer = std::make_unique<morph::qt::QtWebSocketServer>(*server, quint16{0});
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

// ── Graceful shutdown tests (closeGracefully) ────────────────────────────────

TEST_CASE("morph::qt::QtWebSocketServer::closeGracefully closes idle clients with a going-away frame",
          "[qt][ws][shutdown]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    auto wsServer = std::make_unique<morph::qt::QtWebSocketServer>(*server, quint16{0});
    REQUIRE(wsServer->listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer->port())};
    QWebSocket client;
    client.open(url);
    pumpUntil([&] { return client.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(client.state() == QAbstractSocket::ConnectedState);

    QWebSocketProtocol::CloseCode observedCode = QWebSocketProtocol::CloseCodeNormal;
    QObject::connect(&client, &QWebSocket::disconnected, [&] { observedCode = client.closeCode(); });

    bool const drained = wsServer->closeGracefully(std::chrono::milliseconds{500});
    REQUIRE(drained);

    pumpUntil([&] { return client.state() == QAbstractSocket::UnconnectedState; }, 100);
    REQUIRE(client.state() == QAbstractSocket::UnconnectedState);
    REQUIRE(observedCode == QWebSocketProtocol::CloseCodeGoingAway);
}

TEST_CASE("morph::qt::QtWebSocketServer::closeGracefully waits for an in-flight execute before closing",
          "[qt][ws][shutdown]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    auto wsServer = std::make_unique<morph::qt::QtWebSocketServer>(*server, quint16{0});
    REQUIRE(wsServer->listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer->port())};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<WsSlowModel> handler{bridge, &qtExec};

    std::atomic<bool> completed{false};
    std::atomic<int> result{-1};
    handler.execute(WsSlowAction{200})
        .then([&](int val) {
            result.store(val);
            completed.store(true);
        })
        .onError([](const std::exception_ptr&) {});

    // Give the execute a moment to actually reach the server and start
    // running on its strand before beginning the graceful shutdown.
    pumpUntil([] { return false; }, 5);

    bool const drained = wsServer->closeGracefully(std::chrono::milliseconds{2000});
    REQUIRE(drained);
    REQUIRE(completed.load());
    REQUIRE(result.load() == 200);
}

TEST_CASE("morph::qt::QtWebSocketServer::closeGracefully hard-stops once the deadline elapses", "[qt][ws][shutdown]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    auto wsServer = std::make_unique<morph::qt::QtWebSocketServer>(*server, quint16{0});
    REQUIRE(wsServer->listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer->port())};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<WsSlowModel> handler{bridge, &qtExec};

    handler.execute(WsSlowAction{2000}).onError([](const std::exception_ptr&) {});

    pumpUntil([] { return false; }, 5);

    bool const drained = wsServer->closeGracefully(std::chrono::milliseconds{100});
    REQUIRE_FALSE(drained);
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

    auto decodeReply = [](const std::string& raw) { return morph::wire::decode(raw); };

    SECTION("bare unknown envelope kind") {
        // "hello" is now a recognised control kind (protocol-version
        // negotiation, see docs/spec/core/wire.md) — use a string that stays
        // genuinely unrecognised so this test keeps testing what it says.
        morph::wire::Envelope req;
        req.kind = "bogus_kind_xyz";
        auto reply = decodeReply(sendRawAndAwaitReply(sock, QString::fromStdString(morph::wire::encode(req))));
        REQUIRE(reply.kind == "err");
        REQUIRE(reply.message.find("unknown envelope kind") != std::string::npos);
    }

    SECTION("non-JSON garbage") {
        auto reply = decodeReply(sendRawAndAwaitReply(sock, "not-json"));
        REQUIRE(reply.kind == "err");
    }

    SECTION("register without typeId") {
        morph::wire::Envelope req;
        req.kind = "register";  // typeId empty
        auto reply = decodeReply(sendRawAndAwaitReply(sock, QString::fromStdString(morph::wire::encode(req))));
        REQUIRE(reply.kind == "err");
    }

    SECTION("register with unknown typeId") {
        auto reply = decodeReply(sendRawAndAwaitReply(
            sock, QString::fromStdString(morph::wire::encode(morph::wire::makeRegister("NoSuchModel")))));
        REQUIRE(reply.kind == "err");
        REQUIRE(reply.message.find("unknown model type") != std::string::npos);
    }

    SECTION("execute against unknown modelId echoes callId") {
        morph::wire::Envelope req;
        req.kind = "execute";
        req.callId = 7;
        req.modelId = 999;
        req.modelType = "WsEchoModel";
        req.actionType = "WsEchoAction";
        req.body = R"({"value":1})";
        auto reply = decodeReply(sendRawAndAwaitReply(sock, QString::fromStdString(morph::wire::encode(req))));
        REQUIRE(reply.kind == "err");
        REQUIRE(reply.callId == 7U);
        REQUIRE(reply.message == "model not found");
    }

    SECTION("execute with malformed JSON body") {
        auto regReply = decodeReply(sendRawAndAwaitReply(
            sock, QString::fromStdString(morph::wire::encode(morph::wire::makeRegister("WsEchoModel")))));
        REQUIRE(regReply.kind == "ok");

        morph::wire::Envelope req;
        req.kind = "execute";
        req.callId = 9;
        req.modelId = regReply.modelId;
        req.modelType = "WsEchoModel";
        req.actionType = "WsEchoAction";
        req.body = "{not json";
        auto reply = decodeReply(sendRawAndAwaitReply(sock, QString::fromStdString(morph::wire::encode(req))));
        REQUIRE(reply.kind == "err");
        REQUIRE(reply.callId == 9U);
    }

    SECTION("execute against unknown actionTypeId") {
        auto regReply = decodeReply(sendRawAndAwaitReply(
            sock, QString::fromStdString(morph::wire::encode(morph::wire::makeRegister("WsEchoModel")))));
        REQUIRE(regReply.kind == "ok");

        morph::wire::Envelope req;
        req.kind = "execute";
        req.callId = 10;
        req.modelId = regReply.modelId;
        req.modelType = "WsEchoModel";
        req.actionType = "NoSuchAction";
        req.body = "{}";
        auto reply = decodeReply(sendRawAndAwaitReply(sock, QString::fromStdString(morph::wire::encode(req))));
        REQUIRE(reply.kind == "err");
        REQUIRE(reply.callId == 10U);
        REQUIRE(reply.message.find("unknown action") != std::string::npos);
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
    auto badReply = morph::wire::decode(sendRawAndAwaitReply(badSock, "garbage|garbage|garbage"));
    REQUIRE(badReply.kind == "err");

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

// ── Resource-limit tests (QtWebSocketServerConfig) ──────────────────────────

TEST_CASE("morph::qt::QtWebSocketServer: maxConnections rejects connections beyond the cap", "[qt][ws][limits]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServerConfig cfg;
    cfg.maxConnections = 1;
    morph::qt::QtWebSocketServer wsServer{*server, 0, std::nullopt, cfg};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};

    QWebSocket first;
    first.open(url);
    pumpUntil([&] { return first.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(first.state() == QAbstractSocket::ConnectedState);

    QWebSocket second;
    second.open(url);
    // The server accepts the TCP/WS handshake either way (that happens inside
    // QWebSocketServer before our slot runs) and then closes the socket
    // immediately in onNewConnection — so assert on the eventual disconnect
    // rather than a refused connect.
    pumpUntil([&] { return second.state() == QAbstractSocket::UnconnectedState; }, 150);
    REQUIRE(second.state() == QAbstractSocket::UnconnectedState);

    first.close();
    pumpUntil([&] { return first.state() == QAbstractSocket::UnconnectedState; }, 50);
}

TEST_CASE("morph::qt::QtWebSocketServer: maxConnections refusal is logged at warn, naming the cap",
         "[qt][ws][limits][issue30]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServerConfig cfg;
    cfg.maxConnections = 1;
    morph::qt::QtWebSocketServer wsServer{*server, 0, std::nullopt, cfg};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};

    QWebSocket first;
    first.open(url);
    pumpUntil([&] { return first.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(first.state() == QAbstractSocket::ConnectedState);

    std::vector<std::pair<morph::log::LogLevel, std::string>> captured;
    {
        morph::log::ScopedLoggerOverride guard{
            [&](morph::log::LogLevel level, std::string_view msg) { captured.emplace_back(level, std::string{msg}); },
            morph::log::LogLevel::debug};

        QWebSocket second;
        second.open(url);
        pumpUntil([&] { return second.state() == QAbstractSocket::UnconnectedState; }, 150);
        REQUIRE(second.state() == QAbstractSocket::UnconnectedState);
    }

    bool foundWarning = false;
    for (auto const& [level, msg] : captured) {
        if (level == morph::log::LogLevel::warn && msg.find("maxConnections") != std::string::npos) {
            foundWarning = true;
            break;
        }
    }
    CHECK(foundWarning);

    first.close();
    pumpUntil([&] { return first.state() == QAbstractSocket::UnconnectedState; }, 50);
}

TEST_CASE("morph::qt::QtWebSocketServer: connect/disconnect are logged at info, with the live count",
         "[qt][ws][issue30]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());
    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};

    std::vector<std::pair<morph::log::LogLevel, std::string>> captured;
    {
        morph::log::ScopedLoggerOverride guard{
            [&](morph::log::LogLevel level, std::string_view msg) { captured.emplace_back(level, std::string{msg}); },
            morph::log::LogLevel::debug};

        QWebSocket client;
        client.open(url);
        pumpUntil([&] { return client.state() == QAbstractSocket::ConnectedState; }, 100);
        REQUIRE(client.state() == QAbstractSocket::ConnectedState);

        client.close();
        pumpUntil([&] { return client.state() == QAbstractSocket::UnconnectedState; }, 100);
    }

    bool foundConnect = false;
    bool foundDisconnect = false;
    for (auto const& [level, msg] : captured) {
        if (level == morph::log::LogLevel::info && msg.find("accepted") != std::string::npos) {
            foundConnect = true;
        }
        if (level == morph::log::LogLevel::info && msg.find("disconnected") != std::string::npos) {
            foundDisconnect = true;
        }
    }
    CHECK(foundConnect);
    CHECK(foundDisconnect);
}

TEST_CASE("morph::qt::QtWebSocketServer: maxMessageBytes rejects an oversized frame before dispatch",
          "[qt][ws][limits]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServerConfig cfg;
    cfg.maxMessageBytes = 1024;  // much smaller than the 8 MiB wire cap
    morph::qt::QtWebSocketServer wsServer{*server, 0, std::nullopt, cfg};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    QWebSocket sock;
    sock.open(url);
    pumpUntil([&] { return sock.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(sock.state() == QAbstractSocket::ConnectedState);

    morph::wire::Envelope req;
    req.kind = "register";
    req.typeId = "WsEchoModel";
    req.contextKey = std::string(2000, 'x');  // pushes the frame past 1024 bytes
    auto reply = morph::wire::decode(sendRawAndAwaitReply(sock, QString::fromStdString(morph::wire::encode(req))));
    REQUIRE(reply.kind == "err");
    REQUIRE(reply.message.find("maxMessageBytes") != std::string::npos);

    sock.close();
    pumpUntil([&] { return sock.state() == QAbstractSocket::UnconnectedState; }, 50);
}

TEST_CASE("morph::qt::QtWebSocketServer: an oversized frame's err reply carries the rejected call's id",
          "[qt][ws][limits]") {
    // The frame is never decoded, so the id has to be recovered by a bounded
    // prefix scan. Replying with callId 0 would be actively harmful rather than
    // merely unhelpful: 0 is the client's synchronous-reply discriminator, so
    // the error would resume an unrelated parked register/deregister with
    // another call's reply, while the rejected execute never resolved at all.
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServerConfig cfg;
    cfg.maxMessageBytes = 1024;
    morph::qt::QtWebSocketServer wsServer{*server, 0, std::nullopt, cfg};
    REQUIRE(wsServer.listen());

    QUrl const url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    QWebSocket sock;
    sock.open(url);
    pumpUntil([&] { return sock.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(sock.state() == QAbstractSocket::ConnectedState);

    morph::wire::Envelope req;
    req.kind = "execute";
    req.callId = 4242;
    req.modelId = 1;
    req.modelType = "WsEchoModel";
    req.actionType = "WsEchoAction";
    req.body = std::string(2000, 'x');  // pushes the frame past 1024 bytes
    auto reply = morph::wire::decode(sendRawAndAwaitReply(sock, QString::fromStdString(morph::wire::encode(req))));
    REQUIRE(reply.kind == "err");
    REQUIRE(reply.message.contains("maxMessageBytes"));
    REQUIRE(reply.callId == 4242U);

    sock.close();
    pumpUntil([&] { return sock.state() == QAbstractSocket::UnconnectedState; }, 50);
}

TEST_CASE("morph::qt::QtWebSocketServer: default config behaves exactly as before (regression)", "[qt][ws][limits]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};  // no cfg argument at all
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<WsEchoModel> handler{bridge, &qtExec};
    std::atomic<int> result{-1};
    handler.execute(WsEchoAction{5}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {
    });
    pumpUntil([&] { return result.load() != -1; });
    REQUIRE(result.load() == 5);
}

TEST_CASE("morph::qt::QtWebSocketServer: messagesPerSecond throttles a burst on one connection", "[qt][ws][limits]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServerConfig cfg;
    cfg.messagesPerSecond = 5;  // bucket capacity 5, refills at 5/s
    morph::qt::QtWebSocketServer wsServer{*server, 0, std::nullopt, cfg};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    QWebSocket sock;
    sock.open(url);
    pumpUntil([&] { return sock.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(sock.state() == QAbstractSocket::ConnectedState);

    // Register once (consumes 1 token) so subsequent frames target a valid modelId.
    auto regReply = morph::wire::decode(sendRawAndAwaitReply(
        sock, QString::fromStdString(morph::wire::encode(morph::wire::makeRegister("WsEchoModel")))));
    REQUIRE(regReply.kind == "ok");

    // Fire 20 execute frames back-to-back. The bucket started at capacity 5 and
    // lost 1 token to the register above, so at most ~4 of these should be
    // admitted before the bucket empties; the rest are dropped at the transport,
    // never reaching RemoteServer, so no reply arrives for them.
    std::atomic<int> replies{0};
    auto conn =
        QObject::connect(&sock, &QWebSocket::textMessageReceived, [&](const QString&) { replies.fetch_add(1); });
    for (int idx = 0; idx < 20; ++idx) {
        morph::wire::Envelope req;
        req.kind = "execute";
        req.callId = static_cast<uint64_t>(idx + 1);
        req.modelId = regReply.modelId;
        req.modelType = "WsEchoModel";
        req.actionType = "WsEchoAction";
        req.body = R"({"value":1})";
        sock.sendTextMessage(QString::fromStdString(morph::wire::encode(req)));
    }
    // Give the admitted replies a moment to arrive, then a further moment to
    // confirm no additional (wrongly-admitted) replies trickle in.
    pumpUntil([&] { return replies.load() >= 1; }, 100);
    pumpUntil([] { return false; }, 30);
    QObject::disconnect(conn);

    REQUIRE(replies.load() >= 1);
    REQUIRE(replies.load() < 20);

    sock.close();
    pumpUntil([&] { return sock.state() == QAbstractSocket::UnconnectedState; }, 50);
}

TEST_CASE("morph::qt::QtWebSocketServer: messagesPerSecond == 0 never drops frames (regression)", "[qt][ws][limits]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};  // default cfg: messagesPerSecond == 0
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    QWebSocket sock;
    sock.open(url);
    pumpUntil([&] { return sock.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(sock.state() == QAbstractSocket::ConnectedState);

    auto regReply = morph::wire::decode(sendRawAndAwaitReply(
        sock, QString::fromStdString(morph::wire::encode(morph::wire::makeRegister("WsEchoModel")))));
    REQUIRE(regReply.kind == "ok");

    std::atomic<int> replies{0};
    auto conn =
        QObject::connect(&sock, &QWebSocket::textMessageReceived, [&](const QString&) { replies.fetch_add(1); });
    constexpr int total = 20;
    for (int idx = 0; idx < total; ++idx) {
        morph::wire::Envelope req;
        req.kind = "execute";
        req.callId = static_cast<uint64_t>(idx + 1);
        req.modelId = regReply.modelId;
        req.modelType = "WsEchoModel";
        req.actionType = "WsEchoAction";
        req.body = R"({"value":1})";
        sock.sendTextMessage(QString::fromStdString(morph::wire::encode(req)));
    }
    pumpUntil([&] { return replies.load() >= total; }, 200);
    QObject::disconnect(conn);
    REQUIRE(replies.load() == total);

    sock.close();
    pumpUntil([&] { return sock.state() == QAbstractSocket::UnconnectedState; }, 50);
}

TEST_CASE("morph::qt::QtWebSocketServer: handshakeTimeout closes a silent connection", "[qt][ws][limits]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServerConfig cfg;
    cfg.handshakeTimeout = std::chrono::milliseconds{100};
    morph::qt::QtWebSocketServer wsServer{*server, 0, std::nullopt, cfg};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    QWebSocket sock;
    sock.open(url);
    pumpUntil([&] { return sock.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(sock.state() == QAbstractSocket::ConnectedState);

    // Send nothing — wait past the handshake timeout and confirm the server closed it.
    pumpUntil([&] { return sock.state() == QAbstractSocket::UnconnectedState; }, 100);
    REQUIRE(sock.state() == QAbstractSocket::UnconnectedState);
}

TEST_CASE("morph::qt::QtWebSocketServer: sending a frame before handshakeTimeout keeps the connection open",
          "[qt][ws][limits]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServerConfig cfg;
    cfg.handshakeTimeout = std::chrono::milliseconds{200};
    morph::qt::QtWebSocketServer wsServer{*server, 0, std::nullopt, cfg};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    QWebSocket sock;
    sock.open(url);
    pumpUntil([&] { return sock.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(sock.state() == QAbstractSocket::ConnectedState);

    auto reply = morph::wire::decode(sendRawAndAwaitReply(
        sock, QString::fromStdString(morph::wire::encode(morph::wire::makeRegister("WsEchoModel")))));
    REQUIRE(reply.kind == "ok");

    // Outlive the original handshake deadline; the timer should have been
    // cancelled by the frame above, so the connection must still be alive.
    pumpUntil([] { return false; }, 40);
    REQUIRE(sock.state() == QAbstractSocket::ConnectedState);

    sock.close();
    pumpUntil([&] { return sock.state() == QAbstractSocket::UnconnectedState; }, 50);
}

TEST_CASE("morph::qt::QtWebSocketServer: idleTimeout closes a connection that goes silent after activity",
          "[qt][ws][limits]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServerConfig cfg;
    cfg.idleTimeout = std::chrono::milliseconds{150};
    morph::qt::QtWebSocketServer wsServer{*server, 0, std::nullopt, cfg};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    QWebSocket sock;
    sock.open(url);
    pumpUntil([&] { return sock.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(sock.state() == QAbstractSocket::ConnectedState);

    auto reply = morph::wire::decode(sendRawAndAwaitReply(
        sock, QString::fromStdString(morph::wire::encode(morph::wire::makeRegister("WsEchoModel")))));
    REQUIRE(reply.kind == "ok");

    // Go silent past idleTimeout plus the ~1s housekeeping sweep granularity.
    pumpUntil([&] { return sock.state() == QAbstractSocket::UnconnectedState; }, 250);
    REQUIRE(sock.state() == QAbstractSocket::UnconnectedState);
}

TEST_CASE(
    "morph::qt::QtWebSocketServer + RemoteServer::LimitPolicy compose: a server-side executeTimeout"
    " surfaces to a real QtWebSocketBackend client as TimeoutError",
    "[qt][ws][limits]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::backend::LimitPolicy policy;
    policy.executeTimeout = std::chrono::milliseconds{80};
    server->setLimitPolicy(policy);

    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<WsSlowModel> handler{bridge, &qtExec};

    std::atomic<bool> gotTimeoutError{false};
    handler
        .execute(WsSlowAction{300})  // well past the 80ms executeTimeout
        .then([](int) {})
        .onError([&](const std::exception_ptr& exc) {
            try {
                std::rethrow_exception(exc);
            } catch (const morph::backend::TimeoutError&) {
                gotTimeoutError.store(true);
            } catch (...) {
            }
        });

    pumpUntil([&] { return gotTimeoutError.load(); }, 150);
    REQUIRE(gotTimeoutError.load());
}

// ── Connection-scoped cleanup tests ──────────────────────────────────────────

TEST_CASE("QtWebSocketServer reclaims every model a dropped client registered", "[qt][ws][connection-scope]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};

    QWebSocket registrant;
    registrant.open(url);
    pumpUntil([&] { return registrant.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(registrant.state() == QAbstractSocket::ConnectedState);

    // Register two models over the same connection (N > 1, to prove the whole
    // scope is reclaimed, not just one instance).
    auto reg1 = morph::wire::decode(sendRawAndAwaitReply(
        registrant, QString::fromStdString(morph::wire::encode(morph::wire::makeRegister("WsEchoModel")))));
    REQUIRE(reg1.kind == "ok");
    auto reg2 = morph::wire::decode(sendRawAndAwaitReply(
        registrant, QString::fromStdString(morph::wire::encode(morph::wire::makeRegister("WsEchoModel")))));
    REQUIRE(reg2.kind == "ok");
    REQUIRE(reg1.modelId != reg2.modelId);

    // Simulate a crash/drop: abort the socket rather than a clean close, so no
    // deregister is ever sent for either model.
    registrant.abort();
    pumpUntil([] { return false; }, 100);

    // A second, independent client asks the server whether the old ids are still live.
    QWebSocket checker;
    checker.open(url);
    pumpUntil([&] { return checker.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(checker.state() == QAbstractSocket::ConnectedState);

    for (auto modelId : {reg1.modelId, reg2.modelId}) {
        morph::wire::Envelope execReq;
        execReq.kind = "execute";
        execReq.modelId = modelId;
        execReq.modelType = "WsEchoModel";
        execReq.actionType = "WsEchoAction";
        execReq.body = R"({"value":1})";
        auto execReply =
            morph::wire::decode(sendRawAndAwaitReply(checker, QString::fromStdString(morph::wire::encode(execReq))));
        REQUIRE(execReply.kind == "err");
        REQUIRE(execReply.message == "model not found");
    }

    checker.close();
    pumpUntil([&] { return checker.state() == QAbstractSocket::UnconnectedState; }, 50);
}

TEST_CASE("QtWebSocketServer::close() reclaims every client's scope", "[qt][ws][connection-scope]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    auto wsServer = std::make_unique<morph::qt::QtWebSocketServer>(*server, quint16{0});
    REQUIRE(wsServer->listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer->port())};

    QWebSocket clientA;
    clientA.open(url);
    pumpUntil([&] { return clientA.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(clientA.state() == QAbstractSocket::ConnectedState);
    auto regA = morph::wire::decode(sendRawAndAwaitReply(
        clientA, QString::fromStdString(morph::wire::encode(morph::wire::makeRegister("WsEchoModel")))));
    REQUIRE(regA.kind == "ok");

    QWebSocket clientB;
    clientB.open(url);
    pumpUntil([&] { return clientB.state() == QAbstractSocket::ConnectedState; }, 100);
    REQUIRE(clientB.state() == QAbstractSocket::ConnectedState);
    auto regB = morph::wire::decode(sendRawAndAwaitReply(
        clientB, QString::fromStdString(morph::wire::encode(morph::wire::makeRegister("WsEchoModel")))));
    REQUIRE(regB.kind == "ok");

    // Orderly shutdown must reclaim every live client's models too.
    wsServer->close();
    wsServer.reset();
    pumpUntil([] { return false; }, 100);

    // The RemoteServer outlives the transport (owned separately, per
    // Lifetime & ownership), so it can still be asked directly.
    for (auto modelId : {regA.modelId, regB.modelId}) {
        morph::wire::Envelope execReq;
        execReq.kind = "execute";
        execReq.modelId = modelId;
        execReq.modelType = "WsEchoModel";
        execReq.actionType = "WsEchoAction";
        execReq.body = R"({"value":1})";

        std::atomic<bool> replied{false};
        morph::wire::Envelope execResult;
        server->handle(morph::wire::encode(execReq), [&](const std::string& raw) {
            execResult = morph::wire::decode(raw);
            replied.store(true);
        });
        pumpUntil([&] { return replied.load(); }, 50);
        REQUIRE(replied.load());
        REQUIRE(execResult.kind == "err");
        REQUIRE(execResult.message == "model not found");
    }

    clientA.abort();
    clientB.abort();
}

// ── TLS WebSocket tests ──────────────────────────────────────────────────────

TEST_CASE("morph::qt::QtWebSocketBackend TLS: action result delivered via then", "[qt][wss]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0, makeServerTlsConfig()};
    REQUIRE(wsServer.listen());

    QUrl url{QString("wss://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url, morph::model::detail::defaultDispatcher(),
                                                                      morph::model::detail::defaultRegistry(),
                                                                      makeClientTlsConfig());
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
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url, morph::model::detail::defaultDispatcher(),
                                                                      morph::model::detail::defaultRegistry(),
                                                                      makeClientTlsConfig());
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

// ── TLS peer-verification helper tests ───────────────────────────────────────

TEST_CASE("morph::qt::tlsVerifyingConfig sets VerifyPeer", "[qt][tls-helpers]") {
    QSslConfiguration cfg = morph::qt::tlsVerifyingConfig();
    REQUIRE(cfg.peerVerifyMode() == QSslSocket::VerifyPeer);
}

TEST_CASE("morph::qt::tlsPinnedConfig sets VerifyPeer and trusts only the pinned cert", "[qt][tls-helpers]") {
    QFile certFile{QStringLiteral(TESTS_CERTS_DIR "/server.crt")};
    REQUIRE(certFile.open(QIODevice::ReadOnly));
    QSslCertificate cert{&certFile, QSsl::Pem};

    QSslConfiguration cfg = morph::qt::tlsPinnedConfig(cert);
    REQUIRE(cfg.peerVerifyMode() == QSslSocket::VerifyPeer);
    REQUIRE(cfg.caCertificates().size() == 1);
    REQUIRE(cfg.caCertificates().first() == cert);
}

TEST_CASE("morph::qt::tlsInsecureNoVerify sets VerifyNone", "[qt][tls-helpers]") {
    QSslConfiguration cfg = morph::qt::tlsInsecureNoVerify();
    REQUIRE(cfg.peerVerifyMode() == QSslSocket::VerifyNone);
}

// ── TLS peer-verification integration tests ─────────────────────────────────
//
// server.crt/server.key carry a subjectAltName=IP:127.0.0.1 extension so Qt's
// hostname check (these tests connect to the literal IP "127.0.0.1", which
// Qt matches only against SAN IP entries, never the CN) passes for the real
// server. mitm.crt/mitm.key is a second, independently-generated self-signed
// pair standing in for an attacker who intercepted the TCP connection but
// cannot present the pinned certificate's private key.

TEST_CASE("morph::qt::QtWebSocketBackend TLS: tlsPinnedConfig accepts the real server", "[qt][wss][tls-peer]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0, makeServerTlsConfig()};
    REQUIRE(wsServer.listen());

    QUrl url{QString("wss://127.0.0.1:%1").arg(wsServer.port())};
    auto pinned = morph::qt::tlsPinnedConfig(loadCertificate(QStringLiteral(TESTS_CERTS_DIR "/server.crt")));
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url, morph::model::detail::defaultDispatcher(),
                                                                      morph::model::detail::defaultRegistry(), pinned);
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

TEST_CASE("morph::qt::QtWebSocketBackend TLS: tlsPinnedConfig rejects a server presenting a different certificate",
          "[qt][wss][tls-peer]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0, makeMitmServerTlsConfig()};
    REQUIRE(wsServer.listen());

    QUrl url{QString("wss://127.0.0.1:%1").arg(wsServer.port())};
    // Pinned to server.crt, but the server above presents mitm.crt.
    auto pinned = morph::qt::tlsPinnedConfig(loadCertificate(QStringLiteral(TESTS_CERTS_DIR "/server.crt")));
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url, morph::model::detail::defaultDispatcher(),
                                                                      morph::model::detail::defaultRegistry(), pinned);
    REQUIRE_FALSE(backendPtr->waitForConnected(1000));
}

TEST_CASE("morph::qt::QtWebSocketBackend TLS: tlsInsecureNoVerify connects to both the real and mitm servers",
          "[qt][wss][tls-peer]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor realPool{2};
    auto realServer = std::make_shared<morph::backend::RemoteServer>(realPool);
    morph::qt::QtWebSocketServer realWs{*realServer, 0, makeServerTlsConfig()};
    REQUIRE(realWs.listen());

    morph::exec::ThreadPoolExecutor mitmPool{2};
    auto mitmServer = std::make_shared<morph::backend::RemoteServer>(mitmPool);
    morph::qt::QtWebSocketServer mitmWs{*mitmServer, 0, makeMitmServerTlsConfig()};
    REQUIRE(mitmWs.listen());

    auto insecure = morph::qt::tlsInsecureNoVerify();

    QUrl realUrl{QString("wss://127.0.0.1:%1").arg(realWs.port())};
    morph::qt::QtWebSocketBackend realBackend{realUrl, morph::model::detail::defaultDispatcher(),
                                              morph::model::detail::defaultRegistry(), insecure};
    REQUIRE(realBackend.waitForConnected());

    QUrl mitmUrl{QString("wss://127.0.0.1:%1").arg(mitmWs.port())};
    morph::qt::QtWebSocketBackend mitmBackend{mitmUrl, morph::model::detail::defaultDispatcher(),
                                              morph::model::detail::defaultRegistry(), insecure};
    REQUIRE(mitmBackend.waitForConnected());
}

// ── Server exposure guard tests ──────────────────────────────────────────────

TEST_CASE("morph::qt::QtWebSocketServer exposure guard: non-loopback bind without TLS refuses listen()",
          "[qt][ws][exposure-guard]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);

    morph::qt::QtWebSocketServerConfig cfg;
    cfg.bindAddress = QHostAddress::AnyIPv4;  // not loopback
    // cfg.allowPlaintextExposure stays false (default) — the guard should refuse.
    morph::qt::QtWebSocketServer wsServer{*server, 0, std::nullopt, cfg};

    REQUIRE_FALSE(wsServer.listen());
    REQUIRE(wsServer.port() == 0);  // never actually bound
}

TEST_CASE("morph::qt::QtWebSocketServer exposure guard: allowPlaintextExposure permits a non-loopback plaintext bind",
          "[qt][ws][exposure-guard]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);

    morph::qt::QtWebSocketServerConfig cfg;
    cfg.bindAddress = QHostAddress::AnyIPv4;
    cfg.allowPlaintextExposure = true;
    morph::qt::QtWebSocketServer wsServer{*server, 0, std::nullopt, cfg};

    REQUIRE(wsServer.listen());
}

TEST_CASE("morph::qt::QtWebSocketServer exposure guard: TLS permits a non-loopback bind", "[qt][ws][exposure-guard]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);

    morph::qt::QtWebSocketServerConfig cfg;
    cfg.bindAddress = QHostAddress::AnyIPv4;
    // allowPlaintextExposure stays false — a TLS config is enough on its own.
    morph::qt::QtWebSocketServer wsServer{*server, 0, makeServerTlsConfig(), cfg};

    REQUIRE(wsServer.listen());
}

TEST_CASE("morph::qt::QtWebSocketServer exposure guard: loopback bind without TLS still listens (regression)",
          "[qt][ws][exposure-guard]") {
    ensureApp();
    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);

    morph::qt::QtWebSocketServerConfig cfg;  // bindAddress defaults to QHostAddress::LocalHost
    morph::qt::QtWebSocketServer wsServer{*server, 0, std::nullopt, cfg};

    REQUIRE(wsServer.listen());
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
