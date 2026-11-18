// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QMetaObject>
#include <QString>
#include <QUrl>
#include <array>
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

int serveMcpOverQt(std::string const& url, ui::McpOptions options, std::istream& input,
                   // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the protocol's channel, then the log.
                   std::ostream& output, std::ostream& diagnostics) {
    QUrl const parsed{QString::fromStdString(url)};
    if (!parsed.isValid() || (parsed.scheme() != "ws" && parsed.scheme() != "wss")) {
        diagnostics << "client: not a ws:// or wss:// URL: " << url << '\n';
        return 2;
    }
    // A stdio server mounts no frontend, so nothing else made the application object the backend's socket needs.
    static int argc = 1;
    static std::string name{"morph-mcp"};
    static std::array<char*, 2> argv{name.data(), nullptr};
    std::unique_ptr<QCoreApplication> application;
    if (QCoreApplication::instance() == nullptr) {
        application = std::make_unique<QCoreApplication>(argc, argv.data());
    }
    // The backend lives on this thread and must be called here, so its owner is this thread's event loop; stdin is
    // read on a thread of its own, which waits for each reply.
    bool connected = false;  // Declared before the backend, so a handler it still holds never outlives it.
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
    // The backend reports a disconnect for every refused first attempt too. Resetting then would discard the reply
    // to a request held for the first connect, so the reset runs only for a connection that was established. Both
    // handlers run on this thread.
    transport.setConnectHandler([&connected] { connected = true; });
    transport.setDisconnectHandler([&connected, reset = adapter.resetter()] {
        if (std::exchange(connected, false)) {
            reset();
        }
    });
    std::thread stdio{[&adapter, &owner, &input, &output] {
        ui::serveStdioOnOwner(adapter, owner, input, output);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(), [] { QCoreApplication::quit(); }, Qt::QueuedConnection);
    }};
    QCoreApplication::exec();
    stdio.join();
    transport.setDisconnectHandler(nullptr);
    transport.setConnectHandler(nullptr);
    return 0;
}

}  // namespace morph::client::detail
