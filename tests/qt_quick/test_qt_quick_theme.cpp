// SPDX-License-Identifier: Apache-2.0
//
// Theme tokens on Qt Quick (spec 6 §12 "Design package"): a token set reaches a generated screen's widget and a host
// component through the one `Theme` singleton, and a token the theme lacks takes the default.

#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QImage>
#include <QMetaObject>
#include <QMetaProperty>
#include <QObject>
#include <QPalette>
#include <QPointF>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QVariant>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <morph/qt_quick/backend.hpp>
#include <morph/qt_quick/components.hpp>
#include <morph/qt_quick/theme.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/shell_frame.hpp>
#include <morph/ui/theme.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "qt_quick/kinds.hpp"
#include "qt_quick_harness.hpp"

namespace ui = morph::ui;
using morph::qt_quick::Components;
using morph::qt_quick::CustomRequest;
using morph::qt_quick::testing::Scene;

namespace {

/// A theme that sets the text and accent colours and the stale opacity, and nothing else.
ui::ThemeValues brandTheme() {
    auto const tokens = ui::parseTokens(R"({"color": {"$type": "color",
        "text": {"$value": {"colorSpace": "srgb", "components": [0.2, 0.4, 0.6], "hex": "#336699"}},
        "accent": {"$value": "#aa0055"}},
      "opacity": {"stale": {"$type": "number", "$value": 0.3}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    return ui::themeValues(*resolved);
}

/// The colour of the label inside a text widget's item: the descendant whose `text` is @p text.
///
/// Walks the item tree rather than the QObject tree: a view's delegates, a tab button's content among them, are
/// QQuickItem children of the view but not QObject children of the widget.
QColor labelColor(QQuickItem const& item, QString const& text) {
    QList<QQuickItem const*> pending{&item};
    while (!pending.isEmpty()) {
        QQuickItem const* const node = pending.takeLast();
        if (node->property("text").toString() == text && node->property("color").isValid()) {
            return node->property("color").value<QColor>();
        }
        for (QQuickItem const* const child : node->childItems()) {
            pending.append(child);
        }
    }
    FAIL("no label with the text " << text.toStdString());
    return {};
}

/// The descendant item of @p item whose objectName is @p name, or null.
QQuickItem* namedItem(QQuickItem const& item, QString const& name) {
    QList<QQuickItem const*> pending{&item};
    while (!pending.isEmpty()) {
        QQuickItem const* const node = pending.takeLast();
        for (QQuickItem* const child : node->childItems()) {
            if (child->objectName() == name) {
                return child;
            }
            pending.append(child);
        }
    }
    return nullptr;
}

QUrl writeFile(QTemporaryDir const& dir, QString const& name, char const* text) {
    QString const path = dir.filePath(name);
    QFile file{path};
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(text);
    file.close();
    return QUrl::fromLocalFile(path);
}

}  // namespace

TEST_CASE("qt_quick theme: a token set reaches a generated screen's widget and follows a later theme", "[qt_quick]") {
    Scene scene;
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), brandTheme()).empty());
    ui::Mounted const mounted{scene.runtime(), scene.backend(),
                              ui::text({.text = "Samples", .common = {.testId = "title"}})};
    scene.settle();
    QQuickItem const& item = Scene::itemOf(mounted.root());
    CHECK(labelColor(item, QStringLiteral("Samples")) == QColor{"#336699"});

    // Applying the defaults again restores the MorphUi colour on the mounted widget.
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues()).empty());
    scene.settle();
    CHECK(labelColor(item, QStringLiteral("Samples")) == QColor{"#1d1d1f"});
}

// Mutation: build the runs' markup in C++ at setSpans (the mounted run keeps the colour it was mounted with).
TEST_CASE("qt_quick theme: a mounted toned run takes a later theme's colour", "[qt_quick]") {
    Scene scene;
    ui::Mounted const mounted{
        scene.runtime(), scene.backend(),
        ui::text({.spans = std::vector<ui::TextSpan>{{.text = "Past due "}, {.text = "7", .tone = ui::Tone::Err}},
                  .common = {.testId = "line"}})};
    scene.settle();
    QObject const& label = Scene::controlOf(mounted.root());
    auto const* const theme =
        scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    QString const before = theme->property("error").value<QColor>().name();
    CHECK(label.property("text").toString().contains(QStringLiteral("color:") + before));

    auto const tokens = ui::parseTokens(R"({"color": {"$type": "color", "err": {"$value": "#123456"}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved)).empty());
    scene.settle();
    QString const markup = label.property("text").toString();
    CHECK(markup.contains(QStringLiteral("color:#123456")));
    CHECK_FALSE(markup.contains(QStringLiteral("color:") + before));
}

TEST_CASE("qt_quick theme: a tab's label takes the scheme colour under every style", "[qt_quick]") {
    Scene scene;
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), brandTheme()).empty());
    ui::Mounted const mounted{scene.runtime(), scene.backend(),
                              ui::tabs({.tabs = {{.label = "First", .node = ui::text({.text = "one"})},
                                                 {.label = "Second", .node = ui::text({.text = "two"})}},
                                        .selected = std::size_t{0}})};
    scene.settle();
    QQuickItem const& item = Scene::itemOf(mounted.root());
    // The chosen tab reads in the text colour, over an accent underline; the others are muted.
    CHECK(labelColor(item, QStringLiteral("First")) == QColor{"#336699"});
    CHECK(labelColor(item, QStringLiteral("Second")) == QColor{"#6e6e73"});
}

TEST_CASE("qt_quick theme: a host component reads the same tokens; a missing token takes the default", "[qt_quick]") {
    Components components;
    QTemporaryDir const dir;
    REQUIRE(dir.isValid());
    components.addHost("Swatch", 1,
                       writeFile(dir, QStringLiteral("Swatch.qml"),
                                 "import QtQuick\nimport MorphUi\nItem {\n    property var host\n"
                                 "    property color text: Theme.text\n    property color accent: Theme.accent\n"
                                 "    property color muted: Theme.muted\n    property real stale: Theme.staleOpacity\n"
                                 "    implicitWidth: 10\n}\n"));
    Scene scene{morph::qt_quick::BackendOptions{.components = &components}};
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), brandTheme()).empty());
    auto const swatch = scene.backend().createCustom(nullptr, CustomRequest{.name = "Swatch", .props = {}});
    REQUIRE(swatch != nullptr);
    scene.settle();
    QQuickItem const& item = Scene::itemOf(*swatch);
    CHECK(item.property("text").value<QColor>() == QColor{"#336699"});
    CHECK(item.property("accent").value<QColor>() == QColor{"#aa0055"});
    CHECK(item.property("stale").toDouble() == 0.3);
    // The theme sets no muted colour: the MorphUi default stands.
    CHECK(item.property("muted").value<QColor>() == QColor{"#6e6e73"});
}

TEST_CASE("qt_quick theme: the shell's tokens reach Theme", "[qt_quick]") {
    Scene scene;
    auto const tokens = ui::parseTokens(R"({"color": {"background": {"$type": "color", "$value": "#101010"}},
      "radius": {"$type": "dimension", "card": {"$value": {"value": 14, "unit": "px"}}},
      "control": {"height": {"$type": "dimension", "$value": {"value": 40, "unit": "px"}}},
      "elevation": {"card": {"$type": "shadow", "$value": {"color": "#11223344", "offsetX": {"value": 1, "unit": "px"},
        "offsetY": {"value": 3, "unit": "px"}, "blur": {"value": 6, "unit": "px"}, "spread": {"value": 0, "unit": "px"}}}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved)).empty());
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    CHECK(theme->property("background").value<QColor>() == QColor{"#101010"});
    CHECK(theme->property("radiusCard").toReal() == 14.0);
    CHECK(theme->property("radiusControl").toReal() == 6.0);
    CHECK(theme->property("controlHeight").toReal() == 40.0);
    CHECK(theme->property("cardShadowColor").value<QColor>() == QColor::fromRgb(0x11, 0x22, 0x33, 0x44));
    CHECK(theme->property("cardShadowOffsetY").toReal() == 3.0);
    CHECK(theme->property("cardShadowBlur").toReal() == 6.0);
}

// Mutation: drop the `color.ok.bg` row from the colour properties (Theme.successBg keeps its default).
TEST_CASE("qt_quick theme: a tone's bg, border and dot reach Theme, its surfaces and the badge that draws them",
          "[qt_quick]") {
    Scene scene;
    auto const tokens = ui::parseTokens(R"({"color": {"$type": "color",
        "ok": {"bg": {"$value": "#123456"}, "border": {"$value": "#234567"}, "dot": {"$value": "#345678"}},
        "err": {"bg": {"$value": "#456789"}}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    auto const sidebarTokens =
        ui::parseTokens(R"({"color": {"$type": "color", "warn": {"dot": {"$value": "#56789a"}}}})");
    REQUIRE(sidebarTokens);
    auto const sidebarResolved = ui::resolveTokens({&*sidebarTokens});
    REQUIRE(sidebarResolved);
    std::map<std::string, ui::ThemeValues, std::less<>> const surfaces{{"sidebar", ui::themeValues(*sidebarResolved)}};
    CHECK(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved), surfaces).empty());
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    CHECK(theme->property("successBg").value<QColor>() == QColor{"#123456"});
    CHECK(theme->property("successBorder").value<QColor>() == QColor{"#234567"});
    CHECK(theme->property("successDot").value<QColor>() == QColor{"#345678"});
    CHECK(theme->property("errorBg").value<QColor>() == QColor{"#456789"});
    CHECK(theme->property("infoBg").value<QColor>() == QColor{"#e3f2fd"});  // the default stands
    auto* const sidebar = theme->property("sidebarScheme").value<QObject*>();
    REQUIRE(sidebar != nullptr);
    CHECK(sidebar->property("warnDot").value<QColor>() == QColor{"#56789a"});
    auto* const own = theme->property("themeScheme").value<QObject*>();
    REQUIRE(own != nullptr);
    CHECK(own->property("successBg").value<QColor>() == QColor{"#123456"});  // the theme's scheme follows Theme

    auto badge = scene.backend().createBadge(nullptr);
    badge->setTone(ui::Tone::Ok);
    badge->setText("Done");
    scene.settle();
    auto const* const fill = Scene::itemOf(*badge).findChild<QQuickItem*>(QStringLiteral("fill"));
    REQUIRE(fill != nullptr);
    CHECK(fill->property("color").value<QColor>() == QColor{"#123456"});
    // A filled badge is a tag: it draws no border.
    CHECK(fill->property("border").value<QObject*>()->property("width").toReal() == 0.0);

    auto outlined = scene.backend().createBadge(nullptr);
    outlined->setTone(ui::Tone::Ok);
    outlined->setText("Done");
    outlined->setStyle(ui::BadgeStyle::Outline);
    scene.settle();
    auto const* const outline = Scene::itemOf(*outlined).findChild<QQuickItem*>(QStringLiteral("fill"));
    REQUIRE(outline != nullptr);
    CHECK(outline->property("border").value<QObject*>()->property("color").value<QColor>() == QColor{"#234567"});
}

