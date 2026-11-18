// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QMetaObject>
#include <QString>
#include <QUrl>
#include <expected>
#include <istream>
#include <memory>
#include <morph/client/transport.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/qt/qt_tls.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <morph/ui/agent.hpp>
#include <optional>
#include <ostream>
#include <string>
#include <thread>
#include <utility>

#include "mcp_internal.hpp"

namespace morph::client::detail {

std::expected<RemoteTransport, std::string> qtTransport(std::string const& url) {
    QUrl const parsed{QString::fromStdString(url)};
    if (!parsed.isValid() || (parsed.scheme() != "ws" && parsed.scheme() != "wss")) {
        return std::unexpected("not a ws:// or wss:// URL: " + url);
    }
    auto made = std::make_unique<qt::QtWebSocketBackend>(parsed);
    backend::IEnvelopeTransport* const requests = made.get();
    return RemoteTransport{.backend = std::move(made), .requests = requests};
}

int serveMcpOverQt(std::string const& url, ui::McpOptions options, std::istream& input, std::ostream& output,
                   std::ostream& diagnostics) {
    QUrl const parsed{QString::fromStdString(url)};
    if (!parsed.isValid() || (parsed.scheme() != "ws" && parsed.scheme() != "wss")) {
        diagnostics << "client: not a ws:// or wss:// URL: " << url << '\n';
        return 2;
    }
    // A stdio server mounts no frontend, so nothing else made the application object the backend's socket needs.
    static int argc = 1;
    static char name[] = "morph-mcp";
    static char* argv[] = {name, nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (QCoreApplication::instance() == nullptr) {
        application = std::make_unique<QCoreApplication>(argc, argv);
    }
    // The backend lives on this thread and must be called here, so its owner is this thread's event loop; stdin is
    // read on a thread of its own, which waits for each reply.
    qt::QtExecutor owner;
    qt::QtWebSocketBackend::Config config;
    config.retryFirstConnect = true;
#ifndef QT_NO_SSL
    // A wss:// server is verified against the system's certificate authorities.
    std::optional<QSslConfiguration> tls;
    if (parsed.scheme() == "wss") {
        tls = qt::tlsVerifyingConfig();
    }
    qt::QtWebSocketBackend transport{parsed, tls, config};
#else
    qt::QtWebSocketBackend transport{parsed, config};
#endif
    options.callbacks = &owner;
    ui::McpAdapter adapter{transport, std::move(options)};
    transport.setDisconnectHandler(adapter.resetter());
    std::thread stdio{[&adapter, &owner, &input, &output] {
        ui::serveStdioOnOwner(adapter, owner, input, output);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(), [] { QCoreApplication::quit(); }, Qt::QueuedConnection);
    }};
    QCoreApplication::exec();
    stdio.join();
    transport.setDisconnectHandler(nullptr);
    return 0;
}

}  // namespace morph::client::detail
