// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

/// @file
/// @brief `morph::ui::InputEmphasis`: how much a text input stands out. The view tree, the authoring API and the
///        backends share it. Specified in `docs/spec/ui/backend_contract.md`.

namespace morph::ui {

/// @brief How much a text input stands out; each backend maps it to its toolkit.
enum class InputEmphasis : std::uint8_t {
    Normal,     ///< An ordinary field.
    Prominent,  ///< The screen's focal field, such as a scanner's: larger and outlined in the accent.
};

}  // namespace morph::ui
