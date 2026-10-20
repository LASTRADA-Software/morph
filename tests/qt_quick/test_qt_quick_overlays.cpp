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
