// SPDX-License-Identifier: Apache-2.0

#include "billing_screens.hpp"

namespace lims::screens {

void addBillingScreens(morph::ui::author::ScreenRegistry& screens, std::vector<ListedScreen>& listed) {
    static_cast<void>(screens);
    static_cast<void>(listed);
}

std::vector<std::string_view> billingScreenIds() { return {}; }

}  // namespace lims::screens
