// SPDX-License-Identifier: Apache-2.0
#include "lims/app/ledger_wiring.hpp"

#include <morph/core/effects.hpp>
#include <utility>

#include "lims/billing/models/invoice_model.hpp"
#include "lims/ledger/db/ledger_store.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/posting/ledger_port.hpp"

// In the shared-transaction mode `PostInvoice` writes the ledger's entry inside its own action, so a watcher of the
// ledger's entries is told only if the action says so. Billing cannot name the ledger's record; this unit, which
// composes both, can.
BRIDGE_ADD_EFFECTS(lims::PostInvoice, ::morph::effects::changes<lims::JournalEntryView>)

namespace lims::app {

void installLedgerPort(InternalClient& posting, PostingMode mode) {
    setLedgerPort(LedgerPort{
        .mode = mode,
        .post =
            [&posting](PostingRequest request) {
                return posting.dispatch<LedgerModel>(RecordPosting{.request = std::move(request)});
            },
        .postIn = [](Lightweight::DataMapper& mapper,
                     PostingRequest const& request) { return recordPostingIn(mapper, request); },
        .find =
            [&posting](PostingQuery query) {
                return posting.dispatch<LedgerModel>(FindPosting{.query = std::move(query)});
            },
        .rateOn = [&posting](RateQuery query) { return posting.dispatch<LedgerModel>(GetRateOn{.query = query}); },
    });
}

}  // namespace lims::app
