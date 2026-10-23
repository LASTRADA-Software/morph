// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

/// @file
/// @brief `morph::ui::ButtonEmphasis`: how much a button stands out. The view tree, the authoring API and the backends
///        share it. Specified in `docs/spec/ui/backend_contract.md`.

namespace morph::ui {

/// @brief How much a button stands out; each backend maps it to its theme.
enum class ButtonEmphasis : std::uint8_t {
    Normal,   ///< An ordinary action.
    Primary,  ///< The screen's main action.
    Danger,   ///< An action that destroys or cannot be undone.
    Ghost,    ///< An action embedded in other content, with no chrome of its own.
};

}  // namespace morph::ui
