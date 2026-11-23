// SPDX-License-Identifier: Apache-2.0
//
// The spotlight on the real Qt Quick backend (docs/spec/qt_quick/backend.md, "The spotlight"): the cut-out and the
// ring, where the card goes, what the pointer and the keyboard reach, scrolling a clipped target into view, and that
// a widget of another window is never the target. Set MORPH_SPOTLIGHT_GRAB to a file to keep a picture.

#include <QImage>
#include <QPointF>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QString>
#include <QTest>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <morph/qt_quick/backend.hpp>
#include <morph/ui/backend.hpp>
#include <string>
#include <vector>

#include "qt_quick/kinds.hpp"
#include "qt_quick_harness.hpp"

namespace {

using morph::qt_quick::testing::Scene;
namespace detail = morph::qt_quick::detail;
namespace ui = morph::ui;

/// The item a widget takes focus in: a button has no `control` part, so it is the widget's own focus target.
QQuickItem& focusItemOf(ui::Widget const& widget) {
    detail::ItemBase const* const base = detail::baseOf(widget);
    REQUIRE(base != nullptr);
    QQuickItem* const target = base->focusTarget();
    REQUIRE(target != nullptr);
    return *target;
}

QRectF sceneBox(QQuickItem const& item) { return item.mapRectToScene(QRectF{0, 0, item.width(), item.height()}); }

/// A row of three buttons near the top of the window; the middle one carries `target`.
struct Page {
    explicit Page(Scene& scene) {
        root = scene.backend().createStack(nullptr, ui::Axis::Vertical);
        row = scene.backend().createStack(root.get(), ui::Axis::Horizontal);
        before = scene.backend().createButton(row.get());
        before->setLabel("Before");
        target = scene.backend().createButton(row.get());
        target->setLabel("Target");
        target->setTestId("target");
        after = scene.backend().createButton(row.get());
        after->setLabel("After");
    }
    std::unique_ptr<ui::StackWidget> root;
    std::unique_ptr<ui::StackWidget> row;
    std::unique_ptr<ui::ButtonWidget> before;
    std::unique_ptr<ui::ButtonWidget> target;
    std::unique_ptr<ui::ButtonWidget> after;
};

/// A spotlight with a card of two buttons, open on `target`.
struct Card {
    explicit Card(Scene& scene) {
        spotlight = scene.backend().createSpotlight(nullptr);
        back = scene.backend().createButton(spotlight.get());
        back->setLabel("Back");
        next = scene.backend().createButton(spotlight.get());
        next->setLabel("Next");
        spotlight->setTarget(std::string{"target"});
        spotlight->setOpen(true);
        scene.settle();
        impl().track();
    }
    [[nodiscard]] detail::SpotlightImpl& impl() const { return dynamic_cast<detail::SpotlightImpl&>(*spotlight); }
    [[nodiscard]] QQuickItem& item() const { return Scene::itemOf(*spotlight); }
    std::unique_ptr<ui::SpotlightWidget> spotlight;
    std::unique_ptr<ui::ButtonWidget> back;
    std::unique_ptr<ui::ButtonWidget> next;
};

}  // namespace

// Mutation: write the cut-out without the target's scene position (it sits at the window's corner).
TEST_CASE("qt_quick spotlight: the cut-out is the target's scene box, ringed one spacing unit out",
          "[qt_quick][tours]") {
    Scene scene;
    scene.window().resize(1280, 800);
    Page const page{scene};
    Card const card{scene};
    QQuickItem const& target = Scene::itemOf(*page.target);
    CHECK(card.spotlight->targetShown());
    CHECK(card.item().property("ringed").toBool());
    QRectF const cutout = card.item().property("cutout").toRectF();
    CHECK(cutout == sceneBox(target));
    qreal const pad = card.item().property("ringPad").toReal();
    CHECK(pad > 0);
    auto const* const ring = card.item().property("ring").value<QQuickItem*>();
    REQUIRE(ring != nullptr);
    CHECK(sceneBox(*ring) == cutout.adjusted(-pad, -pad, pad, pad));
    if (char const* const file = std::getenv("MORPH_SPOTLIGHT_GRAB")) {  // NOLINT(concurrency-mt-unsafe): one thread.
        CHECK(scene.window().grabWindow().save(QString::fromUtf8(file)));
    }
}

