// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

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
    /// @brief A destructive action that is not the main one: outlined, its label in the error colour, so the
    ///        screen's filled action stays the one that stands out.
    DangerOutline,
};

/// @brief Every emphasis by its document name, `normal` first.
inline constexpr std::array<std::pair<std::string_view, ButtonEmphasis>, 5> kButtonEmphases = {{
    {"normal", ButtonEmphasis::Normal},
    {"primary", ButtonEmphasis::Primary},
    {"danger", ButtonEmphasis::Danger},
    {"ghost", ButtonEmphasis::Ghost},
    {"dangerOutline", ButtonEmphasis::DangerOutline},
}};

/// @brief An emphasis by its document name.
/// @param name The name, such as `dangerOutline`.
/// @return The emphasis; `nullopt` for a name that is none.
[[nodiscard]] constexpr std::optional<ButtonEmphasis> emphasisNamed(std::string_view name) noexcept {
    for (auto const& [known, emphasis] : kButtonEmphases) {
        if (known == name) {
            return emphasis;
        }
    }
    return std::nullopt;
}

/// @brief An emphasis's document name.
/// @param emphasis The emphasis.
/// @return Its name, such as `dangerOutline`.
[[nodiscard]] constexpr std::string_view emphasisName(ButtonEmphasis emphasis) noexcept {
    for (auto const& [name, known] : kButtonEmphases) {
        if (known == emphasis) {
            return name;
        }
    }
    return "normal";
}

}  // namespace morph::ui
