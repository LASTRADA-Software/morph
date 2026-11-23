// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <morph/ui/service.hpp>

/// @file
/// A two-screen application with guided tours, which the client's tour tests open: `tours.home` starts a tour from a
/// button, `tours.samples` carries the targets, and `tours.closed` is listed but not built.

namespace tours {

/// @brief The bundle: the two screens, the unbuilt entry, and an app document with five tours.
/// @return It, or the registry's refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> toursBundle();

/// @brief The same screens with an app document whose only tours belong to `tours.samples`.
/// @return It, or the registry's refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> screenToursBundle();

/// @brief The same screens with no app document, so no tours.
/// @return It, or the registry's refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> noToursBundle();

}  // namespace tours
