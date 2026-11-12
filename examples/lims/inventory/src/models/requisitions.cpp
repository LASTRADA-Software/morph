// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <cstdint>
#include <morph/session/session.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <string_view>

#include "lims/inventory/authorization/inventory_authorization.hpp"
#include "lims/inventory/core/fault_hook.hpp"
#include "lims/inventory/core/units.hpp"
#include "lims/inventory/db/inventory_entity.hpp"
#include "lims/inventory/db/inventory_store.hpp"
#include "lims/inventory/models/procurement_model.hpp"
#include "lims/inventory/models/stock_model.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/core/money.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims {

namespace {

using ::morph::math::DecimalPlaces;
using ::morph::math::Denominator;
using ::morph::math::Numerator;
using ::morph::math::Rational;

constexpr std::int64_t kMillisPerHour = 3'600'000;

using inventory::moveRequisition;
using inventory::visibleRequisition;

/// @brief Requires that the caller raised @p row and that it is still a draft: what adding a line and submitting
///        share.
/// @param row The requisition.
/// @param action The action's name, for the refusal.
/// @throws IllegalTransition if it is not a draft.
/// @throws Forbidden if the caller is not its requester.
void requireOwnDraft(db::RequisitionRecord const& row, char const* action) {
    if (row.state.Value() != static_cast<int>(RequisitionState::Draft)) {
        throw IllegalTransition{std::string{action} + ": only a draft is changed"};
    }
    if (std::string_view{row.requestedBy.Value().ToStringView()} != currentPrincipal()) {
        throw Forbidden{std::string{action} + ": only its requester changes a requisition"};
    }
}

}  // namespace

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
RequisitionView ProcurementModel::execute(const CreateRequisition& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateRequisition: a company, a site and a reason of 1 to 128 bytes are required"};
    }
    auto const company = OrgModel::findCompany(action.company);
    if (!company.has_value()) {
        throw NotFound{"CreateRequisition: company " + std::to_string(*action.company) + " does not exist"};
    }
    auto const site = OrgModel::findSite(action.site);
    if (!site.has_value() || site->companyId != action.company) {
        throw ValidationError{"CreateRequisition: site " + std::to_string(*action.site) + " is not one of company " +
                              std::to_string(*action.company) + "'s"};
    }
    db::RequisitionRecord row;
    row.companyId = *action.company;
    row.siteId = *action.site;
    row.reason = Lightweight::SqlAnsiString<128>{action.reason};
    row.state = static_cast<int>(RequisitionState::Draft);
    row.requestedBy = Lightweight::SqlAnsiString<64>{currentPrincipal()};
    row.createdAt = nowMillis();
    row.totalNum = 0;
    row.totalDen = 1;
    row.currency = static_cast<int>(company->baseCurrency);
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    mapper.Create(row);
    sqlTxn.Commit();
    return inventory::requisitionView(mapper, static_cast<std::int64_t>(row.id.Value()));
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
RequisitionView ProcurementModel::execute(const AddRequisitionLine& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"AddRequisitionLine: a requisition, an item, a quantity and an estimate are required"};
    }
    auto const row = visibleRequisition(action.requisition, "AddRequisitionLine");
    requireOwnDraft(row, "AddRequisitionLine");
    // Reads on a mapper of their own: the transaction below opens with its first write.
    Lightweight::DataMapper reader;
    auto const item = inventory::findItem(reader, *action.item);
    if (!item.has_value() || item->companyId.Value() != row.companyId.Value()) {
        throw NotFound{"AddRequisitionLine: item " + std::to_string(*action.item) + " is not one of requisition " +
                       std::to_string(*action.requisition) + "'s company"};
    }
    auto const quantity = convertStock(*action.quantity, action.unit, inventory::stockUnitOf(*item));
    if (quantity.numerator <= 0) {
        throw ValidationError{"AddRequisitionLine: the quantity must be positive"};
    }
    auto const currency = static_cast<CurrencyCode>(row.currency.Value());
    Rational const& estimate = *action.estimate;
    if (estimate.numerator <= 0 || !fitsCurrency(estimate, currency)) {
        throw ValidationError{
            "AddRequisitionLine: the estimate must be positive and a whole number of the currency's minor units"};
    }
    Rational const before{Numerator{row.totalNum.Value()}, Denominator{row.totalDen.Value()},
                          DecimalPlaces{decimalsOf(currency)}};
    auto const total = ::morph::math::checkedAdd(before, estimate);
    if (!total.has_value()) {
        throw OutOfRange{"AddRequisitionLine: the total does not fit"};
    }
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    // Conditional on the total read: two lines added at once cannot both start from the same total.
    if (!moveRequisition(
            mapper, static_cast<std::int64_t>(row.id.Value()), RequisitionState::Draft,
            [&](auto& query) { query.Set("total_num", total->numerator).Set("total_den", total->denominator); },
            [&](auto& query) {
                (void)query.Where("total_num", "=", row.totalNum.Value())
                    .Where("total_den", "=", row.totalDen.Value());
            })) {
        throw Conflict{"AddRequisitionLine: requisition " + std::to_string(*action.requisition) +
                       " was submitted or changed meanwhile"};
    }
    db::RequisitionLineRecord line;
    line.requisitionId = static_cast<std::int64_t>(row.id.Value());
    line.itemId = *action.item;
    line.quantityNum = quantity.numerator;
    line.quantityDen = quantity.denominator;
    line.estimateNum = estimate.numerator;
    line.estimateDen = estimate.denominator;
    mapper.Create(line);
    sqlTxn.Commit();
    return inventory::requisitionView(mapper, static_cast<std::int64_t>(row.id.Value()));
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
RequisitionView ProcurementModel::execute(const SubmitRequisition& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"SubmitRequisition: a requisition is required"};
    }
    auto const row = visibleRequisition(action.requisitionId, "SubmitRequisition");
    std::string const principal = currentPrincipal();
    requireOwnDraft(row, "SubmitRequisition");
    if (row.totalNum.Value() <= 0) {
        throw ValidationError{"SubmitRequisition: a requisition with no line has nothing to approve"};
    }
    auto const settings = StockModel::settingsOf(CompanyId{row.companyId.Value()});
    if (!settings.has_value()) {
        throw NotFound{"SubmitRequisition: company " + std::to_string(row.companyId.Value()) +
                       " has no inventory settings"};
    }
    auto const& hoursValue = *settings->approvalHours;
    std::int64_t const hours = hoursValue.numerator / hoursValue.denominator;
    std::int64_t const now = nowMillis();
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    // Conditional on the total read: a line added between the read and the write changed it, and the approver's limit
    // must be judged against the requisition as it is.
    if (!moveRequisition(
            mapper, static_cast<std::int64_t>(row.id.Value()), RequisitionState::Draft,
            [&](auto& query) {
                query.Set("state", static_cast<int>(RequisitionState::Submitted))
                    .Set("submitted_by", principal)
                    .Set("submitted_at", now)
                    .Set("deadline_at", now + (hours * kMillisPerHour));
            },
            [&](auto& query) {
                (void)query.Where("total_num", "=", row.totalNum.Value())
                    .Where("total_den", "=", row.totalDen.Value());
            })) {
        throw Conflict{"SubmitRequisition: requisition " + std::to_string(*action.requisitionId) +
                       " was submitted or changed meanwhile"};
    }
    sqlTxn.Commit();
    return inventory::requisitionView(mapper, static_cast<std::int64_t>(row.id.Value()));
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
RequisitionView ProcurementModel::execute(const ApproveRequisition& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"ApproveRequisition: a requisition is required"};
    }
    auto const row = visibleRequisition(action.requisitionId, "ApproveRequisition");
    // The action's guard refuses a draft or an approved requisition as an IllegalTransition before it reads who
    // raised it; this repeats it for a call that reaches the handler without the guard.
    if (row.state.Value() != static_cast<int>(RequisitionState::Submitted)) {
        throw IllegalTransition{"ApproveRequisition: only a submitted requisition is approved"};
    }
    // Judged again just before the write: the guard ran it first, and this reads the scope, the total and the
    // performers once more, so a role or limit changed since is seen. The rule reads on its own connection, so it sees
    // committed rows; this transaction has written nothing yet. That is enough because a submitted requisition's total
    // and performers cannot change (every edit is conditional on Draft, and nothing returns it to Draft), and the
    // conditional update below on Submitted binds what was judged to what is written.
    requireAuthorized(action);
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    if (!moveRequisition(
            mapper, static_cast<std::int64_t>(row.id.Value()), RequisitionState::Submitted,
            [](auto& query) {
                query.Set("state", static_cast<int>(RequisitionState::Approved))
                    .Set("approved_by", ::morph::session::current()->principal);
            },
            [](auto&) {})) {
        throw Conflict{"ApproveRequisition: requisition " + std::to_string(*action.requisitionId) +
                       " was approved meanwhile"};
    }
    sqlTxn.Commit();
    return inventory::requisitionView(mapper, static_cast<std::int64_t>(row.id.Value()));
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
RequisitionView ProcurementModel::execute(const GetRequisition& action) {
    if (!action.validate()) {
        throw ValidationError{"GetRequisition: a requisition is required"};
    }
    auto const row = visibleRequisition(action.requisitionId, "GetRequisition");
    Lightweight::DataMapper mapper;
    return inventory::requisitionView(mapper, static_cast<std::int64_t>(row.id.Value()));
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListRequisitionsResult ProcurementModel::execute(const ListRequisitions& action) {
    if (!action.validate()) {
        throw ValidationError{"ListRequisitions: a company is required"};
    }
    ListRequisitionsResult result;
    auto const visible = OrgModel::visibleCompanies(currentPrincipal());
    if (std::ranges::find(visible, action.company) == visible.end()) {
        return result;
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::RequisitionRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::RequisitionRecord::companyId>, "=", *action.company)
                    .All();
    std::ranges::sort(rows, {}, [](db::RequisitionRecord const& row) { return row.id.Value(); });
    for (auto const& row : rows) {
        result.requisitions.push_back(inventory::requisitionView(mapper, static_cast<std::int64_t>(row.id.Value())));
    }
    return result;
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListRequisitionsResult ProcurementModel::execute(const ListPendingRequisitions& action) {
    static_cast<void>(action);
    std::string const principal = currentPrincipal();
    ListRequisitionsResult result;
    if (principal.empty()) {
        return result;
    }
    auto const visible = OrgModel::visibleCompanies(principal);
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::RequisitionRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::RequisitionRecord::state>, "=",
                           static_cast<int>(RequisitionState::Submitted))
                    .All();
    std::ranges::sort(rows, {}, [](db::RequisitionRecord const& row) { return row.id.Value(); });
    for (auto const& row : rows) {
        if (std::ranges::find(visible, CompanyId{row.companyId.Value()}) == visible.end()) {
            continue;
        }
        ApproveRequisition const approve{.requisitionId = RequisitionId{static_cast<std::int64_t>(row.id.Value())}};
        if (authorizationVerdict(authorizationFactsOf(approve, principal).facts) == Verdict::Allowed) {
            result.requisitions.push_back(
                inventory::requisitionView(mapper, static_cast<std::int64_t>(row.id.Value())));
        }
    }
    return result;
}

}  // namespace lims
