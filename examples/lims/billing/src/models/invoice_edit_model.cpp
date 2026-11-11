// SPDX-License-Identifier: Apache-2.0
#include "lims/billing/models/invoice_edit_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <format>
#include <morph/core/logger.hpp>
#include <morph/session/session.hpp>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "lims/billing/authorization/billing_authorization.hpp"
#include "lims/billing/core/fault_hook.hpp"
#include "lims/billing/core/invoice_math.hpp"
#include "lims/billing/db/billing_entity.hpp"
#include "lims/billing/db/billing_store.hpp"
#include "lims/billing/models/price_list_model.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/org/models/org_model.hpp"

namespace lims {

namespace {

/// @brief How long a lease lives after its last edit.
constexpr std::int64_t kLeaseMillis = static_cast<std::int64_t>(15) * 60 * 1000;

/// @brief A token naming one editing instance: 128 random bits as hex.
/// @return The token.
[[nodiscard]] std::string newToken() {
    std::random_device device;
    std::uniform_int_distribution<std::uint64_t> bits;
    return std::format("{:016x}{:016x}", bits(device), bits(device));
}

/// @brief Whether the lease row of @p invoiceId is no longer as @p held read it, on a connection of its own.
///
/// A write that failed is another editor's doing only if that editor's row is there now; a failure that finds the
/// row unchanged is not contention and is the caller's to surface as it is.
/// @param invoiceId The invoice.
/// @param held The lease rows as the caller read them before its write.
/// @return `true` when a row appeared, vanished, or changed its token or expiry.
[[nodiscard]] bool leaseMovedOn(std::int64_t invoiceId, std::vector<db::InvoiceLeaseRecord> const& held) {
    Lightweight::DataMapper reader;
    auto const now = reader.Query<db::InvoiceLeaseRecord>()
                         .Where(::Lightweight::FieldNameOf<&db::InvoiceLeaseRecord::invoiceId>, "=", invoiceId)
                         .All();
    if (now.size() != held.size()) {
        return true;
    }
    return !now.empty() && (now.front().token.Value().ToStringView() != held.front().token.Value().ToStringView() ||
                            now.front().expiresAt.Value() != held.front().expiresAt.Value());
}

/// @brief The lease on @p invoiceId as it is now.
/// @param mapper The mapper to read through.
/// @param invoiceId The invoice.
/// @return The live lease; its holder is empty when the invoice is free or the lease has lapsed.
[[nodiscard]] InvoiceLeaseView currentLease(Lightweight::DataMapper& mapper, std::int64_t invoiceId) {
    InvoiceLeaseView view{.invoice = InvoiceId{invoiceId}, .holder = {}, .expiresAt = {}};
    auto const rows = mapper.Query<db::InvoiceLeaseRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::InvoiceLeaseRecord::invoiceId>, "=", invoiceId)
                          .All();
    if (!rows.empty() && rows.front().expiresAt.Value() > nowMillis()) {
        view.holder = std::string{rows.front().holder.Value().ToStringView()};
        view.expiresAt = timestampFromMillis(rows.front().expiresAt.Value());
    }
    return view;
}

/// @brief Whether the caller can see @p company.
/// @param company The company.
/// @return `true` when it is one of the caller's visible companies.
[[nodiscard]] bool isVisible(CompanyId company) {
    auto const companies = OrgModel::visibleCompanies(currentPrincipal());
    return std::ranges::find(companies, company) != companies.end();
}

/// @brief Counts the rows a statement changed.
/// @param mapper The caller's mapper.
/// @param sql The statement.
/// @return How many rows it changed.
[[nodiscard]] std::size_t changedRows(Lightweight::DataMapper& mapper, std::string const& sql) {
    Lightweight::SqlStatement statement{mapper.Connection()};
    return static_cast<std::size_t>(statement.ExecuteDirect(sql).NumRowsAffected());
}

/// @brief Requires that @p token holds a live lease on @p invoiceId, and renews it.
///
/// One conditional update does both, so a lease that lapses or is taken over between a check and a renewal cannot be
/// renewed by its old holder.
/// @param mapper The caller's mapper, inside its transaction.
/// @param invoiceId The invoice.
/// @param token The editing instance's token.
/// @throws Conflict if the instance holds no live lease on the invoice.
void requireOwnLease(Lightweight::DataMapper& mapper, std::int64_t invoiceId, std::string const& token) {
    std::int64_t const now = nowMillis();
    auto query = mapper.Connection().Query(db::InvoiceLeaseRecord::TableName).Update();
    query.Set("expires_at", now + kLeaseMillis);
    (void)query.Where("invoice_id", "=", invoiceId).Where("token", "=", token).Where("expires_at", ">", now);
    if (changedRows(mapper, query.ToSql()) != 1U) {
        throw Conflict{"this editor does not hold invoice " + std::to_string(invoiceId) + "'s lease"};
    }
}

/// @brief Deletes the leases @p token holds, except on @p keep.
/// @param mapper The caller's mapper.
/// @param token The editing instance's token.
/// @param keep The invoice whose lease stays, if any.
void deleteLeases(Lightweight::DataMapper& mapper, std::string const& token, std::optional<std::int64_t> keep) {
    for (auto const& row : mapper.Query<db::InvoiceLeaseRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::InvoiceLeaseRecord::token>, "=", token)
                               .All()) {
        if (!keep.has_value() || row.invoiceId.Value() != *keep) {
            mapper.Delete(row);
        }
    }
}