TEST_CASE("qt_quick theme: radius.badge reaches Theme", "[qt_quick]") {
    Scene scene;
    auto const tokens =
        ui::parseTokens(R"({"radius": {"$type": "dimension", "badge": {"$value": {"value": 2, "unit": "px"}}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved)).empty());
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    CHECK(theme->property("radiusBadge").toReal() == 2.0);
}

TEST_CASE("qt_quick theme: a table header's colour reaches Theme, a surface's scheme and the table it draws under",
          "[qt_quick]") {
    Scene scene;
    auto const tokens =
        ui::parseTokens(R"({"color": {"$type": "color", "table": {"header": {"$value": "#102030"}}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    auto const sidebarTokens =
        ui::parseTokens(R"({"color": {"$type": "color", "table": {"header": {"$value": "#405060"}}}})");
    REQUIRE(sidebarTokens);
    auto const sidebarResolved = ui::resolveTokens({&*sidebarTokens});
    REQUIRE(sidebarResolved);
    std::map<std::string, ui::ThemeValues, std::less<>> const surfaces{{"sidebar", ui::themeValues(*sidebarResolved)}};
    CHECK(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved), surfaces).empty());
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    CHECK(theme->property("tableHeader").value<QColor>() == QColor{"#102030"});
    auto* const own = theme->property("themeScheme").value<QObject*>();
    REQUIRE(own != nullptr);
    CHECK(own->property("tableHeader").value<QColor>() == QColor{"#102030"});  // the theme's scheme follows Theme
    auto* const sidebar = theme->property("sidebarScheme").value<QObject*>();
    REQUIRE(sidebar != nullptr);
    CHECK(sidebar->property("tableHeader").value<QColor>() == QColor{"#405060"});

    auto table = scene.backend().createTable(nullptr);
    table->setColumns({{.label = "Name"}});
    scene.settle();
    auto* const background = Scene::itemOf(*table).property("headerBackgroundItem").value<QObject*>();
    REQUIRE(background != nullptr);
    auto const* const fill = background->findChild<QQuickItem*>();
    REQUIRE(fill != nullptr);
    CHECK(fill->property("color").value<QColor>() == QColor{"#102030"});
}

// Mutation: drop `color.chart.2` from either property table in theme.cpp (Theme or the sidebar keeps its default).
TEST_CASE("qt_quick theme: a chart colour reaches Theme and a surface's scheme", "[qt_quick]") {
    Scene scene;
    auto const tokens = ui::parseTokens(R"({"color": {"$type": "color", "chart": {"2": {"$value": "#102030"}}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    auto const sidebarTokens =
        ui::parseTokens(R"({"color": {"$type": "color", "chart": {"2": {"$value": "#405060"}}}})");
    REQUIRE(sidebarTokens);
    auto const sidebarResolved = ui::resolveTokens({&*sidebarTokens});
    REQUIRE(sidebarResolved);
    std::map<std::string, ui::ThemeValues, std::less<>> const surfaces{{"sidebar", ui::themeValues(*sidebarResolved)}};
    CHECK(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved), surfaces).empty());
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    CHECK(theme->property("chart1").value<QColor>() == QColor{"#0072b2"});  // a token the theme lacks: the default
    CHECK(theme->property("chart2").value<QColor>() == QColor{"#102030"});
    auto* const page = theme->property("pageScheme").value<QObject*>();
    REQUIRE(page != nullptr);
    CHECK(page->property("chart2").value<QColor>() == QColor{"#102030"});  // a built-in surface follows Theme
    auto* const sidebar = theme->property("sidebarScheme").value<QObject*>();
    REQUIRE(sidebar != nullptr);
    CHECK(sidebar->property("chart2").value<QColor>() == QColor{"#405060"});
}

namespace {

/// The background a surface draws: the widget's direct child Rectangle at z -1.
QQuickItem const* surfaceBackground(QQuickItem const& item) {
    for (QQuickItem const* const child : item.childItems()) {
        if (child->z() == -1.0 && child->inherits("QQuickRectangle")) {
            return child;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("qt_quick theme: a surface colours its subtree and draws its background", "[qt_quick]") {
    Scene scene;
    auto sidebar = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    sidebar->setSurface("sidebar");
    auto inside = scene.backend().createText(sidebar.get());
    inside->setText("inside");
    auto card = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    card->setSurface("card");
    auto outside = scene.backend().createText(nullptr);
    outside->setText("outside");
    scene.settle();
    CHECK(labelColor(Scene::itemOf(*inside), QStringLiteral("inside")) == QColor{"#c9ccd6"});
    CHECK(labelColor(Scene::itemOf(*outside), QStringLiteral("outside")) == QColor{"#1d1d1f"});
    QQuickItem const* const sidebarBackground = surfaceBackground(Scene::itemOf(*sidebar));
    REQUIRE(sidebarBackground != nullptr);
    CHECK(sidebarBackground->isVisible());
    CHECK(sidebarBackground->property("color").value<QColor>() == QColor{"#1f2430"});
    QQuickItem const* const cardBackground = surfaceBackground(Scene::itemOf(*card));
    REQUIRE(cardBackground != nullptr);
    CHECK(cardBackground->property("color").value<QColor>() == QColor{"#ffffff"});
    CHECK(cardBackground->property("radius").toReal() == 10.0);
    QQuickItem const* const plain = surfaceBackground(Scene::itemOf(*outside));
    CHECK((plain == nullptr || !plain->isVisible()));

    // A theme's own surface of a built-in name replaces it, and every other role is the theme's.
    auto const layer = ui::parseTokens(R"({"color": {"$type": "color", "surface": {"$value": "#123456"}}})");
    REQUIRE(layer);
    auto const resolved = ui::resolveTokens({&*layer});
    REQUIRE(resolved);
    std::map<std::string, ui::ThemeValues, std::less<>> const surfaces{{"sidebar", ui::themeValues(*resolved)}};
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(), surfaces).empty());
    scene.settle();
    CHECK(sidebarBackground->property("color").value<QColor>() == QColor{"#123456"});
    CHECK(labelColor(Scene::itemOf(*inside), QStringLiteral("inside")) == QColor{"#1d1d1f"});
}

TEST_CASE("qt_quick theme: a bundle's theme index applies its base and its surfaces", "[qt_quick]") {
    Scene scene;
    std::map<std::string, std::string, std::less<>> const themes{
        {"theme/index.json",
         R"({"vocab": "ui-theme/1", "base": "theme/base.json", "surfaces": {"sidebar": "theme/sidebar.json"}})"},
        {"theme/base.json", R"({"color": {"$type": "color", "accent": {"$value": "#aa0055"}}})"},
        {"theme/sidebar.json", R"({"color": {"$type": "color", "surface": {"$value": "#e0e8f0"}}})"}};
    CHECK(morph::qt_quick::applyBundleTheme(scene.engine(), themes).empty());
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    CHECK(theme->property("accent").value<QColor>() == QColor{"#aa0055"});
    auto* const sidebar = theme->property("sidebarScheme").value<QObject*>();
    REQUIRE(sidebar != nullptr);
    CHECK(sidebar->property("surface").value<QColor>() == QColor{"#e0e8f0"});
    // A role the surface does not set is the theme's: the base's accent.
    CHECK(sidebar->property("accent").value<QColor>() == QColor{"#aa0055"});

    // A bundle without a theme restores the defaults.
    CHECK(morph::qt_quick::applyBundleTheme(scene.engine(), {}).empty());
    CHECK(theme->property("accent").value<QColor>() == QColor{"#1565c0"});
}

// Mutation: skip `$root` in the token reader (warnBg keeps its default, and the tone's text colour goes with it).
TEST_CASE("qt_quick theme: a bundle writes a tone with its parts as a group with a $root, and they load",
          "[qt_quick]") {
    Scene scene;
    std::map<std::string, std::string, std::less<>> const themes{
        {"theme/index.json", R"({"vocab": "ui-theme/1", "base": "theme/base.json"})"},
        {"theme/base.json", R"({"color": {"$type": "color",
            "warn": {"$root": {"$value": "#7A4A00"}, "bg": {"$value": "#FFF1D6"}, "border": {"$value": "#F1D49A"}}}})"}};
    CHECK(morph::qt_quick::applyBundleTheme(scene.engine(), themes).empty());
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    CHECK(theme->property("warn").value<QColor>() == QColor{"#7A4A00"});
    CHECK(theme->property("warnBg").value<QColor>() == QColor{"#FFF1D6"});
    CHECK(theme->property("warnBorder").value<QColor>() == QColor{"#F1D49A"});
}

TEST_CASE("qt_quick theme: a bundle's token that nothing reads is reported with its file", "[qt_quick]") {
    Scene scene;
    std::map<std::string, std::string, std::less<>> const themes{
        {"theme/index.json", R"({"vocab": "ui-theme/1", "base": "theme/base.json"})"},
        {"theme/base.json",
         R"({"color": {"$type": "color", "accent": {"$value": "#aa0055"}, "sucess": {"$value": "#00ff00"}}})"}};
    auto const problems = morph::qt_quick::applyBundleTheme(scene.engine(), themes);
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().contains("theme/base.json: the token `color.sucess`"));
}

// Mutation: write the scheme only at adoption (the text keeps the theme's colour).
TEST_CASE("qt_quick theme: a surface set after its children exist still colours them", "[qt_quick]") {
    Scene scene;
    auto sidebar = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto const nested = scene.backend().createStack(sidebar.get(), ui::Axis::Vertical);
    auto inside = scene.backend().createText(nested.get());
    inside->setText("late");
    sidebar->setSurface("sidebar");
    scene.settle();
    CHECK(labelColor(Scene::itemOf(*inside), QStringLiteral("late")) == QColor{"#c9ccd6"});
}

// Mutation: skip passSchemeDown in ItemBase::showSurface (the text keeps the first surface's colour).
TEST_CASE("qt_quick theme: a surface changed after mount restyles the widget and its content", "[qt_quick]") {
    Scene scene;
    auto panel = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto const nested = scene.backend().createStack(panel.get(), ui::Axis::Vertical);
    auto inside = scene.backend().createText(nested.get());
    inside->setText("changing");
    panel->setSurface("sidebar");
    scene.settle();
    CHECK(labelColor(Scene::itemOf(*inside), QStringLiteral("changing")) == QColor{"#c9ccd6"});
    QQuickItem const* const background = surfaceBackground(Scene::itemOf(*panel));
    REQUIRE(background != nullptr);
    CHECK(background->property("color").value<QColor>() == QColor{"#1f2430"});

    panel->setSurface("card");
    scene.settle();
    CHECK(background->property("color").value<QColor>() == QColor{"#ffffff"});
    CHECK(labelColor(Scene::itemOf(*inside), QStringLiteral("changing")) == QColor{"#1d1d1f"});

    panel->setSurface("sidebar");
    scene.settle();
    CHECK(labelColor(Scene::itemOf(*inside), QStringLiteral("changing")) == QColor{"#c9ccd6"});
    panel->setSurface("");
    scene.settle();
    CHECK_FALSE(background->isVisible());
    CHECK(labelColor(Scene::itemOf(*inside), QStringLiteral("changing")) == QColor{"#1d1d1f"});
}

namespace {

/// An installed family whose average character width at @p pixels differs from the platform font's, so a test can see
/// the units follow it; the CI image and a workstation ship different fonts, so it is found rather than named.
std::optional<QString> otherFamily(QFont const& platform, int pixels) {
    QFont sized = platform;
    sized.setPixelSize(pixels);
    double const platformWidth = QFontMetricsF{sized}.averageCharWidth();
    for (QString const& family : QFontDatabase::families()) {
        QFont candidate;
        candidate.setFamilies({family});
        candidate.setPixelSize(pixels);
        if (QFontInfo{candidate}.family().compare(family, Qt::CaseInsensitive) == 0 &&
            family.compare(platform.family(), Qt::CaseInsensitive) != 0 &&
            std::abs(QFontMetricsF{candidate}.averageCharWidth() - platformWidth) > 0.5) {
            return family;
        }
    }
    return std::nullopt;
}

// The first installed family that resolves to itself and draws every character at one width; none on a machine
// without a monospace font. No font is bundled in the tree, and the CI image and a workstation ship different ones.
std::optional<QString> fixedPitchFamily() {
    for (QString const& family : QFontDatabase::families()) {
        QFont candidate;
        candidate.setFamilies({family});
        QFontInfo const info{candidate};
        if (info.family().compare(family, Qt::CaseInsensitive) == 0 && info.fixedPitch()) {
            return family;
        }
    }
    return std::nullopt;
}

// The font a text widget's label draws in.
QFont labelFont(ui::Widget const& text) {
    auto const* const control = Scene::itemOf(text).property("control").value<QQuickItem*>();
    REQUIRE(control != nullptr);
    return control->property("font").value<QFont>();
}

}  // namespace

// Mutation: leave MorphText's font alone for Mono (the family and the fixed-pitch checks fail).
TEST_CASE("qt_quick theme: a mono text draws in font.mono, else in the platform's fixed-pitch font", "[qt_quick]") {
    Scene scene;
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    auto mono = scene.backend().createText(nullptr);
    mono->setText("S-26-104170");
    mono->setRole(ui::TextRole::Mono);
    auto normal = scene.backend().createText(nullptr);
    normal->setText("Sample");
    scene.settle();

    // With no font.mono, the platform's fixed-pitch font.
    CHECK(QFontInfo{labelFont(*mono)}.fixedPitch());

    std::optional<QString> const family = fixedPitchFamily();
    REQUIRE(family.has_value());
    ui::ThemeValues values = ui::themeValues();
    values.families.insert_or_assign("font.mono", std::vector<std::string>{family->toStdString()});
    CHECK(morph::qt_quick::applyTheme(scene.engine(), values).empty());
    scene.settle();
    CHECK(theme->property("monoFamilies").toStringList().value(0) == *family);
    CHECK(labelFont(*mono).family() == *family);
    CHECK(labelFont(*normal).family() != *family);
    CHECK(labelFont(*mono).pixelSize() == std::lround(theme->property("bodyPixels").toReal()));

    values.families.insert_or_assign("font.mono", std::vector<std::string>{"NoSuchFont"});
    CHECK(morph::qt_quick::applyTheme(scene.engine(), values) ==
          std::vector<std::string>{"font.mono: none of NoSuchFont is installed"});
}

// Mutation: keep `Theme.strongFont` on the table header (it reads the body size), `Theme.bodyFont` on the caption.
TEST_CASE("qt_quick theme: the small and caption sizes, the display role and the strong weight reach the components",
          "[qt_quick]") {
    Scene scene;
    ui::ThemeValues values = ui::themeValues();
    values.dimensions.insert_or_assign("font.size.body", 13.0);
    values.dimensions.insert_or_assign("font.size.small", 11.0);
    values.dimensions.insert_or_assign("font.size.caption", 12.0);
    values.dimensions.insert_or_assign("font.size.display", 22.0);
    values.numbers.insert_or_assign("font.weight.strong", 600.0);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), values).empty());
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    CHECK(theme->property("smallPixels").toReal() == 11.0);
    CHECK(theme->property("captionPixels").toReal() == 12.0);
    CHECK(theme->property("displayPixels").toReal() == 22.0);
    CHECK(theme->property("strongWeight").toInt() == 600);
    CHECK(theme->property("strongFont").value<QFont>().weight() == QFont::DemiBold);

    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto table = scene.backend().createTable(column.get());
    table->setColumns({{.label = "Name"}});
    auto input = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    input->setCaption("Operator", true);
    auto display = scene.backend().createText(column.get());
    display->setText("7");
    display->setRole(ui::TextRole::Display);
    auto small = scene.backend().createText(column.get());
    small->setText("note");
    small->setRole(ui::TextRole::Small);
    scene.settle();

    auto const* const header = namedItem(Scene::itemOf(*table), QStringLiteral("header0"));
    REQUIRE(header != nullptr);
    CHECK(header->property("font").value<QFont>().pixelSize() == 11);
    CHECK(header->property("font").value<QFont>().weight() == QFont::DemiBold);
    auto const* const caption = namedItem(Scene::itemOf(*input), QStringLiteral("caption"));
    REQUIRE(caption != nullptr);
    CHECK(caption->property("font").value<QFont>().pixelSize() == 12);
    CHECK(labelFont(*display).pixelSize() == 22);
    CHECK(labelFont(*display).weight() == QFont::DemiBold);
    CHECK(labelFont(*small).pixelSize() == 11);
    CHECK(labelFont(*small).weight() == QFont::Normal);
}

// Mutation: bind the units' metrics to the platform font always (unitWidth keeps the platform font's width).
TEST_CASE("qt_quick theme: font.family sets the body font and the units follow it; an unknown family is reported",
          "[qt_quick]") {
    Scene scene;
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    auto const platform = theme->property("platformMetrics").value<QObject*>()->property("font").value<QFont>();
    double const platformUnit = theme->property("unitWidth").toReal();
    int const pixels = static_cast<int>(std::lround(theme->property("bodyPixels").toReal()));
    CHECK(theme->property("bodyFont").value<QFont>().family() == platform.family());

    std::optional<QString> const family = otherFamily(platform, pixels);
    REQUIRE(family.has_value());
    ui::ThemeValues values = ui::themeValues();
    values.families.insert_or_assign("font.family", std::vector<std::string>{family->toStdString()});
    CHECK(morph::qt_quick::applyTheme(scene.engine(), values).empty());
    CHECK(theme->property("bodyFont").value<QFont>().families() == QStringList{*family});
    QFont expected;
    expected.setFamilies({*family});
    expected.setPixelSize(pixels);
    CHECK_FALSE(theme->property("unitWidth").toReal() == platformUnit);
    CHECK(std::abs(theme->property("unitWidth").toReal() - QFontMetricsF{expected}.averageCharWidth()) < 0.01);

    // Back to no family: the platform font and its units.
    CHECK(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues()).empty());
    CHECK(theme->property("unitWidth").toReal() == platformUnit);

    values.families.insert_or_assign("font.family", std::vector<std::string>{"Nowhere Sans", "Absent Serif"});
    CHECK(morph::qt_quick::applyTheme(scene.engine(), values) ==
          std::vector<std::string>{"font.family: none of Nowhere Sans, Absent Serif is installed"});
}

TEST_CASE("theme: a fontFamily token is read as its names", "[qt_quick]") {
    auto const tokens = ui::parseTokens(R"({"font": {"family": {"$type": "fontFamily", "$value": ["A Sans", "B"]}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    CHECK(ui::themeValues(*resolved).families.at("font.family") == std::vector<std::string>{"A Sans", "B"});
    CHECK(ui::themeValues().families.at("font.family").empty());
}

// Mutation: leave Theme.icons unset (the banner draws no icon, and the badge's icon stays its name as text).
TEST_CASE("qt_quick theme: a bundle's icon set draws the icons it names, and a node icon outside it is reported once",
          "[qt_quick]") {
    std::vector<morph::qt_quick::ComponentFailure> reported;
    morph::qt_quick::Components components{
        [&reported](morph::qt_quick::ComponentFailure const& failure) { reported.push_back(failure); }};
    Scene scene{morph::qt_quick::BackendOptions{.components = &components}};
    std::string const tick =
        R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 8"><path d="M1 4l2 2 4-4"/></svg>)";
    CHECK(morph::qt_quick::applyBundleTheme(scene.engine(), {}, {{"tone.ok", tick}}).empty());
    QString const url =
        QStringLiteral("data:image/svg+xml;base64,") + QString::fromLatin1(QByteArray::fromStdString(tick).toBase64());

    auto ok = scene.backend().createBanner(nullptr);
    ok->setTone(ui::Tone::Ok);
    ok->setText("Saved");
    auto info = scene.backend().createBanner(nullptr);
    info->setTone(ui::Tone::Info);
    info->setText("Note");
    auto named = scene.backend().createBadge(nullptr);
    named->setText("Done");
    named->setIcon("tone.ok");
    auto missing = scene.backend().createBadge(nullptr);
    missing->setText("Odd");
    missing->setIcon("sparkle");
    auto again = scene.backend().createBadge(nullptr);
    again->setIcon("sparkle");
    // A framework name the set lacks is drawn by the control itself and is not reported.
    auto chevron = scene.backend().createBadge(nullptr);
    chevron->setIcon("nav.back");
    scene.settle();

    auto iconOf = [](ui::Widget const& widget, char const* name) {
        auto* const icon = Scene::itemOf(widget).findChild<QQuickItem*>(QString::fromLatin1(name));
        REQUIRE(icon != nullptr);
        return icon;
    };
    CHECK(iconOf(*ok, "toneIcon")->property("source").toUrl().toString() == url);
    CHECK(iconOf(*ok, "toneIcon")->isVisible());
    CHECK_FALSE(iconOf(*info, "toneIcon")->isVisible());
    CHECK(iconOf(*named, "icon")->property("source").toUrl().toString() == url);
    CHECK_FALSE(iconOf(*missing, "icon")->isVisible());
    CHECK(reported == std::vector<morph::qt_quick::ComponentFailure>{
                          {.name = "sparkle", .reason = "icon sparkle is not in the theme's icon set"}});

    // Without a declared set nothing is reported: every icon is drawn as text.
    CHECK(morph::qt_quick::applyBundleTheme(scene.engine(), {}).empty());
    auto plain = scene.backend().createBadge(nullptr);
    plain->setIcon("glow");
    scene.settle();
    CHECK(reported.size() == 1);
}

namespace {

/// The brand theme with a card surface, border and radius of its own, so a dialog's colours are the theme's and not
/// the defaults.
ui::ThemeValues cardTheme() {
    auto const tokens = ui::parseTokens(R"({"color": {"$type": "color",
        "text": {"$value": "#336699"},
        "surface": {"$value": "#fafaf0"},
        "border": {"$value": "#a0a0b0"}},
      "radius": {"$type": "dimension", "card": {"$value": {"value": 14, "unit": "px"}}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    return ui::themeValues(*resolved);
}

/// The Popup or Drawer an overlay widget's root item hosts.
QObject* overlayPopupOf(QQuickItem const& overlayItem) {
    for (QObject* const child : overlayItem.children()) {
        if (child->inherits("QQuickPopup")) {
            return child;
        }
    }
    return nullptr;
}

}  // namespace

// Mutation: drop the overlay's `background` (the Basic style's palette.window draws), or have OverlayImpl::scheme()
// return the item's own scheme (the text reads the sidebar's colour).
TEMPLATE_TEST_CASE("qt_quick theme: a dialog and a drawer draw on the theme's card, whatever surface declares them",
                   "[qt_quick]", ui::DialogWidget, ui::DrawerWidget) {
    Scene scene;
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), cardTheme()).empty());
    auto sidebar = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    sidebar->setSurface("sidebar");
    std::unique_ptr<TestType> overlay;
    if constexpr (std::is_same_v<TestType, ui::DialogWidget>) {
        overlay = scene.backend().createDialog(sidebar.get());
    } else {
        overlay = scene.backend().createDrawer(sidebar.get(), ui::Side::End);
    }
    overlay->setTitle("Sign");
    auto inside = scene.backend().createText(overlay.get());
    inside->setText("inside");
    overlay->setOpen(true);
    scene.settle();

    QObject const* const popup = overlayPopupOf(Scene::itemOf(*overlay));
    REQUIRE(popup != nullptr);
    auto const* const background = popup->property("background").value<QQuickItem*>();
    REQUIRE(background != nullptr);
    CHECK(background->property("color").value<QColor>() == QColor{"#fafaf0"});
    CHECK(background->property("border").value<QObject*>()->property("color").value<QColor>() == QColor{"#a0a0b0"});
    if constexpr (std::is_same_v<TestType, ui::DialogWidget>) {
        CHECK(background->property("radius").toReal() == 14.0);
    } else {
        CHECK(background->property("radius").toReal() == 0.0);
    }
    CHECK(popup->property("leftPadding").toReal() == 16.0);
    // The content and the title read the card's text colour, not the sidebar's light one.
    CHECK(labelColor(Scene::itemOf(*inside), QStringLiteral("inside")) == QColor{"#336699"});
    CHECK(labelColor(*popup->property("contentItem").value<QQuickItem*>(), QStringLiteral("Sign")) ==
          QColor{"#336699"});
}

namespace {}  // namespace

// Mutation: drop the bar's `contentItem` (the style's own fill draws, and no item is named `fill`), or colour the
// fill with anything but the scheme's accent.
TEST_CASE("qt_quick theme: a progress bar fills with the accent on the track colour", "[qt_quick]") {
    Scene scene;
    auto const tokens = ui::parseTokens(R"({"color": {"$type": "color",
        "accent": {"$value": "#17784a"}, "track": {"$value": "#d0d4d8"}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved)).empty());
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto progress = scene.backend().createProgress(column.get());
    progress->setValue(0.25);
    scene.settle();

    QQuickItem const& item = Scene::itemOf(*progress);
    QQuickItem const* const fill = namedItem(item, QStringLiteral("fill"));
    QQuickItem const* const track = namedItem(item, QStringLiteral("track"));
    REQUIRE(fill != nullptr);
    REQUIRE(track != nullptr);
    CHECK(fill->property("color").value<QColor>() == QColor{"#17784a"});
    CHECK(track->property("color").value<QColor>() == QColor{"#d0d4d8"});
    CHECK(std::abs(fill->width() - (0.25 * track->width())) <= 1.0);

    // Indeterminate: the segment moves on its own.
    progress->setValue(std::nullopt);
    scene.settle();
    QTest::qWait(100);
    double const before = fill->x();
    QTest::qWait(200);
    CHECK(fill->x() != before);
}

