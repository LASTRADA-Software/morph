// SPDX-License-Identifier: Apache-2.0

#include <exception>
#include <expected>
#include <memory>
#include <morph/client/transport.hpp>
#include <morph/net/socket_backend.hpp>
#include <string>

namespace morph::client::detail {

std::expected<RemoteTransport, std::string> socketTransport(ui::AppContext& context, std::string const& url) {
    try {
        auto made = std::make_unique<net::SocketBackend>(*context.ioLoop(), url);
        backend::IEnvelopeTransport* const requests = made.get();
        return RemoteTransport{.backend = std::move(made), .requests = requests};
    } catch (std::exception const& error) {
        return std::unexpected(std::string{error.what()});
    }
}

}  // namespace morph::client::detail