/// @brief Requires that @p invoice is a draft.
/// @param invoice The invoice.
/// @param action The action's name, for the refusal.
/// @throws IllegalTransition if it is not.
void requireDraft(InvoiceView const& invoice, char const* action) {
    if (invoice.state != InvoiceState::Draft) {
        throw IllegalTransition{std::string{action} + ": only a draft is edited"};
    }
}

}  // namespace

InvoiceEditModel::InvoiceEditModel() : _token{newToken()} {}

InvoiceEditModel::~InvoiceEditModel() {
    if (!_invoice.has_value()) {
        return;
    }
    // A destructor runs wherever the last reference drops: a screen's unmount, or the server reclaiming a closed
    // connection's instances. A failure here is logged, never thrown.
    try {
        Lightweight::DataMapper mapper;
        deleteLeases(mapper, _token, std::nullopt);
    } catch (std::exception const& error) {
        ::morph::log::logError(std::string{"[lims] releasing invoice lease "} + _token + " failed: " + error.what());
    }
}

InvoiceLeaseView InvoiceEditModel::execute(const LeaseInvoice& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"LeaseInvoice: an invoice is required"};
    }
    // The reads run on a mapper of their own, outside the write transaction: the write below is conditional on the
    // rows as they were read, so it needs no lock held since the read, and a concurrent editor's commit is not turned
    // away by one.
    Lightweight::DataMapper reader;
    auto const invoice = invoiceView(reader, *action.invoiceId);
    requireDraft(invoice, "LeaseInvoice");
    std::int64_t const now = nowMillis();
    std::int64_t const expiresAt = now + kLeaseMillis;
    std::string const principal = ::morph::session::current()->principal;
    auto const held =
        reader.Query<db::InvoiceLeaseRecord>()
            .Where(::Lightweight::FieldNameOf<&db::InvoiceLeaseRecord::invoiceId>, "=", *action.invoiceId)
            .All();
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    // A fault point sits between the read and the write, so a test makes a concurrent editor win the lease.
    faultPoint("invoice.beforeLeaseWrite", mapper.Connection());
    try {
        if (held.empty()) {
            db::InvoiceLeaseRecord row;
            row.invoiceId = *action.invoiceId;
            row.holder = Lightweight::SqlAnsiString<64>{principal};
            row.token = Lightweight::SqlAnsiString<32>{_token};
            row.expiresAt = expiresAt;
            mapper.Create(row);
        } else {
            auto const& row = held.front();
            std::string const holder{row.holder.Value().ToStringView()};
            std::string const holderToken{row.token.Value().ToStringView()};
            if (row.expiresAt.Value() > now && holderToken != _token) {
                throw Conflict{"LeaseInvoice: invoice " + std::to_string(*action.invoiceId) + " is being edited by '" +
                               holder + "'"};
            }
            // Conditional on the row as it was read: of two editors taking one lapsed lease, the second finds the
            // token or the expiry changed and changes nothing.
            auto query = mapper.Connection().Query(db::InvoiceLeaseRecord::TableName).Update();
            query.Set("holder", principal).Set("token", _token).Set("expires_at", expiresAt);
            (void)query.Where("invoice_id", "=", *action.invoiceId)
                .Where("token", "=", holderToken)
                .Where("expires_at", "=", row.expiresAt.Value());
            if (changedRows(mapper, query.ToSql()) != 1U) {
                throw Conflict{"LeaseInvoice: invoice " + std::to_string(*action.invoiceId) +
                               " was leased to another editor meanwhile"};
            }
        }
        deleteLeases(mapper, _token, *action.invoiceId);
        sqlTxn.Commit();
    } catch (Lightweight::SqlException const& error) {
        // Contention is decided by what the lease row says now, not by what the error says: a constraint failure
        // that leaves the row as it was is a defect and surfaces as itself.
        if (!isBusy(error) && !leaseMovedOn(*action.invoiceId, held)) {
            throw;
        }
        throw Conflict{"LeaseInvoice: invoice " + std::to_string(*action.invoiceId) +
                       " was leased to another editor meanwhile"};
    }
    _invoice = *action.invoiceId;
    return InvoiceLeaseView{
        .invoice = action.invoiceId,
        .holder = principal,
        .expiresAt = timestampFromMillis(expiresAt),
    };
}

