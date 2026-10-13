// SPDX-License-Identifier: Apache-2.0
//
// gallery_server: serves the gallery's screen and model over a Qt WebSocket. `--port N` (default 8765).

#include <QCoreApplication>
#include <QString>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <string_view>

#include "gallery/gallery.hpp"

int main(int argc, char** argv) {
    try {
        QCoreApplication const app{argc, argv};
        std::uint16_t port = 8765;
        QStringList const args = QCoreApplication::arguments();
        for (qsizetype index = 1; index + 1 < args.size(); ++index) {
            if (args.at(index) == "--port") {
                port = static_cast<std::uint16_t>(args.at(index + 1).toUInt());
            }
        }
        auto bundle = gallery::bundle();
        if (!bundle) {
            std::cerr << "gallery_server: " << bundle.error().message() << '\n';
            return 1;
        }
        morph::exec::ThreadPoolExecutor workers{2};
        morph::backend::ServerConfig config;
        config.services.push_back(std::make_shared<morph::ui::UiService>(*bundle));
        auto server = std::make_shared<morph::backend::RemoteServer>(workers, config);
        morph::qt::QtWebSocketServer socket{*server, port};
        if (!socket.listen()) {
            std::cerr << "gallery_server: cannot listen on port " << port << '\n';
            return 1;
        }
        std::cout << "gallery_server: ws://127.0.0.1:" << socket.port() << '\n' << std::flush;
        return QCoreApplication::exec();
    } catch (std::exception const& error) {
        std::cerr << "gallery_server: " << error.what() << '\n';
        return 1;
    }
}
