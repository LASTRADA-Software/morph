// SPDX-License-Identifier: Apache-2.0
//
// The drift guard's own drift guard.
//
// `QmlSurfaceAudit` (testkit/qml_surface.hpp) exists to fail when a bridge's
// QML-visible surface and the QML that binds it disagree. A guard that
// reports "no findings" no matter what it is pointed at is worse than no
// guard, so every case below drives the audit against a *deliberately broken*
// pair and asserts the specific finding — the mutation is the test, not an
// afterthought to it. The one clean case at the top exists only to prove the
// broken ones are not failing for some unrelated reason.
//
// The fixtures are QML text written to a QTemporaryDir and two throwaway
// QObjects declared here, not any rung's real bridge: the audit must be
// provably sensitive to each drift shape independently, and a real rung can
// only ever demonstrate the shapes it happens to contain. The real rungs'
// suites (examples/bookmarks/tests/test_bookmark_qml_bridges.cpp and its
// siblings) point the same audit at real QML and real bridges.

#include <QDir>
#include <QFile>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVariantList>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <string>

#include "testkit/qml_surface.hpp"

namespace {

using morph::ladder::testkit::blankCommentsAndStrings;
using morph::ladder::testkit::QmlSurfaceAudit;

/// @brief A stand-in bridge with one of each QML-visible member kind.
class SurfaceFixtureBridge : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(int depth READ depth NOTIFY depthChanged)

public:
    /// @brief A `CONSTANT` property, the shape every rung's `schemasJson` has.
    /// @return An empty string; no test reads the value.
    [[nodiscard]] QString title() const { return {}; }

    /// @brief A `NOTIFY`ing property, the shape `kanban`'s `board` has.
    /// @return Zero; no test reads the value.
    [[nodiscard]] int depth() const { return 0; }

    /// @brief A no-argument invokable.
    Q_INVOKABLE void refresh() {}

    /// @brief A one-argument invokable.
    /// @param id Ignored; the audit matches on arity, never on value.
    Q_INVOKABLE void open(qlonglong id) { Q_UNUSED(id) }

signals:
    /// @brief A two-parameter signal.
    /// @param rows Ignored.
    /// @param ok   Ignored.
    void listed(const QVariantList& rows, bool ok);

    /// @brief `depth`'s change notification.
    void depthChanged();
};

/// @brief A second stand-in, so file-scoped bindings have two objects to
///        distinguish.
class OtherFixtureBridge : public QObject {
    Q_OBJECT

public:
    /// @brief The only member; nothing else is needed of it.
    Q_INVOKABLE void ping() {}
};

/// @brief A shared base that publishes one signal — the shape `bank`'s
///        `BankController` has, with six controllers inheriting its `error`.
class BaseFixtureBridge : public QObject {
    Q_OBJECT

signals:
    /// @brief The signal every derived bridge inherits.
    /// @param message Ignored.
    void failed(const QString& message);
};

/// @brief A bridge whose only signal is its base's.
class DerivedFixtureBridge : public BaseFixtureBridge {
    Q_OBJECT

public:
    /// @brief The one member this class declares itself.
    Q_INVOKABLE void act() {}
};

/// @brief Writes @p contents to `<dir>/<name>` and returns the path.
/// @param dir      Directory to write into.
/// @param name     File name, e.g. `"Main.qml"`.
/// @param contents The QML text.
/// @return The written file's absolute path.
QString writeQml(const QTemporaryDir& dir, const QString& name, const QString& contents) {
    const QString path = QDir(dir.path()).filePath(name);
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream stream(&file);
    stream << contents;
    return path;
}

/// @brief The QML every "one thing is broken" case starts from: it binds
///        every member of `SurfaceFixtureBridge` exactly once and nothing
///        else, so the audit is clean against it.
/// @return The QML text.
QString cleanQml() {
    return QStringLiteral(R"(
import QtQuick

Item {
    id: page
    property var fixture: null

    readonly property string heading: page.fixture ? page.fixture.title : ""
    readonly property int level: page.fixture ? page.fixture.depth : 0

    function reload() {
        page.fixture.refresh()
        page.fixture.open(42)
    }

    Connections {
        target: page.fixture

        function onListed(rows, ok) {
            console.log(rows, ok)
        }
    }
}
)");
}

