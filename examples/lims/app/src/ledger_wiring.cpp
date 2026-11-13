// SPDX-License-Identifier: Apache-2.0
#include "lims/app/ledger_wiring.hpp"

#include <utility>

#include "lims/ledger/db/ledger_store.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/posting/ledger_port.hpp"

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
