// SPDX-License-Identifier: Apache-2.0
#include "ledger/dto/auth_dto.hpp"

#include "ledger/auth/ledger_authorizer.hpp"

namespace ledger {

bool Login::validate() const noexcept { return auth::isValidPrincipal(username); }

}  // namespace ledger
