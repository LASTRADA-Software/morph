// SPDX-License-Identifier: Apache-2.0
#include "lims/billing/authorization/billing_authorization.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>

#include "lims/billing/db/billing_entity.hpp"
#include "lims/billing/db/billing_store.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/core/money.hpp"

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

Scope Authorization<AdvanceBillingRun>::scopeOf(AdvanceBillingRun const& action) {
    if (!action.runId.hasValue()) {
        return Scope{CompanyId{}};
    }
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<db::BillingRunRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::BillingRunRecord::id>, "=", *action.runId)
                          .All();
    return rows.empty() ? Scope{CompanyId{}} : Scope{CompanyId{rows.front().companyId.Value()}};
}

Scope Authorization<SubmitInvoice>::scopeOf(SubmitInvoice const& action) {
    auto const row = findInvoiceRecord(action.invoiceId);
    return Scope{row.has_value() ? CompanyId{row->companyId.Value()} : CompanyId{}};
}

Scope Authorization<ApproveInvoice>::scopeOf(ApproveInvoice const& action) {
    auto const row = findInvoiceRecord(action.invoiceId);
    if (!row.has_value()) {
        return Scope{CompanyId{}};
    }
    return escalated(*row) ? Scope{CompanyId{row->companyId.Value()}} : Scope{SiteId{row->siteId.Value()}};
}

::morph::math::Rational Authorization<ApproveInvoice>::amountOf(ApproveInvoice const& action) {
    auto const row = findInvoiceRecord(action.invoiceId);
    if (!row.has_value()) {
        return ::morph::math::Rational{0, ::morph::math::DecimalPlaces{0}};
    }
    if (!row->grossCompanyNum.Value().has_value() || !row->grossCompanyDen.Value().has_value()) {
        throw ValidationError{"ApproveInvoice: invoice " + std::to_string(*action.invoiceId) +
                              " has no value in its company's currency"};
    }
    auto const company = OrgModel::findCompany(CompanyId{row->companyId.Value()});
    if (!company.has_value()) {
        throw ValidationError{"ApproveInvoice: invoice " + std::to_string(*action.invoiceId) + " has no company"};
    }
    return ::morph::math::Rational{::morph::math::Numerator{*row->grossCompanyNum.Value()},
                                   ::morph::math::Denominator{*row->grossCompanyDen.Value()},
                                   ::morph::math::DecimalPlaces{decimalsOf(company->baseCurrency)}};
}

std::optional<::morph::math::Rational> Authorization<ApproveInvoice>::limitOf(std::string_view principal,
                                                                              Scope const& scope) {
    if (auto const* company = std::get_if<CompanyId>(&scope)) {
        return OrgModel::valueLimit(principal, *company);
    }
    auto const site = OrgModel::findSite(std::get<SiteId>(scope));
    return site.has_value() ? OrgModel::valueLimit(principal, site->companyId) : std::nullopt;
}

std::array<std::optional<std::string>, 2> Authorization<ApproveInvoice>::firstPerformersOf(
    ApproveInvoice const& action) {
    auto const row = findInvoiceRecord(action.invoiceId);
    if (!row.has_value()) {
        return {std::nullopt, std::nullopt};
    }
    std::string submitter{row->submittedBy.Value().ToStringView()};
    return {
        std::optional<std::string>{std::string{row->createdBy.Value().ToStringView()}},
        submitter.empty() ? std::nullopt : std::optional<std::string>{std::move(submitter)},
    };
}

}  // namespace lims
