// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <morph/client/source.hpp>
#include <morph/ui/service.hpp>
#include <string_view>

/// @file
/// The LIMS gallery: ten laboratory screens drawn from design mockups, written as ui/1 documents to find what the
/// declarative UI can and cannot express. It has no backend: each screen keeps its sample records in its own state.

namespace lims_gallery {

/// @brief The application's id, as `ui-hello` names it and `--app` selects it.
inline constexpr std::string_view kApplicationId = "lims-gallery";

/// @brief The index screen's id: the list of designs.
inline constexpr std::string_view kIndexScreen = "lims-gallery.index";

/// @brief The form every date of the gallery is written and read in: day, month name, year, which a regulated lab
///        cannot misread across locales.
inline constexpr std::string_view kDatePattern = "dd-MMM-yyyy";

/// @brief The form every date and time of the gallery is written and read in.
inline constexpr std::string_view kDateTimePattern = "dd-MMM-yyyy HH:mm";

/// @brief The gallery's app document: the date patterns, which a client that mounts it gives every screen, and the
///        counts the navigator shows on the review and instrument entries.
/// @return Its text.
[[nodiscard]] std::string appDocument();

/// @brief The application's bundle: every built design, the index, the app document and the green theme.
/// @return The bundle, or the refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle();

/// @brief The gallery as a linked application, for a local-mode client.
/// @return It.
[[nodiscard]] morph::client::LocalApp localApp();

}  // namespace lims_gallery