// Mutation: ignore `readOnly` in MorphInputBackground (the read-only field draws the input fill).
TEST_CASE("qt_quick theme: a read-only input draws on the page's background, an editable one on the input fill",
          "[qt_quick]") {
    Scene scene;
    auto const tokens = ui::parseTokens(R"({"color": {"$type": "color",
        "controlSurface": {"$value": "#fefefe"}, "background": {"$value": "#e9ecef"}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved)).empty());
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto const editable = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    auto const readOnly = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    readOnly->setReadOnly(true);
    auto const select = scene.backend().createSelect(column.get(), ui::SelectStyle::Dropdown);
    select->setReadOnly(true);
    scene.settle();

    auto const backgroundOf = [](ui::Widget const& widget) {
        auto const* const control = Scene::itemOf(widget).property("control").value<QQuickItem*>();
        REQUIRE(control != nullptr);
        auto const* const background = control->property("background").value<QQuickItem*>();
        REQUIRE(background != nullptr);
        return background->property("color").value<QColor>();
    };
    CHECK(backgroundOf(*editable) == QColor{"#fefefe"});
    CHECK(backgroundOf(*readOnly) == QColor{"#e9ecef"});
    CHECK(backgroundOf(*select) == QColor{"#e9ecef"});
    readOnly->setReadOnly(false);
    scene.settle();
    CHECK(backgroundOf(*readOnly) == QColor{"#fefefe"});
}

