// SPDX-License-Identifier: Apache-2.0
//
// The QML adapters' own suite, and specifically the half the offscreen
// engine-load smoke test cannot reach.
//
// `test_gui_qml_smoke.cpp` proves every QML file parses and every *type* it
// names resolves. It cannot prove that a `Connections` handler is bound to a
// signal that exists, or that a delegate reads a key the model actually
// supplies: both are resolved dynamically, against objects that test never
// supplies. Those two are exactly the mistakes a hand-written bridge makes,
// so they are asserted here, from the other side — against the real bridge's
// metaobject and its real property bags.
//
// The rest of the file is ordinary adapter coverage: a typed DTO in, the
// right `QVariantMap` keys out, with the conversions that carry meaning
// (exact decimal text, enum names, `-1` for an unengaged id) checked rather
// than assumed.

#include "lims_qml_conversions.hpp"
#include "result_qml_bridge.hpp"
#include "sample_qml_bridge.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QMetaMethod>
#include <QMetaObject>
#include <QVariantMap>

#include <glaze/glaze.hpp>

#include <morph/session/session.hpp>
#include <morph/util/rational.hpp>

#include <memory>
#include <string>

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

/// @brief An exact rational at @p places decimal places.
/// @param num Numerator.
/// @param den Denominator.
/// @param places Decimal-precision tag.
/// @return The canonical rational.
[[nodiscard]] Rational exact(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return Rational{Numerator{num}, Denominator{den}, DecimalPlaces{places}};
}

/// @brief A rig whose one bridge already carries a session for @p principal.
/// @param principal The identity to install.
/// @return The rig.
[[nodiscard]] std::unique_ptr<BackendRig> makeAuthedRig(std::string principal) {
    auto rig = std::make_unique<BackendRig>(Mode::Local, 1);
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    rig->bridge(0).setDefaultSession(ctx);
    return rig;
}

/// @brief Counts emissions of one signal, and keeps the last payload.
///
/// `QSignalSpy` would do this, but it lives in `Qt6::Test`, which a rung's
/// test target does not link (and should not: `morph_ladder_gui` is
/// Qt-Core-only by presenter rule 1, and pulling Qt Test in for a counter
/// would be the tail wagging the dog). A four-line struct plus a lambda is
/// the same thing without the dependency.
struct Emissions {
    /// @brief How many times the connected signal has fired.
    int count = 0;

    /// @brief The most recent payload, for the signals that carry one.
    QVariantMap last;
};

