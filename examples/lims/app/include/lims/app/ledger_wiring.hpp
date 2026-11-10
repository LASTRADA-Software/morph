// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lims/app/internal_client.hpp"
#include "lims/billing/core/ledger_port.hpp"

/// @file
/// The app composes billing and the ledger: billing reaches the ledger only through the port installed here.

namespace lims::app {

/// @brief Gives billing the ledger: `RecordPosting` and `GetRateOn` dispatched through @p posting, and
///        `recordPostingIn` for the shared transaction. Called once at start-up, before the first dispatch.
/// @param posting The posting principal's client; it must outlive the port.
/// @param mode Which way `PostInvoice` posts.
void installLedgerPort(InternalClient& posting, PostingMode mode);

}  // namespace lims::app