/// @brief Joins @p findings for a Catch2 `INFO` line.
/// @param findings The audit's output.
/// @return One string, newline separated.
std::string describe(const QStringList& findings) { return findings.join(QStringLiteral("\n")).toStdString(); }

}  // namespace

// ═════════════════════════════════════════════════════════════════════════
// The baseline: agreement is silence
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("QmlSurfaceAudit: a QML file binding exactly the bridge's surface produces no findings",
          "[testkit][qml-surface]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("Main.qml"), cleanQml());

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("fixture"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    CHECK(findings.isEmpty());
    CHECK(audit.scannedFiles() == QStringList{QStringLiteral("Main.qml")});
}

// ═════════════════════════════════════════════════════════════════════════
// Direction 1 — QML binds what the bridge does not have.
//
// This is the direction a hand-written metaobject checklist structurally
// cannot cover: its expectation *is* a human transcription of the QML, so a
// QML file that binds a name the bridge never had satisfies the checklist by
// construction.
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("QmlSurfaceAudit: a Connections handler for a signal the bridge does not emit is a finding",
          "[testkit][qml-surface]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    // `onListd` — one letter short of `listed`, the exact silent failure QML's
    // string binding produces: no warning anywhere, the handler simply never
    // runs.
    writeQml(dir, QStringLiteral("Main.qml"),
             cleanQml().replace(QStringLiteral("function onListed(rows, ok)"),
                                QStringLiteral("function onListd(rows, ok)")));

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("fixture"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    REQUIRE_FALSE(findings.isEmpty());
    CHECK_THAT(describe(findings), Catch::Matchers::ContainsSubstring("handles 'onListd'"));
    CHECK_THAT(describe(findings), Catch::Matchers::ContainsSubstring("emits no signal 'listd'"));
    // ...and the now-unhandled real signal is reported from the other side too.
    CHECK_THAT(describe(findings), Catch::Matchers::ContainsSubstring("SurfaceFixtureBridge::listed is a signal"));
}

TEST_CASE("QmlSurfaceAudit: a call to an invokable the bridge does not have is a finding", "[testkit][qml-surface]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("Main.qml"),
             cleanQml().replace(QStringLiteral("page.fixture.refresh()"),
                                QStringLiteral("page.fixture.refresh()\n        page.fixture.reloadEverything()")));

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("fixture"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    CHECK_THAT(describe(findings),
               Catch::Matchers::ContainsSubstring("calls 'fixture.reloadEverything()' but SurfaceFixtureBridge has "
                                                  "no such invokable"));
}

TEST_CASE("QmlSurfaceAudit: reading a property the bridge does not have is a finding", "[testkit][qml-surface]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("Main.qml"),
             cleanQml().replace(QStringLiteral("page.fixture.title"), QStringLiteral("page.fixture.heading")));

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("fixture"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    CHECK_THAT(describe(findings),
               Catch::Matchers::ContainsSubstring("reads 'fixture.heading' but SurfaceFixtureBridge has no such "
                                                  "property"));
}

TEST_CASE("QmlSurfaceAudit: calling an invokable with the wrong number of arguments is a finding",
          "[testkit][qml-surface]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(
        dir, QStringLiteral("Main.qml"),
        cleanQml().replace(QStringLiteral("page.fixture.open(42)"), QStringLiteral("page.fixture.open(42, true)")));

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("fixture"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    CHECK_THAT(describe(findings), Catch::Matchers::ContainsSubstring("calls 'fixture.open()' with 2 argument(s) but "
                                                                      "SurfaceFixtureBridge::open takes 1"));
}

TEST_CASE("QmlSurfaceAudit: a handler declaring more parameters than the signal carries is a finding",
          "[testkit][qml-surface]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("Main.qml"),
             cleanQml().replace(QStringLiteral("function onListed(rows, ok)"),
                                QStringLiteral("function onListed(rows, ok, total)")));

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("fixture"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    CHECK_THAT(describe(findings), Catch::Matchers::ContainsSubstring("handles 'onListed' with 3 parameter(s) but "
                                                                      "SurfaceFixtureBridge::listed carries 2"));
}

// ═════════════════════════════════════════════════════════════════════════
// Direction 2 — the bridge exposes what no QML binds
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("QmlSurfaceAudit: an invokable no scanned QML calls is a finding", "[testkit][qml-surface]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("Main.qml"), cleanQml().replace(QStringLiteral("page.fixture.open(42)"), QString()));

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("fixture"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    CHECK_THAT(describe(findings),
               Catch::Matchers::ContainsSubstring("SurfaceFixtureBridge::open is invokable from QML but no scanned "
                                                  ".qml calls it"));

    SECTION("and allowUnbound() silences exactly that one, with a recorded reason") {
        QmlSurfaceAudit exempted{dir.path()};
        exempted.bind(QStringLiteral("fixture"), bridge);
        exempted.allowUnbound(QStringLiteral("fixture"), QStringLiteral("open"),
                              QStringLiteral("bound from the shipped renderer's own QML"));
        const QStringList remaining = exempted.run();
        INFO(describe(remaining));
        CHECK(remaining.isEmpty());
    }
}

TEST_CASE("QmlSurfaceAudit: a Q_PROPERTY no scanned QML reads is a finding", "[testkit][qml-surface]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("Main.qml"),
             cleanQml().replace(QStringLiteral("page.fixture ? page.fixture.title : \"\""), QStringLiteral("\"\"")));

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("fixture"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    CHECK_THAT(describe(findings),
               Catch::Matchers::ContainsSubstring("SurfaceFixtureBridge::title is a Q_PROPERTY no scanned .qml "
                                                  "reads"));
}

TEST_CASE("QmlSurfaceAudit: a property's NOTIFY signal counts as bound when the property is read",
          "[testkit][qml-surface]") {
    // `depthChanged` is never handled anywhere in the clean fixture QML, and
    // must not be reported: QML binds `depth` directly and the engine
    // subscribes to the notification on its behalf.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("Main.qml"), cleanQml());

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("fixture"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    CHECK_THAT(describe(findings), !Catch::Matchers::ContainsSubstring("depthChanged"));
}

// ═════════════════════════════════════════════════════════════════════════
// The scanner itself: what it must not read, and what it must not miss
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("QmlSurfaceAudit: a member named only in a comment neither creates nor satisfies a reference",
          "[testkit][qml-surface]") {
    // The rungs' QML carries long prose comments that quote real binding
    // sites verbatim. Scanning raw text would invent references that do not
    // exist (a comment naming a member the bridge lacks would fail the audit)
    // and let a comment satisfy the unreferenced-member sweep (a member only
    // *mentioned* would look bound).
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("Main.qml"),
             cleanQml()
                 .replace(QStringLiteral("page.fixture.open(42)"),
                          QStringLiteral("// page.fixture.open(42) — described, not called"))
                 .append(QStringLiteral("\n// page.fixture.thisMemberDoesNotExist()\n"
                                        "/* page.fixture.norDoesThisOne */\n")));

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("fixture"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    // Nothing invented from the comments...
    CHECK_THAT(describe(findings), !Catch::Matchers::ContainsSubstring("thisMemberDoesNotExist"));
    CHECK_THAT(describe(findings), !Catch::Matchers::ContainsSubstring("norDoesThisOne"));
    // ...and the commented-out call did not save `open` from the sweep.
    CHECK_THAT(describe(findings),
               Catch::Matchers::ContainsSubstring("SurfaceFixtureBridge::open is invokable from QML but no scanned "
                                                  ".qml calls it"));
}

TEST_CASE("QmlSurfaceAudit: a member named only inside a string literal is not a reference",
          "[testkit][qml-surface]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(
        dir, QStringLiteral("Main.qml"),
        cleanQml().append(QStringLiteral("\n// trailing\nItem { property string doc: \"page.fixture.ghost()\" }\n")));

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("fixture"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    CHECK(findings.isEmpty());
}

TEST_CASE("blankCommentsAndStrings preserves line count and code outside comments and strings",
          "[testkit][qml-surface]") {
    const QString source = QStringLiteral("a // one\nb /* two\nthree */ c\nd \"e\" f\n");
    const QString blanked = blankCommentsAndStrings(source);
    REQUIRE(blanked.size() == source.size());
    CHECK(blanked.count(QLatin1Char('\n')) == source.count(QLatin1Char('\n')));
    CHECK_THAT(blanked.toStdString(), !Catch::Matchers::ContainsSubstring("one"));
    CHECK_THAT(blanked.toStdString(), !Catch::Matchers::ContainsSubstring("two"));
    CHECK_THAT(blanked.toStdString(), !Catch::Matchers::ContainsSubstring("three"));
    CHECK_THAT(blanked.toStdString(), Catch::Matchers::ContainsSubstring("c"));
    CHECK_THAT(blanked.toStdString(), Catch::Matchers::ContainsSubstring("d"));
    CHECK_THAT(blanked.toStdString(), Catch::Matchers::ContainsSubstring("f"));
}

// ═════════════════════════════════════════════════════════════════════════
// Vacuity — the audit must fail rather than pass when it measured nothing
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("QmlSurfaceAudit: an audit that measured nothing reports that, rather than passing",
          "[testkit][qml-surface]") {
    SurfaceFixtureBridge bridge;

    SECTION("no bridge bound") {
        QTemporaryDir dir;
        REQUIRE(dir.isValid());
        writeQml(dir, QStringLiteral("Main.qml"), cleanQml());
        const QmlSurfaceAudit audit{dir.path()};
        CHECK_THAT(describe(audit.run()), Catch::Matchers::ContainsSubstring("no bridge was bound"));
    }

    SECTION("no .qml files under the directory") {
        QTemporaryDir empty;
        REQUIRE(empty.isValid());
        QmlSurfaceAudit audit{empty.path()};
        audit.bind(QStringLiteral("fixture"), bridge);
        CHECK_THAT(describe(audit.run()), Catch::Matchers::ContainsSubstring("no .qml files found"));
    }

    SECTION("the directory does not exist") {
        QmlSurfaceAudit audit{QStringLiteral("/no/such/directory/anywhere")};
        audit.bind(QStringLiteral("fixture"), bridge);
        CHECK_THAT(describe(audit.run()), Catch::Matchers::ContainsSubstring("no such QML directory"));
    }

    SECTION("the alias appears in no scanned file") {
        QTemporaryDir dir;
        REQUIRE(dir.isValid());
        writeQml(dir, QStringLiteral("Main.qml"), cleanQml());
        QmlSurfaceAudit audit{dir.path()};
        audit.bind(QStringLiteral("misspelledAlias"), bridge);
        CHECK_THAT(describe(audit.run()), Catch::Matchers::ContainsSubstring("referenced by no scanned .qml"));
    }
}

TEST_CASE("QmlSurfaceAudit: a Connections block targeting an unbound alias is a finding", "[testkit][qml-surface]") {
    // Forgetting a bind() call is how a bridge silently ends up with no guard
    // at all, so the audit refuses to be quiet about a signal consumer it was
    // never handed.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("Main.qml"),
             cleanQml().append(QStringLiteral("\nItem {\n    id: extra\n    property var otherBridge: null\n"
                                              "    Connections {\n        target: extra.otherBridge\n"
                                              "        function onPinged() {}\n    }\n}\n")));

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("fixture"), bridge);

    CHECK_THAT(describe(audit.run()),
               Catch::Matchers::ContainsSubstring("a Connections block targets 'otherBridge' but no bridge was "
                                                  "bound to that alias"));
}

// ═════════════════════════════════════════════════════════════════════════
// Context-property shells — `Connections { target: <bare alias> }`
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("QmlSurfaceAudit: a Connections block on a bare alias is scanned", "[testkit][qml-surface]") {
    // A shell that publishes its bridges with QQmlContext::setContextProperty
    // rather than setInitialProperties writes the target as a bare identifier,
    // because the bridge is a root-context name and not a property of
    // anything. examples/bank/gui/main.cpp is one, and every one of its six
    // `Connections { target: app }` blocks went unscanned while the audit
    // required an `<id>.<alias>` target -- six handlers whose signal names
    // nothing checked at all.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    SurfaceFixtureBridge bridge;

    const auto contextPropertyQml = [](const QString& handler) {
        return QStringLiteral(R"(
import QtQuick
Item {
    id: page
    readonly property string heading: fixture.title
    readonly property int level: fixture.depth
    function reload() { fixture.refresh(); fixture.open(42) }
    Connections {
        target: fixture
        function %1(rows, ok) { console.log(rows, ok) }
    }
}
)")
            .arg(handler);
    };

    SECTION("and agreement is still silence") {
        writeQml(dir, QStringLiteral("Main.qml"), contextPropertyQml(QStringLiteral("onListed")));
        QmlSurfaceAudit audit{dir.path()};
        audit.bind(QStringLiteral("fixture"), bridge);

        const QStringList findings = audit.run();
        INFO(describe(findings));
        CHECK(findings.isEmpty());
    }

    SECTION("a handler for a signal the bridge lacks is a finding") {
        writeQml(dir, QStringLiteral("Main.qml"), contextPropertyQml(QStringLiteral("onListd")));
        QmlSurfaceAudit audit{dir.path()};
        audit.bind(QStringLiteral("fixture"), bridge);

        CHECK_THAT(describe(audit.run()),
                   Catch::Matchers::ContainsSubstring("handles 'onListd' but SurfaceFixtureBridge emits no "
                                                      "signal 'listd'"));
    }
}

TEST_CASE("QmlSurfaceAudit: a bare Connections target that is not a bound alias is left alone",
          "[testkit][qml-surface]") {
    // The cost of reading bare targets: an identifier that is not an alias is
    // far more often a local `id`, and nothing distinguishes the two. Such a
    // block must therefore be ignored outright rather than reported as an
    // unbound bridge -- otherwise every `Connections { target: someTimer }` in
    // every rung becomes a finding.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("Main.qml"),
             cleanQml().append(QStringLiteral("\nItem {\n    Timer { id: ticker }\n"
                                              "    Connections {\n        target: ticker\n"
                                              "        function onTriggered() {}\n    }\n}\n")));

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("fixture"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    CHECK(findings.isEmpty());
}

// ═════════════════════════════════════════════════════════════════════════
// Inherited surface — resolved in one direction, swept in the other
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("QmlSurfaceAudit: a handler for an inherited signal is correct QML", "[testkit][qml-surface]") {
    // `bank`'s six controllers each inherit `error` from a shared
    // `BankController` base and every screen handles it. Resolving `onFailed`
    // against the derived class's *own* members only would report all six as
    // broken screens, which is backwards: QML reaches inherited members
    // exactly as it reaches declared ones.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("Main.qml"), QStringLiteral(R"(
import QtQuick
Item {
    id: page
    property var derived: null
    function go() { page.derived.act() }
    Connections {
        target: page.derived
        function onFailed(message) { console.log(message) }
    }
}
)"));

    DerivedFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("derived"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    CHECK(findings.isEmpty());
}

TEST_CASE("QmlSurfaceAudit: an inherited member no QML binds is not swept", "[testkit][qml-surface]") {
    // The other half of the same decision. `failed` is the base's to answer
    // for; reporting it once per derived bridge would be noise no rung could
    // act on, and there is no per-derived-class fix for it. Only `act`, which
    // this class declares, is swept -- and it is, so the audit is not simply
    // quiet here.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("Main.qml"), QStringLiteral(R"(
import QtQuick
Item {
    id: page
    property var derived: null
    readonly property string nothing: page.derived ? page.derived.objectName : ""
}
)"));

    DerivedFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("derived"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    CHECK(findings.size() == 1);
    CHECK_THAT(describe(findings),
               Catch::Matchers::ContainsSubstring(
                   "DerivedFixtureBridge::act is invokable from QML but no scanned .qml calls it"));
}

// ═════════════════════════════════════════════════════════════════════════
// File-scoped bindings — one alias name, different bridges per file
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("QmlSurfaceAudit: bindIn() scopes an alias to one file", "[testkit][qml-surface]") {
    // `ledger`'s sub-views each call their own bridge `bridge`, so the same
    // alias must be resolvable to a different class per file.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("First.qml"), QStringLiteral(R"(
import QtQuick
Item {
    id: page
    property var bridge: null
    readonly property string heading: page.bridge ? page.bridge.title : ""
    readonly property int level: page.bridge ? page.bridge.depth : 0
    function reload() { page.bridge.refresh(); page.bridge.open(1) }
    Connections {
        target: page.bridge
        function onListed(rows, ok) { console.log(rows, ok) }
    }
}
)"));
    writeQml(dir, QStringLiteral("Second.qml"), QStringLiteral(R"(
import QtQuick
Item {
    id: page
    property var bridge: null
    function poke() { page.bridge.ping() }
}
)"));

    SurfaceFixtureBridge first;
    OtherFixtureBridge second;
    QmlSurfaceAudit audit{dir.path()};
    audit.bindIn(QStringLiteral("First.qml"), QStringLiteral("bridge"), first);
    audit.bindIn(QStringLiteral("Second.qml"), QStringLiteral("bridge"), second);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    CHECK(findings.isEmpty());
}

TEST_CASE("QmlSurfaceAudit: one object reached through two aliases has its coverage pooled",
          "[testkit][qml-surface]") {
    // `ledger`'s Main.qml calls its bridge `ledgerBridge` and hands the same
    // instance to a sub-view that calls it `bridge`. A member bound through
    // either alias is bound, so the sweep must run once per *object*, not once
    // per binding — otherwise every member one alias happens not to use is
    // reported.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("Shell.qml"), QStringLiteral(R"(
import QtQuick
Item {
    id: root
    property var shellAlias: null
    readonly property string heading: root.shellAlias ? root.shellAlias.title : ""
    function boot() { root.shellAlias.refresh() }
    Connections {
        target: root.shellAlias
        function onListed(rows, ok) { console.log(rows, ok) }
    }
}
)"));
    writeQml(dir, QStringLiteral("Pane.qml"), QStringLiteral(R"(
import QtQuick
Item {
    id: pane
    property var paneAlias: null
    readonly property int level: pane.paneAlias ? pane.paneAlias.depth : 0
    function pick() { pane.paneAlias.open(7) }
}
)"));

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bindIn(QStringLiteral("Shell.qml"), QStringLiteral("shellAlias"), bridge);
    audit.bindIn(QStringLiteral("Pane.qml"), QStringLiteral("paneAlias"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    CHECK(findings.isEmpty());
}

TEST_CASE("QmlSurfaceAudit: an exemption that no longer suppresses anything is itself a finding",
          "[testkit][qml-surface]") {
    // `allowUnbound()` is a second hand-written list, so it gets the same
    // treatment as the first: it may not rot unnoticed.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("Main.qml"), cleanQml());
    SurfaceFixtureBridge bridge;

    SECTION("a member the bridge does not have") {
        QmlSurfaceAudit audit{dir.path()};
        audit.bind(QStringLiteral("fixture"), bridge);
        audit.allowUnbound(QStringLiteral("fixture"), QStringLiteral("deletedLastYear"),
                           QStringLiteral("bound from somewhere else"));
        CHECK_THAT(describe(audit.run()),
                   Catch::Matchers::ContainsSubstring("stale allowUnbound('fixture', 'deletedLastYear'"));
    }

    SECTION("an alias no bridge is bound to") {
        QmlSurfaceAudit audit{dir.path()};
        audit.bind(QStringLiteral("fixture"), bridge);
        audit.allowUnbound(QStringLiteral("someOtherAlias"), QStringLiteral("refresh"),
                           QStringLiteral("bound from somewhere else"));
        CHECK_THAT(describe(audit.run()), Catch::Matchers::ContainsSubstring("no bridge is bound to that alias"));
    }

    SECTION("a member the QML does bind after all") {
        QmlSurfaceAudit audit{dir.path()};
        audit.bind(QStringLiteral("fixture"), bridge);
        audit.allowUnbound(QStringLiteral("fixture"), QStringLiteral("refresh"),
                           QStringLiteral("bound from somewhere else"));
        CHECK_THAT(describe(audit.run()),
                   Catch::Matchers::ContainsSubstring("unnecessary allowUnbound('fixture', 'refresh'"));
    }

    SECTION("a NOTIFY signal the QML covers transitively, by reading its property") {
        // `depthChanged` is never named in the QML at all -- `cleanQml()`
        // reads `depth` directly, and the engine subscribes to the
        // notification on its own behalf (see the "counts as bound when the
        // property is read" case above). An exemption recorded against the
        // signal by name is therefore just as redundant as one recorded
        // against a directly-called member: the property read covers it, so
        // the exemption suppresses nothing and must report itself as such.
        QmlSurfaceAudit audit{dir.path()};
        audit.bind(QStringLiteral("fixture"), bridge);
        audit.allowUnbound(QStringLiteral("fixture"), QStringLiteral("depthChanged"),
                           QStringLiteral("bound from somewhere else"));
        CHECK_THAT(describe(audit.run()),
                   Catch::Matchers::ContainsSubstring("unnecessary allowUnbound('fixture', 'depthChanged'"));
    }
}

#include "test_qml_surface.moc"

// ── Optional surface: a read that probes for `undefined` is a question ──────
//
// A member can exist in one configure and not another -- kanban's
// `BoardBridge::deadLetterCount` is behind MORPH_BUILD_OFFLINE_SQLITE -- and the
// QML that uses it guards the read so the binding stays inert when it is
// absent. Reporting that as "reads a property the bridge does not have" is
// backwards: the only way to satisfy the audit would be to delete the guard
// that makes the QML correct.
//
// Every shape the scanner recognises gets its own case here. The first draft of
// this rule shipped four comparison spellings and a `typeof` branch with
// exactly one of them exercised -- by a rung whose QML happens to use `!==` --
// and the `typeof` branch turned out not to work at all.

namespace {

/// @brief QML that reads `fixture.<member>`, optionally guarding the read.
/// @param member The member to read -- one the fixture bridge does not have.
/// @param guard  The guard expression, or empty for an unguarded read.
/// @return QML text: the clean surface plus this one extra read.
QString qmlReading(const QString& member, const QString& guard) {
    const QString line =
        guard.isEmpty()
            ? QStringLiteral("    readonly property int extra: page.fixture.%1").arg(member)
            : QStringLiteral("    readonly property int extra: %1 ? page.fixture.%2 : 0").arg(guard, member);
    return cleanQml().replace(QStringLiteral("    function reload() {"),
                              line + QStringLiteral("\n\n    function reload() {"));
}

}  // namespace

TEST_CASE("QmlSurfaceAudit: a read guarded against undefined is a probe, not a finding", "[testkit][qml-surface]") {
    // Every spelling the scanner accepts. `queueDepth` is deliberately not on
    // the fixture bridge: the QML is asking whether it exists.
    const QString guard = GENERATE(QStringLiteral("page.fixture.queueDepth !== undefined"),
                                   QStringLiteral("page.fixture.queueDepth === undefined"),
                                   QStringLiteral("page.fixture.queueDepth != undefined"),
                                   QStringLiteral("page.fixture.queueDepth == undefined"),
                                   QStringLiteral("typeof page.fixture.queueDepth"));
    INFO("guard: " << guard.toStdString());

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("Main.qml"), qmlReading(QStringLiteral("queueDepth"), guard));

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("fixture"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    CHECK(findings.isEmpty());
}

TEST_CASE("QmlSurfaceAudit: an unguarded read of a member the bridge lacks is still a finding",
          "[testkit][qml-surface]") {
    // The teeth. A narrowing rule can only silence findings, so the case that
    // matters is the one it must NOT silence.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("Main.qml"), qmlReading(QStringLiteral("queueDepth"), QString{}));

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("fixture"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    REQUIRE(findings.size() == 1);
    CHECK_THAT(findings.first().toStdString(), Catch::Matchers::ContainsSubstring("queueDepth"));
    CHECK_THAT(findings.first().toStdString(), Catch::Matchers::ContainsSubstring("no such property"));
}

TEST_CASE("QmlSurfaceAudit: a probe in one file does not excuse an unguarded read in another",
          "[testkit][qml-surface]") {
    // The file scoping the rule documents. A guard is normally written once, on
    // the binding that gates the rest of a view, so it excuses every read of
    // that member *within its file* and nothing beyond it. Without the scope one
    // guard anywhere would blind the audit to that member everywhere.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    writeQml(dir, QStringLiteral("Main.qml"),
             qmlReading(QStringLiteral("queueDepth"), QStringLiteral("page.fixture.queueDepth !== undefined")));
    writeQml(dir, QStringLiteral("Other.qml"), qmlReading(QStringLiteral("queueDepth"), QString{}));

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("fixture"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    REQUIRE(findings.size() == 1);
    CHECK_THAT(findings.first().toStdString(), Catch::Matchers::ContainsSubstring("Other.qml"));
}

TEST_CASE("QmlSurfaceAudit: a guarded read of a member the bridge DOES have still counts as binding it",
          "[testkit][qml-surface]") {
    // A probe must not become a way to hide dead surface: guarding a read of a
    // member that exists still means the QML uses it, so it must not then be
    // reported as unbound. `depth` is on the fixture bridge.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString qml =
        cleanQml().replace(QStringLiteral("page.fixture.depth : 0"),
                           QStringLiteral("page.fixture.depth !== undefined ? page.fixture.depth : 0"));
    writeQml(dir, QStringLiteral("Main.qml"), qml);

    SurfaceFixtureBridge bridge;
    QmlSurfaceAudit audit{dir.path()};
    audit.bind(QStringLiteral("fixture"), bridge);

    const QStringList findings = audit.run();
    INFO(describe(findings));
    CHECK(findings.isEmpty());
}
