// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <QtQuickTest/quicktest.h>

#include <QCoreApplication>
#include <QEvent>
#include <QKeySequence>
#include <QList>
#include <QPointF>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <catch2/catch_test_macros.hpp>
#include <morph/qt_quick/backend.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/backend.hpp>
#include <string>

#include "qt_quick/kinds.hpp"
#include "qt_quick/widget.hpp"

/// @file
/// @brief What every morph_qt_quick test needs: a shown window with a backend on it, a runtime whose owner the
///        test pumps, and real input sent to the window.

namespace morph::qt_quick::testing {

/// A window on the offscreen platform with a backend whose roots fill it.
class Scene {
public:
    explicit Scene(BackendOptions options = {}) : _backend{_engine, *_window.contentItem(), std::move(options)} {
        _window.resize(800, 600);
        _window.show();
        REQUIRE(QTest::qWaitForWindowExposed(&_window));
        _window.requestActivate();
        static_cast<void>(QTest::qWaitForWindowActive(&_window));
    }

    Scene(Scene const&) = delete;
    Scene& operator=(Scene const&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(Scene&&) = delete;
    ~Scene() { flushQt(); }

    [[nodiscard]] Backend& backend() noexcept { return _backend; }
    [[nodiscard]] QQuickWindow& window() noexcept { return _window; }
    [[nodiscard]] QQmlEngine& engine() noexcept { return _engine; }
    [[nodiscard]] reactive::Runtime& runtime() noexcept { return _runtime; }
    [[nodiscard]] morph::testing::StepExecutor& owner() noexcept { return _owner; }

    /// Runs everything posted to the owner and to Qt, until nothing is left, and lays the window out.
    void settle() {
        for (int round = 0; round < 50; ++round) {
            _owner.runAll();
            flushQt();
            if (_owner.pending() == 0) {
                break;
            }
        }
        if (QQuickTest::qIsPolishScheduled(&_window)) {
            static_cast<void>(QQuickTest::qWaitForPolish(&_window));
        }
        _owner.runAll();
        flushQt();
    }

    /// The centre of a widget's item, in window coordinates.
    [[nodiscard]] static QPoint centreOf(QQuickItem const& item) {
        return item.mapToScene(QPointF{item.width() / 2, item.height() / 2}).toPoint();
    }

    /// Clicks an item at its centre with the left button.
    void click(QQuickItem const& item, Qt::KeyboardModifiers modifiers = {}) {
        settle();
        QTest::mouseClick(&_window, Qt::LeftButton, modifiers, centreOf(item));
        settle();
    }

    /// Double-clicks an item at its centre.
    void doubleClick(QQuickItem const& item) {
        settle();
        QPoint const at = centreOf(item);
        QTest::mouseClick(&_window, Qt::LeftButton, {}, at);
        QTest::mouseDClick(&_window, Qt::LeftButton, {}, at);
        settle();
    }

    /// Presses a key on whatever has focus.
    void key(Qt::Key key, Qt::KeyboardModifiers modifiers = {}) {
        QTest::keyClick(&_window, key, modifiers);
        settle();
    }

    /// Types text on whatever has focus, one key per character.
    void type(QString const& text) {
        for (QChar const character : text) {
            Qt::Key const code = character.isLetterOrNumber() && character.unicode() < 128
                                     ? static_cast<Qt::Key>(character.toUpper().unicode())
                                     : Qt::Key_unknown;
            QTest::sendKeyEvent(QTest::Click, &_window, code, QString{character}, Qt::NoModifier);
        }
        settle();
    }

    /// Enters text on whatever has focus in one key event, as an input method commits a composition: a field
    /// reports it as one edit.
    void enter(QString const& text) {
        QTest::sendKeyEvent(QTest::Click, &_window, Qt::Key_unknown, text, Qt::NoModifier);
        settle();
    }

    /// The item behind a widget.
    [[nodiscard]] static QQuickItem& itemOf(ui::Widget const& widget) {
        QQuickItem* const item = Backend::itemOf(widget);
        REQUIRE(item != nullptr);
        return *item;
    }

    /// Every item inside @p item, depth first, that inherits the C++ class @p className.
    [[nodiscard]] static QList<QQuickItem*> itemsOf(QQuickItem& item, char const* className) {
        QList<QQuickItem*> found;
        for (QQuickItem* const child : item.childItems()) {
            if (child->inherits(className)) {
                found.append(child);
            }
            found.append(itemsOf(*child, className));
        }
        return found;
    }

    /// Presses the platform's key for a standard text action, such as moving to the start of the line.
    void standardKey(QKeySequence::StandardKey action) {
        QList<QKeySequence> const bindings = QKeySequence::keyBindings(action);
        REQUIRE_FALSE(bindings.isEmpty());
        QKeyCombination const combination = bindings.first()[0];
        key(combination.key(), combination.keyboardModifiers());
    }

    /// The control a field shows.
    [[nodiscard]] static QQuickItem& controlOf(ui::Widget const& widget) {
        detail::ItemBase const* const base = detail::baseOf(widget);
        REQUIRE(base != nullptr);
        auto* const control = qobject_cast<QQuickItem*>(base->part("control"));
        REQUIRE(control != nullptr);
        return *control;
    }

private:
    static void flushQt() {
        for (int round = 0; round < 3; ++round) {
            QCoreApplication::sendPostedEvents();
            QCoreApplication::processEvents(QEventLoop::AllEvents);
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }

    morph::testing::StepExecutor _owner;
    reactive::Runtime _runtime{_owner};
    QQmlEngine _engine;
    QQuickWindow _window;
    Backend _backend;
};

}  // namespace morph::qt_quick::testing