namespace {

/// A button, a text input and a check box in a column, on @p scene.
struct Controls {
    explicit Controls(Scene& scene)
        : column{scene.backend().createStack(nullptr, ui::Axis::Vertical)},
          button{scene.backend().createButton(column.get())},
          input{scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine)},
          box{scene.backend().createCheckbox(column.get())} {
        button->setLabel("Book");
        box->setLabel("Signed");
    }

    std::unique_ptr<ui::StackWidget> column;
    std::unique_ptr<ui::ButtonWidget> button;
    std::unique_ptr<ui::TextInputWidget> input;
    std::unique_ptr<ui::CheckboxWidget> box;
};

/// Resizes @p scene's window to @p units of its theme's unit width.
void resizeToUnits(Scene& scene, double units) {
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    scene.window().resize(static_cast<int>(std::ceil(units * theme->property("unitWidth").toDouble())), 600);
    scene.settle();
}

/// The height of a field's control, or of a button's own item.
qreal controlHeight(ui::Widget const& widget) {
    QQuickItem const& item = Scene::itemOf(widget);
    auto const* const control = item.property("control").value<QQuickItem*>();
    return control != nullptr ? control->height() : item.height();
}

}  // namespace

// Mutation: have the backend not write Theme.touchTargets for its width class (a compact window's controls stay 32).
TEST_CASE("qt_quick theme: controls are touch targets while the window is compact", "[qt_quick]") {
    Scene scene;
    Controls const controls{scene};
    resizeToUnits(scene, 60);
    CHECK(controlHeight(*controls.button) == 44.0);
    CHECK(controlHeight(*controls.input) == 44.0);
    CHECK(controlHeight(*controls.box) >= 44.0);

    resizeToUnits(scene, 180);
    CHECK(controlHeight(*controls.button) == 32.0);
    CHECK(controlHeight(*controls.input) == 32.0);

    // The touch height is the theme's, beside its control height.
    auto const tokens = ui::parseTokens(R"({"control": {"$type": "dimension",
        "height": {"$value": {"value": 30, "unit": "px"}},
        "touchHeight": {"$value": {"value": 48, "unit": "px"}}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved)).empty());
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    CHECK(theme->property("controlHeight").toReal() == 30.0);
    CHECK(theme->property("touchHeight").toReal() == 48.0);
    scene.settle();
    CHECK(controlHeight(*controls.button) == 30.0);
    resizeToUnits(scene, 60);
    CHECK(controlHeight(*controls.button) == 48.0);
}

namespace {

/// The attached `Accessible` object of @p item, which QML parents to the item it is attached to.
QObject const* accessibleOf(QQuickItem const& item) {
    for (QObject const* const child : item.children()) {
        if (QByteArrayView{child->metaObject()->className()} == QByteArrayView{"QQuickAccessibleAttached"}) {
            return child;
        }
    }
    FAIL("no Accessible attached to the item");
    return nullptr;
}

/// The background item of a button widget's control.
QQuickItem& buttonBackground(ui::ButtonWidget const& button) {
    auto const* const control = Scene::itemOf(button).property("focusTarget").value<QQuickItem*>();
    REQUIRE(control != nullptr);
    auto* const background = control->property("background").value<QQuickItem*>();
    REQUIRE(background != nullptr);
    return *background;
}

}  // namespace

