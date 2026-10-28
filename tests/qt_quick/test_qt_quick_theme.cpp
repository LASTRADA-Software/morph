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
#include <QObject>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <morph/qt_quick/backend.hpp>
#include <morph/qt_quick/components.hpp>
#include <morph/qt_quick/theme.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/theme.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

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

TEST_CASE("qt_quick theme: a tab's label takes the scheme colour under every style", "[qt_quick]") {
    Scene scene;
    REQUIRE(morph::qt_quick::applyTheme(scene.engine(), brandTheme()).empty());
    ui::Mounted const mounted{scene.runtime(), scene.backend(),
                              ui::tabs({.tabs = {{.label = "First", .node = ui::text({.text = "one"})},
                                                 {.label = "Second", .node = ui::text({.text = "two"})}},
                                        .selected = std::size_t{0}})};
    scene.settle();
    QQuickItem const& item = Scene::itemOf(mounted.root());
    CHECK(labelColor(item, QStringLiteral("First")) == QColor{"#aa0055"});
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
    CHECK(fill->property("border").value<QObject*>()->property("color").value<QColor>() == QColor{"#234567"});
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

namespace {

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

}  // namespace

// Mutation: drop the bar's `contentItem` (the style's own fill draws, and no item is named `fill`), or colour the
// fill with anything but the scheme's accent.
TEST_CASE("qt_quick theme: a progress bar fills with the accent on the neutral track", "[qt_quick]") {
    Scene scene;
    auto const tokens = ui::parseTokens(R"({"color": {"$type": "color",
        "accent": {"$value": "#17784a"}, "neutral": {"$value": "#d0d4d8"}}})");
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
        "surface": {"$value": "#fefefe"}, "background": {"$value": "#e9ecef"}}})");
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
QQuickItem const& buttonBackground(ui::ButtonWidget const& button) {
    auto const* const control = Scene::itemOf(button).property("focusTarget").value<QQuickItem*>();
    REQUIRE(control != nullptr);
    auto const* const background = control->property("background").value<QQuickItem*>();
    REQUIRE(background != nullptr);
    return *background;
}

}  // namespace

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
    QColor const shown = buttonBackground(*picked).property("color").value<QColor>();
    CHECK(shown.rgb() == tint.rgb());
    CHECK(std::abs(shown.alphaF() - 0.12F) < 0.01F);
    auto* const theme = scene.engine().singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    REQUIRE(theme != nullptr);
    CHECK(buttonBackground(*plain).property("color").value<QColor>() ==
          theme->property("surface").value<QColor>());

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
    auto const tokens = ui::parseTokens(R"({"color": {"$type": "color", "border": {"$value": "#999999"},
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
    QQuickItem const* const description =
        Scene::itemOf(*toned).findChild<QQuickItem*>(QStringLiteral("description"));
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
