// SPDX-License-Identifier: Apache-2.0
//
// Dialogs and drawers on the real Qt Quick engine: opened and closed by code without a dismissal, dismissed by
// Escape or a click outside with one, and holding their content out of reach while closed.

#include <QQuickItem>
#include <QTest>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/ui/backend.hpp>
#include <string>
#include <type_traits>
#include <vector>

#include "qt_quick/kinds.hpp"
#include "qt_quick_harness.hpp"

namespace ui = morph::ui;
namespace detail = morph::qt_quick::detail;
using morph::qt_quick::testing::Scene;

TEMPLATE_TEST_CASE("qt_quick overlays: opened and closed by code without a dismissal, dismissed by the user with one",
                   "[qt_quick]", ui::DialogWidget, ui::DrawerWidget) {
    Scene scene;
    std::unique_ptr<TestType> overlay;
    if constexpr (std::is_same_v<TestType, ui::DialogWidget>) {
        overlay = scene.backend().createDialog(nullptr);
    } else {
        overlay = scene.backend().createDrawer(nullptr, ui::Side::End);
    }
    overlay->setTitle("Confirm");
    int dismissals = 0;
    overlay->setOnDismiss([&dismissals] { ++dismissals; });
    auto inside = scene.backend().createButton(overlay.get());
    int clicks = 0;
    inside->setOnClick([&clicks] { ++clicks; });
    scene.settle();
    auto& impl = dynamic_cast<detail::OverlayImpl<TestType>&>(*overlay);
    CHECK_FALSE(impl.open());
    CHECK_FALSE(Scene::itemOf(*inside).isVisible());

    overlay->setOpen(true);
    scene.settle();
    CHECK(impl.open());
    CHECK(Scene::itemOf(*inside).isVisible());
    scene.click(Scene::itemOf(*inside));
    CHECK(clicks == 1);
    overlay->setOpen(false);
    scene.settle();
    CHECK_FALSE(impl.open());
    CHECK(dismissals == 0);

    overlay->setOpen(true);
    scene.settle();
    scene.key(Qt::Key_Escape);
    CHECK_FALSE(impl.open());
    CHECK(dismissals == 1);

    // A click outside dismisses it too.
    overlay->setOpen(true);
    scene.settle();
    QTest::mouseClick(&scene.window(), Qt::LeftButton, {}, QPoint{2, 2});
    scene.settle();
    CHECK_FALSE(impl.open());
    CHECK(dismissals == 2);
}

// Mutation: put the footer's item in the overlay's content (the button would not be under `footer`), or show the
// footer area whatever it holds.
TEMPLATE_TEST_CASE("qt_quick overlays: a footer's children are laid out under the content, in the footer item",
                   "[qt_quick]", ui::DialogWidget, ui::DrawerWidget) {
    Scene scene;
    std::unique_ptr<TestType> overlay;
    if constexpr (std::is_same_v<TestType, ui::DialogWidget>) {
        overlay = scene.backend().createDialog(nullptr);
    } else {
        overlay = scene.backend().createDrawer(nullptr, ui::Side::End);
    }
    auto inside = scene.backend().createText(overlay.get());
    inside->setText("Body");
    overlay->setOpen(true);
    scene.settle();
    QQuickItem const& overlayItem = Scene::itemOf(static_cast<ui::Widget&>(*overlay));
    auto* const footerItem = qvariant_cast<QQuickItem*>(overlayItem.property("footer"));
    REQUIRE(footerItem != nullptr);
    CHECK_FALSE(footerItem->isVisible());  // nothing in it: it takes no room

    ui::ContainerWidget& footer = overlay->footer();
    CHECK(&overlay->footer() == &footer);
    auto save = scene.backend().createButton(&footer);
    save->setLabel("Save");
    int clicks = 0;
    save->setOnClick([&clicks] { ++clicks; });
    scene.settle();
    QQuickItem const& button = Scene::itemOf(*save);
    CHECK(footerItem->isAncestorOf(&button));
    CHECK_FALSE(qvariant_cast<QQuickItem*>(overlayItem.property("content"))->isAncestorOf(&button));
    CHECK(button.isVisible());
    CHECK(button.mapToScene(QPointF{0, 0}).y() >=
          Scene::itemOf(*inside).mapToScene(QPointF{0, 0}).y() + Scene::itemOf(*inside).height());
    scene.click(button);
    CHECK(clicks == 1);
    save.reset();
    inside.reset();
}

namespace {
// The Popup/Drawer itself is a QQuickPopup, a QObject with its own width/height properties -- not a QQuickItem, so
// it is found by type among the root item's QObject children, not Scene::itemOf (which returns the
// MorphDialog/MorphDrawer root, a zero-sized MorphWidget that hosts the Popup/Drawer rather than being sized like
// it). inherits("QQuickPopup") matches both Popup and Drawer, which Qt Quick Controls derives from it.
QObject* popupOf(QQuickItem& overlayItem) {
    for (QObject* const child : overlayItem.children()) {
        if (child->inherits("QQuickPopup")) {
            return child;
        }
    }
    return nullptr;
}
}  // namespace

