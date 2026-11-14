// SPDX-License-Identifier: Apache-2.0
//
// Built twice, linked with every lims module. With LIMS_PROBE_DECLARES_LEDGER_EFFECT it adds, from the composing
// code, the declaration billing cannot make: PostInvoice changes the ledger's Ledger_Entry. Either way it reads the
// effects registry once and prints whether the configuration resolved.

#include <iostream>
#include <morph/core/effects.hpp>

#include "lims/billing/models/invoice_model.hpp"
#include "lims/ledger/dto/entry_dto.hpp"

#ifdef LIMS_PROBE_DECLARES_LEDGER_EFFECT
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_EFFECTS(lims::PostInvoice, ::morph::effects::updates<lims::InvoiceView>,
                        ::morph::effects::changes<lims::JournalEntryView>)
// NOLINTEND(misc-const-correctness)
#endif

int main() {
    try {
        static_cast<void>(::morph::effects::actionsHolding("Billing_Invoice"));
        std::cout << "effects resolved\n";
        return 0;
    } catch (::morph::model::EffectsRegistrationError const& error) {
        std::cout << error.what() << '\n';
        return 1;
    }
}
