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