// Mutation: keep `scheme.neutral` in MorphButtonBackground (the hovered item reads the neutral tone's text colour).
TEST_CASE("qt_quick theme: the neutral tone, the hover fill and the track are separate tokens", "[qt_quick]") {
    Scene scene;
    auto const tokens = ui::parseTokens(R"({"color": {"$type": "color", "neutral": {"$value": "#363C46"}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved)).empty());
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    CHECK(theme->property("neutral").value<QColor>() == QColor{"#363C46"});
    CHECK(theme->property("hover").value<QColor>() == QColor{0x1d, 0x1d, 0x1f, 0x14});
    CHECK(theme->property("track").value<QColor>() == QColor{"#e5e5ea"});
    CHECK(theme->property("neutralBg").value<QColor>() == QColor{0x6e, 0x6e, 0x73, 0x26});

    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto item = scene.backend().createButton(column.get());
    item->setLabel("Entry");
    item->setStyle(ui::ButtonStyle::Item);
    item->setOnClick([] {});
    auto ghost = scene.backend().createButton(column.get());
    ghost->setLabel("Link");
    ghost->setEmphasis(ui::ButtonEmphasis::Ghost);
    ghost->setOnClick([] {});
    scene.settle();
    for (ui::ButtonWidget const* const button : {item.get(), ghost.get()}) {
        auto* const background = &buttonBackground(*button);
        REQUIRE(background->setProperty("hovered", true));
        CHECK(background->property("color").value<QColor>() == QColor{0x1d, 0x1d, 0x1f, 0x14});
    }

    auto badge = scene.backend().createBadge(column.get());
    badge->setTone(ui::Tone::Neutral);
    badge->setText("Draft");
    scene.settle();
    auto const* const fill = Scene::itemOf(*badge).findChild<QQuickItem*>(QStringLiteral("fill"));
    REQUIRE(fill != nullptr);
    CHECK(fill->property("color").value<QColor>() == QColor{0x6e, 0x6e, 0x73, 0x26});
}

// Mutations: revert the ghost label to `scheme.text`; revert a selected item's fill to the accent tint; fill a
// selected push button with `scheme.selection` (its rest fill is the accent tint).
TEST_CASE("qt_quick theme: a ghost button's label is the accent, a selected item fills with the selection colour",
          "[qt_quick]") {
    Scene scene;
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), brandTheme()).empty());
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    auto const accent = theme->property("accent").value<QColor>();
    auto const selection = theme->property("selection").value<QColor>();
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto ghost = scene.backend().createButton(column.get());
    ghost->setLabel("Clear all");
    ghost->setEmphasis(ui::ButtonEmphasis::Ghost);
    ghost->setOnClick([] {});
    auto item = scene.backend().createButton(column.get());
    item->setLabel("Entry");
    item->setStyle(ui::ButtonStyle::Item);
    item->setSelected(true);
    item->setOnClick([] {});
    auto push = scene.backend().createButton(column.get());
    push->setLabel("Pick");
    push->setSelected(true);
    push->setOnClick([] {});
    scene.settle();

    auto const* const ghostControl = Scene::itemOf(*ghost).property("focusTarget").value<QQuickItem*>();
    REQUIRE(ghostControl != nullptr);
    auto const* const label = ghostControl->property("contentItem").value<QQuickItem*>();
    REQUIRE(label != nullptr);
    CHECK(label->property("color").value<QColor>() == accent);
    CHECK(buttonBackground(*item).property("color").value<QColor>() == selection);
    // A selected push button keeps the accent tint and its ring.
    auto const tint = buttonBackground(*push).property("color").value<QColor>();
    CHECK(tint.rgb() == accent.rgb());
    CHECK(std::abs(tint.alphaF() - 0.12F) < 0.01F);
}

// Mutations: drop the ring's `selected` condition (the unselected button's ring shows); leave a selected push
// button's fill alone (no tint); make the control `checkable` (a click flips `Accessible.checked`).
TEST_CASE("qt_quick theme: a selected push button keeps its size, rings in the accent, tints and reads checked",
          "[qt_quick]") {
    Scene scene;
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), brandTheme()).empty());
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto plain = scene.backend().createButton(column.get());
    plain->setLabel("A1");
    plain->setSelected(false);
    plain->setOnClick([] {});
    auto picked = scene.backend().createButton(column.get());
    picked->setLabel("A2");
    picked->setSelected(true);
    picked->setOnClick([] {});
    scene.settle();
    QQuickItem const& plainItem = Scene::itemOf(*plain);
    QQuickItem const& pickedItem = Scene::itemOf(*picked);
    CHECK(pickedItem.implicitWidth() == plainItem.implicitWidth());
    CHECK(pickedItem.implicitHeight() == plainItem.implicitHeight());

    QColor const accent{"#aa0055"};
    auto const* const ring = buttonBackground(*picked).findChild<QQuickItem*>(QStringLiteral("selectionRing"));
    REQUIRE(ring != nullptr);
    CHECK(ring->isVisible());
    CHECK(ring->property("border").value<QObject*>()->property("color").value<QColor>() == accent);
    auto const* const plainRing = buttonBackground(*plain).findChild<QQuickItem*>(QStringLiteral("selectionRing"));
    REQUIRE(plainRing != nullptr);
    CHECK_FALSE(plainRing->isVisible());

    QColor tint = accent;
    tint.setAlphaF(0.12F);
    auto const shown = buttonBackground(*picked).property("color").value<QColor>();
    CHECK(shown.rgb() == tint.rgb());
    CHECK(std::abs(shown.alphaF() - 0.12F) < 0.01F);
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    CHECK(buttonBackground(*plain).property("color").value<QColor>() == theme->property("surface").value<QColor>());

    CHECK(accessibleOf(pickedItem)->property("checkable").toBool());
    CHECK(accessibleOf(pickedItem)->property("checked").toBool());
    CHECK_FALSE(accessibleOf(plainItem)->property("checked").toBool());
    scene.click(pickedItem);
    scene.settle();
    CHECK(accessibleOf(pickedItem)->property("checked").toBool());
    CHECK(buttonBackground(*picked).findChild<QQuickItem*>(QStringLiteral("selectionRing"))->isVisible());
}

// Mutation: drop `tone` from MorphInputBackground's border (the toned and the refused inputs read `color.border`).
TEST_CASE("qt_quick theme: a toned or refused input is outlined in the tone, with a ring, focused or not",
          "[qt_quick]") {
    Scene scene;
    auto const tokens = ui::parseTokens(R"({"color": {"$type": "color", "controlBorder": {"$value": "#999999"},
        "err": {"bg": {"$value": "#ffe0e0"}}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved)).empty());
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto const toned = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    toned->setTone(ui::Tone::Err);
    toned->setDescription("Out of spec: above 0.20");
    auto const refused = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    refused->setErrors({"Password incorrect."});
    auto const plain = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    scene.settle();
    auto const backgroundOf = [](ui::Widget const& widget) {
        auto* const background = Scene::controlOf(widget).property("background").value<QQuickItem*>();
        REQUIRE(background != nullptr);
        return background;
    };
    auto const borderOf = [&backgroundOf](ui::Widget const& widget) {
        return backgroundOf(widget)->property("border").value<QObject*>()->property("color").value<QColor>();
    };
    auto const ringOf = [&backgroundOf](ui::Widget const& widget) {
        QQuickItem const* const ring = backgroundOf(widget)->findChild<QQuickItem*>(QStringLiteral("toneRing"));
        REQUIRE(ring != nullptr);
        return ring->isVisible() ? ring->property("color").value<QColor>() : QColor{};
    };
    CHECK(borderOf(*toned) == QColor{"#c62828"});
    CHECK(ringOf(*toned) == QColor{"#ffe0e0"});
    CHECK(borderOf(*refused) == QColor{"#c62828"});
    CHECK(ringOf(*refused) == QColor{"#ffe0e0"});
    CHECK(borderOf(*plain) == QColor{"#999999"});
    CHECK_FALSE(ringOf(*plain).isValid());
    toned->focus();
    scene.settle();
    CHECK(borderOf(*toned) == QColor{"#c62828"});
    CHECK(backgroundOf(*toned)->property("border").value<QObject*>()->property("width").toInt() == 2);
    // The description is drawn under the control in the tone's colour, and is the control's accessible description.
    QQuickItem const* const description = Scene::itemOf(*toned).findChild<QQuickItem*>(QStringLiteral("description"));
    REQUIRE(description != nullptr);
    CHECK(description->property("color").value<QColor>() == QColor{"#c62828"});
    CHECK(Scene::itemOf(*toned).property("accessibleDescription").toString() ==
          QStringLiteral("Out of spec: above 0.20"));
}

// Mutation: ignore `prominent` in MorphInputBackground (the border is then 1 px of `color.border`).
TEST_CASE("qt_quick theme: a prominent input keeps the touch height and an accent border", "[qt_quick]") {
    Scene scene;
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), brandTheme()).empty());
    auto const input = scene.backend().createTextInput(nullptr, ui::TextInputMode::SingleLine);
    input->setEmphasis(ui::InputEmphasis::Prominent);
    scene.settle();
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    auto* const background = Scene::controlOf(*input).property("background").value<QQuickItem*>();
    REQUIRE(background != nullptr);
    CHECK(Scene::controlOf(*input).height() >= theme->property("touchHeight").toReal());
    QObject const* const border = background->property("border").value<QObject*>();
    CHECK(border->property("width").toInt() == 2);
    CHECK(border->property("color").value<QColor>() == QColor{"#aa0055"});
}

namespace {

/// The `background` item of a field widget's control, found by the widget's test id.
QQuickItem const& fieldBackground(char const* testId) {
    morph::qt_quick::detail::ItemBase const* const widget =
        morph::qt_quick::detail::findWidget(QString::fromLatin1(testId));
    REQUIRE(widget != nullptr);
    auto const* const control = widget->item()->property("control").value<QQuickItem*>();
    REQUIRE(control != nullptr);
    auto const* const background = control->property("background").value<QQuickItem*>();
    REQUIRE(background != nullptr);
    return *background;
}

QObject* themeOf(Scene& scene) {
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    return theme;
}

/// The border colour of @p item's `border` group.
QColor borderColorOf(QQuickItem const& item) {
    return item.property("border").value<QObject*>()->property("color").value<QColor>();
}

}  // namespace

