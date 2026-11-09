// SPDX-License-Identifier: Apache-2.0
#include "lims/org/authorization/org_authorization.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>

#include "lims/org/db/org_entity.hpp"

namespace lims {

Scope Authorization<RevokeRole>::scopeOf(RevokeRole const& action) {
    if (!action.assignmentId.hasValue()) {
        return Scope{CompanyId{}};
    }
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<db::RoleAssignmentRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::RoleAssignmentRecord::id>, "=", *action.assignmentId)
                          .All();
    if (rows.empty()) {
        return Scope{CompanyId{}};
    }
    auto const id = static_cast<std::int64_t>(rows.front().scopeId.Value());
    return rows.front().scopeKind.Value() == static_cast<int>(ScopeKind::Company) ? Scope{CompanyId{id}}
                                                                                  : Scope{SiteId{id}};
}

}  // namespace lims
