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

/// @brief The optional param every screen reads its width class from before `{"env": "widthClass"}`: `compact`,
///        `medium` or `expanded`. Tests and the render smoke set it to mount a screen at a given width.
inline constexpr std::string_view kWidthParam = "width";

/// @brief The application's bundle: every built design, the index, and the green theme.
/// @return The bundle, or the refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle();

/// @brief The gallery as a linked application, for a local-mode client.
/// @return It.
[[nodiscard]] morph::client::LocalApp localApp();

}  // namespace lims_gallery
