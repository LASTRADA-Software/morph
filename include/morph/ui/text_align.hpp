// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>

/// @file
/// @brief `morph::ui::TextAlign`: where a text places each of its lines within its own width. The view tree, the
///        authoring API and the backends share it. Specified in `docs/spec/ui/backend_contract.md`, "Alignment".

namespace morph::ui {

/// @brief Where a `Text` places each of its lines within its own width.
enum class TextAlign : std::uint8_t {
    Start,   ///< At the leading edge: left, or right under a right-to-left layout.
    Center,  ///< Centred.
    End,     ///< At the trailing edge: right, or left under a right-to-left layout.
};

/// @brief A text's alignment by name, as the loader checked it: `start`, `center` or `end`.
/// @param name The name; empty reads as the leading edge.
/// @return The alignment.
[[nodiscard]] inline TextAlign textAlignNamed(std::string_view name) {
    if (name == "center") {
        return TextAlign::Center;
    }
    return name == "end" ? TextAlign::End : TextAlign::Start;
}

}  // namespace morph::ui
