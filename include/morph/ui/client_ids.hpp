// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <string_view>

#include "chat/panel_id.hpp"

/// @file
/// @brief The ids of the documents a client generates for itself, which no server document may take: host values and
///        commands are scoped by the id a mounted document carries. Specified in `docs/spec/ui/host_vocabulary.md`.

namespace morph::ui {

/// @brief The tour card's document id.
inline constexpr std::string_view kTourCardId = "morph.tour";

/// @brief A document id the client reserves, and whose it is.
struct ReservedClientId {
    /// @brief The id.
    std::string_view id;
    /// @brief What the client shows under it, as a refusal names it: "the chat panel".
    std::string_view owner;
};

/// @brief Every id the client reserves.
inline constexpr std::array<ReservedClientId, 2> kReservedClientIds = {{
    {.id = chat::kChatPanelId, .owner = "the chat panel"},
    {.id = kTourCardId, .owner = "the tour card"},
}};

/// @brief The reservation of an id, when it is one the client keeps for itself.
/// @param id The document or catalog entry id.
/// @return The reservation, or null for an id a server may use.
[[nodiscard]] constexpr ReservedClientId const* reservedClientId(std::string_view id) noexcept {
    // NOLINTNEXTLINE(readability-qualified-auto): an array iterator is a pointer on some standard libraries only.
    auto const found = std::ranges::find(kReservedClientIds, id, &ReservedClientId::id);
    return found == kReservedClientIds.end() ? nullptr : &*found;
}

}  // namespace morph::ui
