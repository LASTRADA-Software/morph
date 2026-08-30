// SPDX-License-Identifier: Apache-2.0
#include "crm/core/authz.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <morph/session/session.hpp>
#include <string>

#include "crm/core/errors.hpp"
#include "crm/core/model_support.hpp"
#include "crm/db/crm_entity.hpp"

namespace crm {

namespace {

/// @brief The shared role lookup both `requireRole` and `callerRoleOn` use:
///        `Role::Manager` (fail-open) if @p accountId has no declared roles
///        at all, otherwise the requesting @p principal's own row, or
///        `Role::Viewer` if @p principal has none.
Role lookupRole(AccountId accountId, const std::string& principal) {
    Lightweight::DataMapper mapper;
    auto roleRows = mapper.Query<db::AccountRoleRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::AccountRoleRecord::account>, "=", *accountId)
                        .All();
    if (roleRows.empty()) {
        return Role::Manager;  // fail-open: no roles declared for this account yet — see authz.hpp's doc comment
    }
    for (const auto& row : roleRows) {
        if (std::string{row.principal.Value().ToStringView()} == principal) {
            return roleFromString(row.role.Value().ToStringView());
        }
    }
    return Role::Viewer;  // implicit, if this principal has no row of its own
}

}  // namespace

void requireRole(AccountId accountId, Role minimum) {
    requirePrincipal();  // throws EmptyPrincipalError first — see authz.hpp's ordering note
    const std::string principal{::morph::session::current()->principal};
    const Role callerRole = lookupRole(accountId, principal);
    if (static_cast<std::uint8_t>(callerRole) < static_cast<std::uint8_t>(minimum)) {
        throw Forbidden{"caller's role on this account does not permit this action"};
    }
}

Role callerRoleOn(AccountId accountId) {
    requirePrincipal();
    const std::string principal{::morph::session::current()->principal};
    return lookupRole(accountId, principal);
}

}  // namespace crm
