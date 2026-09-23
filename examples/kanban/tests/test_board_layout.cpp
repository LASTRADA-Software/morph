// SPDX-License-Identifier: Apache-2.0
//
// BoardView's column strip, measured rather than eyeballed.
//
// The board is the one screen in this rung whose content has a hard minimum
// width: every column delegate is a fixed 240px Rectangle, so a board with N
// columns needs N*240 + (N-1)*8 pixels laid end to end, and no amount of
// window resizing makes that number smaller. Everything else on this rung's
// screens is elastic -- labels elide, list delegates take their view's width,
// forms wrap -- so nothing else can become unreachable by being too narrow.
//
// Three properties are asserted here, at the *shipped* window size rather than
// at whatever size an item loaded as a bare root happens to get:
//
//   1. The Activity panel beside the board stays a sidebar, rather than
//      expanding to whatever its widest activity summary wants.
//   2. A board wider than the area it is drawn in is *reachable*: the column
//      strip scrolls horizontally, and the last column can be brought fully
//      into view.
//   3. The board area never collapses below one usable column. Asserted at the
//      shipped width and again at a width where the sidebar and the board
//      cannot both have what they ask for, which is the only size at which
//      that floor decides anything.
//
// Reachability, not mere existence, is the point. A column that is laid out
// past a clipping edge with no scroll container in front of it is drawn
// nowhere and can be scrolled to by nothing -- the board is not "partly
// visible" in that state, its right-hand columns are simply gone.
//
// Why Main.qml and not BoardView on its own. The widths under test are the
// ones the real shell produces: the 1280x860 window, its 8px margins, the
// StackView inside them, and BoardView's own 8px margins inside that. Loading
// BoardView as a bare root object would size it from its own implicit width
// and measure a screen the user never sees. So this loads Main.qml with both
// bridges attached and then runs `stack.push(boardPage)` -- which is verbatim
// the statement Main.qml's own `onProjectOpened` handler runs -- through
// QQmlExpression in Main's own context. No synthesized input events
// (examples/TESTING.md presenter rule 6): the expression is the handler's
// body, not a click on the thing that would invoke it.
//
// Runs under QT_QPA_PLATFORM=offscreen, against the QGuiApplication
// testkit_main.cpp owns when this rung's test binary is built. Compiled away
// entirely without MORPH_LADDER_QML_URI, i.e. in a configure with no
// MORPH_BUILD_FORMS_QML, exactly like the two QML suites beside it.

#ifdef MORPH_LADDER_QML_URI

#include <QList>
#include <QPointF>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <catch2/catch_test_macros.hpp>
#include <kanban/models/project_admin_model.hpp>
#include <memory>
#include <morph/session/session.hpp>
#include <string>

#include "board_qml_bridge.hpp"
#include "project_admin_qml_bridge.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

/// The column delegate's fixed width, as BoardView.qml declares it.
constexpr qreal kColumnWidth = 240;

/// The gap between two column delegates, as BoardView.qml declares it.
constexpr qreal kColumnSpacing = 8;

/// Enough columns that the strip cannot fit the board area at any sane window
/// size: five of them need 1232px, against the ~960px the 1280px-wide shipped
/// window leaves once its own margins and the Activity sidebar are taken out.
constexpr int kColumnCount = 5;

/// A window too narrow to give the board a whole column once the Activity
/// sidebar has taken its 280: 500 - 32 (the two nested 8px margin rings) - 8
/// (the row's spacing) - 280 leaves 180, well under one column. Something has
/// to give at this size, and the floor is what decides which -- the board
/// keeps a usable column and the sidebar is the one that runs off the edge.
constexpr int kNarrowWindowWidth = 500;

/// @brief A rig whose one bridge already carries a valid session for
///        @p principal — the same recipe test_gui_forms_render.cpp uses.
/// @param principal The identity to install.
/// @return The rig, owning the bridge and executor the adapters take.
[[nodiscard]] std::unique_ptr<BackendRig> makeAuthedRig(std::string principal) {
    auto rig = std::make_unique<BackendRig>(Mode::Local, 1);
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    rig->bridge(0).setDefaultSession(ctx);
    return rig;
}

/// @brief Seeds one project (the rig's principal is its Manager) straight
///        through `ProjectAdminModel`'s own handler.
/// @param rig The rig whose bridge/executor to dispatch the seed through.
/// @return The new project's id, as its plain number.
[[nodiscard]] qlonglong seedProject(BackendRig& rig) {
    morph::bridge::BridgeHandler<kanban::ProjectAdminModel> creator{rig.bridge(0), rig.executor()};
    const auto id = morph::ladder::testkit::awaitQt(creator.execute(kanban::CreateProject{.name = "Sprint Board"})).id;
    return id.hasValue() ? static_cast<qlonglong>(*id) : -1;
}

