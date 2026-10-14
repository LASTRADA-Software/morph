// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <morph/ui/service.hpp>
#include <string_view>

/// @file
/// The forms demo's screens as UI documents (spec 4 §5): the lab's forms, the intake wizard and the samples
/// collection over `LabModel`, linked in process by the generic client's local mode. Toolkit-free: this target links
/// morph and the lab's headers only.

namespace lab::screens {

/// @brief The application's id, as `ui-hello` names it.
inline constexpr std::string_view kApplicationId = "forms";

/// @brief Computes a dry density from a mass and a volume.
inline constexpr std::string_view kDensity = "lab.density";

/// @brief Records a measurement against a sample.
inline constexpr std::string_view kMeasure = "lab.measure";

/// @brief Registers a sample and records its first measurement, as a wizard.
inline constexpr std::string_view kIntake = "lab.intake";

/// @brief Lists the samples, with their row and collection actions.
inline constexpr std::string_view kSamples = "lab.samples";

/// @brief The application's bundle: every lab screen, registered and validated.
/// @return The bundle, or the refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle();

}  // namespace lab::screens
