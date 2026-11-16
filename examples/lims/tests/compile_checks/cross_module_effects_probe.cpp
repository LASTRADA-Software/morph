// SPDX-License-Identifier: Apache-2.0
//
// Built twice, linked with every lims module. With LIMS_PROBE_DECLARES_LEDGER_EFFECT it adds, from the composing
// code, the declaration billing cannot make: PostInvoice changes the ledger's Ledger_Entry. Either way it reads the
// effects registry once and prints whether the configuration resolved; the added build also prints whether the
// action's effects hold both its own `updates` of the invoice and the added `changes` of the entry.

#include <algorithm>
#include <iostream>
#include <morph/core/effects.hpp>
#include <vector>

#include "lims/billing/models/invoice_model.hpp"
#include "lims/ledger/dto/entry_dto.hpp"

#ifdef LIMS_PROBE_DECLARES_LEDGER_EFFECT
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_ADD_EFFECTS(lims::PostInvoice, ::morph::effects::changes<lims::JournalEntryView>)
// NOLINTEND(misc-const-correctness)
#endif

int main() {
    try {
        static_cast<void>(::morph::effects::actionsHolding("Billing_Invoice"));
        std::cout << "effects resolved\n";
#ifdef LIMS_PROBE_DECLARES_LEDGER_EFFECT
        auto const effects =
            ::morph::effects::effectsOf("PostInvoice").value_or(std::vector<::morph::effects::Effect>{});
        bool const keepsOwn = std::ranges::contains(
            effects, ::morph::effects::Effect{.kind = ::morph::effects::Kind::Updates, .record = "Billing_Invoice"});
        bool const addsEntry = std::ranges::contains(
            effects, ::morph::effects::Effect{.kind = ::morph::effects::Kind::Changes, .record = "Ledger_Entry"});
        std::cout << "PostInvoice keeps its own effect: " << keepsOwn
                  << "\nPostInvoice adds Ledger_Entry: " << addsEntry << '\n';
        return keepsOwn && addsEntry ? 0 : 1;
#else
        return 0;
#endif
    } catch (::morph::model::EffectsRegistrationError const& error) {
        std::cout << error.what() << '\n';
        return 1;
    }
}
