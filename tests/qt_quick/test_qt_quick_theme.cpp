// SPDX-License-Identifier: Apache-2.0
//
// Theme tokens on Qt Quick (spec 6 §12 "Design package"): a token set reaches a generated screen's widget and a host
// component through the one `Theme` singleton, and a token the theme lacks takes the default.

#include <QColor>
#include <QFile>
#include <QObject>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <map>
#include <memory>
#include <morph/qt_quick/backend.hpp>
#include <morph/qt_quick/components.hpp>
#include <morph/qt_quick/theme.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/theme.hpp>
#include <morph/ui/view.hpp>
#include <string_view>

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
QColor labelColor(QQuickItem const& item, QString const& text) {
    for (QObject const* const child : item.findChildren<QObject*>()) {
        if (child->property("text").toString() == text && child->property("color").isValid()) {
            return child->property("color").value<QColor>();
        }
    }
    FAIL("no label with the text " << text.toStdString());
    return {};
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
    Scene scene{morph::qt_quick::BackendOptions{.components = &components, .onWidthClass = {}}};
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
        {"theme/sidebar.json", R"({"color": {"$type": "color", "surface": {"$value": "#102030"}}})"}};
    CHECK(morph::qt_quick::applyBundleTheme(scene.engine(), themes).empty());
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    CHECK(theme->property("accent").value<QColor>() == QColor{"#aa0055"});
    auto* const sidebar = theme->property("sidebarScheme").value<QObject*>();
    REQUIRE(sidebar != nullptr);
    CHECK(sidebar->property("surface").value<QColor>() == QColor{"#102030"});
    // A role the surface does not set is the theme's: the base's accent.
    CHECK(sidebar->property("accent").value<QColor>() == QColor{"#aa0055"});

    // A bundle without a theme restores the defaults.
    CHECK(morph::qt_quick::applyBundleTheme(scene.engine(), {}).empty());
    CHECK(theme->property("accent").value<QColor>() == QColor{"#1565c0"});
}
