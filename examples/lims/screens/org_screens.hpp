// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/author.hpp>
#include <string_view>
#include <vector>

#include "lims_screens.hpp"

/// @file
/// The org module's administration screens, registered by `lims::screens::bundle()`.

namespace lims::screens {

/// @brief Companies and sites: what the caller can see of each, and a form to create one.
/// @return The screen.
[[nodiscard]] morph::ui::author::Screen companiesScreen();

/// @brief Users and role assignments: what the caller can see of each, a form to assign a role, and a revoke per
///        assignment.
/// @return The screen.
[[nodiscard]] morph::ui::author::Screen accessScreen();

/// @brief Registers the org module's screens and lists them for the bundle.
/// @param screens The registry the screens are added to.
/// @param listed The list the bundle's catalog entries are built from; the module's screens are appended to it.
void addOrgScreens(morph::ui::author::ScreenRegistry& screens, std::vector<ListedScreen>& listed);

/// @brief The ids of the org module's screens, in the order `addOrgScreens` registers them.
/// @return The ids.
[[nodiscard]] std::vector<std::string_view> orgScreenIds();

}  // namespace lims::screens
