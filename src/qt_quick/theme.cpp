// SPDX-License-Identifier: Apache-2.0

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QFontInfo>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>
#include <array>
#include <functional>
#include <map>
#include <morph/qt_quick/theme.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::qt_quick {

namespace {

/// The `Theme` property each colour token of the set sets.
constexpr std::array<std::pair<std::string_view, char const*>, 24> kColorProperties = {{
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
    {"color.ok.bg", "successBg"},
    {"color.ok.border", "successBorder"},
    {"color.ok.dot", "successDot"},
    {"color.warn.bg", "warnBg"},
    {"color.warn.border", "warnBorder"},
    {"color.warn.dot", "warnDot"},
    {"color.err.bg", "errorBg"},
    {"color.err.border", "errorBorder"},
    {"color.err.dot", "errorDot"},
    {"color.info.bg", "infoBg"},
    {"color.info.border", "infoBorder"},
    {"color.info.dot", "infoDot"},
}};

/// The scheme property each colour token of a surface sets.
constexpr std::array<std::pair<std::string_view, char const*>, 24> kSchemeProperties = {{
    {"color.surface", "surface"},
    {"color.text", "text"},
    {"color.muted", "muted"},
    {"color.accent", "accent"},
    {"color.focus", "focus"},
    {"color.selection", "selection"},
    {"color.border", "border"},
    {"color.neutral", "neutral"},
    {"color.err", "error"},
    {"color.ok", "success"},
    {"color.info", "info"},
    {"color.warn", "warn"},
    {"color.ok.bg", "successBg"},
    {"color.ok.border", "successBorder"},
    {"color.ok.dot", "successDot"},
    {"color.warn.bg", "warnBg"},
    {"color.warn.border", "warnBorder"},
    {"color.warn.dot", "warnDot"},
    {"color.err.bg", "errorBg"},
    {"color.err.border", "errorBorder"},
    {"color.err.dot", "errorDot"},
    {"color.info.bg", "infoBg"},
    {"color.info.border", "infoBorder"},
    {"color.info.dot", "infoDot"},
}};

/// The `Theme` property each dimension token of the set sets, besides the spacing scale.
constexpr std::array<std::pair<std::string_view, char const*>, 5> kDimensionProperties = {{
    {"radius.control", "radiusControl"},
    {"radius.card", "radiusCard"},
    {"control.height", "controlHeight"},
    {"font.size.body", "bodySize"},
    {"font.size.heading", "headingSize"},
}};

// The colours, the stale opacity, the background and the dimensions; each property that would not take its value
// is added to @p unset.
void setPlainTokens(QObject& theme, ui::ThemeValues const& values, std::vector<std::string>& unset) {
    for (auto const& [path, property] : kColorProperties) {
        auto const found = values.colors.find(path);
        if (found == values.colors.end()) {
            continue;
        }
        ui::Rgba const& color = found->second;
        if (!theme.setProperty(property, QColor{color.r, color.g, color.b, color.a})) {
            unset.emplace_back(property);
        }
    }
    if (auto const stale = values.numbers.find("opacity.stale");
        stale != values.numbers.end() && !theme.setProperty("staleOpacity", stale->second)) {
        unset.emplace_back("staleOpacity");
    }
    if (auto const background = values.colors.find("color.background"); background != values.colors.end()) {
        ui::Rgba const& color = background->second;
        if (!theme.setProperty("background", QColor{color.r, color.g, color.b, color.a})) {
            unset.emplace_back("background");
        }
    }
    for (auto const& [path, property] : kDimensionProperties) {
        auto const found = values.dimensions.find(path);
        if (found != values.dimensions.end() && !theme.setProperty(property, found->second)) {
            unset.emplace_back(property);
        }
    }
}

// The families of `font.family`; one Qt resolves to none of them falls back to Qt's own substitute and is reported, so
// a theme naming a font the machine lacks says so rather than drawing in another.
void setFamilies(QObject& theme, ui::ThemeValues const& values, std::vector<std::string>& unset) {
    auto const found = values.families.find("font.family");
    if (found == values.families.end()) {
        return;
    }
    QStringList names;
    std::string listed;
    for (std::string const& name : found->second) {
        names.append(QString::fromStdString(name));
        listed += (listed.empty() ? "" : ", ") + name;
    }
    if (!theme.setProperty("fontFamilies", QVariant::fromValue(names))) {
        unset.emplace_back("fontFamilies");
        return;
    }
    if (names.isEmpty()) {
        return;
    }
    QFont font;
    font.setFamilies(names);
    if (!names.contains(QFontInfo{font}.family(), Qt::CaseInsensitive)) {
        unset.push_back("font.family: none of " + listed + " is installed");
    }
}

// The card's shadow and the spacing steps, which only a complete set of steps replaces.
void setShadowAndSpace(QObject& theme, ui::ThemeValues const& values, std::vector<std::string>& unset) {
    if (auto const card = values.shadows.find("elevation.card"); card != values.shadows.end()) {
        ui::ThemeShadow const& shadow = card->second;
        std::array<std::pair<char const*, QVariant>, 5> const parts{{
            {"cardShadowColor", QColor{shadow.color.r, shadow.color.g, shadow.color.b, shadow.color.a}},
            {"cardShadowOffsetX", shadow.offsetX},
            {"cardShadowOffsetY", shadow.offsetY},
            {"cardShadowBlur", shadow.blur},
            {"cardShadowSpread", shadow.spread},
        }};
        for (auto const& [property, value] : parts) {
            if (!theme.setProperty(property, value)) {
                unset.emplace_back(property);
            }
        }
    }
    QList<qreal> space;
    for (int step = 0; step <= ui::kMaxSpacingStep; ++step) {
        auto const found = values.dimensions.find("space." + std::to_string(step));
        if (found == values.dimensions.end()) {
            break;
        }
        space.push_back(found->second);
    }
    if (space.size() == ui::kMaxSpacingStep + 1 && !theme.setProperty("space", QVariant::fromValue(space))) {
        unset.emplace_back("space");
    }
}

// Each surface's colours, onto the scheme the theme keeps for it.
void setSurfaces(QObject& theme, std::map<std::string, ui::ThemeValues, std::less<>> const& surfaces,
                 std::vector<std::string>& unset) {
    for (auto const& [name, surface] : surfaces) {
        QVariant made;
        QMetaObject::invokeMethod(&theme, "schemeObject", Qt::DirectConnection, qReturnArg(made),
                                  QVariant{QString::fromStdString(name)});
        auto* const scheme = made.value<QObject*>();
        if (scheme == nullptr) {
            unset.push_back(name);
            continue;
        }
        for (auto const& [path, property] : kSchemeProperties) {
            auto const found = surface.colors.find(path);
            if (found == surface.colors.end()) {
                continue;
            }
            ui::Rgba const& color = found->second;
            if (!scheme->setProperty(property, QColor{color.r, color.g, color.b, color.a})) {
                unset.push_back(name + "." + property);
            }
        }
    }
}

}  // namespace

