// SPDX-License-Identifier: Apache-2.0

#include <QString>
#include <QUrl>
#include <expected>
#include <memory>
#include <morph/client/transport.hpp>
#include <morph/qt/qt_websocket_backend.hpp>
#include <string>

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

}  // namespace morph::client::detail
