// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <string_view>
#include <vector>

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

/// @brief One screen of the bundle, as its module lists it.
struct ListedScreen {
    /// @brief The screen's id.
    std::string id;
    /// @brief The navigator's module: `org`, `lab`, `billing`, `ledger`, `inventory` or `shell`; empty for a screen
    ///        that belongs to none.
    std::string module;
    /// @brief The title: a string, or a catalogue reference.
    morph::ui::author::Handle<std::string> title;
    /// @brief The screen as the registry holds it, or the refusal.
    std::expected<morph::ui::author::RegisteredScreen const*, morph::ui::Refusal> registered;
    /// @brief Whether the client may fetch the screen before sign-in.
    bool isPublic = false;
};

/// @brief The ids of every screen of the bundle, in the order `bundle()` registers them.
/// @return The ids.
[[nodiscard]] std::vector<std::string> allScreenIds();

/// @brief Adds the message catalogues to a bundle: per locale, the module files merged into one `i18n/<locale>.json`.
/// @param served The bundle under construction.
/// @return Nothing, or the refusal: a key found in two module files, a file that is not a flat object of messages, or
///         the catalogue's own validation.
[[nodiscard]] std::expected<void, morph::ui::Refusal> addCatalogues(morph::ui::UiRegistry& served);

/// @brief Adds the application document to a bundle: the day and the date-and-time patterns every screen's `date`
///        function writes (`09-Nov-2026`, `09-Nov-2026 14:30`), which the mockups show on screen.
/// @param served The bundle under construction.
/// @return Nothing, or the refusal of the document.
[[nodiscard]] std::expected<void, morph::ui::Refusal> addApplicationDocument(morph::ui::UiRegistry& served);

/// @brief Prepares lims to run in process: installs a token issuer with a development secret, which `LimsLogin` mints
///        its tokens with and a server installs at start-up, and, in a lab with no supervisor yet, makes @p user
///        Supervisor at the seeded company through the bootstrap `AssignRole`, so a fresh local client can register
///        samples with no other setup. With `LIMS_SCENARIO=1` in the environment it appoints `anna` the first
///        supervisor instead and writes the mockups' scenario (`seedScenario`), so the people the scenario names
///        hold their roles.
/// @param user The local principal, from `--user`; empty to appoint nobody.
void prepareLocal(std::string_view user);

/// @brief The application's bundle: every lims screen, registered and validated.
/// @return The bundle, or the refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle();

}  // namespace lims::screens
