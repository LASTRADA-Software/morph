// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lims/app/internal_client.hpp"
#include "lims/posting/ledger_port.hpp"

/// @file
/// The app composes billing and the ledger: billing and inventory reach the ledger only through the one port installed
/// here.

namespace lims::app {

/// @brief Gives every poster the ledger: `RecordPosting` and `GetRateOn` dispatched through @p posting, and
///        `recordPostingIn` for the shared transaction. Called once at start-up, before the first dispatch.
/// @param posting The posting principal's client; it must outlive the port.
/// @param mode Which way a poster posts.
void installLedgerPort(InternalClient& posting, PostingMode mode);

}  // namespace lims::app
