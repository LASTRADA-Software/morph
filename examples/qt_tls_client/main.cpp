// SPDX-License-Identifier: Apache-2.0
//
// Worked example: authenticated TLS peer verification over the Qt WebSocket
// transport (morph::qt). See README.md for what this demonstrates and how to
// run it.

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QUrl>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_tls.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <string>
#include <thread>

// PingModel/PingAction are declared at file scope (not inside an anonymous
// namespace) because BRIDGE_REGISTER_MODEL/BRIDGE_REGISTER_ACTION token-paste
// the type name into a generated identifier — see tests/qt/qt_test_models.hpp.
struct PingAction {
    std::string message;
};

struct PingModel {
    std::string execute(PingAction action) { return "pong: " + action.message; }
};

BRIDGE_REGISTER_MODEL(PingModel, "PingModel")
BRIDGE_REGISTER_ACTION(PingModel, PingAction, "PingAction")

namespace {

void pumpUntil(const std::function<bool()>& done, int maxIterations = 300) {
    for (int idx = 0; idx < maxIterations && !done(); ++idx) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

QSslCertificate loadCertificate(const QString& path) {
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly)) {
        std::cerr << "failed to open certificate: " << path.toStdString() << "\n";
        std::exit(1);
    }
    return QSslCertificate{&file, QSsl::Pem};
}

QSslConfiguration loadServerTls(const QString& certPath, const QString& keyPath) {
    QFile certFile{certPath};
    QFile keyFile{keyPath};
    if (!certFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly)) {
        std::cerr << "failed to open server cert/key\n";
        std::exit(1);
    }
    QSslConfiguration cfg;
    cfg.setLocalCertificate(QSslCertificate{&certFile, QSsl::Pem});
    cfg.setPrivateKey(QSslKey{&keyFile, QSsl::Rsa, QSsl::Pem});
    return cfg;
}

// Connects to `url` with `tls`, sends one PingAction, and reports the outcome.
// Returns true if the connection came up AND the round trip completed.
bool tryPing(const QUrl& url, const QSslConfiguration& tls, const std::string& label) {
    auto backendPtr = std::make_unique<morph::qt::QtWebSocketBackend>(url, morph::model::detail::defaultDispatcher(),
                                                                      morph::model::detail::defaultRegistry(), tls);
    if (!backendPtr->waitForConnected(1000)) {
        std::cout << "[" << label << "] TLS handshake REFUSED\n";
        return false;
    }

    morph::qt::QtExecutor qtExec;
    morph::bridge::Bridge bridge{std::move(backendPtr)};
    morph::bridge::BridgeHandler<PingModel> handler{bridge, &qtExec};

    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
    std::string reply;
    handler.execute(PingAction{"hello"})
        .then([&](std::string val) {
            reply = std::move(val);
            ok.store(true);
            done.store(true);
        })
        .onError([&](const std::exception_ptr&) { done.store(true); });

    pumpUntil([&] { return done.load(); }, 300);
    if (ok.load()) {
        std::cout << "[" << label << "] connected, reply: " << reply << "\n";
    } else {
        std::cout << "[" << label << "] connected but the call did not complete\n";
    }
    return ok.load();
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app{argc, argv};

    const QString certsDir = QStringLiteral(QT_TLS_EXAMPLE_CERTS_DIR);

    // The real server presents certs/server.crt (SAN: IP:127.0.0.1).
    morph::exec::ThreadPoolExecutor realPool{2};
    auto realServer = std::make_shared<morph::backend::RemoteServer>(realPool);
    morph::qt::QtWebSocketServer realWs{*realServer, 0,
                                        loadServerTls(certsDir + "/server.crt", certsDir + "/server.key")};
    if (!realWs.listen()) {
        std::cerr << "failed to start the real server\n";
        return 1;
    }

    // The "mitm" server presents a DIFFERENT self-signed certificate
    // (certs/mitm.crt) — it cannot produce server.crt's private key, so it
    // stands in for an attacker on the same network path who is not the real
    // host the pinned config trusts.
    morph::exec::ThreadPoolExecutor mitmPool{2};
    auto mitmServer = std::make_shared<morph::backend::RemoteServer>(mitmPool);
    morph::qt::QtWebSocketServer mitmWs{*mitmServer, 0, loadServerTls(certsDir + "/mitm.crt", certsDir + "/mitm.key")};
    if (!mitmWs.listen()) {
        std::cerr << "failed to start the mitm server\n";
        return 1;
    }

    QUrl realUrl{QString("wss://127.0.0.1:%1").arg(realWs.port())};
    QUrl mitmUrl{QString("wss://127.0.0.1:%1").arg(mitmWs.port())};

    bool pass = true;

    std::cout << "== tlsPinnedConfig(), pinned to certs/server.crt ==\n";
    QSslConfiguration pinned = morph::qt::tlsPinnedConfig(loadCertificate(certsDir + "/server.crt"));
    if (!tryPing(realUrl, pinned, "pinned -> real server")) {
        std::cerr << "FAIL: pinned client could not reach the real server\n";
        pass = false;
    }
    if (tryPing(mitmUrl, pinned, "pinned -> mitm server")) {
        std::cerr << "FAIL: pinned client connected to the mitm server -- pinning did not protect it\n";
        pass = false;
    }

    std::cout << "\n== tlsInsecureNoVerify(), for contrast -- never do this against an untrusted network ==\n";
    QSslConfiguration insecure = morph::qt::tlsInsecureNoVerify();
    if (!tryPing(realUrl, insecure, "insecure -> real server")) {
        std::cerr << "FAIL: insecure client could not reach the real server\n";
        pass = false;
    }
    if (!tryPing(mitmUrl, insecure, "insecure -> mitm server")) {
        std::cerr << "FAIL: insecure client could not reach the mitm server\n";
        pass = false;
    }
    std::cout << "(the insecure client connected to BOTH servers -- this is exactly why VerifyNone is "
                 "MITM-vulnerable; use tlsPinnedConfig() or tlsVerifyingConfig() against a real network)\n";

    if (!pass) {
        std::cerr << "\nEXAMPLE FAILED\n";
        return 1;
    }
    std::cout << "\nEXAMPLE OK\n";
    return 0;
}
