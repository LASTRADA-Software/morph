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

}  // namespace

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
    Scene scene{morph::qt_quick::BackendOptions{.components = &components, .onWidthClass = {}}};
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