// Mutation: keep `scheme.border` on the check box's unchecked outline, and on the input background's resting one.
TEST_CASE("qt_quick theme: a control is outlined in controlBorder, not in the divider-weight border", "[qt_quick]") {
    Scene scene;
    auto const tokens = ui::parseTokens(R"({"color": {"$type": "color",
        "border": {"$value": "#DDE1E6"}, "controlBorder": {"$value": "#C3C9D2"}, "divider": {"$value": "#EDF0F3"}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved)).empty());
    QObject const* const theme = themeOf(scene);
    CHECK(theme->property("controlBorder").value<QColor>() == QColor{"#C3C9D2"});
    CHECK(theme->property("divider").value<QColor>() == QColor{"#EDF0F3"});

    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto box = scene.backend().createCheckbox(column.get());
    box->setLabel("Signed");
    auto input = scene.backend().createTextInput(column.get(), ui::TextInputMode::SingleLine);
    input->setTestId("input");
    auto button = scene.backend().createButton(column.get());
    button->setLabel("Book");
    auto table = scene.backend().createTable(column.get());
    table->setColumns({{.label = "Name"}});
    auto row = scene.backend().createStack(table.get(), ui::Axis::Horizontal);
    table->setRowKey(*row, ui::Key{std::string{"a"}});
    auto cell = scene.backend().createText(row.get());
    cell->setText("cell");
    scene.settle();

    CHECK(borderColorOf(fieldBackground("input")) == QColor{"#C3C9D2"});
    QQuickItem const& boxItem = Scene::itemOf(*box);
    auto const* const indicator = boxItem.findChild<QQuickItem*>(QStringLiteral("boxIndicator"));
    REQUIRE(indicator != nullptr);
    CHECK(borderColorOf(*indicator) == QColor{"#C3C9D2"});
    auto const* const control = Scene::itemOf(*button).property("focusTarget").value<QQuickItem*>();
    REQUIRE(control != nullptr);
    CHECK(borderColorOf(*control->property("background").value<QQuickItem*>()) == QColor{"#C3C9D2"});
    auto const* const rowBackground = Scene::itemOf(*row).property("backgroundItem").value<QQuickItem*>();
    REQUIRE(rowBackground != nullptr);
    auto const* const rule = rowBackground->findChild<QQuickItem*>(QStringLiteral("rule"));
    REQUIRE(rule != nullptr);
    CHECK(rule->property("color").value<QColor>() == QColor{"#EDF0F3"});
}

// Mutation: keep `fill: scheme.surface` in MorphButtonBackground and `scheme.surface` in MorphInputBackground.
TEST_CASE("qt_quick theme: a control on a tinted surface fills with controlSurface, not with the surface's tint",
          "[qt_quick]") {
    Scene scene;
    auto const base = ui::parseTokens(R"({"color": {"$type": "color", "controlSurface": {"$value": "#FFFFFF"}}})");
    auto const bar = ui::parseTokens(R"({"color": {"$type": "color", "surface": {"$value": "#EAF5EE"}}})");
    REQUIRE(base);
    REQUIRE(bar);
    auto const resolved = ui::resolveTokens({&*base});
    auto const barResolved = ui::resolveTokens({&*base, &*bar});
    REQUIRE(resolved);
    REQUIRE(barResolved);
    std::map<std::string, ui::ThemeValues, std::less<>> const surfaces{{"selected", ui::themeValues(*barResolved)}};
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved), surfaces).empty());

    auto const row = scene.backend().createStack(nullptr, ui::Axis::Horizontal);
    row->setSurface("selected");
    auto button = scene.backend().createButton(row.get());
    button->setLabel("Export");
    auto input = scene.backend().createTextInput(row.get(), ui::TextInputMode::SingleLine);
    input->setTestId("tinted_input");
    scene.settle();
    CHECK(buttonBackground(*button).property("color").value<QColor>() == QColor{"#FFFFFF"});
    CHECK(fieldBackground("tinted_input").property("color").value<QColor>() == QColor{"#FFFFFF"});
}

namespace {

QQuickItem const& panelFrame(ui::Widget const& panel) {
    auto const* const frame = Scene::itemOf(panel).findChild<QQuickItem*>(QStringLiteral("frame"));
    REQUIRE(frame != nullptr);
    return *frame;
}

QQuickItem const* panelShadow(ui::Widget const& panel) {
    return Scene::itemOf(panel).findChild<QQuickItem*>(QStringLiteral("frameShadow"));
}

}  // namespace

// Control: fill the panel with the scheme it inherits, and the frame reads the page's background.
TEST_CASE("qt_quick theme: a panel on the page is a card, and its input fills with controlSurface", "[qt_quick]") {
    Scene scene;
    QObject const* const theme = themeOf(scene);
    REQUIRE(theme->property("surface").value<QColor>() != theme->property("background").value<QColor>());
    auto page = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    page->setSurface("page");
    auto panel = scene.backend().createPanel(page.get());
    auto input = scene.backend().createTextInput(panel.get(), ui::TextInputMode::SingleLine);
    input->setTestId("card_input");
    scene.settle();
    CHECK(panelFrame(*panel).property("color").value<QColor>() == theme->property("surface").value<QColor>());
    CHECK(fieldBackground("card_input").property("color").value<QColor>() ==
          theme->property("controlSurface").value<QColor>());
    CHECK(panelShadow(*panel)->isVisible());
}

TEST_CASE("qt_quick theme: a panel that names the page surface is flat on the page's background", "[qt_quick]") {
    Scene scene;
    QObject const* const theme = themeOf(scene);
    auto page = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    page->setSurface("page");
    auto panel = scene.backend().createPanel(page.get());
    panel->setSurface("page");
    scene.settle();
    CHECK(panelShadow(*panel)->isVisible() == false);
    // The widget draws the page surface behind the frame, which stays unfilled so nothing is drawn twice.
    CHECK(panelFrame(*panel).property("color").value<QColor>() == QColor{Qt::transparent});
    QList<QQuickItem*> const filled = Scene::itemsOf(Scene::itemOf(*panel), "QQuickRectangle");
    bool backgroundFound = false;
    for (QQuickItem const* const rectangle : filled) {
        backgroundFound = backgroundFound || rectangle->property("color").value<QColor>() ==
                                                 theme->property("background").value<QColor>();
    }
    CHECK(backgroundFound);
}

TEST_CASE("qt_quick theme: a panel in a panel draws no shadow, and a card surface draws one frame", "[qt_quick]") {
    Scene scene;
    auto outer = scene.backend().createPanel(nullptr);
    auto inner = scene.backend().createPanel(outer.get());
    auto named = scene.backend().createPanel(nullptr);
    named->setSurface("card");
    scene.settle();
    CHECK(panelShadow(*outer)->isVisible());
    CHECK_FALSE(panelShadow(*inner)->isVisible());
    CHECK(panelFrame(*named).property("color").value<QColor>() == QColor{Qt::transparent});
    CHECK_FALSE(panelShadow(*named)->isVisible());
    // The empty surface that a bound value evaluates to is the default, a card.
    named->setSurface("");
    scene.settle();
    CHECK(panelShadow(*named)->isVisible());
}

namespace {

ui::Rgba rgbaOf(QColor const& color) {
    return ui::Rgba{.r = static_cast<std::uint8_t>(color.red()),
                    .g = static_cast<std::uint8_t>(color.green()),
                    .b = static_cast<std::uint8_t>(color.blue()),
                    .a = 255};
}

}  // namespace

// Control: remove the sidebar's `controlSurface` literal; the field then reads the theme's `#ffffff`, 1.6:1 to its
// text.
TEST_CASE("qt_quick theme: the dark sidebar's search field and pin buttons use the sidebar's own roles",
          "[qt_quick]") {
    Scene scene;
    morph::ui::ShellFrameOptions options;
    options.applicationName = "App";
    options.navigator.entries = {morph::ui::CatalogEntry{.id = "lims.review", .title = "Review", .group = "Lab"}};
    options.navigator.pins = {"lims.review"};
    morph::ui::ShellFrame frame{scene.backend(), nullptr, std::move(options)};
    frame.showSignedIn(true);
    scene.settle();
    QObject const* const theme = themeOf(scene);
    auto* const sidebar = theme->property("sidebarScheme").value<QObject*>();
    REQUIRE(sidebar != nullptr);

    auto const field = fieldBackground("navigator_search").property("color").value<QColor>();
    auto const text = sidebar->property("text").value<QColor>();
    CHECK(ui::detail::contrastRatio(rgbaOf(text), rgbaOf(field)) >= 4.5);
    CHECK(field == sidebar->property("controlSurface").value<QColor>());

    auto const backgroundOf = [](char const* testId) {
        morph::qt_quick::detail::ItemBase const* const widget =
            morph::qt_quick::detail::findWidget(QString::fromLatin1(testId));
        REQUIRE(widget != nullptr);
        auto* const control = widget->item()->property("focusTarget").value<QQuickItem*>();
        REQUIRE(control != nullptr);
        auto* const background = control->property("background").value<QQuickItem*>();
        REQUIRE(background != nullptr);
        return background;
    };
    // A hovered navigator entry (a list item) fills with the sidebar's hover colour, which shows on the dark column.
    QQuickItem* const entry = backgroundOf("nav_lims.review");
    REQUIRE(entry->setProperty("hovered", true));
    auto const hover = entry->property("color").value<QColor>();
    CHECK(hover == sidebar->property("hover").value<QColor>());
    CHECK(hover.alpha() > 0);

    // A pin is a push button: it rests on the sidebar's control fill, outlined in the sidebar's control border.
    QQuickItem const* const pin = backgroundOf("pin_lims.review");
    CHECK(pin->property("color").value<QColor>() == sidebar->property("controlSurface").value<QColor>());
    CHECK(borderColorOf(*pin) == sidebar->property("controlBorder").value<QColor>());
}

// Mutation: leave one role out of a built-in scheme in Theme.qml (it keeps MorphScheme's default, not the theme's).
TEST_CASE("qt_quick theme: every built-in scheme sets every colour role MorphScheme declares", "[qt_quick]") {
    Scene scene;
    ui::ThemeValues values = ui::themeValues();
    std::uint8_t step = 0;
    for (auto& [path, color] : values.colors) {
        step = static_cast<std::uint8_t>(step + 7);
        color = ui::Rgba{.r = static_cast<std::uint8_t>(31 + step),
                         .g = static_cast<std::uint8_t>(250 - step),
                         .b = static_cast<std::uint8_t>(step * 3U),
                         .a = 255};
    }
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), values).empty());
    QObject* const theme = themeOf(scene);
    QVariant made;
    QMetaObject::invokeMethod(theme, "schemeObject", Qt::DirectConnection, qReturnArg(made),
                              QVariant{QStringLiteral("probe")});
    auto* const probe = made.value<QObject*>();
    REQUIRE(probe != nullptr);
    int roles = 0;
    for (char const* const name :
         {"themeScheme", "pageScheme", "cardScheme", "sidebarScheme", "toolbarScheme", "barScheme"}) {
        auto* const scheme = theme->property(name).value<QObject*>();
        REQUIRE(scheme != nullptr);
        QMetaObject const* const meta = probe->metaObject();
        for (int index = 0; index < meta->propertyCount(); ++index) {
            QMetaProperty const property = meta->property(index);
            if (property.metaType() != QMetaType::fromType<QColor>()) {
                continue;
            }
            ++roles;
            INFO(name << "." << property.name());
            CHECK(scheme->property(property.name()).value<QColor>() !=
                  probe->property(property.name()).value<QColor>());
        }
    }
    // Six schemes, each with every role: more than the roles of any one.
    CHECK(roles >= 6 * 35);
}

