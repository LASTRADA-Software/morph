// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/author.hpp>

/// @file
/// The gallery's screen as the C++ builders write it. Apart from `gallery.hpp`, so a translation unit that needs only
/// the served bundle does not compile the builders.

namespace gallery {

/// @brief The gallery screen.
/// @return The screen.
[[nodiscard]] morph::ui::author::Screen homeScreen();

}  // namespace gallery
