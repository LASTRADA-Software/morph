// SPDX-License-Identifier: Apache-2.0

#include <expected>
#include <morph/client/transport.hpp>
#include <morph/ui/frontend.hpp>
#include <string>

namespace morph::client {

std::expected<RemoteTransport, std::string> makeTransport(ui::AppContext& context,
                                                          [[maybe_unused]] std::string const& url) {
    if (context.ioLoop() != nullptr) {
#if MORPH_CLIENT_TRANSPORT_NET
        return detail::socketTransport(context, url);
#else
        return std::unexpected(std::string{"this frontend connects with morph::net, which is not built here"});
#endif
    }
#if MORPH_CLIENT_TRANSPORT_QT
    return detail::qtTransport(url);
#else
    return std::unexpected(std::string{"this frontend connects with Qt WebSockets, which are not built here"});
#endif
}

}  // namespace morph::client