namespace {

/// How many pixels inside @p item's rectangle of @p image are dark enough to be text on a light fill.
int darkPixelsIn(QImage const& image, QQuickItem const& item) {
    QRectF const area = item.mapRectToScene(QRectF{0, 0, item.width(), item.height()});
    int dark = 0;
    for (int y = static_cast<int>(area.top()); y < static_cast<int>(area.bottom()); ++y) {
        for (int x = static_cast<int>(area.left()) + 6; x < static_cast<int>(area.right()); ++x) {
            dark += image.pixelColor(x, y).lightness() < 120 ? 1 : 0;
        }
    }
    return dark;
}

}  // namespace

// Mutation: drop `contentColumn.parent = button` in MorphButton.qml (the content stays under the button's own fill,
// and a selected item's text disappears).
TEST_CASE("qt_quick theme: a selected or hovered item's fill is drawn beneath its content", "[qt_quick]") {
    Scene scene;
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), brandTheme()).empty());
    scene.window().resize(300, 200);
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto item = scene.backend().createButton(column.get());
    item->setStyle(ui::ButtonStyle::Item);
    item->setLabel("Entry");
    item->setOnClick([] {});
    auto text = scene.backend().createText(&item->content());
    text->setText("Impurity B, 0.12 %");
    scene.settle();

    QQuickItem const& itemItem = Scene::itemOf(*item);
    auto* const background = &buttonBackground(*item);
    int const rest = darkPixelsIn(scene.window().grabWindow(), itemItem);
    CHECK(rest > 20);

    REQUIRE(background->setProperty("hovered", true));
    scene.settle();
    CHECK(darkPixelsIn(scene.window().grabWindow(), itemItem) > 20);

    REQUIRE(background->setProperty("hovered", false));
    item->setSelected(true);
    scene.settle();
    CHECK(darkPixelsIn(scene.window().grabWindow(), itemItem) > 20);
}

namespace {

/// A theme with a red control border and the surfaces `well` (solid, yellow) and `empty` (dashed, yellow), so a
/// control on either shows where its outline and its surface are drawn.
std::map<std::string, ui::ThemeValues, std::less<>> wellSurfaces() {
    auto const solid = ui::parseTokens(R"({"color": {"$type": "color", "surface": {"$value": "#ffee88"},
        "controlBorder": {"$value": "#ff0000"}}})");
    auto const dashed = ui::parseTokens(R"({"color": {"$type": "color", "surface": {"$value": "#ffee88"},
        "controlBorder": {"$value": "#ff0000"}},
      "border": {"style": {"$type": "strokeStyle", "$value": "dashed"}}})");
    REQUIRE(solid);
    REQUIRE(dashed);
    auto const solidResolved = ui::resolveTokens({&*solid});
    auto const dashedResolved = ui::resolveTokens({&*dashed});
    REQUIRE(solidResolved);
    REQUIRE(dashedResolved);
    return {{"well", ui::themeValues(*solidResolved)}, {"empty", ui::themeValues(*dashedResolved)}};
}

/// The count of red pixels along the row of device pixels @p row of @p image, from @p first to @p last, and how many
/// runs of them there are.
struct RedRuns {
    int pixels = 0;
    int runs = 0;
};

RedRuns redAlong(QImage const& image, int row, int first, int last) {
    RedRuns found;
    bool inside = false;
    for (int x = first; x < last; ++x) {
        QColor const color = image.pixelColor(x, row);
        bool const red = color.red() > 200 && color.green() < 80 && color.blue() < 80;
        found.pixels += red ? 1 : 0;
        found.runs += red && !inside ? 1 : 0;
        inside = red;
    }
    return found;
}

}  // namespace

// Mutation: leave `radius: 0` on the surface's background in MorphWidget (the corner reads the surface's yellow).
TEST_CASE("qt_quick theme: a round button draws its surface with its own shape", "[qt_quick]") {
    Scene scene;
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(), wellSurfaces()).empty());
    scene.window().resize(300, 200);
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto const well = scene.backend().createButton(column.get());
    well->setLabel("S1");
    well->setSurface("well");
    well->setShape(ui::ControlShape::Round);
    well->setSize(ui::ButtonSize::Small);
    well->setOnClick([] {});
    scene.settle();
    QQuickItem const& item = Scene::itemOf(*well);
    QQuickItem const* const background = surfaceBackground(item);
    REQUIRE(background != nullptr);
    CHECK(background->property("radius").toReal() == item.height() / 2);
    CHECK(item.width() >= item.height());
    QImage const image = scene.window().grabWindow();
    QPointF const origin = item.mapToScene(QPointF{1.0, 1.0});
    CHECK(image.pixelColor(static_cast<int>(origin.x()), static_cast<int>(origin.y())) != QColor{"#ffee88"});
}

// Mutation: leave `radius: height / 2` out of a round badge's fill (it keeps the tag's corners).
TEST_CASE("qt_quick theme: a round badge is a circle or a stadium, in the mono face when asked", "[qt_quick]") {
    Scene scene;
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues()).empty());
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto const badge = scene.backend().createBadge(column.get());
    badge->setText("QC");
    badge->setStyle(ui::BadgeStyle::Outline);
    badge->setShape(ui::ControlShape::Round);
    badge->setMono(true);
    scene.settle();
    QQuickItem const& item = Scene::itemOf(*badge);
    QQuickItem const* const fill = item.findChild<QQuickItem*>(QStringLiteral("fill"));
    QQuickItem const* const label = item.findChild<QQuickItem*>(QStringLiteral("label"));
    REQUIRE(fill != nullptr);
    REQUIRE(label != nullptr);
    CHECK(fill->property("radius").toReal() == fill->height() / 2);
    CHECK(item.width() >= item.height());
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    auto const font = label->property("font").value<QFont>();
    CHECK(font.family() == theme->property("monoFont").value<QFont>().family());
}

// Mutation: leave the padding at the button's own (the label elides in the six-unit track) or ignore `small` (the
// same).
TEST_CASE("qt_quick theme: a small mono button keeps its label whole in a narrow track, a normal one elides it",
          "[qt_quick]") {
    Scene scene;
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues()).empty());
    scene.window().resize(400, 200);
    auto const grid = scene.backend().createGrid(nullptr);
    grid->setTracks({{.width = ui::Sizing::fixed(6), .align = ui::Align::Start},
                     {.width = ui::Sizing::fixed(6), .align = ui::Align::Start}});
    auto const small = scene.backend().createButton(grid.get());
    small->setLabel("STD1");
    small->setShape(ui::ControlShape::Round);
    small->setSize(ui::ButtonSize::Small);
    small->setMono(true);
    auto const normal = scene.backend().createButton(grid.get());
    normal->setLabel("STD1");
    normal->setShape(ui::ControlShape::Round);
    normal->setMono(true);
    scene.settle();
    auto const truncated = [](QQuickItem& item) {
        QList<QQuickItem*> const texts = Scene::itemsOf(item, "QQuickText");
        REQUIRE_FALSE(texts.isEmpty());
        return texts.front()->property("truncated").toBool();
    };
    CHECK_FALSE(truncated(Scene::itemOf(*small)));
    CHECK(truncated(Scene::itemOf(*normal)));
}

// Mutation: read the token and ignore it (no outline is drawn dashed), or draw the solid border as well.
TEST_CASE("qt_quick theme: a button on a dashed surface is outlined in dashes, not in a border",
          "[qt_quick][dashed]") {
    Scene scene;
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(), wellSurfaces()).empty());
    scene.window().resize(300, 200);
    auto const column = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto const dashed = scene.backend().createButton(column.get());
    dashed->setLabel("A wide caption for the outline");
    dashed->setSurface("empty");
    auto const solid = scene.backend().createButton(column.get());
    solid->setLabel("A wide caption for the outline");
    solid->setSurface("well");
    scene.settle();
    auto const* const outline = buttonBackground(*dashed).findChild<QQuickItem*>(QStringLiteral("dashedOutline"));
    REQUIRE(outline != nullptr);
    CHECK(outline->isVisible());
    CHECK(buttonBackground(*dashed).property("border").value<QObject*>()->property("width").toReal() == 0.0);
    CHECK(outline->width() == buttonBackground(*dashed).width());
    CHECK(outline->height() == buttonBackground(*dashed).height());
    auto const* const none = buttonBackground(*solid).findChild<QQuickItem*>(QStringLiteral("dashedOutline"));
    REQUIRE(none != nullptr);
    CHECK_FALSE(none->isVisible());
    CHECK(buttonBackground(*solid).property("border").value<QObject*>()->property("width").toReal() == 1.0);

    // The stroke has gaps along the top edge, at the window's device resolution.
    QImage const image = scene.window().grabWindow();
    qreal const ratio = image.devicePixelRatio();
    QQuickItem const& item = Scene::itemOf(*dashed);
    QPointF const corner = item.mapToScene(QPointF{0.0, 0.0});
    int const row = static_cast<int>((corner.y() + 0.5) * ratio);
    int const first = static_cast<int>(corner.x() * ratio) + static_cast<int>(10 * ratio);
    int const last = static_cast<int>((corner.x() + item.width()) * ratio) - static_cast<int>(10 * ratio);
    RedRuns const edge = redAlong(image, row, first, last);
    INFO("device pixel ratio " << ratio << ", " << edge.pixels << " red pixels in " << edge.runs << " runs");
    CHECK(edge.runs >= 3);
    CHECK(edge.pixels < (last - first) * 3 / 4);
    CHECK(edge.pixels > (last - first) / 3);
}

// Mutation: leave `shape.iconOnly` and `font.overline.case` out of `kThemeTokens`, and both read their defaults.
TEST_CASE("qt_quick theme: the theme's keywords reach the Theme singleton", "[qt_quick]") {
    Scene scene;
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues()).empty());
    CHECK_FALSE(theme->property("iconOnlySquare").toBool());
    CHECK_FALSE(theme->property("overlineCaps").toBool());

    auto const tokens = ui::parseTokens(R"({"shape": {"iconOnly": {"$type": "keyword", "$value": "square"}},
        "font": {"overline": {"case": {"$type": "keyword", "$value": "uppercase"}}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved)).empty());
    CHECK(theme->property("iconOnlySquare").toBool());
    CHECK(theme->property("overlineCaps").toBool());
}