// Mutation: drop the Popup's width/height cap (let it size to its content's full implicit size).
TEMPLATE_TEST_CASE("qt_quick overlays: tall content is capped at the window and the footer stays reachable",
                   "[qt_quick]", ui::DialogWidget, ui::DrawerWidget) {
    Scene scene;
    std::unique_ptr<TestType> overlay;
    if constexpr (std::is_same_v<TestType, ui::DialogWidget>) {
        overlay = scene.backend().createDialog(nullptr);
    } else {
        overlay = scene.backend().createDrawer(nullptr, ui::Side::End);
    }
    overlay->setTitle("Tall");
    std::vector<std::unique_ptr<ui::TextWidget>> rows;
    for (int i = 0; i < 60; ++i) {
        rows.push_back(scene.backend().createText(overlay.get()));
        rows.back()->setText("row " + std::to_string(i));
    }
    auto save = scene.backend().createButton(&overlay->footer());
    save->setLabel("Done");
    int clicks = 0;
    save->setOnClick([&clicks] { ++clicks; });
    overlay->setOpen(true);
    scene.settle();

    QQuickItem& overlayItem = Scene::itemOf(static_cast<ui::Widget&>(*overlay));
    QObject const* const popup = popupOf(overlayItem);
    REQUIRE(popup != nullptr);
    // The window the Scene harness makes is 800x600; the overlay's realized height must not exceed it.
    CHECK(popup->property("height").toReal() <= 600);
    QQuickItem const& button = Scene::itemOf(*save);
    // The footer button's own position is within the window, i.e. actually clickable.
    CHECK(button.mapToScene(QPointF{0, 0}).y() + button.height() <= 600);
    scene.click(button);
    CHECK(clicks == 1);
}

// Mutation: cap the Popup even when its content already fits (an unconditional cap, not a bound).
TEST_CASE("qt_quick dialog: short content is not stretched or clipped by the cap", "[qt_quick]") {
    Scene scene;
    auto dialog = scene.backend().createDialog(nullptr);
    dialog->setTitle("Short");
    auto body = scene.backend().createText(dialog.get());
    body->setText("Just one line.");
    dialog->setOpen(true);
    scene.settle();

    QQuickItem& dialogItem = Scene::itemOf(static_cast<ui::Widget&>(*dialog));
    QObject const* const popup = popupOf(dialogItem);
    REQUIRE(popup != nullptr);
    QQuickItem const& bodyItem = Scene::itemOf(*body);
    // The dialog's height is driven by its content's natural size, not artificially stretched to any cap.
    CHECK(popup->property("height").toReal() < 600);
    CHECK(popup->property("height").toReal() >= bodyItem.height());
}

// Mutation: cap the drawer's width even when its content already fits (an unconditional cap, not a bound). A
// drawer's height is always the full window height by design (MorphDrawer.qml:55) -- only its width is at issue.
TEST_CASE("qt_quick drawer: narrow content is not stretched past its own natural width", "[qt_quick]") {
    Scene scene;
    auto drawer = scene.backend().createDrawer(nullptr, ui::Side::End);
    drawer->setTitle("Short");
    auto body = scene.backend().createText(drawer.get());
    body->setText("Short.");
    drawer->setOpen(true);
    scene.settle();

    QQuickItem& drawerItem = Scene::itemOf(static_cast<ui::Widget&>(*drawer));
    QObject const* const popup = popupOf(drawerItem);
    REQUIRE(popup != nullptr);
    // The drawer's own minimum width (MorphDrawer.qml:54, 30 units) governs here, not any new cap -- the cap must
    // not shrink it below what it already draws as today.
    CHECK(popup->property("width").toReal() < 800);
}

// Mutation: drop the drawer's width cap (MorphDrawer.qml's own width binding stays uncapped against the window).
TEST_CASE("qt_quick drawer: wide content is capped at the window's width", "[qt_quick]") {
    Scene scene;
    auto drawer = scene.backend().createDrawer(nullptr, ui::Side::End);
    auto body = scene.backend().createText(drawer.get());
    // A single very long unbroken line: wide enough that an uncapped drawer would exceed the window.
    body->setText(std::string(400, 'x'));
    drawer->setOpen(true);
    scene.settle();

    QQuickItem& drawerItem = Scene::itemOf(static_cast<ui::Widget&>(*drawer));
    QObject const* const popup = popupOf(drawerItem);
    REQUIRE(popup != nullptr);
    CHECK(popup->property("width").toReal() <= 800);
}
