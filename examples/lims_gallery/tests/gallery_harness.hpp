// SPDX-License-Identifier: Apache-2.0
#pragma once

// First, ahead of every header that brings view.hpp: GCC's -Wshadow reports value.hpp's enumerator `Type::Key` when
// view.hpp's `ui::Key` was declared before it.
// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/screen_harness.hpp>

#include "lims_gallery/lims_gallery.hpp"

/// @file
/// Mounts one gallery screen at a width class, on `RecordingBackend`, in local mode: what every design's test starts
/// from.

namespace lims_gallery::testing {

/// @brief The two width classes every design draws.
inline constexpr std::array<std::string_view, 2> kWidths{"compact", "expanded"};

/// @brief One screen of the gallery, mounted at a width class and driven by test id.
class GalleryScreen : public morph::examples::testing::ScreenHarness {
public:
    /// @brief Mounts @p screenId as the client would at @p width.
    /// @param screenId The screen's id.
    /// @param width    `compact`, `medium` or `expanded`.
    GalleryScreen(std::string_view screenId, std::string_view width)
        : ScreenHarness{localApp(), std::string{screenId}, morph::examples::testing::RigMode::Local,
                        morph::ui::ParamMap{{std::string{kWidthParam}, morph::ui::Value{std::string{width}}}}} {}

    /// @brief Whether a widget of @p kind with @p testId is mounted and not hidden itself.
    /// @param kind   The widget kind, as `RecordingBackend` names it.
    /// @param testId The test id.
    /// @return Whether it is.
    [[nodiscard]] bool shows(std::string_view kind, std::string_view testId) {
        auto const found = find(kind, testId);
        return found.has_value() && backend().prop(*found, "visible") != "false";
    }

    /// @brief A property of the widget of @p kind with @p testId.
    /// @param kind   The widget kind.
    /// @param testId The test id.
    /// @param name   The property.
    /// @return Its formatted value, or nothing when no such widget is mounted.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the widget's kind, its test id, then the property.
    [[nodiscard]] std::optional<std::string> prop(std::string_view kind, std::string_view testId,
                                                  std::string_view name) {
        auto const found = find(kind, testId);
        if (!found) {
            return std::nullopt;
        }
        return backend().prop(*found, name);
    }

    /// @brief Fails the test, listing them, when the screen reported a problem.
    void requireNoProblems() const {
        for (std::string const& problem : problems()) {
            UNSCOPED_INFO(problem);
        }
        CHECK(problems().empty());
    }
};

}  // namespace lims_gallery::testing
