// SPDX-License-Identifier: Apache-2.0

#include "lims/models/auth_model.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

#include "lims/core/errors.hpp"

namespace lims {

namespace {

// Far enough ahead that a development session never expires under a test.
constexpr std::int64_t kTokenExpiresAtMs = 4102444800000;

constexpr std::string_view kServicePrincipalPrefix = "system:";

std::mutex& issuerMutex() {
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<::morph::session::TokenIssuer>& issuerSlot() {
    static std::shared_ptr<::morph::session::TokenIssuer> slot;
    return slot;
}

}  // namespace

bool LimsLogin::validate() const noexcept {
    constexpr std::size_t kMaxPrincipalBytes = 64;
    if (username.empty() || username.size() > kMaxPrincipalBytes) {
        return false;
    }
    return std::ranges::all_of(username, [](char const character) {
        auto const byte = static_cast<unsigned char>(character);
        return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
               byte == '.' || byte == '_' || byte == '-' || byte == ':';
    });
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
LimsLoginResult LimsAuthModel::execute(const LimsLogin& action) {
    if (!action.validate()) {
        throw ValidationError{"LimsLogin: username must be a valid principal"};
    }
    // A service principal is the server's own; minting one on request would hand a caller its authority.
    if (std::string_view{action.username}.starts_with(kServicePrincipalPrefix)) {
        throw ValidationError{"LimsLogin: the 'system:' principal namespace is reserved"};
    }
    auto const issuer = auth::tokenIssuer();
    if (!issuer) {
        throw ValidationError{"LimsLogin: no token issuer installed"};
    }
    auto token = issuer->issue(::morph::session::SessionToken{
        .principal = action.username, .issuedAtMs = 0, .expiresAtMs = kTokenExpiresAtMs, .roles = {}});
    return LimsLoginResult{.token = std::move(token), .principal = action.username};
}

namespace auth {

void setTokenIssuer(std::shared_ptr<::morph::session::TokenIssuer> issuer) {
    std::scoped_lock const lock{issuerMutex()};
    issuerSlot() = std::move(issuer);
}

std::shared_ptr<::morph::session::TokenIssuer> tokenIssuer() {
    std::scoped_lock const lock{issuerMutex()};
    return issuerSlot();
}

}  // namespace auth

}  // namespace lims