// Mutation: place the card at the cut-out's own position (it covers the target).
TEST_CASE("qt_quick spotlight: the card stays inside the window and off the target, wide and compact",
          "[qt_quick][tours]") {
    for (int const width : {1280, 420}) {
        INFO("window width " << width);
        Scene scene;
        scene.window().resize(width, 800);
        Page const page{scene};
        Card const card{scene};
        auto const* const box = card.item().property("card").value<QQuickItem*>();
        REQUIRE(box != nullptr);
        QRectF const placed = sceneBox(*box);
        QRectF const window{0, 0, static_cast<qreal>(scene.window().width()),
                            static_cast<qreal>(scene.window().height())};
        CHECK(window.contains(placed));
        CHECK_FALSE(placed.intersects(sceneBox(Scene::itemOf(*page.target))));
    }
}

// Mutation: give the cut-out a scrim rectangle of its own (the click on the target is swallowed).
TEST_CASE("qt_quick spotlight: a click inside the cut-out reaches the target, one on the scrim reaches nothing",
          "[qt_quick][tours]") {
    Scene scene;
    scene.window().resize(1280, 800);
    Page const page{scene};
    int targetClicks = 0;
    int beforeClicks = 0;
    page.target->setOnClick([&targetClicks] { ++targetClicks; });
    page.before->setOnClick([&beforeClicks] { ++beforeClicks; });
    Card const card{scene};
    scene.click(Scene::itemOf(*page.target));
    scene.click(Scene::itemOf(*page.before));
    CHECK(targetClicks == 1);
    CHECK(beforeClicks == 0);
}

// Mutation: skip `scrollIntoView` in `track` (the target stays below the scroll area's box).
TEST_CASE("qt_quick spotlight: a target in a scrolled-away ScrollWidget is scrolled into view", "[qt_quick][tours]") {
    Scene scene;
    scene.window().resize(800, 600);
    auto const root = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto const scroll = scene.backend().createScroll(root.get(), ui::Axis::Vertical);
    scroll->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::fixed(6)});
    std::vector<std::unique_ptr<ui::TextWidget>> lines;
    for (int index = 0; index < 60; ++index) {
        lines.push_back(scene.backend().createText(scroll.get()));
        lines.back()->setText("Line " + std::to_string(index));
    }
    lines.back()->setTestId("target");
    scene.settle();
    QRectF const viewport = sceneBox(Scene::itemOf(*scroll));
    REQUIRE_FALSE(viewport.contains(sceneBox(Scene::itemOf(*lines.back()))));
    auto const spotlight = scene.backend().createSpotlight(nullptr);
    spotlight->setTarget(std::string{"target"});
    spotlight->setOpen(true);
    scene.settle();
    dynamic_cast<detail::SpotlightImpl&>(*spotlight).track();
    scene.settle();
    CHECK(viewport.contains(sceneBox(Scene::itemOf(*lines.back()))));
}

// Mutation: let Tab through in `filterKey` (focus leaves the card for the page's buttons).
TEST_CASE("qt_quick spotlight: Tab stays in the card, and on a waiting step reaches the target", "[qt_quick][tours]") {
    Scene scene;
    scene.window().resize(1280, 800);
    Page const page{scene};
    Card const card{scene};
    QQuickItem const& back = focusItemOf(*card.back);
    QQuickItem const& next = focusItemOf(*card.next);
    QQuickItem const& target = focusItemOf(*page.target);
    CHECK(back.hasActiveFocus());
    scene.key(Qt::Key_Tab);
    CHECK(next.hasActiveFocus());
    scene.key(Qt::Key_Tab);
    CHECK(back.hasActiveFocus());
    card.spotlight->setTargetFocusable(true);
    scene.key(Qt::Key_Tab);
    scene.key(Qt::Key_Tab);
    CHECK(target.hasActiveFocus());
    scene.key(Qt::Key_Tab);
    CHECK(back.hasActiveFocus());
}

// Mutation: leave the focus where it is in `setOpen(false)` (the field never gets it back).
TEST_CASE("qt_quick spotlight: closing hands focus back to where it was", "[qt_quick][tours]") {
    Scene scene;
    auto const root = scene.backend().createStack(nullptr, ui::Axis::Vertical);
    auto const field = scene.backend().createTextInput(root.get(), ui::TextInputMode::SingleLine);
    scene.settle();
    field->focus();
    scene.settle();
    Card const card{scene};
    CHECK_FALSE(Scene::controlOf(*field).hasActiveFocus());
    card.spotlight->setOpen(false);
    scene.settle();
    CHECK(Scene::controlOf(*field).hasActiveFocus());
}

