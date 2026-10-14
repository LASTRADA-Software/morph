// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <morph/ui/service.hpp>
#include <string_view>

/// @file
/// pastebin's screen as a UI document (spec 4 §5): create, list, open, edit and delete pastes. Served by the pastebin
/// server and linked in process by the generic client's local mode. Toolkit-free: this target links morph and the
/// rung's models only.

namespace pastebin::screens {

/// @brief The application's id, as `ui-hello` names it.
inline constexpr std::string_view kApplicationId = "pastebin";

/// @brief The pastes: a create form, the list, and the paste opened.
inline constexpr std::string_view kPastes = "pastebin.pastes";

/// @brief The application's bundle: the pastebin screen, registered and validated.
/// @return The bundle, or the refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle();

}  // namespace pastebin::screens
