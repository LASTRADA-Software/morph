// SPDX-License-Identifier: Apache-2.0

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QFontDatabase>
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

#include "qt_quick/context.hpp"

namespace morph::qt_quick {

namespace {

/// The `Theme` property each colour token of the set sets.
constexpr std::array<std::pair<std::string_view, char const*>, 38> kColorProperties = {{
    {"color.text", "text"},
    {"color.muted", "muted"},
    {"color.err", "error"},
    {"color.ok", "success"},
    {"color.info", "info"},
    {"color.warn", "warn"},
    {"color.neutral", "neutral"},
    {"color.neutral.bg", "neutralBg"},
    {"color.neutral.border", "neutralBorder"},
    {"color.neutral.dot", "neutralDot"},
    {"color.hover", "hover"},
    {"color.track", "track"},
    {"color.controlSurface", "controlSurface"},
    {"color.controlBorder", "controlBorder"},
    {"color.divider", "divider"},
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
    {"color.table.header", "tableHeader"},
    {"color.chart.1", "chart1"},
    {"color.chart.2", "chart2"},
    {"color.chart.3", "chart3"},
    {"color.chart.4", "chart4"},
    {"color.scrim", "scrim"},
}};

/// The scheme property each colour token of a surface sets.
constexpr std::array<std::pair<std::string_view, char const*>, 37> kSchemeProperties = {{
    {"color.surface", "surface"},
    {"color.text", "text"},
    {"color.muted", "muted"},
    {"color.accent", "accent"},
    {"color.focus", "focus"},
    {"color.selection", "selection"},
    {"color.border", "border"},
    {"color.neutral", "neutral"},
    {"color.neutral.bg", "neutralBg"},
    {"color.neutral.border", "neutralBorder"},
    {"color.neutral.dot", "neutralDot"},
    {"color.hover", "hover"},
    {"color.track", "track"},
    {"color.controlSurface", "controlSurface"},
    {"color.controlBorder", "controlBorder"},
    {"color.divider", "divider"},
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
    {"color.table.header", "tableHeader"},
    {"color.chart.1", "chart1"},
    {"color.chart.2", "chart2"},
    {"color.chart.3", "chart3"},
    {"color.chart.4", "chart4"},
}};

/// The `Theme` property each dimension token of the set sets, besides the spacing scale.
constexpr std::array<std::pair<std::string_view, char const*>, 13> kDimensionProperties = {{
    {"radius.control", "radiusControl"},
    {"radius.card", "radiusCard"},
    {"radius.badge", "radiusBadge"},
    {"control.height", "controlHeight"},
    {"control.touchHeight", "touchHeight"},
    {"font.size.body", "bodySize"},
    {"font.size.heading", "headingSize"},
    {"font.size.small", "smallSize"},
    {"font.size.caption", "captionSize"},
    {"font.size.mono", "monoSize"},
    {"font.size.display", "displaySize"},
    {"table.cellPaddingX", "tableCellPaddingX"},
    {"table.cellPaddingY", "tableCellPaddingY"},
}};

// Whether a surface's outlines are stroked in dashes: `border.style`.
bool dashed(ui::ThemeValues const& values) {
    auto const found = values.strokes.find("border.style");
    return found != values.strokes.end() && found->second == "dashed";
}

// Whether the keyword at @p path names @p name.
bool keywordIs(ui::ThemeValues const& values, std::string_view path, std::string_view name) {
    auto const found = values.keywords.find(path);
    return found != values.keywords.end() && found->second == name;
}

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
    if (!theme.setProperty("borderDashed", dashed(values))) {
        unset.emplace_back("borderDashed");
    }
    if (!theme.setProperty("iconOnlySquare", keywordIs(values, "shape.iconOnly", "square"))) {
        unset.emplace_back("iconOnlySquare");
    }
    if (!theme.setProperty("collapsibleMarker", !keywordIs(values, "collapsible.marker", "none"))) {
        unset.emplace_back("collapsibleMarker");
    }
    if (!theme.setProperty("overlineCaps", keywordIs(values, "font.overline.case", "uppercase"))) {
        unset.emplace_back("overlineCaps");
    }
    if (auto const weight = values.numbers.find("font.weight.strong");
        weight != values.numbers.end() && !theme.setProperty("strongWeight", static_cast<int>(weight->second))) {
        unset.emplace_back("strongWeight");
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

// The families of a family token (`font.family`, `font.mono`) onto @p property; one Qt resolves to none of them falls
// back to Qt's own substitute and is reported, so a theme naming a font the machine lacks says so rather than drawing
// in another.
void setFamilies(QObject& theme, ui::ThemeValues const& values, std::string_view path, char const* property,
                 std::vector<std::string>& unset) {
    auto const found = values.families.find(path);
    if (found == values.families.end()) {
        return;
    }
    QStringList names;
    std::string listed;
    for (std::string const& name : found->second) {
        names.append(QString::fromStdString(name));
        listed += (listed.empty() ? "" : ", ") + name;
    }
    if (!theme.setProperty(property, QVariant::fromValue(names))) {
        unset.emplace_back(property);
        return;
    }
    if (names.isEmpty()) {
        return;
    }
    QFont font;
    font.setFamilies(names);
    if (!names.contains(QFontInfo{font}.family(), Qt::CaseInsensitive)) {
        unset.push_back(std::string{path} + ": none of " + listed + " is installed");
    }
}

// A shadow token onto the five `Theme` properties that begin with @p prefix.
void setShadow(QObject& theme, ui::ThemeValues const& values, std::string_view path, std::string_view prefix,
               std::vector<std::string>& unset) {
    auto const found = values.shadows.find(path);
    if (found == values.shadows.end()) {
        return;
    }
    ui::ThemeShadow const& shadow = found->second;
    std::array<std::pair<char const*, QVariant>, 5> const parts{{
        {"ShadowColor", QColor{shadow.color.r, shadow.color.g, shadow.color.b, shadow.color.a}},
        {"ShadowOffsetX", shadow.offsetX},
        {"ShadowOffsetY", shadow.offsetY},
        {"ShadowBlur", shadow.blur},
        {"ShadowSpread", shadow.spread},
    }};
    for (auto const& [suffix, value] : parts) {
        std::string const property = std::string{prefix} + suffix;
        if (!theme.setProperty(property.c_str(), value)) {
            unset.push_back(property);
        }
    }
}

// The card's and the popup's shadows and the spacing steps, which only a complete set of steps replaces.
void setShadowAndSpace(QObject& theme, ui::ThemeValues const& values, std::vector<std::string>& unset) {
    setShadow(theme, values, "elevation.card", "card", unset);
    setShadow(theme, values, "elevation.popup", "popup", unset);
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

// Each surface's colours, and how it strokes an outline, onto the scheme the theme keeps for it.
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
        if (!scheme->setProperty("borderDashed", dashed(surface))) {
            unset.push_back(name + ".borderDashed");
        }
    }
}

}  // namespace

namespace detail {

bool writePlatformMonoFamily(QObject& theme) {
    return theme.setProperty("platformMonoFamily", QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
}

}  // namespace detail

std::vector<std::string> applyTheme(QQmlEngine& engine, ui::ThemeValues const& values,
                                    std::map<std::string, ui::ThemeValues, std::less<>> const& surfaces) {
    std::vector<std::string> unset;
    auto* const theme = engine.singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    if (theme == nullptr) {
        unset.emplace_back("Theme");
        return unset;
    }
    setPlainTokens(*theme, values, unset);
    setFamilies(*theme, values, "font.family", "fontFamilies", unset);
    setFamilies(*theme, values, "font.mono", "monoFontFamilies", unset);
    if (!detail::writePlatformMonoFamily(*theme)) {
        unset.emplace_back("platformMonoFamily");
    }
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
    std::vector<std::string> problems = applyTheme(engine, ui::themeValues(*base), surfaces);
    for (std::string const& problem : ui::themeProblems(*index, lookup)) {
        problems.push_back("theme: " + problem);
    }
    return problems;
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
