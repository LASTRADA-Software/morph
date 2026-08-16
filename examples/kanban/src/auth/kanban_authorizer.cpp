// SPDX-License-Identifier: Apache-2.0
#include "kanban/auth/kanban_authorizer.hpp"

namespace kanban::auth {

bool KanbanAuthorizer::authorizeRegister(const ::morph::session::Context& /*ctx*/,
                                        std::string_view /*modelType*/) const {
    return true;
}

bool KanbanAuthorizer::authorizeInstance(const ::morph::session::Context& /*ctx*/, std::string_view /*modelType*/,
                                        std::string_view /*actionType*/, std::uint64_t /*modelId*/,
                                        std::string_view /*ownerPrincipal*/) const {
    return true;
}

}  // namespace kanban::auth
