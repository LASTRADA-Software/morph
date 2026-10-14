// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <morph/ui/service.hpp>
#include <string_view>

/// @file
/// kanban's screens as UI documents (spec 4 §5): sign-in and the person's projects, and a project's board of
/// swimlanes by columns, whose cards move by drag and drop. Served by the kanban server and linked in process by the
/// generic client's local mode. Toolkit-free: this target links morph and the rung's models only.

namespace kanban::screens {

/// @brief The application's id, as `ui-hello` names it.
inline constexpr std::string_view kApplicationId = "kanban";

/// @brief Sign-in, the one screen served before it: a successful login opens the projects.
inline constexpr std::string_view kLogin = "kanban.login";

/// @brief The signed-in person's projects, and creating one.
inline constexpr std::string_view kProjects = "kanban.projects";

/// @brief One project's board (`projectId`).
inline constexpr std::string_view kBoard = "kanban.board";

/// @brief Prepares kanban to run in process: installs a token issuer with a development secret, which `Login` mints
///        its tokens with and a server installs at start-up.
void prepareLocal();

/// @brief The application's bundle: every kanban screen, registered and validated.
/// @return The bundle, or the refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle();

}  // namespace kanban::screens
