// SPDX-License-Identifier: Apache-2.0

#include "inventory_screens.hpp"

namespace lims::screens {

void addInventoryScreens(morph::ui::author::ScreenRegistry& screens, std::vector<ListedScreen>& listed) {
    static_cast<void>(screens);
    static_cast<void>(listed);
}

std::vector<std::string_view> inventoryScreenIds() { return {}; }

}  // namespace lims::screens
