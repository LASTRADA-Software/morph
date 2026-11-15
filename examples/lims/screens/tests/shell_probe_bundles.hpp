// SPDX-License-Identifier: Apache-2.0
//
// The bundles the shell tests serve. They are built in their own translation unit so that the test that drives them
// need not include every module's models.
#pragma once

#include <expected>
#include <memory>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <string_view>

#include "lims_shell_parts.hpp"

namespace lims::shelltest {

/// @brief The screen id of the sign-in a probe bundle opens with.
inline constexpr std::string_view kProbeLogin = "test.login";

/// @brief The screen id of the first screen a signed-in person lands on.
inline constexpr std::string_view kProbeHome = "test.home";

/// @brief The screen id of the ledger probe.
inline constexpr std::string_view kScoped = "test.scoped";

/// @brief Adds a registered screen to @p served as an entry of @p module.
/// @param served The bundle under construction.
/// @param registered The screen as the registry holds it.
/// @param screenId The screen's id.
/// @param module The navigator's module.
/// @param isPublic Whether it opens before sign-in.
/// @return Nothing, or the refusal.
std::expected<void, morph::ui::Refusal> list(morph::ui::UiRegistry& served,
                                             morph::ui::author::RegisteredScreen const& registered,
                                             std::string_view screenId, std::string module, bool isPublic = false);

/// @brief The bundle's tail every probe shares: the application document, lims's two catalogues and the shell's parts.
/// @param served The bundle under construction.
/// @param available What the shell may declare parts for.
/// @return The bundle, or the refusal.
std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> finish(
    morph::ui::UiRegistry& served, lims::screens::ShellAvailability const& available);

/// @brief The bundle of one screen per module, a sign-in, and the shell declared over all of it.
/// @return The bundle, or the refusal.
std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> modulesBundle();

/// @brief A bundle of the one probe screen, with the shell declared over the ledger.
/// @param contextBound Whether the probe reads the context.
/// @return The bundle, or the refusal.
std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> scopedBundle(bool contextBound);

}  // namespace lims::shelltest