// Mutation: leave `collapsible.marker` out of `kThemeTokens` (a bundle's `none` is dropped and the marker stays
// shown).
TEST_CASE("qt_quick theme: the collapsible marker reaches the Theme singleton", "[qt_quick]") {
    Scene scene;
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues()).empty());
    CHECK(theme->property("collapsibleMarker").toBool());

    auto const tokens = ui::parseTokens(R"({"collapsible": {"marker": {"$type": "keyword", "$value": "none"}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved)).empty());
    CHECK_FALSE(theme->property("collapsibleMarker").toBool());
}

// Mutation: skip the write (the glyph table stays empty, so a menu trigger and an icon-only button have no glyph).
TEST_CASE("qt_quick theme: the framework's glyphs reach Theme.iconGlyphs whether or not the bundle has an icon set",
          "[qt_quick]") {
    Scene scene;
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    CHECK(morph::qt_quick::applyBundleTheme(scene.engine(), {}).empty());
    QVariantMap const glyphs = theme->property("iconGlyphs").toMap();
    CHECK(glyphs.value(QStringLiteral("more")).toString() == QStringLiteral("⋯"));
    CHECK(glyphs.value(QStringLiteral("nav.menu")).toString() == QStringLiteral("☰"));
    CHECK(glyphs.value(QStringLiteral("close")).toString() == QStringLiteral("×"));
    CHECK_FALSE(glyphs.contains(QStringLiteral("empty")));
}

namespace {

/// The SVG text of a `data:image/svg+xml;base64,` URL.
QString svgOf(QString const& url) {
    QString const prefix = QStringLiteral("data:image/svg+xml;base64,");
    REQUIRE(url.startsWith(prefix));
    return QString::fromUtf8(QByteArray::fromBase64(url.mid(prefix.size()).toLatin1()));
}

std::string dotIcon() {
    return R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 8"><circle cx="4" cy="4" r="3" fill="currentColor"/></svg>)";
}

}  // namespace

// Mutation: answer `iconSource(name)` whatever the colour (the SVG keeps `currentColor` and Qt draws it black).
TEST_CASE("qt_quick theme: an icon that paints currentColor is drawn in its control's colour", "[qt_quick]") {
    Scene scene;
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    std::string const plain =
        R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 8 8"><path d="M1 4l2 2 4-4"/></svg>)";
    REQUIRE(morph::qt_quick::applyBundleTheme(scene.engine(), {}, {{"dot", dotIcon()}, {"tick", plain}}).empty());

    QString tinted;
    REQUIRE(QMetaObject::invokeMethod(theme, "iconSourceIn", Q_RETURN_ARG(QString, tinted),
                                      Q_ARG(QString, QStringLiteral("dot")), Q_ARG(QColor, QColor{"#ffffff"})));
    QString const svg = svgOf(tinted);
    CHECK(svg.contains(QStringLiteral("fill=\"#ffffff\"")));
    CHECK_FALSE(svg.contains(QStringLiteral("currentColor")));

    // An icon without currentColor is drawn as the set gives it, and a name the set lacks has no source.
    QString untouched;
    REQUIRE(QMetaObject::invokeMethod(theme, "iconSourceIn", Q_RETURN_ARG(QString, untouched),
                                      Q_ARG(QString, QStringLiteral("tick")), Q_ARG(QColor, QColor{"#ffffff"})));
    QString plainUrl;
    REQUIRE(QMetaObject::invokeMethod(theme, "iconSource", Q_RETURN_ARG(QString, plainUrl),
                                      Q_ARG(QString, QStringLiteral("tick"))));
    CHECK(untouched == plainUrl);
    QString missing = QStringLiteral("x");
    REQUIRE(QMetaObject::invokeMethod(theme, "iconSourceIn", Q_RETURN_ARG(QString, missing),
                                      Q_ARG(QString, QStringLiteral("absent")), Q_ARG(QColor, QColor{"#ffffff"})));
    CHECK(missing.isEmpty());
}

// Mutation: draw the icon from `iconSource(name)` in the button (the circle is black whatever the accent).
TEST_CASE("qt_quick theme: a ghost button draws a currentColor icon in its caption colour", "[qt_quick]") {
    Scene scene;
    REQUIRE(morph::qt_quick::applyBundleTheme(scene.engine(), {}, {{"dot", dotIcon()}}).empty());
    auto const tokens = ui::parseTokens(R"({"color": {"$type": "color", "accent": {"$value": "#ff0000"}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved)).empty());
    auto button = scene.backend().createButton(nullptr);
    button->setIcon("dot");
    button->setEmphasis(ui::ButtonEmphasis::Ghost);
    scene.settle();
    QQuickItem const* const icon = Scene::itemOf(*button).findChild<QQuickItem*>(QStringLiteral("contentIcon"));
    REQUIRE(icon != nullptr);
    // The image decodes its data URL off the GUI thread.
    for (int tries = 0; tries < 200 && !icon->isVisible(); ++tries) {
        QTest::qWait(10);
        scene.settle();
    }
    REQUIRE(icon->isVisible());
    QImage const image = scene.window().grabWindow();
    QPointF const centre = icon->mapToScene(QPointF{icon->width() / 2, icon->height() / 2});
    qreal const ratio = image.devicePixelRatio();
    QColor const pixel = image.pixelColor(static_cast<int>(centre.x() * ratio), static_cast<int>(centre.y() * ratio));
    INFO("pixel " << pixel.name().toStdString());
    CHECK(pixel.red() > 200);
    CHECK(pixel.green() < 60);
    CHECK(pixel.blue() < 60);
}

// Mutation: answer `barScheme` for `toolbarJoined` (the first check fails), or give `toolbarJoinedScheme` roles of its
// own that are not bound to the bar's (the second fails).
TEST_CASE("qt_quick theme: the joined top bar's scheme is its own and follows the bar's", "[qt_quick]") {
    Scene scene;
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    auto const schemeOf = [theme](char const* name) {
        QVariant made;
        REQUIRE(QMetaObject::invokeMethod(theme, "schemeObject", Qt::DirectConnection, qReturnArg(made),
                                          QVariant{QString::fromLatin1(name)}));
        auto* const found = made.value<QObject*>();
        REQUIRE(found != nullptr);
        return found;
    };
    CHECK(schemeOf("toolbarJoined")->property("surface").value<QColor>() ==
          schemeOf("bar")->property("surface").value<QColor>());
    {
        std::map<std::string, std::string, std::less<>> const themes{
            {"theme/index.json",
             R"({"vocab": "ui-theme/1", "base": "theme/base.json", "surfaces": {"toolbarJoined": "theme/joined.json"}})"},
            {"theme/base.json", R"({"color": {"$type": "color", "accent": {"$value": "#aa0055"}}})"},
            {"theme/joined.json", R"({"color": {"$type": "color", "surface": {"$value": "#EEEEEE"}}})"}};
        REQUIRE(morph::qt_quick::applyBundleTheme(scene.engine(), themes).empty());
        CHECK(schemeOf("toolbarJoined")->property("surface").value<QColor>() == QColor{"#EEEEEE"});
        CHECK(schemeOf("bar")->property("surface").value<QColor>() != QColor{"#EEEEEE"});
    }
    {
        std::map<std::string, std::string, std::less<>> const themes{
            {"theme/index.json",
             R"({"vocab": "ui-theme/1", "base": "theme/base.json", "surfaces": {"bar": "theme/bar.json"}})"},
            {"theme/base.json", R"({"color": {"$type": "color", "accent": {"$value": "#aa0055"}}})"},
            {"theme/bar.json", R"({"color": {"$type": "color", "surface": {"$value": "#DDDDDD"}}})"}};
        REQUIRE(morph::qt_quick::applyBundleTheme(scene.engine(), themes).empty());
        CHECK(schemeOf("bar")->property("surface").value<QColor>() == QColor{"#DDDDDD"});
        CHECK(schemeOf("toolbarJoined")->property("surface").value<QColor>() == QColor{"#DDDDDD"});
    }
}

// Mutation: leave the surface's `listSelectionFill` unwritten (the sidebar keeps the theme's), or write the theme's
// value to every surface (the theme's own scheme turns `fill`).
TEST_CASE("qt_quick theme: a surface's list selection is its own and the theme's scheme keeps accent", "[qt_quick]") {
    Scene scene;
    auto const sidebarTokens = ui::parseTokens(R"({"list": {"selection": {"$type": "keyword", "$value": "fill"}}})");
    REQUIRE(sidebarTokens);
    auto const sidebarResolved = ui::resolveTokens({&*sidebarTokens});
    REQUIRE(sidebarResolved);
    std::map<std::string, ui::ThemeValues, std::less<>> const surfaces{{"sidebar", ui::themeValues(*sidebarResolved)}};
    CHECK(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(), surfaces).empty());
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    auto* const sidebar = theme->property("sidebarScheme").value<QObject*>();
    auto* const own = theme->property("themeScheme").value<QObject*>();
    auto* const page = theme->property("pageScheme").value<QObject*>();
    REQUIRE((sidebar != nullptr && own != nullptr && page != nullptr));
    CHECK(sidebar->property("listSelectionFill").toBool());
    CHECK_FALSE(own->property("listSelectionFill").toBool());
    CHECK_FALSE(page->property("listSelectionFill").toBool());
}

// Mutation: leave a built-in surface's `listSelectionFill` unbound (the sidebar or the bar keeps accent under a theme
// that fills).
TEST_CASE("qt_quick theme: a theme's list selection reaches every built-in surface that does not set its own",
          "[qt_quick]") {
    Scene scene;
    auto const tokens = ui::parseTokens(R"({"list": {"selection": {"$type": "keyword", "$value": "fill"}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    CHECK(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved)).empty());
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    for (char const* const name : {"themeScheme", "pageScheme", "cardScheme", "sidebarScheme", "toolbarScheme",
                                   "barScheme", "toolbarJoinedScheme"}) {
        auto* const scheme = theme->property(name).value<QObject*>();
        REQUIRE(scheme != nullptr);
        INFO(name);
        CHECK(scheme->property("listSelectionFill").toBool());
    }
}

// Mutation: leave `input.paddingX` out of the dimension table in theme.cpp (the bundle's 10 never reaches Theme).
TEST_CASE("qt_quick theme: the input inset reaches the Theme singleton", "[qt_quick]") {
    Scene scene;
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues()).empty());
    CHECK(theme->property("inputPaddingX").toReal() == 8.0);
    auto const tokens =
        ui::parseTokens(R"({"input": {"paddingX": {"$type": "dimension", "$value": {"value": 10, "unit": "px"}}}})");
    REQUIRE(tokens);
    auto const resolved = ui::resolveTokens({&*tokens});
    REQUIRE(resolved);
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), ui::themeValues(*resolved)).empty());
    CHECK(theme->property("inputPaddingX").toReal() == 10.0);
}
