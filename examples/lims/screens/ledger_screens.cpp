// SPDX-License-Identifier: Apache-2.0

#include "ledger_screens.hpp"

namespace lims::screens {

void addLedgerScreens(morph::ui::author::ScreenRegistry& screens, std::vector<ListedScreen>& listed) {
    static_cast<void>(screens);
    static_cast<void>(listed);
}

std::vector<std::string_view> ledgerScreenIds() { return {}; }

}  // namespace lims::screens
