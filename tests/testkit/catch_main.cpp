// SPDX-License-Identifier: Apache-2.0
//
// The `main` for every morph Catch2 binary that does not need to own a
// QCoreApplication, in place of Catch2::Catch2WithMain.
//
// It exists for one reason: `--log-level`. A Catch2 event listener would have
// been a smaller change, but listeners are constructed after the command line
// is parsed and so cannot add an option to it -- taking the level from the CLI
// means owning `main` and driving Catch::Session directly.
//
// The Qt-owning suites (tests/qt, tests/net_qt_interop, examples/common's
// testkit, src/qt/forms/tests) cannot link this file, because they already
// define their own `main` to control the QCoreApplication's lifetime. They call
// the same morph::testkit::configureSession() from inside it, so the option
// behaves identically across every suite.

#include <catch2/catch_session.hpp>
#include <testkit/log_level.hpp>

int main(int argc, char* argv[]) {
    Catch::Session session;
    if (const auto exitCode = morph::testkit::configureSession(session, argc, argv)) {
        return *exitCode;
    }
    return session.run();
}
