// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <optional>
#include <string>

#include "lims/billing/db/billing_entity.hpp"
#include "lims/billing/dto/invoice_dto.hpp"
#include "lims/billing/dto/line_dto.hpp"
#include "lims/posting/posting.hpp"

/// @file
/// Billing's ORM-level reads and writes shared by its models. Billing-internal: no model header includes it, so a
/// consumer of a model does not pull in the ORM.

namespace lims {

/// @brief Maps a line row to its view.
/// @param row The row.
/// @return The view; `amount` is set only when both halves of the price are stored.
/// @throws ValidationError if the row stores a state or currency outside its enumeration.
[[nodiscard]] BillableLineView billableLineView(db::BillableLineRecord const& row);

/// @brief Drafts an invoice over every unbilled line of @p client at @p site, in the caller's transaction, and marks
///        the lines invoiced.
/// @param mapper The caller's mapper, inside its transaction.
/// @param company The company.
/// @param site The site.
/// @param client The client.
/// @param author Who drafts it.
/// @return The invoice's id; nothing when the client has no unbilled line there.
/// @throws NotFound if the company has no billing settings or the client is not billed there.
/// @throws ValidationError if a line is in another currency than the client is billed in.
/// @throws OutOfRange if a total does not fit.
[[nodiscard]] std::optional<std::int64_t> draftInvoice(Lightweight::DataMapper& mapper, CompanyId company, SiteId site,
                                                       ClientId client, std::string const& author);

/// @brief The invoice @p invoiceId with its lines, through @p mapper.
/// @param mapper An open mapper.
/// @param invoiceId The invoice.
/// @return Its view; `level` is `Company` once its approval deadline has passed.
/// @throws NotFound if it does not exist.
/// @throws ValidationError if a row holds a state or currency outside its enumeration.
[[nodiscard]] InvoiceView invoiceView(Lightweight::DataMapper& mapper, std::int64_t invoiceId);

/// @brief Numbers and posts invoice @p invoiceId in the caller's transaction: writes its row first, then takes the
///        next number of the entry's fiscal year and records it.
///
/// The invoice's row is written before the series is read, so this transaction holds the database's write lock first
/// and a second posting waits for it instead of reading the same next number. The fiscal year is the entry's
/// date's, not the clock's: a retry converges on the entry the first attempt recorded, which may lie in the year
/// before the retry's. Reaches the fault point `post.beforeCommit` once the number is written.
/// @param mapper The caller's mapper, inside its transaction.
/// @param invoiceId The invoice.
/// @param entry The ledger's entry for it.
/// @param author Who posts it.
/// @return The invoice's number, such as `INV-2026-000001`.
/// @throws NotFound if the invoice or its company does not exist.
[[nodiscard]] std::string assignNumberAndPost(Lightweight::DataMapper& mapper, std::int64_t invoiceId,
                                              PostingResult const& entry, std::string const& author);

/// @brief The row of invoice @p invoice.
/// @param invoice The invoice.
/// @return It, or nothing when there is no such invoice.
[[nodiscard]] std::optional<db::InvoiceRecord> findInvoiceRecord(InvoiceId invoice);

/// @brief Whether the approval of @p invoice is past its deadline, so only an approver at its company can approve it.
///
/// The one predicate the approval rule's scope and the view's level share; a submitted invoice's deadline is
/// positive, and a draft has none. Reads the clock.
/// @param invoice The invoice's row.
/// @return True from the deadline instant on.
[[nodiscard]] bool escalated(db::InvoiceRecord const& invoice);

}  // namespace lims
