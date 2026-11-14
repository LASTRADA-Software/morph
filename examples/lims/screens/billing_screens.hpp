// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/author.hpp>
#include <string_view>
#include <vector>

#include "lims_screens.hpp"

/// @file
/// The billing module's screens, registered by `lims::screens::bundle()`.

namespace lims::screens {

/// @brief Registers the billing module's screens and lists them for the bundle.
/// @param screens The registry the screens are added to.
/// @param listed The list the bundle's catalog entries are built from; the module's screens are appended to it.
void addBillingScreens(morph::ui::author::ScreenRegistry& screens, std::vector<ListedScreen>& listed);

/// @brief The ids of the billing module's screens, in the order `addBillingScreens` registers them.
/// @return The ids.
[[nodiscard]] std::vector<std::string_view> billingScreenIds();

}  // namespace lims::screens
