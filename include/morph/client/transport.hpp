// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <string>

#include "../core/backend.hpp"
#include "../core/envelope_transport.hpp"
#include "../ui/frontend.hpp"

/// @file
/// @brief The transport a remote client connects with, chosen by the frontend it runs under (spec 4 §3).
///
/// Specified in `docs/spec/client/client.md`.

namespace morph::client {

/// @brief A connection to a server: the bridge's backend and its request verb, which are one object.
struct RemoteTransport {
    /// @brief The backend.
    std::unique_ptr<backend::detail::IBackend> backend;
    /// @brief The same object's `IEnvelopeTransport`; owned by `backend`.
    backend::IEnvelopeTransport* requests = nullptr;
};

/// @brief Connects to @p url with the transport the frontend calls for: a `SocketBackend` on the context's I/O loop
///        when it has one (the terminal frontend), a `QtWebSocketBackend` otherwise (Qt Quick).
/// @param context The running frontend's context.
/// @param url     `ws://host:port`.
/// @return The transport, connecting; or why there is none: the transport the frontend needs is not built, or the
///         URL is refused.
[[nodiscard]] std::expected<RemoteTransport, std::string> makeTransport(ui::AppContext& context,
                                                                        std::string const& url);

namespace detail {

/// @brief A `SocketBackend` on @p context's loop; defined only where `morph::net` is built.
/// @param context The context; its loop must not be null.
/// @param url     The server.
/// @return The transport, or why the URL is refused.
[[nodiscard]] std::expected<RemoteTransport, std::string> socketTransport(ui::AppContext& context,
                                                                          std::string const& url);

/// @brief A `QtWebSocketBackend`; defined only where `morph::qt` is built.
/// @param url The server.
/// @return The transport, or why the URL is refused.
[[nodiscard]] std::expected<RemoteTransport, std::string> qtTransport(std::string const& url);

}  // namespace detail

}  // namespace morph::client
