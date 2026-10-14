// SPDX-License-Identifier: Apache-2.0

#include "app/transport.hpp"

#include <expected>
#include <morph/ui/frontend.hpp>
#include <string>

namespace morph::examples {

std::expected<RemoteTransport, std::string> makeTransport(ui::AppContext& context,
                                                          [[maybe_unused]] std::string const& url) {
    if (context.ioLoop() != nullptr) {
#if MORPH_EXAMPLES_TRANSPORT_NET
        return detail::socketTransport(context, url);
#else
        return std::unexpected(std::string{"this frontend connects with morph::net, which is not built here"});
#endif
    }
#if MORPH_EXAMPLES_TRANSPORT_QT
    return detail::qtTransport(url);
#else
    return std::unexpected(std::string{"this frontend connects with Qt WebSockets, which are not built here"});
#endif
}

}  // namespace morph::examples
