// SPDX-License-Identifier: Apache-2.0
#include "lims/app/internal_client.hpp"

#include <cstdint>
#include <morph/session/session_auth.hpp>

#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"

namespace lims::app {

namespace {

/// @brief When a service token expires: 2100-01-01. The server mints it at start-up and never re-issues it.
constexpr std::int64_t kServiceTokenExpiresAtMs = 4102444800000;

}  // namespace

InternalClient::InternalClient(::morph::backend::RemoteServer& server, std::string_view secret, std::string principal)
    : _principal{std::move(principal)} {
    if (!isServicePrincipal(_principal)) {
        throw ValidationError{"InternalClient: '" + _principal + "' is not in the reserved 'system:' namespace"};
    }
    // Minted here, not through LimsLogin, which refuses the reserved namespace: that refusal is what keeps a client
    // from obtaining this authority. The secret is the authorizer's, so the token verifies like a user's.
    ::morph::session::TokenIssuer const issuer{std::string{secret}, ::morph::session::hmacSha256};
    ::morph::session::Context session;
    session.principal = _principal;
    session.token = issuer.issue(::morph::session::SessionToken{
        .principal = _principal,
        .issuedAtMs = 0,
        .expiresAtMs = kServiceTokenExpiresAtMs,
        .roles = {},
    });
    onOwner([this, &server, &session] {
        _bridge.emplace(std::make_unique<::morph::backend::SimulatedRemoteBackend>(server), _owner);
        _bridge->setDefaultSession(session);
    });
}

InternalClient::~InternalClient() {
    onOwner([this] { _bridge.reset(); });
}

void InternalClient::onOwner(std::function<void()> const& task) {
    std::promise<void> done;
    auto finished = done.get_future();
    _owner.post([&task, &done] {
        try {
            task();
            done.set_value();
        } catch (...) {
            done.set_exception(std::current_exception());
        }
    });
    finished.get();
}

}  // namespace lims::app
