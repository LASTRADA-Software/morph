// SPDX-License-Identifier: Apache-2.0
#include "kanban/auth/kanban_authorizer.hpp"

namespace kanban::auth {

namespace {
std::shared_ptr<::morph::session::TokenIssuer>& issuerSlot() {
    static std::shared_ptr<::morph::session::TokenIssuer> issuer;
    return issuer;
}
}  // namespace

void setTokenIssuer(std::shared_ptr<::morph::session::TokenIssuer> issuer) {
    issuerSlot() = std::move(issuer);
}

std::shared_ptr<::morph::session::TokenIssuer> tokenIssuer() {
    return issuerSlot();
}

}  // namespace kanban::auth
