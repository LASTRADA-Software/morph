// SPDX-License-Identifier: Apache-2.0
//
// Qt-owning Catch2 main, copied from tests/qt/test_qt_websocket.cpp's pattern:
// the application object must outlive every QObject Catch2 constructs during
// the run and be destroyed before static teardown, or Qt's cleanup runs
// against a torn-down app (observed upstream as a heap-corruption abort on
// shutdown).
//
// MORPH_LADDER_TESTKIT_GUI_APP (defined by morph_add_rung() for a rung whose
// test binary carries the offscreen QML engine-load smoke test, and by nothing
// else) upgrades that object from QCoreApplication to QGuiApplication.
// QGuiApplication *is* a QCoreApplication, so every existing test behaves
// identically; what it adds is a platform integration, without which Qt Quick
// cannot instantiate a window at all. Left off, this file is byte-for-byte the
// plain QCoreApplication main ladder_common_tests has always used — which is
// what keeps examples/TESTING.md presenter rule 1 ("presenters must
// instantiate under a plain QCoreApplication") honestly exercised somewhere.

#include <QEvent>
#include <catch2/catch_session.hpp>

#ifdef MORPH_LADDER_TESTKIT_GUI_APP
#include <QGuiApplication>
using LadderTestApplication = QGuiApplication;
#else
#include <QCoreApplication>
using LadderTestApplication = QCoreApplication;
#endif

int main(int argc, char* argv[]) {
    LadderTestApplication app{argc, argv};
    int result = Catch::Session().run(argc, argv);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    return result;
}