/// @brief Depth-first search of the *visual* item tree under @p root for an
///        item with @p name as its `objectName`.
///
/// `QObject::findChild` is not enough for anything a `Repeater` created — a
/// delegate's visual parent is inside the tree while its `QObject` parent is
/// not — and BoardView's column delegates are exactly that, two Repeaters
/// deep.
/// @param root The item to search under (searched itself first).
/// @param name The `objectName` to find.
/// @return The item, or `nullptr`.
[[nodiscard]] QQuickItem* findItem(QQuickItem* root, const QString& name) {
    if (root == nullptr) {
        return nullptr;
    }
    if (root->objectName() == name) {
        return root;
    }
    const QList<QQuickItem*> kids = root->childItems();
    for (QQuickItem* kid : kids) {
        if (QQuickItem* hit = findItem(kid, name); hit != nullptr) {
            return hit;
        }
    }
    return nullptr;
}

/// @brief Whether @p item lies wholly inside @p viewport's own bounds.
///
/// The test of "reachable": an item scrolled into view has both edges within
/// the viewport it is clipped by. A half-pixel slack absorbs the layout's own
/// rounding, which can leave a scrolled-to-end position a fraction short.
/// @param item     The item to place.
/// @param viewport The clipping item to place it against.
/// @return `true` when no part of @p item falls outside @p viewport.
[[nodiscard]] bool fullyInside(QQuickItem* item, QQuickItem* viewport) {
    if (item == nullptr || viewport == nullptr) {
        return false;
    }
    const QPointF topLeft = item->mapToItem(viewport, QPointF{0, 0});
    return topLeft.x() >= -0.5 && topLeft.x() + item->width() <= viewport->width() + 0.5;
}

/// @brief Loads @p typeName from this rung's QML module with @p properties set
///        as initial properties, asserting a root object was produced and no
///        warning this rung is responsible for was emitted.
///
/// @par The one tolerated warning
/// `DynamicForm.qml` declares `onOptionsReceived` in a `Connections` block
/// whose `target` is the controller, unconditionally, and a controller that
/// serves no `morph::forms::Choice` field has no such signal. Every conforming
/// controller in the ladder therefore warns once per form the moment a real
/// controller is attached. Tolerated by exact text rather than by dropping the
/// assertion, so any *other* warning still fails — see
/// test_gui_forms_render.cpp's own note on the same filter.
/// @param engine     The engine to load into (kept alive by the caller).
/// @param typeName   Unqualified QML type name within `MORPH_LADDER_QML_URI`.
/// @param properties Initial properties for the root object.
/// @return The root object.
[[nodiscard]] QObject* loadRoot(QQmlApplicationEngine& engine, const char* typeName, const QVariantMap& properties) {
    QStringList unexpected;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, [&unexpected](const QList<QQmlError>& warnings) {
        for (const QQmlError& warning : warnings) {
            const QString text = warning.toString();
            if (text.contains(QStringLiteral("onOptionsReceived")) &&
                text.contains(QStringLiteral("MorphForms/qml/DynamicForm.qml"))) {
                continue;
            }
            unexpected.append(text);
        }
    });
    engine.setInitialProperties(properties);
    engine.loadFromModule(MORPH_LADDER_QML_URI, typeName);
    INFO(unexpected.join(QStringLiteral("\n")).toStdString());
    CHECK(unexpected.isEmpty());
    REQUIRE_FALSE(engine.rootObjects().isEmpty());
    return engine.rootObjects().front();
}

}  // namespace