InvoiceLeaseView InvoiceEditModel::execute(const ReleaseInvoice& action) {
    static_cast<void>(action);
    requirePrincipal();
    if (!_invoice.has_value()) {
        return InvoiceLeaseView{.invoice = {}, .holder = {}, .expiresAt = {}};
    }
    std::int64_t const invoiceId = *_invoice;
    Lightweight::DataMapper mapper;
    deleteLeases(mapper, _token, std::nullopt);
    _invoice.reset();
    // What is left, not what was assumed: after a takeover the lease is another editor's and the answer says so.
    return currentLease(mapper, invoiceId);
}

InvoiceView InvoiceEditModel::execute(const RemoveInvoiceLine& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"RemoveInvoiceLine: a line is required"};
    }
    // The line and its invoice are read on a mapper of their own, so the write transaction below opens with its first
    // write (the lease renewal): a transaction that read first and wrote later could be turned away by a concurrent
    // commit with a raw "database is locked" instead of a Conflict.
    std::int64_t invoiceId = 0;
    {
        Lightweight::DataMapper reader;
        auto const lines = reader.Query<db::BillableLineRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::BillableLineRecord::id>, "=", *action.line)
                               .All();
        if (lines.empty() || !lines.front().invoiceId.Value().has_value()) {
            throw NotFound{"RemoveInvoiceLine: line " + std::to_string(*action.line) + " is not on an invoice"};
        }
        invoiceId = *lines.front().invoiceId.Value();
        // A line of a company the caller cannot see is not found, as it would not be for a line that is on no invoice.
        if (!isVisible(invoiceView(reader, invoiceId).company)) {
            throw NotFound{"RemoveInvoiceLine: line " + std::to_string(*action.line) + " is not on an invoice"};
        }
    }
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    requireOwnLease(mapper, invoiceId, _token);
    auto const invoice = invoiceView(mapper, invoiceId);
    requireDraft(invoice, "RemoveInvoiceLine");
    auto const settings = PriceListModel::settingsOf(invoice.company);
    if (!settings.has_value()) {
        throw NotFound{"RemoveInvoiceLine: company " + std::to_string(*invoice.company) + " has no billing settings"};
    }
    auto release = mapper.Connection().Query(db::BillableLineRecord::TableName).Update();
    release.Set("state", static_cast<int>(BillableLineState::Unbilled)).Set("invoice_id", Lightweight::SqlNullValue);
    (void)release.Where("id", "=", *action.line)
        .Where("invoice_id", "=", invoiceId)
        .Where("state", "=", static_cast<int>(BillableLineState::Invoiced));
    if (changedRows(mapper, release.ToSql()) != 1U) {
        throw Conflict{"RemoveInvoiceLine: line " + std::to_string(*action.line) + " was taken off meanwhile"};
    }
    std::vector<::morph::math::Rational> amounts;
    for (auto const& line : invoiceView(mapper, invoiceId).lines) {
        amounts.push_back(*line.amount);
    }
    auto const totals = invoiceTotals(amounts, *settings->taxRate, invoice.currency);
    auto retotal = mapper.Connection().Query(db::InvoiceRecord::TableName).Update();
    retotal.Set("net_num", totals.net.numerator)
        .Set("net_den", totals.net.denominator)
        .Set("tax_num", totals.tax.numerator)
        .Set("tax_den", totals.tax.denominator)
        .Set("gross_num", totals.gross.numerator)
        .Set("gross_den", totals.gross.denominator);
    (void)retotal.Where("id", "=", invoiceId).Where("state", "=", static_cast<int>(InvoiceState::Draft));
    if (changedRows(mapper, retotal.ToSql()) != 1U) {
        throw Conflict{"RemoveInvoiceLine: invoice " + std::to_string(invoiceId) + " was submitted meanwhile"};
    }
    sqlTxn.Commit();
    return invoiceView(mapper, invoiceId);
}

InvoiceView InvoiceEditModel::execute(const SetInvoiceReference& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"SetInvoiceReference: a reference is at most " +
                              std::to_string(SetInvoiceReference::kMaxReferenceBytes) + " bytes"};
    }
    if (!_invoice.has_value()) {
        throw Conflict{"this editor holds no invoice lease"};
    }
    std::int64_t const invoiceId = *_invoice;
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    requireOwnLease(mapper, invoiceId, _token);
    requireDraft(invoiceView(mapper, invoiceId), "SetInvoiceReference");
    auto query = mapper.Connection().Query(db::InvoiceRecord::TableName).Update();
    query.Set("reference", action.reference);
    (void)query.Where("id", "=", invoiceId).Where("state", "=", static_cast<int>(InvoiceState::Draft));
    if (changedRows(mapper, query.ToSql()) != 1U) {
        throw Conflict{"SetInvoiceReference: invoice " + std::to_string(invoiceId) + " was submitted meanwhile"};
    }
    sqlTxn.Commit();
    return invoiceView(mapper, invoiceId);
}

}  // namespace lims
