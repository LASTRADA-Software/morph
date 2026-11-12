// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <core/async/Task.hpp>
#include <cstdint>
#include <morph/session/session.hpp>
#include <string>

#include "lims/billing/authorization/billing_authorization.hpp"
#include "lims/billing/core/fault_hook.hpp"
#include "lims/billing/core/refusal_codes.hpp"
#include "lims/billing/db/billing_entity.hpp"
#include "lims/billing/db/billing_store.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/billing/models/price_list_model.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims/posting/ledger_port.hpp"

namespace lims {

namespace {

constexpr std::int64_t kMillisPerHour = 3'600'000;

/// @brief The row of @p invoice.
/// @param invoice The invoice.
/// @param action The action's name, for the refusal.
/// @return The row.
/// @throws NotFound if there is no such invoice.
[[nodiscard]] db::InvoiceRecord invoiceRecord(InvoiceId invoice, char const* action) {
    auto row = findInvoiceRecord(invoice);
    if (!row.has_value()) {
        throw NotFound{std::string{action} + ": invoice " + std::to_string(*invoice) + " does not exist"};
    }
    return *row;
}

/// @brief Runs a conditional update of invoice @p invoiceId from @p from, and counts the rows it changed.
///
/// A fault point sits between the caller's read and this write, so a test makes a concurrent action win it.
/// @tparam Set Callable taking the update's query builder.
/// @tparam Guard Callable taking the update's query builder, adding conditions on what the caller read.
/// @param mapper The caller's mapper, inside its transaction.
/// @param invoiceId The invoice.
/// @param from The state the caller read and requires still.
/// @param set Adds the columns to change.
/// @param guard Adds the further columns the caller read and requires unchanged.
/// @return Whether exactly one row matched.
template <typename Set, typename Guard>
[[nodiscard]] bool updateInvoiceIf(Lightweight::DataMapper& mapper, std::int64_t invoiceId, InvoiceState from,
                                   Set const& set, Guard const& guard) {
    faultPoint("invoice.beforeStateChange", mapper.Connection());
    auto query = mapper.Connection().Query(db::InvoiceRecord::TableName).Update();
    set(query);
    (void)query.Where("id", "=", invoiceId).Where("state", "=", static_cast<int>(from));
    guard(query);
    Lightweight::SqlStatement statement{mapper.Connection()};
    return statement.ExecuteDirect(query.ToSql()).NumRowsAffected() == 1U;
}

/// @brief Runs a conditional update of invoice @p invoiceId that requires only its state, from @p from.
/// @tparam Set Callable taking the update's query builder.
/// @param mapper The caller's mapper, inside its transaction.
/// @param invoiceId The invoice.
/// @param from The state the caller read and requires still.
/// @param set Adds the columns to change.
/// @return Whether exactly one row matched.
template <typename Set>
[[nodiscard]] bool updateInvoiceIf(Lightweight::DataMapper& mapper, std::int64_t invoiceId, InvoiceState from,
                                   Set const& set) {
    return updateInvoiceIf(mapper, invoiceId, from, set, [](auto&) {});
}

/// @brief Requires that no editor other than the caller holds a live lease on @p invoiceId.
///
/// A lease held by the submitter is not an obstacle: a clerk submits what they have been editing. The read runs on a
/// connection of its own: the caller's transaction must open with its write, because a transaction that read first
/// holds a shared lock that a concurrent editor's commit then waits on.
/// @param invoiceId The invoice.
/// @throws Conflict naming the holder if another principal's lease is live.
void requireNoOtherEditor(std::int64_t invoiceId) {
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<db::InvoiceLeaseRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::InvoiceLeaseRecord::invoiceId>, "=", invoiceId)
                          .All();
    if (rows.empty() || rows.front().expiresAt.Value() <= nowMillis()) {
        return;
    }
    std::string const holder{rows.front().holder.Value().ToStringView()};
    if (holder != ::morph::session::current()->principal) {
        throw Conflict{std::string{kInvoiceLeasedCode},
                       {{"holder", OrgModel::displayNameOf(holder)}},
                       "SubmitInvoice: invoice " + std::to_string(invoiceId) + " is being edited by '" + holder + "'"};
    }
}

}  // namespace

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
::core::async::Task<InvoiceView> InvoiceModel::execute(SubmitInvoice action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"SubmitInvoice: an invoice is required"};
    }
    // Taken before the first await: the session is the dispatch's, and a resumption may not run inside it.
    std::string const principal = ::morph::session::current()->principal;
    auto const row = invoiceRecord(action.invoiceId, "SubmitInvoice");
    if (row.state.Value() != static_cast<int>(InvoiceState::Draft)) {
        throw IllegalTransition{"SubmitInvoice: only a draft is submitted"};
    }
    requireNoOtherEditor(static_cast<std::int64_t>(row.id.Value()));
    CompanyId const company{row.companyId.Value()};
    auto const currency = static_cast<CurrencyCode>(row.currency.Value());
    auto const companyView = OrgModel::findCompany(company);
    if (!companyView.has_value()) {
        throw NotFound{"SubmitInvoice: company " + std::to_string(*company) + " does not exist"};
    }
    auto const base = companyView->baseCurrency;
    auto const settings = PriceListModel::settingsOf(company);
    if (!settings.has_value()) {
        throw NotFound{"SubmitInvoice: company " + std::to_string(*company) + " has no billing settings"};
    }
    // The gross in the company's currency is what the approver's limit is judged against. In the company's currency
    // it is the invoice's own gross; in another it is that gross at the ledger's rate on the submission date, rounded
    // half-up to the company currency's decimals.
    std::int64_t grossCompanyNum = row.grossNum.Value();
    std::int64_t grossCompanyDen = row.grossDen.Value();
    if (currency != base) {
        std::string const unvalued = "SubmitInvoice: invoice " + std::to_string(*action.invoiceId) + " is in " +
                                     std::string{currencyName(currency)} + " and no rate values it in " +
                                     std::string{currencyName(base)};
        auto const& port = ledgerPort();
        if (!port.rateOn) {
            throw ValidationError{unvalued};
        }
        auto const dated = co_await port.rateOn(
            RateQuery{.company = company, .from = currency, .to = base, .on = timestampFromMillis(nowMillis())});
        if (!dated.rate.hasValue()) {
            throw ValidationError{unvalued};
        }
        ::morph::math::Rational const gross{::morph::math::Numerator{row.grossNum.Value()},
                                            ::morph::math::Denominator{row.grossDen.Value()},
                                            ::morph::math::DecimalPlaces{decimalsOf(currency)}};
        auto const converted = ::morph::math::checkedMul(gross, *dated.rate);
        if (!converted.has_value()) {
            throw OutOfRange{"SubmitInvoice: the gross is too large to convert exactly"};
        }
        auto const valued = roundToCurrency(*converted, base);
        grossCompanyNum = valued.numerator;
        grossCompanyDen = valued.denominator;
    }
    auto const& hoursValue = *settings->approvalHours;
    std::int64_t const hours = hoursValue.numerator / hoursValue.denominator;
    std::int64_t const now = nowMillis();
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    // The update requires the invoice to be a draft with the gross read above: of two submissions, the second finds
    // it submitted; an edit that removed a line while the rate was awaited changed the gross, and recording the
    // converted stale amount would judge the approver's limit against an invoice that no longer exists.
    if (!updateInvoiceIf(
            mapper, static_cast<std::int64_t>(row.id.Value()), InvoiceState::Draft,
            [&](auto& query) {
                query.Set("state", static_cast<int>(InvoiceState::Submitted))
                    .Set("submitted_by", principal)
                    .Set("submitted_at", now)
                    .Set("deadline_at", now + (hours * kMillisPerHour))
                    .Set("gross_company_num", grossCompanyNum)
                    .Set("gross_company_den", grossCompanyDen);
            },
            [&](auto& query) {
                (void)query.Where("gross_num", "=", row.grossNum.Value())
                    .Where("gross_den", "=", row.grossDen.Value());
            })) {
        throw Conflict{"SubmitInvoice: invoice " + std::to_string(*action.invoiceId) +
                       " was submitted or edited meanwhile"};
    }
    sqlTxn.Commit();
    co_return invoiceView(mapper, static_cast<std::int64_t>(row.id.Value()));
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
InvoiceView InvoiceModel::execute(const ApproveInvoice& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"ApproveInvoice: an invoice is required"};
    }
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto const row = invoiceRecord(action.invoiceId, "ApproveInvoice");
    // The action's guard refuses a draft or an approved invoice as an IllegalTransition before it reads who created
    // the invoice; this repeats it for a call that reaches the handler without the guard.
    if (row.state.Value() != static_cast<int>(InvoiceState::Submitted)) {
        throw IllegalTransition{"ApproveInvoice: only a submitted invoice is approved"};
    }
    // Judged again just before the write: the guard ran it first, on every path, and this reads the scope, the gross
    // and the performers once more, so a role or limit changed since is seen. The rule reads on its own connection, so
    // it sees committed rows, not this transaction's; this transaction has written nothing yet. That is enough because
    // a submitted invoice's gross and performers cannot change (every edit is conditional on Draft, and nothing
    // returns an invoice to Draft), and the conditional update below on Submitted binds what was judged to what is
    // written; it is also what makes the write safe against a concurrent approval.
    requireAuthorized(action);
    if (!updateInvoiceIf(mapper, static_cast<std::int64_t>(row.id.Value()), InvoiceState::Submitted, [](auto& query) {
            query.Set("state", static_cast<int>(InvoiceState::Approved))
                .Set("approved_by", ::morph::session::current()->principal);
        })) {
        throw Conflict{"ApproveInvoice: invoice " + std::to_string(*action.invoiceId) + " was approved meanwhile"};
    }
    sqlTxn.Commit();
    return invoiceView(mapper, static_cast<std::int64_t>(row.id.Value()));
}

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListInvoicesResult InvoiceModel::execute(const ListPendingApprovals& action) {
    static_cast<void>(action);
    std::string const principal = currentPrincipal();
    ListInvoicesResult result;
    if (principal.empty()) {
        return result;
    }
    auto const visible = OrgModel::visibleCompanies(principal);
    Lightweight::DataMapper mapper;
    auto const rows = mapper.Query<db::InvoiceRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::InvoiceRecord::state>, "=",
                                 static_cast<int>(InvoiceState::Submitted))
                          .All();
    for (auto const& row : rows) {
        if (std::ranges::find(visible, CompanyId{row.companyId.Value()}) == visible.end()) {
            continue;
        }
        ApproveInvoice const approve{.invoiceId = InvoiceId{static_cast<std::int64_t>(row.id.Value())}};
        bool allowed = false;
        try {
            allowed = authorizationVerdict(authorizationFactsOf(approve, principal).facts) == Verdict::Allowed;
        } catch (ValidationError const&) {
            continue;  // no company-currency value: nobody can approve it, so it is nobody's to act on
        }
        if (allowed) {
            // A row that does not map is reported, not dropped from every inbox.
            result.invoices.push_back(invoiceView(mapper, static_cast<std::int64_t>(row.id.Value())));
        }
    }
    std::ranges::sort(result.invoices, {}, [](InvoiceView const& invoice) { return *invoice.id; });
    return result;
}

}  // namespace lims
