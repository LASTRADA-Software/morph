// SPDX-License-Identifier: Apache-2.0
#include "lims/billing/authorization/billing_authorization.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>

#include "lims/billing/db/billing_entity.hpp"

namespace lims {

Scope Authorization<SetPrice>::scopeOf(SetPrice const& action) {
    if (!action.priceList.hasValue()) {
        return Scope{CompanyId{}};
    }
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<db::PriceListRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::PriceListRecord::id>, "=", *action.priceList)
                          .All();
    return rows.empty() ? Scope{CompanyId{}} : Scope{CompanyId{rows.front().companyId.Value()}};
}

}  // namespace lims
