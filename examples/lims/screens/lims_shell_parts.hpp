// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/ui/bundle.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/shell_parts.hpp>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// The application shell's declared parts: the scope switcher, a count per module, one search over the modules, the
/// approvals inbox and the signed-in person's menu.

namespace lims::screens {

/// @brief The screen the inbox button opens.
inline constexpr std::string_view kApprovals = "lims.approvals";

/// @brief What the catalog offers the shell to draw its parts against.
struct ShellAvailability {
    /// @brief The modules that list at least one screen; a module without a screen has no group to badge and no screen
    ///        for a hit to open, so it declares no count and no search.
    std::vector<std::string> modules;
    /// @brief Whether the inbox's screen is registered.
    bool approvals = false;
};

/// @brief The shell's parts, declared as data over lims's own models.
///
/// The scope switcher lists the companies the caller can see; the chosen company is every screen's context. Each
/// module that has screens contributes its own count (samples to verify, invoices in work, periods closing,
/// requisitions waiting) and its own search; the inbox sums the approvals waiting at the chosen company.
/// @param available The modules and screens the bundle registers.
/// @return The parts.
[[nodiscard]] morph::ui::ShellParts shellParts(ShellAvailability const& available);

/// @brief The catalog facts of the models and actions the shell's parts read.
///
/// No screen reads a module's count or search: the shell does, so the registry's catalog does not know them from the
/// screens. Passed to `UiRegistry::setShellParts` beside the parts, so the catalog holds them as it holds a screen's.
/// @return The lookups over the facts.
[[nodiscard]] morph::ui::Catalog shellReads();

}  // namespace lims::screens
