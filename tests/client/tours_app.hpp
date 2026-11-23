// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <morph/ui/service.hpp>
#include <morph/ui/tour_registry.hpp>
#include <string>

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

/// @brief A runtime tour registry over `toursBundle()`, its model registered on the process's default dispatcher and
///        registry, where an in-process source's and a local backend's model calls run. ctest runs each case in a
///        process of its own, so the registration reaches no other case.
/// @return The registry.
[[nodiscard]] std::shared_ptr<morph::ui::TourRegistry> toursRegistry();

/// @brief A general runtime tour @p id with one step titled @p stepTitle, and a second.
/// @param id        The tour's id.
/// @param stepTitle The first step's title.
/// @return Its JSON text.
[[nodiscard]] std::string runtimeTour(std::string const& id, std::string const& stepTitle = "News");

}  // namespace tours
