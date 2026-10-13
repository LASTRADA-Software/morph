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
