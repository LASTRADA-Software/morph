// SPDX-License-Identifier: Apache-2.0
//
// Standalone Qt WebSocket server used by process-separation tests.
//
// Usage: qt_test_server [--tls cert.pem key.pem]
// Reads "quit\n" on stdin to exit. Prints "READY|<port>\n" to stdout (flushed)
// once the server is bound and listening; the parent test process scrapes that
// line to discover the OS-assigned port.

#include <QCoreApplication>
#include <QFile>
#include <QSocketNotifier>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <morph/core/executor.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <morph/core/remote.hpp>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include "qt_test_models.hpp"

namespace {

std::optional<QSslConfiguration> loadServerTls(const QString& certPath, const QString& keyPath) {
    QFile certFile{certPath};
    QFile keyFile{keyPath};
    if (!certFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    QSslConfiguration cfg;
    cfg.setLocalCertificate(QSslCertificate{&certFile, QSsl::Pem});
    cfg.setPrivateKey(QSslKey{&keyFile, QSsl::Rsa, QSsl::Pem});
    return cfg;
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app{argc, argv};

    std::optional<QSslConfiguration> tls;
    for (int idx = 1; idx < argc; ++idx) {
        if (std::string{argv[idx]} == "--tls" && idx + 2 < argc) {
            tls = loadServerTls(QString::fromUtf8(argv[idx + 1]), QString::fromUtf8(argv[idx + 2]));
            if (!tls.has_value()) {
                std::cerr << "qt_test_server: failed to load TLS cert/key\n";
                return 2;
            }
            idx += 2;
        }
    }

    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::qt::QtWebSocketServer wsServer{*server, 0, tls};
    if (!wsServer.listen()) {
        std::cerr << "qt_test_server: listen() failed\n";
        return 3;
    }

    // Print the bound port to stdout so the test parent can find it. flush()
    // is required — stdout is line-buffered when attached to a pipe.
    std::cout << "READY|" << wsServer.port() << "\n";
    std::cout.flush();

    // Watch stdin for a "quit" command so the test can shut us down cleanly.
#ifdef _WIN32
    // MSVC (and other Windows toolchains) flag the POSIX `fileno` name itself
    // (C4996) even though the symbol it names is the one actually deprecated;
    // `_fileno` is the ISO-conformant replacement on Windows.
    auto* stdinWatcher = new QSocketNotifier(_fileno(stdin), QSocketNotifier::Read, &app);
#else
    auto* stdinWatcher = new QSocketNotifier(fileno(stdin), QSocketNotifier::Read, &app);
#endif
    QObject::connect(stdinWatcher, &QSocketNotifier::activated, &app, [] {
        std::string line;
        if (!std::getline(std::cin, line)) {
            QCoreApplication::quit();
            return;
        }
        if (line == "quit") {
            QCoreApplication::quit();
        }
    });

    return QCoreApplication::exec();
}
