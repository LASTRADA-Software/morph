// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string_view>

/// @file
/// @brief The chat panel document's id, apart from the panel so a check that only needs the id does not include it.

namespace morph::ui::chat {

/// @brief The panel document's id.
inline constexpr std::string_view kChatPanelId = "morph.chat";

}  // namespace morph::ui::chat
