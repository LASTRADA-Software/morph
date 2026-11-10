// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <core/async/Task.hpp>
#include <cstdint>
#include <string>
#include <utility>

#include "lims/billing/authorization/billing_authorization.hpp"
#include "lims/billing/core/fault_hook.hpp"
#include "lims/billing/core/invoice_math.hpp"
#include "lims/billing/core/ledger_port.hpp"
#include "lims/billing/db/billing_store.hpp"
#include "lims/billing/models/invoice_model.hpp"
#include "lims/billing/models/price_list_model.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims {

namespace {

/// @brief What a clerk is told when the database turned a posting away because another one is writing.
/// @param invoiceId The invoice.
/// @return The refusal.
[[nodiscard]] Conflict contended(std::int64_t invoiceId) {
    return Conflict{"PostInvoice: invoice " + std::to_string(invoiceId) + " is being posted by another clerk"};
}

/// @brief Numbers and posts the invoice in a transaction of its own: the second commit of a dispatched posting.
/// @param invoiceId The invoice.
/// @param posted The ledger's entry, already committed.
/// @param author Who posts it.
/// @return The posted invoice.
/// @throws Conflict if the invoice was posted meanwhile or the database turned this posting away as busy.
[[nodiscard]] InvoiceView numberAndCommit(std::int64_t invoiceId, PostingResult const& posted,
                                          std::string const& author) {
    Lightweight::DataMapper mapper;
    faultPoint("post.afterLedger", mapper.Connection());
    try {
        Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
        static_cast<void>(assignNumberAndPost(mapper, invoiceId, posted, author));
        sqlTxn.Commit();
    } catch (Lightweight::SqlException const& error) {
        if (!isBusy(error)) {
            throw;
        }
        throw contended(invoiceId);
    }
    return invoiceView(mapper, invoiceId);
}

/// @brief Records the entry through billing's own mapper and numbers and posts the invoice: both halves in one
///        transaction, committed once.
/// @param port The installed port.
/// @param request The posting.
/// @param invoiceId The invoice.
/// @param author Who posts it.
/// @return The posted invoice.
/// @throws Conflict if the invoice was posted meanwhile or the database turned this posting away as busy.
[[nodiscard]] InvoiceView postInOneTransaction(LedgerPort const& port, PostingRequest const& request,
                                               std::int64_t invoiceId, std::string const& author) {
    Lightweight::DataMapper mapper;
    try {
        Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
        PostingResult const posted = port.postIn(mapper, request);
        faultPoint("post.afterLedger", mapper.Connection());
        static_cast<void>(assignNumberAndPost(mapper, invoiceId, posted, author));
        sqlTxn.Commit();
    } catch (Lightweight::SqlException const& error) {
        if (!isBusy(error)) {
            throw;
        }
        throw contended(invoiceId);
    }
    return invoiceView(mapper, invoiceId);
}

}  // namespace

// An action handler: the registry calls it on the instance, like every other.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
::core::async::Task<InvoiceView> InvoiceModel::execute(PostInvoice action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"PostInvoice: an invoice is required"};
    }
    requireRoleSomewhere<PostInvoice>();
    // Taken before the first await: the session is the dispatch's, and a resumption may not run inside it.
    std::string const author = currentPrincipal();
    InvoiceView invoice;
    {
        Lightweight::DataMapper mapper;
        invoice = invoiceView(mapper, *action.invoiceId);
    }
    requireAuthorized(action);
    if (invoice.state != InvoiceState::Approved) {
        throw IllegalTransition{"PostInvoice: only an approved invoice is posted"};
    }
    auto const& port = ledgerPort();
    if (!port.post || !port.postIn || !port.find || !port.rateOn) {
        throw ValidationError{"PostInvoice: no ledger is wired to post to"};
    }
    auto const company = OrgModel::findCompany(invoice.company);
    if (!company.has_value()) {
        throw NotFound{"PostInvoice: company " + std::to_string(*invoice.company) + " does not exist"};
    }
    auto const settings = PriceListModel::settingsOf(invoice.company);
    if (!settings.has_value()) {
        throw NotFound{"PostInvoice: company " + std::to_string(*invoice.company) + " has no billing settings"};
    }
    std::string const sourceKey = "billing.invoice:" + std::to_string(*invoice.id);
    auto when = ::morph::ladder::now();
    if (port.mode == PostingMode::Dispatched) {
        // A retry repeats the request the ledger recorded, date included: the ledger answers a repeated key only for
        // an identical request, so the entry's own date is the date to send, and its rate the rate on that date.
        auto const earlier = co_await port.find(PostingQuery{.company = invoice.company, .sourceKey = sourceKey});
        if (earlier.entryId.hasValue()) {
            when = earlier.date;
        }
    }
    ::morph::math::Rational rate{1, ::morph::math::DecimalPlaces{6}};
    if (invoice.currency != company->baseCurrency) {
        auto const dated = co_await port.rateOn(
            RateQuery{.company = invoice.company, .from = invoice.currency, .to = company->baseCurrency, .on = when});
        if (!dated.rate.hasValue()) {
            throw ValidationError{"PostInvoice: no " + std::string{currencyName(invoice.currency)} + "/" +
                                  std::string{currencyName(company->baseCurrency)} + " rate today"};
        }
        rate = *dated.rate;
    }
    PostingRequest request = postingFor(InvoiceToPost{
        .company = invoice.company,
        .companyCurrency = company->baseCurrency,
        .currency = invoice.currency,
        .rate = rate,
        .date = when,
        .sourceKey = sourceKey,
        .sourceReference = "invoice " + std::to_string(*invoice.id),
        .net = *invoice.net,
        .tax = *invoice.tax,
        .gross = *invoice.gross,
        .receivableAccount = settings->receivableAccount,
        .revenueAccount = settings->revenueAccount,
        .taxAccount = settings->taxAccount,
        .roundingAccount = settings->roundingAccount,
    });
    if (port.mode == PostingMode::SharedTransaction) {
        // One transaction: the ledger's entry is written through billing's own mapper and commits with the number.
        co_return postInOneTransaction(port, request, *invoice.id, author);
    }
    // The first commit: the ledger records the entry as its own action. Between it and the second, an entry exists
    // for an invoice that is not posted; a retry converges on that entry, since the ledger records each key once.
    PostingResult const posted = co_await port.post(std::move(request));
    co_return numberAndCommit(*invoice.id, posted, author);
}

}  // namespace lims
