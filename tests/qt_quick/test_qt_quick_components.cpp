// SPDX-License-Identifier: Apache-2.0
//
// Custom components and restyling (spec 3 §6, spec 5 §10): a component receives its props and reports its events;
// a digest that does not match, a compile error, a missing component, a host component below the version asked
// for and a missing prop each leave the fallback in its place and are reported once; a restyle whose component
// lacks the kind's properties is refused when it is mapped.

#include <QByteArray>
#include <QCryptographicHash>
#include <QFile>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QUrl>
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <memory>
#include <morph/qt_quick/backend.hpp>
#include <morph/qt_quick/components.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <utility>
#include <vector>

#include "qt_quick/kinds.hpp"
#include "qt_quick_harness.hpp"

namespace ui = morph::ui;
namespace detail = morph::qt_quick::detail;
using morph::qt_quick::ComponentFailure;
using morph::qt_quick::Components;
using morph::qt_quick::CustomRequest;
using morph::qt_quick::testing::Scene;

namespace {

constexpr char const* kChart = R"(import QtQuick
Item {
    id: root
    property var host: null
    property string title: ""
    property string limit: ""
    property bool live: false
    property real zoom: 1
    implicitWidth: 100
    implicitHeight: 20
    function pick(key) { root.host.fire("pointSelected", key) }
    function pickMany() { root.host.fire("pointsSelected", ["i:1", "i:2"]) }
}
)";

std::string digestOf(std::string const& qml) {
    return "sha256:" +
           QCryptographicHash::hash(QByteArray::fromStdString(qml), QCryptographicHash::Sha256).toHex().toStdString();
}

// A QML file in a temporary directory, for a host component.
QUrl writeFile(QTemporaryDir const& dir, QString const& name, char const* text) {
    QString const path = dir.filePath(name);
    QFile file{path};
    REQUIRE(file.open(QIODevice::WriteOnly));
    file.write(text);
    file.close();
    return QUrl::fromLocalFile(path);
}

// Mounts the custom component, or the fallback where it cannot be loaded, as a renderer does for a `custom` node.
struct Shown {
    std::unique_ptr<morph::qt_quick::CustomWidget> custom;
    std::unique_ptr<ui::Mounted> fallback;
};

Shown show(Scene& scene, CustomRequest const& request) {
    Shown shown;
    shown.custom = scene.backend().createCustom(nullptr, request);
    if (shown.custom == nullptr) {
        shown.fallback = std::make_unique<ui::Mounted>(
            scene.runtime(), scene.backend(), ui::text({.text = "fallback", .common = {.testId = "fallback"}}));
    }
    scene.settle();
    return shown;
}

