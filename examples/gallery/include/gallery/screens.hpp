// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/author.hpp>

/// @file
/// The gallery's screen as the C++ builders write it. Apart from `gallery.hpp` because the builders are a server
/// header that a Qt translation unit cannot include: Qt's `emit` keyword macro collides with `Screen::emit`.

namespace gallery {

/// @brief The gallery screen.
/// @return The screen.
[[nodiscard]] morph::ui::author::Screen homeScreen();

}  // namespace gallery
