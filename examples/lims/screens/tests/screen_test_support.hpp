// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_message.hpp>
#include <morph/client/source.hpp>
#include <string>
#include <string_view>
#include <testkit/screen_harness.hpp>

#include "lims_screens.hpp"

/// @file
/// What every lims screen test starts from: the application, the problems a screen reported, and a widget's property.

namespace lims::screens::test {

/// @brief lims in local mode, as the generic client opens it.
/// @return The application over `lims::screens::bundle()`.
[[nodiscard]] inline morph::client::LocalApp limsApp() {
    return morph::client::LocalApp{.name = "lims", .bundle = &lims::screens::bundle, .setUp = {}};
}

/// @brief Reports each problem the screen raised (a refused document, a mount error, an interpreter diagnostic), so
///        the assertion that follows prints them.
/// @param harness The harness the screen is mounted in.
inline void reportProblems(morph::examples::testing::ScreenHarness const& harness) {
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
}

/// @brief A property of the widget of @p kind with @p testId.
/// @param harness The harness the screen is mounted in.
/// @param kind The widget kind.
/// @param testId The widget's test id.
/// @param property The property's name.
/// @return The property as text; empty when no such widget is mounted.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the widget's kind, its test id, then the property.
[[nodiscard]] inline std::string prop(morph::examples::testing::ScreenHarness& harness, std::string_view kind,
                                      std::string_view testId, std::string_view property) {
    auto const found = harness.find(kind, testId);
    return found ? harness.backend().prop(*found, property) : std::string{};
}

}  // namespace lims::screens::test
