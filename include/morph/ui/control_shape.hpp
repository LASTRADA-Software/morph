// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

/// @file
/// @brief `morph::ui::ControlShape` and `morph::ui::ButtonSize`: the outline and the density of a button or a badge.
///        The view tree, the authoring API and the backends share them. Specified in
///        `docs/spec/ui/backend_contract.md`.

namespace morph::ui {

/// @brief The outline of a button or a badge.
enum class ControlShape : std::uint8_t {
    Default,  ///< The theme's corners.
    Round,    ///< Corners of half the height, and at least as wide as tall: a circle for a short label, a stadium for
              ///< a longer one.
    Square,   ///< A button for a glyph (`‹`, `⋯`): the theme's corners, no least width, little side padding, at least
              ///< as wide as tall and a strong caption at the body size. A longer caption gives a wider button. Only a
              ///< button has it; a badge does not.
};

/// @brief How much room a button gives its caption.
enum class ButtonSize : std::uint8_t {
    Normal,  ///< The control's padding and the body size.
    Small,   ///< A compact caption: the least side padding and the small type step, at the theme's `button.smallHeight`,
             ///< which may be above or below the control height; without it, the control height.
};

/// @brief Every shape by its document name, `default` first.
inline constexpr std::array<std::pair<std::string_view, ControlShape>, 3> kControlShapes = {{
    {"default", ControlShape::Default},
    {"round", ControlShape::Round},
    {"square", ControlShape::Square},
}};

/// @brief Every size by its document name, `normal` first.
inline constexpr std::array<std::pair<std::string_view, ButtonSize>, 2> kButtonSizes = {{
    {"normal", ButtonSize::Normal},
    {"small", ButtonSize::Small},
}};

/// @brief Every DTCG font weight name with its weight, in the order the standard lists them. A `weight` of a button
///        and a theme's `fontWeight` token read the same names.
inline constexpr std::array<std::pair<std::string_view, int>, 18> kWeightNames = {{
    {"thin", 100},
    {"hairline", 100},
    {"extra-light", 200},
    {"ultra-light", 200},
    {"light", 300},
    {"normal", 400},
    {"regular", 400},
    {"book", 400},
    {"medium", 500},
    {"semi-bold", 600},
    {"demi-bold", 600},
    {"bold", 700},
    {"extra-bold", 800},
    {"ultra-bold", 800},
    {"black", 900},
    {"heavy", 900},
    {"extra-black", 950},
    {"ultra-black", 950},
}};

/// @brief A font weight by its DTCG name.
/// @param name The name, such as `semi-bold`.
/// @return The weight, 100 to 950; `nullopt` for a name that is none.
[[nodiscard]] constexpr std::optional<int> weightNamed(std::string_view name) noexcept {
    for (auto const& [known, weight] : kWeightNames) {
        if (known == name) {
            return weight;
        }
    }
    return std::nullopt;
}

/// @brief A shape by its document name.
/// @param name The name, such as `round`.
/// @return The shape; `nullopt` for a name that is none.
[[nodiscard]] constexpr std::optional<ControlShape> shapeNamed(std::string_view name) noexcept {
    for (auto const& [known, shape] : kControlShapes) {
        if (known == name) {
            return shape;
        }
    }
    return std::nullopt;
}

/// @brief A shape's document name.
/// @param shape The shape.
/// @return Its name, such as `round`.
[[nodiscard]] constexpr std::string_view shapeName(ControlShape shape) noexcept {
    for (auto const& [name, known] : kControlShapes) {
        if (known == shape) {
            return name;
        }
    }
    return "default";
}

/// @brief A size by its document name.
/// @param name The name, such as `small`.
/// @return The size; `nullopt` for a name that is none.
[[nodiscard]] constexpr std::optional<ButtonSize> sizeNamed(std::string_view name) noexcept {
    for (auto const& [known, size] : kButtonSizes) {
        if (known == name) {
            return size;
        }
    }
    return std::nullopt;
}

/// @brief A size's document name.
/// @param size The size.
/// @return Its name, such as `small`.
[[nodiscard]] constexpr std::string_view sizeName(ButtonSize size) noexcept {
    for (auto const& [name, known] : kButtonSizes) {
        if (known == size) {
            return name;
        }
    }
    return "normal";
}

}  // namespace morph::ui