/// @brief Whether @p object's metaobject declares a signal named @p name.
///
/// This is what makes a QML `Connections { function onFoo() }` binding
/// checkable from C++: QML derives the handler name from the signal name, so
/// a signal that does not exist is a handler that never fires — silently.
/// @param object The object whose metaobject to search.
/// @param name The signal name, without the `on` prefix or a signature.
/// @return `true` when a signal of that name exists.
[[nodiscard]] bool hasSignalNamed(const QObject& object, const char* name) {
    const auto* meta = object.metaObject();
    for (int i = 0; i < meta->methodCount(); ++i) {
        const auto method = meta->method(i);
        if (method.methodType() == QMetaMethod::Signal && method.name() == QByteArray{name}) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("Every signal the QML files connect to exists on the bridge that emits it",
          "[lims][gui][qml-bridge]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    lims::gui::SampleBridge sampleBridge{rig->bridge(0), rig->executor()};
    lims::gui::ResultBridge resultBridge{rig->bridge(0), rig->executor()};

    // Main.qml: `Connections { target: root.sampleBridge; function
    // onSampleChanged(sample) {...} }`. If this signal is ever renamed, the
    // handler stops firing and the smoke test still passes — so it is pinned
    // here.
    CHECK(hasSignalNamed(sampleBridge, "sampleChanged"));
    // Every other signal a `NOTIFY` clause or a view binds to.
    CHECK(hasSignalNamed(sampleBridge, "clientRegistered"));
    CHECK(hasSignalNamed(sampleBridge, "failed"));
    CHECK(hasSignalNamed(sampleBridge, "bound"));

    CHECK(hasSignalNamed(resultBridge, "analysesListed"));
    CHECK(hasSignalNamed(resultBridge, "resultsListed"));
    CHECK(hasSignalNamed(resultBridge, "conflictsListed"));
    CHECK(hasSignalNamed(resultBridge, "resultCaptured"));
    CHECK(hasSignalNamed(resultBridge, "resultVerified"));
    CHECK(hasSignalNamed(resultBridge, "conflictResolved"));
    CHECK(hasSignalNamed(resultBridge, "sampleAttached"));
    CHECK(hasSignalNamed(resultBridge, "failed"));
    CHECK(hasSignalNamed(resultBridge, "bound"));
}

TEST_CASE("The sample property bag carries every key the QML reads", "[lims][gui][qml-bridge]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    lims::gui::SampleBridge bridge{rig->bridge(0), rig->executor()};

    Emissions registered;
    QObject::connect(&bridge, &lims::gui::SampleBridge::clientRegistered, [&registered] { ++registered.count; });
    bridge.registerClient(QStringLiteral("Waterworks Ltd"));
    REQUIRE(pumpUntil([&] { return registered.count == 1; }));
    CHECK(bridge.clientId() >= 1);

    Emissions changed;
    QObject::connect(&bridge, &lims::gui::SampleBridge::sampleChanged, [&changed] { ++changed.count; });
    bridge.registerSample(bridge.clientId(), QStringLiteral("WW-1"));
    REQUIRE(pumpUntil([&] { return changed.count == 1; }));

    const auto sample = bridge.sample();
    // Main.qml reads id/state/version; SampleView.qml reads state. A missing
    // key is `undefined` in QML — a blank label, never an error.
    for (const auto* key : {"id", "clientId", "reference", "state", "version", "registeredAtMs"}) {
        INFO("missing key: " << key);
        CHECK(sample.contains(QString::fromUtf8(key)));
    }
    // The state is the model's own stable name, not an integer a binding
    // would have to translate.
    CHECK(sample.value(QStringLiteral("state")).toString() == QStringLiteral("registered"));
    CHECK(sample.value(QStringLiteral("version")).toLongLong() == 1);
    CHECK(sample.value(QStringLiteral("reference")).toString() == QStringLiteral("WW-1"));
}

TEST_CASE("A refusal reaches QML as a message, and lastError holds it", "[lims][gui][qml-bridge]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    lims::gui::SampleBridge bridge{rig->bridge(0), rig->executor()};

    Emissions failed;
    QString emitted;
    QObject::connect(&bridge, &lims::gui::SampleBridge::failed, [&](const QString& message) {
        ++failed.count;
        emitted = message;
    });
    // Nothing is attached yet, so this cannot succeed.
    bridge.refresh();
    REQUIRE(pumpUntil([&] { return failed.count == 1; }));
    CHECK_FALSE(bridge.lastError().isEmpty());
    // The property and the signal payload are the same string: a view may
    // bind to either without them ever disagreeing.
    CHECK(bridge.lastError() == emitted);
    
}

TEST_CASE("A result row carries the exact decimal, not a rounded number", "[lims][gui][qml-bridge]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    lims::gui::SampleBridge sampleBridge{rig->bridge(0), rig->executor()};
    lims::gui::ResultBridge resultBridge{rig->bridge(0), rig->executor()};

    // A catalogue entry to capture against. Defined through the model
    // directly: this rung's GUI has no catalogue-editing surface, by design
    // (the picker is read-only).
    lims::AnalysisCatalogModel catalog;
    morph::session::Context ctx;
    ctx.principal = "alice";
    const morph::session::detail::ScopedContext scope{ctx};
    const auto nitrate = catalog.execute(
        lims::DefineAnalysis{.name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3});

    Emissions registered;
    QObject::connect(&sampleBridge, &lims::gui::SampleBridge::clientRegistered, [&registered] { ++registered.count; });
    sampleBridge.registerClient(QStringLiteral("Waterworks Ltd"));
    REQUIRE(pumpUntil([&] { return registered.count == 1; }));

    Emissions changed;
    QObject::connect(&sampleBridge, &lims::gui::SampleBridge::sampleChanged, [&changed] { ++changed.count; });
    sampleBridge.registerSample(sampleBridge.clientId(), QStringLiteral("WW-1"));
    REQUIRE(pumpUntil([&] { return changed.count == 1; }));
    sampleBridge.receiveSample();
    REQUIRE(pumpUntil([&] { return changed.count == 2; }));
    sampleBridge.startWork();
    REQUIRE(pumpUntil([&] { return changed.count == 3; }));

    const auto sampleId = sampleBridge.sample().value(QStringLiteral("id")).toLongLong();
    Emissions attached;
    QObject::connect(&resultBridge, &lims::gui::ResultBridge::sampleAttached, [&attached] { ++attached.count; });
    resultBridge.openSample(sampleId);
    REQUIRE(pumpUntil([&] { return attached.count == 1; }));

    // 2.4 typed into a QML TextField arrives as a double and must come back
    // out as the exact decimal "2.4" — the value stored is 12/5, and a lab
    // report that showed anything else would be showing a different number
    // from the one on file.
    Emissions captured;
    QObject::connect(&resultBridge, &lims::gui::ResultBridge::resultCaptured, [&captured] { ++captured.count; });
    resultBridge.captureReading(*nitrate.versionId, 2.4, QString{}, 0);
    REQUIRE(pumpUntil([&] { return captured.count == 1; }));

    Emissions listed;
    QObject::connect(&resultBridge, &lims::gui::ResultBridge::resultsListed, [&listed] { ++listed.count; });
    resultBridge.refreshResults();
    REQUIRE(pumpUntil([&] { return listed.count == 1; }));

    const auto rows = resultBridge.results();
    REQUIRE(rows.size() == 1);
    const auto row = rows.front().toMap();
    // ResultEntryView.qml's delegate reads exactly these.
    for (const auto* key : {"id", "qualifier", "valueText", "unit", "hasValue", "outOfSpec", "capturedBy"}) {
        INFO("missing key: " << key);
        CHECK(row.contains(QString::fromUtf8(key)));
    }
    CHECK(row.value(QStringLiteral("hasValue")).toBool());
    CHECK(row.value(QStringLiteral("valueText")).toString() == QStringLiteral("2.4"));
    // The unit is its own key, so a table can put it in the column header
    // rather than repeating it in every cell.
    CHECK(row.value(QStringLiteral("unit")).toString() == QStringLiteral("mg/L"));
    CHECK(row.value(QStringLiteral("qualifier")).toString() == QStringLiteral("measured"));
    CHECK(row.value(QStringLiteral("capturedBy")).toString() == QStringLiteral("alice"));
    // 2.4 mg/L is inside the fixture analysis's specification range, so the
    // row carries the flag as false rather than omitting it — a table cell
    // that is sometimes absent is a cell a delegate cannot bind to.
    CHECK_FALSE(row.value(QStringLiteral("outOfSpec")).toBool());
}

TEST_CASE("A no-number result names which claim it is, rather than showing a blank",
          "[lims][gui][qml-bridge]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    lims::gui::SampleBridge sampleBridge{rig->bridge(0), rig->executor()};
    lims::gui::ResultBridge resultBridge{rig->bridge(0), rig->executor()};

    lims::AnalysisCatalogModel catalog;
    morph::session::Context ctx;
    ctx.principal = "alice";
    const morph::session::detail::ScopedContext scope{ctx};
    const auto lead = catalog.execute(
        lims::DefineAnalysis{.name = "Lead", .canonicalUnit = "mg_per_L", .decimalPlaces = 3});

    Emissions registered;
    QObject::connect(&sampleBridge, &lims::gui::SampleBridge::clientRegistered, [&registered] { ++registered.count; });
    sampleBridge.registerClient(QStringLiteral("Waterworks Ltd"));
    REQUIRE(pumpUntil([&] { return registered.count == 1; }));
    Emissions changed;
    QObject::connect(&sampleBridge, &lims::gui::SampleBridge::sampleChanged, [&changed] { ++changed.count; });
    sampleBridge.registerSample(sampleBridge.clientId(), QStringLiteral("WW-1"));
    REQUIRE(pumpUntil([&] { return changed.count == 1; }));
    sampleBridge.receiveSample();
    REQUIRE(pumpUntil([&] { return changed.count == 2; }));
    sampleBridge.startWork();
    REQUIRE(pumpUntil([&] { return changed.count == 3; }));

    Emissions attached;
    QObject::connect(&resultBridge, &lims::gui::ResultBridge::sampleAttached, [&attached] { ++attached.count; });
    resultBridge.openSample(sampleBridge.sample().value(QStringLiteral("id")).toLongLong());
    REQUIRE(pumpUntil([&] { return attached.count == 1; }));

    Emissions captured;
    QObject::connect(&resultBridge, &lims::gui::ResultBridge::resultCaptured,
                     [&captured](const QVariantMap& row) {
                         ++captured.count;
                         captured.last = row;
                     });
    resultBridge.captureQualifier(*lead.versionId, QStringLiteral("belowLOD"));
    REQUIRE(pumpUntil([&] { return captured.count == 1; }));

    const auto row = captured.last;
    CHECK_FALSE(row.value(QStringLiteral("hasValue")).toBool());
    CHECK(row.value(QStringLiteral("valueText")).toString().isEmpty());
    // The distinction the whole §3 encoding exists to keep: the row says
    // *which* no-number claim it is, so a table cell can never collapse
    // "below the detection limit" into "we did not look".
    CHECK(row.value(QStringLiteral("qualifier")).toString() == QStringLiteral("belowLOD"));
}

TEST_CASE("An unengaged id crosses as -1, never as a real key", "[lims][gui][qml-bridge]") {
    // The sentinel the delegates and invokables rely on. A real key is never
    // -1 (ServerSideAutoIncrement starts at 1), so QML can test for it.
    CHECK(lims::gui::idNumber(lims::SampleId{}) == -1);
    CHECK(lims::gui::idNumber(lims::SampleId{7}) == 7);
    CHECK(lims::gui::idNumber(lims::ConflictId{}) == -1);
}

TEST_CASE("Quantity text is exact and empty-safe", "[lims][gui][qml-bridge]") {
    // 1/400 mg/L is 0.0025 — a trace concentration a float round trip would
    // already have perturbed.
    CHECK(lims::gui::valueText(lims::Concentration{exact(1, 400, 4)}) == QStringLiteral("0.0025"));
    CHECK(lims::gui::valueText(lims::Concentration{}).isEmpty());
}

// ── The schema-driven surface ──────────────────────────────────────────────

TEST_CASE("The served schema document carries a form for every typed-field action",
          "[lims][gui][qml-bridge][forms]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    lims::gui::SampleBridge sampleBridge{rig->bridge(0), rig->executor()};
    lims::gui::ResultBridge resultBridge{rig->bridge(0), rig->executor()};

    // The QML parses this with `JSON.parse`, so it has to be one valid
    // document — not six concatenated ones.
    const auto document = sampleBridge.schemasJson();
    CHECK(document == resultBridge.schemasJson());
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, document.toStdString()));

    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM
    for (const auto* actionType : {"RegisterClient", "RegisterSample", "RejectSample", "ReturnForRework",
                                   "CaptureConcentration", "ResolveConflict"}) {
        INFO("missing form: " << actionType);
        REQUIRE(dom.contains(actionType));
        // A schema a renderer can actually use, not an empty object.
        CHECK(dom[actionType].contains("properties"));
    }

    // The capture form is the one that matters: it is what puts the rung's
    // cross-field rules in front of the shipped renderer's own evaluator.
    const auto& capture = dom["CaptureConcentration"];
    REQUIRE(capture.contains("x-rules"));
    const auto rules = glz::write_json(capture["x-rules"]).value_or(std::string{});
    CHECK(rules.find("exactlyOneOf") != std::string::npos);
    CHECK(rules.find("requiredWhen") != std::string::npos);
    CHECK(rules.find("visibleWhen") != std::string::npos);
    // And the entry-unit machinery the operator actually types into.
    CHECK(glz::write_json(capture["properties"]["value"]).value_or(std::string{}).find("x-unitAlternatives") !=
          std::string::npos);
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

TEST_CASE("submitIfValid refuses an action its surface does not own, loudly",
          "[lims][gui][qml-bridge][forms]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    lims::gui::SampleBridge bridge{rig->bridge(0), rig->executor()};

    QString actionType;
    bool ok = true;
    QString payload;
    int replies = 0;
    QObject::connect(&bridge, &lims::gui::SampleBridge::replyReceived,
                     [&](const QString& type, bool succeeded, const QString& text) {
                         actionType = type;
                         ok = succeeded;
                         payload = text;
                         ++replies;
                     });

    // A typo in a QML `actionType` string must surface as a failed reply, not
    // as a button that silently does nothing — which is what a bare
    // `executeJson` on an unregistered name would look like from the screen.
    bridge.submitIfValid(QStringLiteral("PublishSample"), QStringLiteral("{}"));
    REQUIRE(pumpUntil([&] { return replies == 1; }));
    CHECK(actionType == QStringLiteral("PublishSample"));
    CHECK_FALSE(ok);
    CHECK(payload.contains(QStringLiteral("does not own")));
}

TEST_CASE("A form body submitted through the schema path reaches the model", "[lims][gui][qml-bridge][forms]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    lims::gui::SampleBridge bridge{rig->bridge(0), rig->executor()};

    QString payload;
    bool ok = false;
    int replies = 0;
    QObject::connect(&bridge, &lims::gui::SampleBridge::replyReceived,
                     [&](const QString&, bool succeeded, const QString& text) {
                         ok = succeeded;
                         payload = text;
                         ++replies;
                     });

    // The exact shape `DynamicForm::previewLine` produces for
    // schemaJson<RegisterClient>(): one JSON object keyed by wire field name.
    bridge.submitIfValid(QStringLiteral("RegisterClient"), QStringLiteral(R"({"name":"Waterworks Ltd"})"));
    REQUIRE(pumpUntil([&] { return replies == 1; }));
    INFO("reply: " << payload.toStdString());
    CHECK(ok);
    // The reply is the action's own result JSON, so a renderer can read the
    // new id straight out of it.
    CHECK(payload.contains(QStringLiteral("clientId")));
}
