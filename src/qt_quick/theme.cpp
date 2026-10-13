// SPDX-License-Identifier: Apache-2.0

#include <QColor>
#include <QObject>
#include <QQmlEngine>
#include <QVariant>
#include <array>
#include <morph/qt_quick/theme.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::qt_quick {

namespace {

/// The `Theme` property each colour token of the set sets. The semantic `bg`, `border` and `dot` colours have no
/// property yet: no built-in component draws them.
constexpr std::array<std::pair<std::string_view, char const*>, 12> kColorProperties = {{
    {"color.text", "text"},
    {"color.muted", "muted"},
    {"color.err", "error"},
    {"color.ok", "success"},
    {"color.info", "info"},
    {"color.warn", "warn"},
    {"color.neutral", "neutral"},
    {"color.surface", "surface"},
    {"color.border", "border"},
    {"color.selection", "selection"},
    {"color.accent", "accent"},
    {"color.focus", "focus"},
}};

}  // namespace

std::vector<std::string> applyTheme(QQmlEngine& engine, ui::ThemeValues const& values) {
    std::vector<std::string> unset;
    auto* const theme = engine.singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    if (theme == nullptr) {
        unset.emplace_back("Theme");
        return unset;
    }
    for (auto const& [path, property] : kColorProperties) {
        auto const found = values.colors.find(path);
        if (found == values.colors.end()) {
            continue;
        }
        ui::Rgba const& color = found->second;
        if (!theme->setProperty(property, QColor{color.r, color.g, color.b, color.a})) {
            unset.emplace_back(property);
        }
    }
    if (auto const stale = values.numbers.find("opacity.stale");
        stale != values.numbers.end() && !theme->setProperty("staleOpacity", stale->second)) {
        unset.emplace_back("staleOpacity");
    }
    return unset;
}

}  // namespace morph::qt_quick
