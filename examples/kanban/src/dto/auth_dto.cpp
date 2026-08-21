// SPDX-License-Identifier: Apache-2.0
#include "kanban/dto/auth_dto.hpp"

#include "kanban/auth/kanban_authorizer.hpp"

namespace kanban {

bool Login::validate() const noexcept { return auth::isValidPrincipal(username); }

}  // namespace kanban