TEST_CASE("BoardView keeps a usable board area and keeps every column reachable", "[kanban][gui][qml-layout]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    const qlonglong projectId = seedProject(*rig);
    REQUIRE(projectId > 0);

    kanban::gui::ProjectAdminBridge adminBridge{rig->bridge(0), rig->executor()};
    kanban::gui::BoardBridge bridge{rig->bridge(0), rig->executor()};

    bool boardChanged = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::boardChanged, [&boardChanged] { boardChanged = true; });
    auto settle = [&boardChanged] {
        const bool arrived = pumpUntil([&boardChanged] { return boardChanged; });
        boardChanged = false;
        return arrived;
    };

    bridge.openBoard(QString::number(projectId));
    REQUIRE(settle());

    // A board too wide for any viewport this window can offer. Built through
    // the bridge's own typed entry point, so the widths measured below are
    // over columns the model really has.
    for (int i = 0; i < kColumnCount; ++i) {
        bridge.createColumn(QStringLiteral("Column %1").arg(i + 1), 0);
        REQUIRE(settle());
    }
    bridge.stopPolling();  // Deterministic: nothing may re-enter while widths are read.

    const QVariantList columns = bridge.board().value(QStringLiteral("columns")).toList();
    REQUIRE(columns.size() == kColumnCount);
    const QString lastColumnId = columns.back().toMap().value(QStringLiteral("id")).toString();

    QQmlApplicationEngine engine;
    QObject* root = loadRoot(engine, "Main",
                             {{QStringLiteral("projectAdminBridge"), QVariant::fromValue(&adminBridge)},
                              {QStringLiteral("boardBridge"), QVariant::fromValue(&bridge)}});
    auto* window = qobject_cast<QQuickWindow*>(root);
    REQUIRE(window != nullptr);

    // The shipped size, read off the window rather than restated here: a
    // change to Main.qml's `width` is a change to what this test measures.
    const qreal windowWidth = window->width();
    REQUIRE(windowWidth > 0);

    // Exactly what Main.qml's own onProjectOpened handler does once a project
    // is chosen. `stack` and `boardPage` are ids in Main.qml's root context,
    // which is the context this expression is evaluated in.
    QQmlExpression pushBoard{qmlContext(root), root, QStringLiteral("stack.push(boardPage)")};
    pushBoard.evaluate();
    REQUIRE_FALSE(pushBoard.hasError());

    QQuickItem* boardView = nullptr;
    REQUIRE(pumpUntil([&] {
        boardView = qobject_cast<QQuickItem*>(window->findChild<QObject*>(QStringLiteral("boardView")));
        return boardView != nullptr && boardView->width() > 0;
    }));

    QQuickItem* boardArea = findItem(boardView, QStringLiteral("boardArea"));
    QQuickItem* activityPanel = findItem(boardView, QStringLiteral("activityPanel"));
    REQUIRE(boardArea != nullptr);
    REQUIRE(activityPanel != nullptr);

    // Let the layout reach its final geometry: the polish pass that sizes the
    // row's two children runs on the window's own frame, not inside push().
    REQUIRE(pumpUntil([&] { return boardArea->width() > 0 && activityPanel->width() > 0; }));

    INFO("window " << windowWidth << ", board area " << boardArea->width() << ", activity " << activityPanel->width());

    // 1. The sidebar stays a sidebar. A ColumnLayout inside a RowLayout fills
    //    width by default, so without an explicit bound Activity competes with
    //    the board for space rather than taking its declared 280.
    CHECK(activityPanel->width() <= 320);

    // 2. The board area never collapses below one whole column plus the gaps
    //    around it. Below that the first column is cut in half and there is no
    //    width at which the board is usable.
    CHECK(boardArea->width() >= kColumnWidth + 2 * kColumnSpacing);

    // 3. The strip really is wider than the area it is drawn in -- otherwise
    //    the reachability assertion below would pass on a board that never
    //    needed scrolling, and prove nothing.
    const qreal stripExtent = kColumnCount * kColumnWidth + (kColumnCount - 1) * kColumnSpacing;
    CHECK(stripExtent > boardArea->width());

    QQuickItem* flick = findItem(boardArea, QStringLiteral("boardFlickable"));
    REQUIRE(flick != nullptr);
    CHECK(flick->property("contentWidth").toReal() >= stripExtent);

    // 4. The horizontal scrollbar is on screen, so the strip can be dragged
    //    and not merely flicked at.
    QQuickItem* scrollBar = findItem(boardView, QStringLiteral("boardScrollBar"));
    REQUIRE(scrollBar != nullptr);
    CHECK(scrollBar->isVisible());

    // 5. The last column starts out of view -- the condition scrolling has to
    //    rescue -- and scrolling to the end brings it wholly into view.
    QQuickItem* lastColumn = findItem(boardView, QStringLiteral("boardColumn_") + lastColumnId);
    REQUIRE(lastColumn != nullptr);
    // The `Row` holding the strip positions its children on the window's
    // polish pass, not when the delegate is built, so every column reads x=0
    // until that has run once. Waiting for the last one to reach its place is
    // what makes the two assertions below measure a laid-out strip.
    const qreal lastColumnX = (kColumnCount - 1) * (kColumnWidth + kColumnSpacing);
    REQUIRE(pumpUntil([&] { return lastColumn->x() >= lastColumnX - 0.5; }));
    CHECK_FALSE(fullyInside(lastColumn, flick));

    const qreal contentWidth = flick->property("contentWidth").toReal();
    flick->setProperty("contentX", QVariant{contentWidth - flick->width()});
    REQUIRE(pumpUntil([&] { return fullyInside(lastColumn, flick); }));
    CHECK(fullyInside(lastColumn, flick));

    // 6. And the floor is load-bearing on its own, not merely implied by the
    //    sidebar's bound. At the shipped width there is room for both, so
    //    narrow the window until there is not -- the size at which the floor
    //    is the only thing deciding that the board is not what gives way.
    const qreal boardAreaWhenWide = boardArea->width();
    window->setWidth(kNarrowWindowWidth);
    REQUIRE(pumpUntil([&] { return boardView->width() > 0 && boardView->width() <= kNarrowWindowWidth; }));
    // The row re-runs on the next polish pass, not inside setWidth, so wait for
    // the board to have moved off its wide-window size rather than reading last
    // frame's numbers back.
    REQUIRE(pumpUntil([&] { return boardArea->width() < boardAreaWhenWide; }));
    INFO("narrow window " << window->width() << ", board area " << boardArea->width() << ", activity "
                          << activityPanel->width());
    CHECK(boardArea->width() >= kColumnWidth + 2 * kColumnSpacing);
}

#endif  // MORPH_LADDER_QML_URI
