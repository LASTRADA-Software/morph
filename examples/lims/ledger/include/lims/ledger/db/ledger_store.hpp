// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>

#include "lims/posting/posting.hpp"

/// @file
/// Recording a posting through a caller's open mapper. Ledger-internal: `LedgerModel::record` is it inside a
/// transaction of its own, and the app's port hands it billing's mapper for the shared-transaction posting.

namespace lims {

/// @brief Records @p request as a posted entry through @p mapper, once per company and source key; the caller
///        commits.
/// @param mapper The caller's mapper, inside its transaction.
/// @param request The request.
/// @return The entry and its date; the first one recorded under the request's key when it is repeated.
/// @throws ValidationError, NotFound, Conflict as `LedgerModel::record` documents.
[[nodiscard]] PostingResult recordPostingIn(Lightweight::DataMapper& mapper, PostingRequest const& request);

}  // namespace lims
