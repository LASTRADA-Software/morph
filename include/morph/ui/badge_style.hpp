// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

/// @file
/// @brief `morph::ui::BadgeStyle`: whether a badge is filled, outlined or quiet. The view tree, the authoring API and
///        the backends share it. Specified in `docs/spec/ui/backend_contract.md`.

namespace morph::ui {

/// @brief How a `Badge` is drawn.
enum class BadgeStyle : std::uint8_t {
    Filled,   ///< The tone's fill, its border and its colour for the text.
    Outline,  ///< The surface's fill, with the tone's colour as border and text.
    Quiet,    ///< The surface's fill with the control border and the muted text colour, whatever the tone; the tone
              ///< colours only an icon. A low-key label, such as the routine priority beside an urgent one.
};

}  // namespace morph::ui
