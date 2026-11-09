// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/author.hpp>

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

}  // namespace lims::screens
