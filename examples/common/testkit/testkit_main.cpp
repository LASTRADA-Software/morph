// SPDX-License-Identifier: Apache-2.0
//
// Qt-owning Catch2 main, copied from tests/qt/test_qt_websocket.cpp's pattern:
// the application object must outlive every QObject Catch2 constructs during
// the run and be destroyed before static teardown, or Qt's cleanup runs
// against a torn-down app (observed upstream as a heap-corruption abort on
// shutdown).

#include <QCoreApplication>
#include <QEvent>
#include <catch2/catch_session.hpp>
#include <testkit/log_level.hpp>

int main(int argc, char* argv[]) {
    QCoreApplication const app{argc, argv};
    Catch::Session session;
    int const result = morph::testkit::runSession(session, argc, argv);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    return result;
}
