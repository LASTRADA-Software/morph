// SPDX-License-Identifier: Apache-2.0
//
// Standalone Qt WebSocket client used by process-separation tests.
//
// Usage: qt_test_client <ws-url> [--tls] [--fail]
//   --tls   pass an "accept any cert" QSslConfiguration so wss:// works against
//           the test server's self-signed cert.
//   --fail  drive EchoFailAction (which throws on the server) and expect
//           onError to fire. Without this flag the client drives EchoAction{21}
//           and expects 42 back.
// Returns 0 on success and a small nonzero code on failure that names the step
// that failed — easier to debug than a single bool.

#include <QCoreApplication>
#include <QEventLoop>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>
#include <morph/bridge.hpp>
#include <morph/executor.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "qt_test_models.hpp"

namespace {

void pumpUntil(const std::function<bool()>& done, int maxIterations = 200) {
    for (int idx = 0; idx < maxIterations && !done(); ++idx) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app{argc, argv};

    if (argc < 2) {
        std::cerr << "usage: qt_test_client <ws-url> [--tls] [--fail]\n";
        return 1;
    }

    QUrl url{QString::fromUtf8(argv[1])};
    bool useTls = false;
    bool useFail = false;
    for (int idx = 2; idx < argc; ++idx) {
        std::string arg{argv[idx]};
        if (arg == "--tls") {
            useTls = true;
        } else if (arg == "--fail") {
            useFail = true;
        }
    }

    std::optional<QSslConfiguration> tls;
    if (useTls) {
        QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
        cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
        tls = cfg;
    }

    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(
        url, morph::model::detail::defaultDispatcher(), morph::model::detail::defaultRegistry(), tls);
    if (!backendPtr->waitForConnected(3000)) {
        std::cerr << "qt_test_client: connect failed\n";
        return 10;
    }

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<ProcTestEchoModel> handler{bridge, &qtExec};

    if (useFail) {
        std::atomic<bool> errored{false};
        handler.execute(ProcTestEchoFailAction{})
            .then([](int) {})
            .onError([&](const std::exception_ptr&) { errored.store(true); });
        pumpUntil([&] { return errored.load(); }, 300);
        if (!errored.load()) {
            std::cerr << "qt_test_client: expected onError, none fired\n";
            return 11;
        }
        return 0;
    }

    std::atomic<int> result{-1};
    std::atomic<bool> errored{false};
    handler.execute(ProcTestEchoAction{21})
        .then([&](int val) { result.store(val); })
        .onError([&](const std::exception_ptr&) { errored.store(true); });

    pumpUntil([&] { return result.load() != -1 || errored.load(); }, 300);
    if (errored.load()) {
        std::cerr << "qt_test_client: unexpected onError\n";
        return 12;
    }
    if (result.load() != 42) {
        std::cerr << "qt_test_client: expected 42, got " << result.load() << "\n";
        return 13;
    }
    return 0;
}