// The first item under @p root, depth first, that @p matches; null when none does.
// NOLINTNEXTLINE(misc-no-recursion): one level per nesting of the scene's items.
QQuickItem* findItem(QQuickItem* root, std::function<bool(QQuickItem const&)> const& matches) {
    if (root == nullptr) {
        return nullptr;
    }
    if (matches(*root)) {
        return root;
    }
    for (QQuickItem* const child : root->childItems()) {
        if (QQuickItem* const found = findItem(child, matches)) {
            return found;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("qt_quick components: a bundle component receives its props and reports its events", "[qt_quick]") {
    std::vector<ComponentFailure> reported;
    Components components{[&reported](ComponentFailure const& failure) { reported.push_back(failure); }};
    components.addBundle("Chart", kChart, digestOf(kChart));
    Scene scene{morph::qt_quick::BackendOptions{.components = &components, .onWidthClass = {}}};
    Shown const shown = show(scene, CustomRequest{.name = "Chart", .props = {"title", "limit", "live", "zoom"}});
    REQUIRE(shown.custom != nullptr);
    shown.custom->setTextProp("title", "Trend");
    shown.custom->setTextProp("limit", "12.50");
    shown.custom->setBoolProp("live", true);
    shown.custom->setNumberProp("zoom", 2.5);
    std::vector<std::pair<std::string, std::string>> events;
    shown.custom->setOnEvent([&events](std::string event, std::string payload) {
        events.emplace_back(std::move(event), std::move(payload));
    });
    QQuickItem& item = Scene::itemOf(*shown.custom);
    CHECK(item.property("title").toString() == QStringLiteral("Trend"));
    CHECK(item.property("limit").toString() == QStringLiteral("12.50"));
    CHECK(item.property("live").toBool());
    CHECK(item.property("zoom").toDouble() == 2.5);
    QMetaObject::invokeMethod(&item, "pick", Q_ARG(QVariant, QStringLiteral("i:9007199254740993")));
    QMetaObject::invokeMethod(&item, "pickMany");
    for (auto const& [event, payload] : events) {
        UNSCOPED_INFO(event << " " << payload);
    }
    CHECK(events == std::vector<std::pair<std::string, std::string>>{{"pointSelected", "i:9007199254740993"},
                                                                     {"pointsSelected", R"(["i:1","i:2"])"}});
    CHECK(reported.empty());
}

TEST_CASE("qt_quick components: a component that cannot be loaded leaves the fallback, reported once", "[qt_quick]") {
    std::vector<ComponentFailure> reported;
    Components components{[&reported](ComponentFailure const& failure) { reported.push_back(failure); }};
    QTemporaryDir const dir;
    REQUIRE(dir.isValid());
    components.addBundle("Tampered", kChart, digestOf(std::string{kChart} + " "));
    components.addBundle("Broken", "import QtQuick\nItem { property int x: \n",
                         digestOf("import QtQuick\nItem { property int x: \n"));
    components.addHost("Old", 1,
                       writeFile(dir, QStringLiteral("Old.qml"), "import QtQuick\nItem { property var host }\n"));
    components.addBundle("NoLimit", "import QtQuick\nItem { property string title }\n",
                         digestOf("import QtQuick\nItem { property string title }\n"));
    Scene scene{morph::qt_quick::BackendOptions{.components = &components, .onWidthClass = {}}};

    struct Expectation {
        CustomRequest request;
        std::string reason;
    };
    std::vector<Expectation> const cases{
        {.request = {.name = "Tampered"}, .reason = "component Tampered does not match its digest"},
        {.request = {.name = "Broken"}, .reason = "component Broken does not compile"},
        {.request = {.name = "Missing"}, .reason = "component Missing is not registered"},
        {.request = {.name = "Old", .minVersion = 2}, .reason = "host component Old is version 1, below the 2"},
        {.request = {.name = "NoLimit", .props = {"title", "limit"}},
         .reason = "component NoLimit does not declare prop limit"},
    };
    for (Expectation const& expected : cases) {
        INFO(expected.request.name);
        reported.clear();
        Shown const first = show(scene, expected.request);
        Shown const second = show(scene, expected.request);
        CHECK(first.custom == nullptr);
        REQUIRE(first.fallback != nullptr);
        CHECK(detail::baseOf(first.fallback->root())->shownText() == "fallback");
        CHECK(detail::baseOf(first.fallback->root())->item()->isVisible());
        for (ComponentFailure const& failure : reported) {
            UNSCOPED_INFO(failure.name << ": " << failure.reason);
        }
        REQUIRE(reported.size() == 1);
        CHECK(reported.at(0).name == expected.request.name);
        CHECK(reported.at(0).reason.starts_with(expected.reason));
    }
    CHECK(components.failures().size() == cases.size());
}

TEST_CASE("qt_quick components: without components every custom node shows its fallback", "[qt_quick]") {
    Scene scene;
    Shown const shown = show(scene, CustomRequest{.name = "Chart"});
    CHECK(shown.custom == nullptr);
    CHECK(shown.fallback != nullptr);
}

TEST_CASE("qt_quick components: a host component loads into the backend's engine at the version asked for",
          "[qt_quick]") {
    Components components;
    QTemporaryDir const dir;
    REQUIRE(dir.isValid());
    components.addHost(
        "Grid", 3,
        writeFile(dir, QStringLiteral("Grid.qml"),
                  "import QtQuick\nItem { property var host; property string title; implicitWidth: 5 }\n"));
    Scene scene{morph::qt_quick::BackendOptions{.components = &components, .onWidthClass = {}}};
    Shown const shown = show(scene, CustomRequest{.name = "Grid", .props = {"title"}, .minVersion = 3});
    REQUIRE(shown.custom != nullptr);
    CHECK(qmlEngine(&Scene::itemOf(*shown.custom)) == &scene.engine());
    CHECK(components.failures().empty());
}

TEST_CASE("qt_quick components: a restyle maps a kind to a component that declares the kind's API", "[qt_quick]") {
    std::vector<ComponentFailure> reported;
    Components components{[&reported](ComponentFailure const& failure) { reported.push_back(failure); }};
    QTemporaryDir const dir;
    REQUIRE(dir.isValid());
    components.addHost("BrandButton", 1,
                       writeFile(dir, QStringLiteral("BrandButton.qml"),
                                 "import QtQuick\nimport MorphUi\nMorphButton { property bool brand: true }\n"));
    components.addHost("PlainButton", 1,
                       writeFile(dir, QStringLiteral("PlainButton.qml"),
                                 "import QtQuick\nItem { property var host; property string text }\n"));
    QQmlEngine probeEngine;

    auto refused = components.restyle(probeEngine, "button", "PlainButton");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().name == "button");
    CHECK(refused.error().reason.contains("does not declare"));
    CHECK(refused.error().reason.contains("label"));
    CHECK_FALSE(components.restyle(probeEngine, "column", "BrandButton").has_value());
    CHECK_FALSE(components.restyle(probeEngine, "button", "Nowhere").has_value());
    REQUIRE(components.restyle(probeEngine, "button", "BrandButton").has_value());

    Scene scene{morph::qt_quick::BackendOptions{.components = &components, .onWidthClass = {}}};
    auto button = scene.backend().createButton(nullptr);
    button->setLabel("Pay");
    int clicks = 0;
    button->setOnClick([&clicks] { ++clicks; });
    scene.settle();
    QQuickItem const& item = Scene::itemOf(*button);
    CHECK(item.property("brand").toBool());
    CHECK(item.property("label").toString() == QStringLiteral("Pay"));
    scene.click(item);
    CHECK(clicks == 1);
    // A kind without a restyle keeps its MorphUi component.
    auto text = scene.backend().createText(nullptr);
    CHECK_FALSE(Scene::itemOf(*text).property("brand").isValid());
}

// Mutation: drop the `textArea` entry from the restylable kinds (the restyle is refused and the area is MorphUi's).
TEST_CASE("qt_quick components: a textArea restyle is what a multi-line input is made of", "[qt_quick]") {
    std::vector<ComponentFailure> reported;
    Components components{[&reported](ComponentFailure const& failure) { reported.push_back(failure); }};
    QTemporaryDir const dir;
    REQUIRE(dir.isValid());
    components.addHost("BrandArea", 1,
                       writeFile(dir, QStringLiteral("BrandArea.qml"),
                                 "import QtQuick\nimport MorphUi\nMorphTextArea { property bool brand: true }\n"));
    components.addHost("PlainArea", 1,
                       writeFile(dir, QStringLiteral("PlainArea.qml"),
                                 "import QtQuick\nimport MorphUi\n"
                                 "MorphField { property string placeholder; property bool edited; "
                                 "property bool assigning }\n"));
    QQmlEngine probeEngine;
    auto refused = components.restyle(probeEngine, "textArea", "PlainArea");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().reason.contains("does not declare"));
    CHECK(refused.error().reason.contains("assign"));
    REQUIRE(components.restyle(probeEngine, "textArea", "BrandArea").has_value());

    Scene scene{morph::qt_quick::BackendOptions{.components = &components, .onWidthClass = {}}};
    auto area = scene.backend().createTextInput(nullptr, ui::TextInputMode::Multiline);
    auto line = scene.backend().createTextInput(nullptr, ui::TextInputMode::SingleLine);
    std::vector<std::string> edits;
    area->setOnChange([&edits](std::string const& text) { edits.push_back(text); });
    area->setText("first");
    scene.settle();
    CHECK(Scene::itemOf(*area).property("brand").toBool());
    CHECK_FALSE(Scene::itemOf(*line).property("brand").isValid());
    CHECK(Scene::controlOf(*area).property("text").toString() == QStringLiteral("first"));
    scene.click(Scene::controlOf(*area));
    scene.enter(QStringLiteral("x"));
    REQUIRE_FALSE(edits.empty());
    CHECK(edits.back().contains('x'));
    CHECK(reported.size() == 1);  // the refusal above
}

// Mutation: make the row's background from the stock MorphUi component instead of the context's (the restyle's
// marker and height never show); or stop writing `alternate` (the stripes do not follow the move).
TEST_CASE("qt_quick components: a tableRow restyle draws every row's background, and the row takes its height",
          "[qt_quick]") {
    std::vector<ComponentFailure> reported;
    Components components{[&reported](ComponentFailure const& failure) { reported.push_back(failure); }};
    QTemporaryDir const dir;
    REQUIRE(dir.isValid());
    components.addHost("BrandRow", 1,
                       writeFile(dir, QStringLiteral("BrandRow.qml"),
                                 "import QtQuick\nimport MorphUi\n"
                                 "MorphTableRowBackground { property bool brand: true; implicitHeight: 40 }\n"));
    components.addHost("PlainRow", 1,
                       writeFile(dir, QStringLiteral("PlainRow.qml"),
                                 "import QtQuick\nItem { property bool selected; property bool hovered }\n"));
    QQmlEngine probeEngine;
    auto refused = components.restyle(probeEngine, "tableRow", "PlainRow");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().name == "tableRow");
    CHECK(refused.error().reason.contains("does not declare"));
    CHECK(refused.error().reason.contains("alternate"));
    REQUIRE(components.restyle(probeEngine, "tableRow", "BrandRow").has_value());

    Scene scene{morph::qt_quick::BackendOptions{.components = &components, .onWidthClass = {}}};
    auto table = scene.backend().createTable(nullptr);
    table->setColumns({{.label = "Name"}});
    std::vector<std::unique_ptr<ui::StackWidget>> rows;
    std::vector<std::unique_ptr<ui::TextWidget>> cells;
    for (std::string const name : {"a", "b", "c"}) {
        rows.push_back(scene.backend().createStack(table.get(), ui::Axis::Horizontal));
        table->setRowKey(*rows.back(), ui::Key{name});
        cells.push_back(scene.backend().createText(rows.back().get()));
        cells.back()->setText(name);
    }
    table->setSelection({ui::Key{std::string{"b"}}});
    scene.settle();
    auto background = [](ui::StackWidget const& row) -> QObject& {
        auto* const found = Scene::itemOf(row).property("backgroundItem").value<QObject*>();
        REQUIRE(found != nullptr);
        return *found;
    };
    auto alternates = [&rows] {
        std::vector<bool> out;
        for (auto const& row : rows) {
            out.push_back(Scene::itemOf(*row).property("alternate").toBool());
        }
        return out;
    };
    for (auto const& row : rows) {
        CHECK(background(*row).property("brand").toBool());
        CHECK(Scene::itemOf(*row).implicitHeight() == 40.0);
    }
    CHECK(background(*rows.at(1)).property("selected").toBool());
    CHECK_FALSE(background(*rows.at(0)).property("selected").toBool());
    CHECK(alternates() == std::vector<bool>{false, true, false});
    CHECK(background(*rows.at(1)).property("alternate").toBool());
    // c moves first: c, a, b.
    table->moveChild(*rows.at(2), 0);
    scene.settle();
    CHECK(alternates() == std::vector<bool>{true, false, false});
    // a goes: c, b.
    rows.erase(rows.begin());
    scene.settle();
    CHECK(alternates() == std::vector<bool>{true, false});
    CHECK(reported.size() == 1);  // the refusal above, and nothing since
}

