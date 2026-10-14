// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <morph/ui/service.hpp>
#include <string_view>

/// @file
/// polls' screens as UI documents (spec 4 §5): creating a poll, and voting on one. Served by the polls server and
/// linked in process by the generic client's local mode. Toolkit-free: this target links morph and the rung's models
/// only.

namespace polls::screens {

/// @brief The application's id, as `ui-hello` names it.
inline constexpr std::string_view kApplicationId = "polls";

/// @brief Creates a poll and opens it.
inline constexpr std::string_view kCreate = "polls.create";

/// @brief One poll (`pollId`): its options and counts, the participant's votes, comments and the activity feed.
inline constexpr std::string_view kVote = "polls.vote";

/// @brief The application's bundle: every polls screen, registered and validated.
/// @return The bundle, or the refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle();

}  // namespace polls::screens
