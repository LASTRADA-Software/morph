// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <app/app_source.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <morph/ui/service.hpp>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// The gallery: one screen that uses every node kind the interpreter lowers, served by a small application, as a
/// visual check for renderer authors (spec 1 §8).

namespace gallery {

/// @brief One row the gallery's lists show.
struct Fact {
    /// @brief The row's key.
    std::int64_t id = 0;
    /// @brief What it is.
    std::string label;
    /// @brief Its value.
    std::string value;
};

/// @brief Lists the facts.
struct ListFacts {};

/// @brief The gallery's one model: a fixed list of facts, so a query crosses the bridge in every mode.
struct GalleryModel {
    /// @brief The facts.
    /// @param action The request.
    /// @return Three facts.
    [[nodiscard]] std::vector<Fact> execute(ListFacts const& action) const;
};

/// @brief The application's id, as `ui-hello` names it.
inline constexpr std::string_view kApplicationId = "gallery";

/// @brief The screen's id.
inline constexpr std::string_view kHomeScreen = "gallery.home";

/// @brief The node kinds of `ui/1` the gallery leaves out, because the interpreter does not lower them yet: the
///        forms engine's.
[[nodiscard]] std::vector<std::string_view> pendingKinds();

/// @brief The application's bundle: the gallery screen, registered and validated.
/// @return The bundle, or the refusal.
[[nodiscard]] std::expected<std::shared_ptr<morph::ui::ServedBundle const>, morph::ui::Refusal> bundle();

/// @brief The gallery as a linked application, for a local-mode client.
/// @return It.
[[nodiscard]] morph::examples::LocalApp localApp();

}  // namespace gallery
