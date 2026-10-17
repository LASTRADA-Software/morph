// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <morph/ui/service.hpp>
#include <string_view>

/// @file
/// bookmarks' screens as UI documents (spec 4 §5): a public sign-in screen, then the bookmark forms over two models,
/// the bookmark list with a bulk archive, and the tags. Served by the bookmarks server and linked in process by the
/// generic client's local mode. Toolkit-free: this target links morph and the rung's models only.

namespace bookmarks::screens {

/// @brief The application's id, as `ui-hello` names it.
inline constexpr std::string_view kApplicationId = "bookmarks";

/// @brief Everything once signed in, in one screen: the forms, the list and the tags.
inline constexpr std::string_view kBookmarks = "bookmarks.main";

/// @brief Sign-in, the one screen served before it: a successful login opens the bookmarks.
inline constexpr std::string_view kLogin = "bookmarks.login";

/// The shared feed: every user's shared bookmarks, newest first.
inline constexpr std::string_view kShared = "bookmarks.shared";

/// The tags, with renaming and merging.
inline constexpr std::string_view kTags = "bookmarks.tags";

/// Import and export of a Netscape bookmarks file.
inline constexpr std::string_view kImport = "bookmarks.import";

/// @brief Prepares bookmarks to run in process: installs a token issuer with a development secret, which `Login`
///        mints its tokens with and a server installs at start-up.
void prepareLocal();

/// @brief The application's bundle: the bookmarks screen, registered and validated.
/// @return The bundle, or the refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle();

}  // namespace bookmarks::screens
