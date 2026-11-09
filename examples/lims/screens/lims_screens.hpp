// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <morph/ui/service.hpp>
#include <string_view>

/// @file
/// lims' screens as UI documents (spec 4 §5): registration, the sample lifecycle and result entry, served by the
/// lims server and linked in-process by the generic client's local mode. Toolkit-free: this target links morph and
/// lims' models only.

namespace lims::screens {

/// @brief The application's id, as `ui-hello` names it.
inline constexpr std::string_view kApplicationId = "lims";

/// @brief Registers a client and a sample, then opens the sample's lifecycle.
inline constexpr std::string_view kRegister = "lims.register";

/// @brief One sample's lifecycle: its state and version, and the transitions its state allows.
inline constexpr std::string_view kLifecycle = "lims.lifecycle";

/// @brief Result entry for one sample: capture, the results, verification and the offline conflicts.
inline constexpr std::string_view kResults = "lims.results";

/// @brief Sample entry: a private working copy of one sample (or a new one), edited section by section and saved.
inline constexpr std::string_view kSample = "lims.sample";

/// @brief Ignition tests: one section per test, each stored as it changes, so a chat's change applies at once.
inline constexpr std::string_view kIgnition = "lims.ignition";

/// @brief Companies and their sites: the lists, and a form to create each.
inline constexpr std::string_view kCompanies = "lims.org.companies";

/// @brief Users and their role assignments: the lists, a form to assign a role, and a revoke per assignment.
inline constexpr std::string_view kAccess = "lims.org.access";

/// @brief Sign-in, the one screen served before it: a successful login opens sample registration. In process the
///        client signs in its `--user` itself, so this is the server's way in.
inline constexpr std::string_view kLogin = "lims.login";

/// @brief Prepares lims to run in process: installs a token issuer with a development secret, which `LimsLogin` mints
///        its tokens with and a server installs at start-up, and, in a lab with no supervisor yet, makes @p user
///        Supervisor at the seeded company through the bootstrap `AssignRole`, so a fresh local client can register
///        samples with no other setup.
/// @param user The local principal, from `--user`; empty to appoint nobody.
void prepareLocal(std::string_view user);

/// @brief The application's bundle: every lims screen, registered and validated.
/// @return The bundle, or the refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle();

}  // namespace lims::screens