// Mutation: make `findWidget` and the index one process-wide list again (the other window's twin is ringed).
TEST_CASE("qt_quick spotlight: a widget of another window is never the target", "[qt_quick][tours]") {
    Scene scene;
    QQuickWindow other;
    other.resize(400, 300);
    morph::qt_quick::Backend second{scene.engine(), *other.contentItem()};
    auto const elsewhere = second.createButton(nullptr);
    elsewhere->setLabel("Elsewhere");
    elsewhere->setTestId("target");
    auto const spotlight = scene.backend().createSpotlight(nullptr);
    spotlight->setTarget(std::string{"target"});
    spotlight->setOpen(true);
    scene.settle();
    CHECK_FALSE(spotlight->targetShown());
}

// Mutation: start the chord walk at the focused item only, as a card key while the target has focus is lost.
TEST_CASE(
    "qt_quick spotlight: Escape and the arrows reach the card whatever has focus, Enter and Space stay with the "
    "focused button",
    "[qt_quick][tours]") {
    Scene scene;
    scene.window().resize(1280, 800);
    Page const page{scene};
    Card const card{scene};
    std::vector<std::string> chords;
    auto const hear = [&chords](std::string const& chord) { chords.push_back(chord); };
    card.back->setKeys({"Left"}, hear);
    card.next->setKeys({"Right", "Escape", "Space", "Enter"}, hear);
    int backClicks = 0;
    int nextClicks = 0;
    card.back->setOnClick([&backClicks] { ++backClicks; });
    card.next->setOnClick([&nextClicks] { ++nextClicks; });
    QQuickItem const& back = focusItemOf(*card.back);
    REQUIRE(back.hasActiveFocus());

    scene.key(Qt::Key_Right);
    CHECK(chords == std::vector<std::string>{"Right"});
    scene.key(Qt::Key_Escape);
    CHECK(chords == std::vector<std::string>{"Right", "Escape"});

    card.spotlight->setTargetFocusable(true);
    scene.key(Qt::Key_Tab);
    scene.key(Qt::Key_Tab);
    REQUIRE(focusItemOf(*page.target).hasActiveFocus());
    chords.clear();
    scene.key(Qt::Key_Escape);
    scene.key(Qt::Key_Left);
    CHECK(chords == std::vector<std::string>{"Escape", "Left"});

    scene.key(Qt::Key_Tab);
    REQUIRE(back.hasActiveFocus());
    chords.clear();
    scene.key(Qt::Key_Space);
    CHECK(chords.empty());
    CHECK(backClicks == 1);
    scene.key(Qt::Key_Return);
    CHECK(chords.empty());
    CHECK(backClicks == 2);
}

// Mutation: let go of Tab when focus is outside the card (a card button that hid takes the trap with it).
TEST_CASE("qt_quick spotlight: Tab keeps cycling the card after its focused button hides", "[qt_quick][tours]") {
    Scene scene;
    scene.window().resize(1280, 800);
    Page const page{scene};
    Card const card{scene};
    REQUIRE(focusItemOf(*card.back).hasActiveFocus());
    card.back->setVisible(false);
    scene.settle();
    scene.key(Qt::Key_Tab);
    CHECK(focusItemOf(*card.next).hasActiveFocus());
    scene.key(Qt::Key_Tab);
    CHECK(focusItemOf(*card.next).hasActiveFocus());
    CHECK_FALSE(focusItemOf(*page.before).hasActiveFocus());
}

namespace {

/// A tall target in a short ScrollWidget, below lines of text.
struct Tall {
    explicit Tall(Scene& scene) {
        root = scene.backend().createStack(nullptr, ui::Axis::Vertical);
        scroll = scene.backend().createScroll(root.get(), ui::Axis::Vertical);
        scroll->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::fixed(6)});
        for (int index = 0; index < 10; ++index) {
            lines.push_back(scene.backend().createText(scroll.get()));
            lines.back()->setText("Line " + std::to_string(index));
        }
        tall = scene.backend().createStack(scroll.get(), ui::Axis::Vertical);
        tall->setLayout({.width = ui::Sizing::stretch(), .height = ui::Sizing::fixed(30)});
        tall->setTestId("target");
        scene.settle();
    }
    [[nodiscard]] QQuickItem& flickable() const { return Scene::itemOf(*scroll); }
    std::unique_ptr<ui::StackWidget> root;
    std::unique_ptr<ui::ScrollWidget> scroll;
    std::vector<std::unique_ptr<ui::TextWidget>> lines;
    std::unique_ptr<ui::StackWidget> tall;
};

/// The scrolling item inside a ScrollWidget's item.
QQuickItem* flickableIn(QQuickItem& item) {
    if (item.inherits("QQuickFlickable")) {
        return &item;
    }
    for (QQuickItem* const child : item.childItems()) {
        if (QQuickItem* const found = flickableIn(*child)) {
            return found;
        }
    }
    return nullptr;
}

}  // namespace

