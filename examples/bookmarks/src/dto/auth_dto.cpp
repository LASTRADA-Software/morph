// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/dto/auth_dto.hpp"

#include "bookmarks/auth/bookmarks_authorizer.hpp"

namespace bookmarks {

bool Login::validate() const noexcept { return auth::isValidPrincipal(username); }

}  // namespace bookmarks
