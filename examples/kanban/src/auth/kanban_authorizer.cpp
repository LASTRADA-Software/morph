// SPDX-License-Identifier: Apache-2.0
#include "kanban/auth/kanban_authorizer.hpp"

#include <mutex>

namespace kanban::auth {

namespace detail {

std::mutex& tokenIssuerMutex() {
    static std::mutex mtx;
    return mtx;
}

std::shared_ptr<::morph::session::TokenIssuer>& tokenIssuerSlot() {
    static std::shared_ptr<::morph::session::TokenIssuer> slot;
    return slot;
}

}  // namespace detail

void setTokenIssuer(std::shared_ptr<::morph::session::TokenIssuer> issuer) {
    const std::scoped_lock lock{detail::tokenIssuerMutex()};
    detail::tokenIssuerSlot() = std::move(issuer);
}

std::shared_ptr<::morph::session::TokenIssuer> tokenIssuer() {
    const std::scoped_lock lock{detail::tokenIssuerMutex()};
    return detail::tokenIssuerSlot();
}

}  // namespace kanban::auth
