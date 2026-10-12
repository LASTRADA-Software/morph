// SPDX-License-Identifier: Apache-2.0
//
// The main of morph_qt_quick_tests: one QGuiApplication for the whole run, on the platform the environment names
// (ctest sets QT_QPA_PLATFORM=offscreen), with the Basic style and the software scene graph.

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <catch2/catch_session.hpp>
#include <testkit/log_level.hpp>

int main(int argc, char* argv[]) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication const app{argc, argv};
    Catch::Session session;
    int const result = morph::testkit::runSession(session, argc, argv);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    return result;
}
