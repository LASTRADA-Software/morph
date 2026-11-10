// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <optional>
#include <string>

#include "lims/billing/db/billing_entity.hpp"
#include "lims/billing/dto/invoice_dto.hpp"
#include "lims/billing/dto/line_dto.hpp"

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

}  // namespace lims
