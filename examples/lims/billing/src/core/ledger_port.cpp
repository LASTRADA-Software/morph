// SPDX-License-Identifier: Apache-2.0
#include "lims/billing/core/ledger_port.hpp"

#include <utility>

namespace lims {

namespace {

/// @brief The installed port; a function-local static, so it exists before any translation unit's first use.
[[nodiscard]] LedgerPort& installed() {
    static LedgerPort port;
    return port;
}

}  // namespace

void setLedgerPort(LedgerPort port) { installed() = std::move(port); }

LedgerPort const& ledgerPort() { return installed(); }

}  // namespace lims
