// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

/// @file
/// @brief `morph::ui::BannerActionStyle`: how a banner draws its action. The view tree, the authoring API and the
///        backends share it. Specified in `docs/spec/ui/backend_contract.md`.

namespace morph::ui {

/// @brief How a banner draws its action.
enum class BannerActionStyle : std::uint8_t {
    Ghost,   ///< An unframed button at the trailing edge, its label in the accent.
    Button,  ///< A framed, normal button at the trailing edge.
    Link,    ///< The action's text inside the message, underlined in the accent, wrapping with the sentence.
};

/// @brief Every style by its document name, `ghost` first.
inline constexpr std::array<std::pair<std::string_view, BannerActionStyle>, 3> kBannerActionStyles = {{
    {"ghost", BannerActionStyle::Ghost},
    {"button", BannerActionStyle::Button},
    {"link", BannerActionStyle::Link},
}};

/// @brief A style by its document name.
/// @param name The name, such as `link`.
/// @return The style; `nullopt` for a name that is none.
[[nodiscard]] constexpr std::optional<BannerActionStyle> bannerActionStyleNamed(std::string_view name) noexcept {
    for (auto const& [known, style] : kBannerActionStyles) {
        if (known == name) {
            return style;
        }
    }
    return std::nullopt;
}

/// @brief A style's document name.
/// @param style The style.
/// @return Its name, such as `link`.
[[nodiscard]] constexpr std::string_view bannerActionStyleName(BannerActionStyle style) noexcept {
    for (auto const& [name, known] : kBannerActionStyles) {
        if (known == style) {
            return name;
        }
    }
    return "ghost";
}

}  // namespace morph::ui
