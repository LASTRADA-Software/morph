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

/// @brief The application's bundle: every lims screen, registered and validated.
/// @return The bundle, or the refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle();

}  // namespace lims::screens
