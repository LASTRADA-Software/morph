// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

/// @file
/// @brief `morph::ui::ButtonStyle`: whether a button is drawn as a push button or as an entry of a list. The view
///        tree, the authoring API and the backends share it. Specified in `docs/spec/ui/backend_contract.md`.

namespace morph::ui {

/// @brief How a button is drawn; it behaves the same either way.
enum class ButtonStyle : std::uint8_t {
    Push,  ///< A framed button as wide as its label.
    Item,  ///< An entry of a list: as wide as its line, its label at the start, unframed, and marked while selected.
};

}  // namespace morph::ui