// Mutation: scroll to the far edge of a target taller than the viewport (contentY flips each frame), or scroll on
// every frame (the person's own scrolling is undone).
TEST_CASE("qt_quick spotlight: a target taller than its viewport shows its top and stays put", "[qt_quick][tours]") {
    Scene scene;
    scene.window().resize(800, 600);
    Tall const page{scene};
    QQuickItem* const flick = flickableIn(page.flickable());
    REQUIRE(flick != nullptr);
    auto const spotlight = scene.backend().createSpotlight(nullptr);
    auto& impl = dynamic_cast<detail::SpotlightImpl&>(*spotlight);
    spotlight->setTarget(std::string{"target"});
    spotlight->setOpen(true);
    scene.settle();
    QQuickItem const& target = Scene::itemOf(*page.tall);
    REQUIRE(target.height() > flick->height());
    qreal const settled = flick->property("contentY").toReal();
    CHECK(settled > 0);
    CHECK(sceneBox(target).top() == sceneBox(*flick).top());
    for (int frame = 0; frame < 5; ++frame) {
        impl.track();
        scene.settle();
        CHECK(flick->property("contentY").toReal() == settled);
    }
    flick->setProperty("contentY", 0.0);
    impl.track();
    scene.settle();
    CHECK(flick->property("contentY").toReal() == 0.0);
}

// Mutation: let `track` scroll while the spotlight is closed.
TEST_CASE("qt_quick spotlight: a closed spotlight never scrolls the page", "[qt_quick][tours]") {
    Scene scene;
    scene.window().resize(800, 600);
    Tall const page{scene};
    QQuickItem* const flick = flickableIn(page.flickable());
    REQUIRE(flick != nullptr);
    auto const spotlight = scene.backend().createSpotlight(nullptr);
    spotlight->setTarget(std::string{"target"});
    scene.settle();
    dynamic_cast<detail::SpotlightImpl&>(*spotlight).track();
    scene.settle();
    CHECK(flick->property("contentY").toReal() == 0.0);
}

// Mutation: centre on the ring rectangle even with no target (the card sits at the corner).
TEST_CASE("qt_quick spotlight: a missing target rings nothing and centres the card", "[qt_quick][tours]") {
    Scene scene;
    scene.window().resize(1280, 800);
    Page const page{scene};
    auto const spotlight = scene.backend().createSpotlight(nullptr);
    auto const button = scene.backend().createButton(spotlight.get());
    button->setLabel("Next");
    spotlight->setTarget(std::string{"nowhere"});
    spotlight->setOpen(true);
    scene.settle();
    CHECK_FALSE(spotlight->targetShown());
    QQuickItem& item = Scene::itemOf(*spotlight);
    CHECK_FALSE(item.property("ringed").toBool());
    auto const* const box = item.property("card").value<QQuickItem*>();
    REQUIRE(box != nullptr);
    QRectF const placed = sceneBox(*box);
    CHECK(std::abs(placed.center().x() - scene.window().width() / 2.0) <= 1.0);
    CHECK(std::abs(placed.center().y() - scene.window().height() / 2.0) <= 1.0);
}

// Mutation: bind the layer's visibility to `open` alone (a hidden spotlight item leaves its layer showing).
TEST_CASE("qt_quick spotlight: hiding the spotlight hides its layer", "[qt_quick][tours]") {
    Scene scene;
    scene.window().resize(1280, 800);
    Page const page{scene};
    Card const card{scene};
    auto const* const box = card.item().property("card").value<QQuickItem*>();
    REQUIRE(box != nullptr);
    REQUIRE(box->isVisible());
    card.spotlight->setVisible(false);
    scene.settle();
    CHECK_FALSE(box->isVisible());
}

// Mutation: make the cut-out the ring's box (the band between the target and the ring takes presses meant for
// the scrim).
TEST_CASE("qt_quick spotlight: the scrim reaches the target's box, so the ring's band is covered",
          "[qt_quick][tours]") {
    Scene scene;
    scene.window().resize(1280, 800);
    Page const page{scene};
    Card const card{scene};
    QRectF const cutout = card.item().property("cutout").toRectF();
    auto const* const left =
        scene.window().contentItem()->findChild<QQuickItem*>(QStringLiteral("spotlight_scrim_left"));
    auto const* const top =
        scene.window().contentItem()->findChild<QQuickItem*>(QStringLiteral("spotlight_scrim_top"));
    REQUIRE(left != nullptr);
    REQUIRE(top != nullptr);
    CHECK(sceneBox(*left).right() == cutout.left());
    CHECK(sceneBox(*top).bottom() == cutout.top());
}
