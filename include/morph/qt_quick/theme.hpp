// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <string>
#include <vector>

#include "../ui/theme.hpp"

class QQmlEngine;

/// @file
/// @brief `applyTheme`: the `ui-theme/1` token values set on an engine's `MorphUi` `Theme` singleton, which every
///        built-in component and every host component in that engine reads.
///
/// Specified in `docs/spec/qt_quick/backend.md`, "Layout and theme".

namespace morph::qt_quick {

/// @brief Sets a theme's values on @p engine's `Theme` singleton.
///
/// Every `MorphUi` component binds its colours to `Theme`, and a host component that imports `MorphUi` reads the same
/// instance, so both update. A path of the set the theme lacks has its default in @p values (`ui::themeValues`), which
/// is the `Theme` property's own default, so applying `ui::themeValues()` restores the defaults. Call it on the
/// engine's thread, before or after screens mount.
/// @param engine The engine the backend compiles its components in.
/// @param values The values, from `ui::themeValues`.
/// @return The `Theme` properties that could not be set; empty when every one was.
std::vector<std::string> applyTheme(QQmlEngine& engine, ui::ThemeValues const& values);

}  // namespace morph::qt_quick
