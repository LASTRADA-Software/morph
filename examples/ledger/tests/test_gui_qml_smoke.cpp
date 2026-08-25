// SPDX-License-Identifier: Apache-2.0
//
// Offscreen engine-load smoke test (examples/TESTING.md, presenter rule 6).
//
// Every view is loaded standalone, with no bridge wired at all. That is only
// possible because main.cpp passes the bridges as *initial properties* and
// every view guards on a `null` default -- so this test is also what keeps
// that convention honest: bind a view to a context property, or dereference a
// bridge unguarded, and these cases stop creating a root object.
//
// Compiled only when the rung's QML module exists (MORPH_BUILD_FORMS_QML=ON);
// the ladder CI leg's distro Qt is below the 6.5 floor that requires, so this
// file is deliberately inert there rather than failing.
#ifdef MORPH_LADDER_QML_URI

#include <QList>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QString>
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace {

/// @brief Loads @p typeName from this rung's QML module and reports the first
///        warning the engine emitted, if any.
/// @param typeName The QML type to instantiate.
/// @param created Set to whether a root object was produced.
/// @return The first warning's text, or an empty string.
[[nodiscard]] std::string firstWarningLoading(const char* typeName, bool& created) {
    QQmlApplicationEngine engine;

    QString firstWarning;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, [&firstWarning](const QList<QQmlError>& warnings) {
        if (firstWarning.isEmpty() && !warnings.isEmpty()) {
            firstWarning = warnings.front().toString();
        }
    });

    engine.loadFromModule(MORPH_LADDER_QML_URI, typeName);
    created = !engine.rootObjects().isEmpty();
    return firstWarning.toStdString();
}

}  // namespace

TEST_CASE("ledger's QML engine loads Main.qml and creates a root object with no errors", "[ledger][gui][qml-smoke]") {
    bool created = false;
    CHECK(firstWarningLoading("Main", created) == std::string{});
    REQUIRE(created);
}

TEST_CASE("ledger's accounts screen loads standalone with no errors", "[ledger][gui][qml-smoke]") {
    bool created = false;
    CHECK(firstWarningLoading("LedgerView", created) == std::string{});
    REQUIRE(created);
}

TEST_CASE("ledger's budgets screen loads standalone with no errors", "[ledger][gui][qml-smoke]") {
    bool created = false;
    CHECK(firstWarningLoading("BudgetView", created) == std::string{});
    REQUIRE(created);
}

TEST_CASE("ledger's rules screen loads standalone with no errors", "[ledger][gui][qml-smoke]") {
    bool created = false;
    CHECK(firstWarningLoading("RulesView", created) == std::string{});
    REQUIRE(created);
}

TEST_CASE("ledger's statement screen loads standalone with no errors", "[ledger][gui][qml-smoke]") {
    bool created = false;
    CHECK(firstWarningLoading("ReportView", created) == std::string{});
    REQUIRE(created);
}

#endif  // MORPH_LADDER_QML_URI