std::vector<std::string> applyTheme(QQmlEngine& engine, ui::ThemeValues const& values,
                                    std::map<std::string, ui::ThemeValues, std::less<>> const& surfaces) {
    std::vector<std::string> unset;
    auto* const theme = engine.singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    if (theme == nullptr) {
        unset.emplace_back("Theme");
        return unset;
    }
    setPlainTokens(*theme, values, unset);
    setFamilies(*theme, values, unset);
    setShadowAndSpace(*theme, values, unset);
    setSurfaces(*theme, surfaces, unset);
    return unset;
}

namespace {

// The bundle's icon set on `Theme.icons`, each icon a `data:` URL of its SVG, and whether there is one.
std::vector<std::string> setIcons(QQmlEngine& engine, std::map<std::string, std::string, std::less<>> const& icons) {
    auto* const theme = engine.singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    if (theme == nullptr) {
        return {"Theme"};
    }
    QVariantMap urls;
    for (auto const& [name, svg] : icons) {
        urls.insert(QString::fromStdString(name), QStringLiteral("data:image/svg+xml;base64,") +
                                                      QString::fromLatin1(QByteArray::fromStdString(svg).toBase64()));
    }
    std::vector<std::string> unset;
    if (!theme->setProperty("icons", QVariant::fromValue(urls))) {
        unset.emplace_back("icons");
    }
    if (!theme->setProperty("iconSet", !icons.empty())) {
        unset.emplace_back("iconSet");
    }
    return unset;
}

// The theme index's base and surfaces, or the defaults without an index.
std::vector<std::string> applyIndex(QQmlEngine& engine,
                                    std::map<std::string, std::string, std::less<>> const& themes) {
    std::optional<ui::ThemeIndex> index;
    std::map<std::string, ui::TokenSet, std::less<>> files;
    for (auto const& [fileId, text] : themes) {
        if (ui::isThemeIndex(text)) {
            if (auto parsed = ui::parseThemeIndex(text)) {
                index = std::move(*parsed);
            } else {
                return {"theme index " + fileId + ": " + parsed.error()};
            }
        } else if (auto tokens = ui::parseTokens(text)) {
            files.insert_or_assign(fileId, std::move(*tokens));
        } else {
            return {"theme file " + fileId + ": " + tokens.error()};
        }
    }
    if (!index) {
        return applyTheme(engine, ui::themeValues());
    }
    ui::TokenFileLookup const lookup = [&files](std::string_view fileId) -> ui::TokenSet const* {
        auto const found = files.find(fileId);
        return found == files.end() ? nullptr : &found->second;
    };
    auto const base = ui::resolveTheme(*index, lookup, {}, {}, {});
    if (!base) {
        return {"theme: " + base.error()};
    }
    std::map<std::string, ui::ThemeValues, std::less<>> surfaces;
    for (auto const& [name, file] : index->surfaces) {
        static_cast<void>(file);
        auto const layered = ui::resolveTheme(*index, lookup, {}, {}, name);
        if (!layered) {
            return {"theme surface " + name + ": " + layered.error()};
        }
        surfaces.insert_or_assign(name, ui::themeValues(*layered));
    }
    return applyTheme(engine, ui::themeValues(*base), surfaces);
}

}  // namespace

std::vector<std::string> applyBundleTheme(
    QQmlEngine& engine,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the bundle's two maps, in its order.
    std::map<std::string, std::string, std::less<>> const& themes,
    std::map<std::string, std::string, std::less<>> const& icons) {
    std::vector<std::string> problems = setIcons(engine, icons);
    auto applied = applyIndex(engine, themes);
    problems.insert(problems.end(), applied.begin(), applied.end());
    return problems;
}

}  // namespace morph::qt_quick
