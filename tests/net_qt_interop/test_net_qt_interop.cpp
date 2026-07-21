// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <atomic>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/net/socket_backend.hpp>
#include <morph/net/socket_server.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <stdexcept>
#include <string>
#include <thread>

// Deliberately NOT in an anonymous namespace: glaze's reflection-based
// get_name() needs these types to have external linkage (see
// tests/qt/test_qt_websocket.cpp's WsEchoAction/WsEchoModel for the same
// convention).
struct InteropEchoAction {
    int value = 0;
};

struct InteropEchoModel {
    int execute(InteropEchoAction action) { return action.value; }
};

BRIDGE_REGISTER_MODEL(InteropEchoModel, "InteropEchoModel")
BRIDGE_REGISTER_ACTION(InteropEchoModel, InteropEchoAction, "InteropEchoAction")

namespace {
void spinUntil(const std::function<bool()>& done, int maxIterations = 200) {
    for (int i = 0; i < maxIterations && !done(); ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::ExcludeUserInputEvents);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// `QtWebSocketServer`'s accept/handshake machinery is entirely event-loop
// driven (QWebSocketServer's socket notifiers only fire while Qt events are
// being pumped). `SocketBackend::waitForConnected()` is a plain
// condition-variable wait with no event-loop pumping of its own -- correct
// for a "no Qt required" backend, but calling it directly on this test's Qt
// thread would block the very event loop the peer QtWebSocketServer needs in
// order to ever accept the connection and reply to the handshake, deadlocking
// until the timeout. Poll with a short per-call timeout instead, pumping Qt
// events between attempts, so the QtWebSocketServer side of the connection
// actually gets to make progress.
bool waitForConnectedPumpingQt(morph::net::SocketBackend& backend, int maxIterations = 500) {
    for (int i = 0; i < maxIterations; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (backend.waitForConnected(std::chrono::milliseconds{10})) {
            return true;
        }
    }
    return false;
}

// `BridgeHandler`'s constructor calls `IBackend::registerModelWithContext`
// synchronously; on `SocketBackend` that blocks the calling thread on a
// condition variable until the server's reply arrives (see
// `SocketBackend::registerModel`'s `sendSync`). When the peer is a
// `QtWebSocketServer`, that reply only ever gets produced by pumping the Qt
// event loop -- so constructing the handler directly on this test's Qt thread
// would deadlock exactly like the unpumped `waitForConnected()` above.
// Build it on a worker thread instead, while this (Qt) thread keeps pumping
// events until construction (and therefore the blocking register call)
// completes.
template <typename Model>
std::unique_ptr<morph::bridge::BridgeHandler<Model>> makeHandlerPumpingQt(morph::bridge::Bridge& bridge,
                                                                          ::morph::exec::IExecutor* cbExec) {
    std::unique_ptr<morph::bridge::BridgeHandler<Model>> handler;
    std::atomic<bool> done{false};
    std::thread worker{[&] {
        handler = std::make_unique<morph::bridge::BridgeHandler<Model>>(bridge, cbExec);
        done.store(true, std::memory_order_release);
    }};
    while (!done.load(std::memory_order_acquire)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    worker.join();
    return handler;
}
}  // namespace

// ── morph::net::SocketBackend client <-> morph::qt::QtWebSocketServer ──────

TEST_CASE("SocketBackend interop: connects to a QtWebSocketServer and completes an action", "[net][qt][interop]") {
    REQUIRE(QCoreApplication::instance() != nullptr);

    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::qt::QtWebSocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    std::string url = "ws://127.0.0.1:" + std::to_string(wsServer.port());
    auto backendPtr = std::make_unique<morph::net::SocketBackend>(url);
    REQUIRE(waitForConnectedPumpingQt(*backendPtr));

    morph::exec::ThreadPoolExecutor cbPool{1};
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    auto handler = makeHandlerPumpingQt<InteropEchoModel>(bridge, &cbPool);
    REQUIRE(handler != nullptr);

    std::atomic<int> result{-1};
    handler->execute(InteropEchoAction{123})
        .then([&](int val) { result.store(val); })
        .onError([](const std::exception_ptr&) {});

    spinUntil([&] { return result.load() != -1; });
    REQUIRE(result.load() == 123);
}

// ── morph::qt::QtWebSocketBackend client <-> morph::net::SocketServer ──────

TEST_CASE("QtWebSocketBackend interop: connects to a SocketServer and completes an action", "[net][qt][interop]") {
    REQUIRE(QCoreApplication::instance() != nullptr);

    morph::exec::ThreadPoolExecutor serverPool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(serverPool);
    morph::net::SocketServer wsServer{*server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<InteropEchoModel> handler{bridge, &qtExec};

    std::atomic<int> result{-1};
    handler.execute(InteropEchoAction{456})
        .then([&](int val) { result.store(val); })
        .onError([](const std::exception_ptr&) {});

    spinUntil([&] { return result.load() != -1; });
    REQUIRE(result.load() == 456);
}

// ── Custom main: own the QCoreApplication explicitly (see tests/qt/test_qt_websocket.cpp) ──
int main(int argc, char* argv[]) {
    QCoreApplication app{argc, argv};
    int result = Catch::Session().run(argc, argv);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    return result;
}
