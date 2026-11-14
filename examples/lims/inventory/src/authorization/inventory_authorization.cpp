// SPDX-License-Identifier: Apache-2.0
#include "lims/inventory/authorization/inventory_authorization.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <utility>
#include <variant>

#include "lims/inventory/db/inventory_entity.hpp"
#include "lims/inventory/db/inventory_store.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims {

Scope Authorization<RecordOpeningLot>::scopeOf(RecordOpeningLot const& action) {
    if (!action.location.hasValue()) {
        return Scope{SiteId{}};
    }
    Lightweight::DataMapper mapper;
    auto const location = inventory::findLocation(mapper, *action.location);
    return Scope{location.has_value() ? SiteId{location->siteId.Value()} : SiteId{}};
}

Scope Authorization<AddRequisitionLine>::scopeOf(AddRequisitionLine const& action) {
    if (!action.requisition.hasValue()) {
        return Scope{SiteId{}};
    }
    Lightweight::DataMapper mapper;
    auto const row = inventory::findRequisition(mapper, *action.requisition);
    return Scope{row.has_value() ? SiteId{row->siteId.Value()} : SiteId{}};
}

Scope Authorization<SubmitRequisition>::scopeOf(SubmitRequisition const& action) {
    if (!action.requisitionId.hasValue()) {
        return Scope{SiteId{}};
    }
    Lightweight::DataMapper mapper;
    auto const row = inventory::findRequisition(mapper, *action.requisitionId);
    return Scope{row.has_value() ? SiteId{row->siteId.Value()} : SiteId{}};
}

Scope Authorization<ApproveRequisition>::scopeOf(ApproveRequisition const& action) {
    if (!action.requisitionId.hasValue()) {
        return Scope{CompanyId{}};
    }
    Lightweight::DataMapper mapper;
    auto const row = inventory::findRequisition(mapper, *action.requisitionId);
    if (!row.has_value()) {
        return Scope{CompanyId{}};
    }
    return inventory::requisitionEscalated(*row) ? Scope{CompanyId{row->companyId.Value()}}
                                                 : Scope{SiteId{row->siteId.Value()}};
}

Scope Authorization<CreatePurchaseOrder>::scopeOf(CreatePurchaseOrder const& action) {
    if (!action.requisition.hasValue()) {
        return Scope{CompanyId{}};
    }
    Lightweight::DataMapper mapper;
    auto const row = inventory::findRequisition(mapper, *action.requisition);
    return Scope{CompanyId{row.has_value() ? row->companyId.Value() : 0}};
}

Scope Authorization<ReceiveGoods>::scopeOf(ReceiveGoods const& action) {
    if (!action.order.hasValue()) {
        return Scope{CompanyId{}};
    }
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<db::PurchaseOrderRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::PurchaseOrderRecord::id>, "=", *action.order)
                          .All();
    return Scope{CompanyId{rows.empty() ? 0 : rows.front().companyId.Value()}};
}

void Authorization<ApproveRequisition>::checkState(ApproveRequisition const& action) {
    Lightweight::DataMapper mapper;
    auto const row = inventory::findRequisition(mapper, *action.requisitionId);
    if (row.has_value() && row->state.Value() != static_cast<int>(RequisitionState::Submitted)) {
        throw IllegalTransition{"ApproveRequisition: only a submitted requisition is approved"};
    }
}

::morph::math::Rational Authorization<ApproveRequisition>::amountOf(ApproveRequisition const& action) {
    Lightweight::DataMapper mapper;
    auto const row = inventory::findRequisition(mapper, *action.requisitionId);
    if (!row.has_value()) {
        return ::morph::math::Rational{0, ::morph::math::DecimalPlaces{0}};
    }
    return ::morph::math::Rational{
        ::morph::math::Numerator{row->totalNum.Value()}, ::morph::math::Denominator{row->totalDen.Value()},
        ::morph::math::DecimalPlaces{decimalsOf(static_cast<CurrencyCode>(row->currency.Value()))}};
}

std::optional<::morph::math::Rational> Authorization<ApproveRequisition>::limitOf(std::string_view principal,
                                                                                  Scope const& scope) {
    if (auto const* company = std::get_if<CompanyId>(&scope)) {
        return OrgModel::valueLimit(principal, *company, LimsRole::PurchaseApprover);
    }
    auto const site = OrgModel::findSite(std::get<SiteId>(scope));
    return site.has_value() ? OrgModel::valueLimit(principal, site->companyId, LimsRole::PurchaseApprover)
                            : std::nullopt;
}

std::array<std::optional<std::string>, 2> Authorization<ApproveRequisition>::firstPerformersOf(
    ApproveRequisition const& action) {
    Lightweight::DataMapper mapper;
    auto const row = inventory::findRequisition(mapper, *action.requisitionId);
    if (!row.has_value()) {
        return {std::nullopt, std::nullopt};
    }
    std::string submitter{row->submittedBy.Value().ToStringView()};
    return {
        std::optional<std::string>{std::string{row->requestedBy.Value().ToStringView()}},
        submitter.empty() ? std::nullopt : std::optional<std::string>{std::move(submitter)},
    };
}

}  // namespace lims
