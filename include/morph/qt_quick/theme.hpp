// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <functional>
#include <map>
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
/// A surface named in @p surfaces gets every colour role from its values: the theme resolved with that surface's layer
/// (`ui::resolveTheme`), so a role the surface does not set is the theme's. A built-in surface (`page`, `card`,
/// `sidebar`, `toolbar`) the map does not name keeps its built-in colours.
/// @param engine   The engine the backend compiles its components in.
/// @param values   The values, from `ui::themeValues`.
/// @param surfaces The theme's surfaces by name, each resolved with its layer; empty keeps the built-in ones.
/// @return The `Theme` properties that could not be set, a surface's as `<name>.<role>`; empty when every one was.
std::vector<std::string> applyTheme(QQmlEngine& engine, ui::ThemeValues const& values,
                                    std::map<std::string, ui::ThemeValues, std::less<>> const& surfaces = {});

/// @brief Applies a bundle's theme to @p engine's `Theme`: the theme index's base, each of its surfaces resolved
///        with that surface's layer, and its icon set. A bundle with no theme index applies the defaults and no icon
///        set, so a client that opens a second application does not keep the first one's look.
/// @param engine The engine the backend compiles its components in.
/// @param themes The bundle's theme files by id (`ui::Bundle::themes`).
/// @param icons  The bundle's icon set, by icon name, as SVG text (`ui::Bundle::icons`); empty declares none.
/// @return Why the theme could not be applied, or the `Theme` properties that could not be set; empty when it was
///         applied whole.
std::vector<std::string> applyBundleTheme(QQmlEngine& engine,
                                          std::map<std::string, std::string, std::less<>> const& themes,
                                          std::map<std::string, std::string, std::less<>> const& icons = {});

}  // namespace morph::qt_quick
