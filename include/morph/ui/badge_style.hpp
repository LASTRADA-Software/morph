// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

/// @file
/// @brief `morph::ui::BadgeStyle`: whether a badge is filled or outlined. The view tree, the authoring API and the
///        backends share it. Specified in `docs/spec/ui/backend_contract.md`.

namespace morph::ui {

/// @brief How a `Badge` is drawn.
enum class BadgeStyle : std::uint8_t {
    Filled,   ///< The tone's fill, its border and its colour for the text.
    Outline,  ///< The surface's fill, with the tone's colour as border and text.
};

}  // namespace morph::ui
