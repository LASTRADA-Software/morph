// SPDX-License-Identifier: Apache-2.0
#include "lims/inventory/authorization/inventory_authorization.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>

#include "lims/inventory/db/inventory_entity.hpp"
#include "lims/inventory/db/inventory_store.hpp"

namespace lims {

Scope Authorization<RecordOpeningLot>::scopeOf(RecordOpeningLot const& action) {
    if (!action.location.hasValue()) {
        return Scope{SiteId{}};
    }
    Lightweight::DataMapper mapper;
    auto const location = inventory::findLocation(mapper, *action.location);
    return Scope{location.has_value() ? SiteId{location->siteId.Value()} : SiteId{}};
}

}  // namespace lims