// Mutation: mount the fallback without asking the backend (the bundle component is never shown).
TEST_CASE(
    "qt_quick components: a custom node mounted through the view tree shows its component, with its props "
    "bound and its events handed back",
    "[qt_quick]") {
    std::vector<ComponentFailure> reported;
    Components components{[&reported](ComponentFailure const& failure) { reported.push_back(failure); }};
    components.addBundle("Chart", kChart, digestOf(kChart));
    Scene scene{morph::qt_quick::BackendOptions{.components = &components, .onWidthClass = {}}};
    std::vector<std::pair<std::string, std::string>> events;
    // The props are appended one at a time: copying a `CustomProp` out of a braced initializer list makes GCC 15 at
    // -O2 report the `std::function` inside a `Prop` as maybe uninitialized.
    auto node = [&events](std::string name) {
        ui::Custom custom{.name = std::move(name),
                          .fallback = ui::text({.text = "fallback", .common = {.testId = "fallback"}}),
                          .props = {},
                          .minVersion = 0,
                          .onEvent = [&events](std::string const& event, std::string const& payload) {
                              events.emplace_back(event, payload);
                          }};
        custom.props.push_back(ui::CustomProp{.name = "title", .value = ui::Prop<std::string>{"Trend"}});
        custom.props.push_back(ui::CustomProp{.name = "limit", .value = ui::Prop<std::string>{"12.50"}});
        custom.props.push_back(ui::CustomProp{.name = "live", .value = ui::Prop<bool>{true}});
        custom.props.push_back(ui::CustomProp{.name = "zoom", .value = ui::Prop<double>{2.5}});
        return ui::custom(std::move(custom));
    };
    {
        ui::Mounted const mounted{scene.runtime(), scene.backend(), node("Chart")};
        scene.settle();
        CHECK(reported.empty());
        QQuickItem* const root = scene.window().contentItem();
        CHECK(findItem(root, [](QQuickItem const& candidate) { return candidate.objectName() == "fallback"; }) ==
              nullptr);
        QQuickItem* const item = findItem(
            root, [](QQuickItem const& candidate) { return candidate.property("title").toString() == "Trend"; });
        REQUIRE(item != nullptr);
        CHECK(item->property("limit").toString() == QStringLiteral("12.50"));
        CHECK(item->property("live").toBool());
        CHECK(item->property("zoom").toDouble() == 2.5);
        QMetaObject::invokeMethod(item, "pick", Q_ARG(QVariant, QStringLiteral("i:7")));
        CHECK(events == std::vector<std::pair<std::string, std::string>>{{"pointSelected", "i:7"}});
    }
    {
        ui::Mounted const mounted{scene.runtime(), scene.backend(), node("Missing")};
        scene.settle();
        CHECK(findItem(scene.window().contentItem(),
                       [](QQuickItem const& candidate) { return candidate.objectName() == "fallback"; }) != nullptr);
    }
}
