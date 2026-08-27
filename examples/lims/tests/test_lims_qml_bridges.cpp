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

#include <QMetaMethod>
#include <QMetaObject>
#include <QVariantMap>
#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/session/session.hpp>
#include <morph/util/rational.hpp>
#include <string>

#include "lims_qml_conversions.hpp"
#include "result_qml_bridge.hpp"
#include "sample_qml_bridge.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

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

/// @brief Submits @p capture the way ResultEntryView.qml submits it: as one
///        schema-driven form body through `submitIfValid`.
///
/// The body is `glz::write_json` of the action itself rather than a hand-typed
/// literal, so the test cannot drift from the wire shape the model reads --
/// and, unlike a typed invokable, this path carries the reading as the exact
/// rational the schema declares instead of a `double` the entry point rounds.
/// @param bridge  The result bridge to submit through.
/// @param capture The capture to send.
/// @return `true` when the reply arrived and reported success.
[[nodiscard]] bool submitCapture(lims::gui::ResultBridge& bridge, const lims::CaptureConcentration& capture) {
    const auto body = glz::write_json(capture);
    if (!body) {
        return false;
    }
    bool ok = false;
    int replies = 0;
    const auto connection = QObject::connect(&bridge, &lims::gui::ResultBridge::replyReceived,
                                             [&](const QString&, bool succeeded, const QString& payload) {
                                                 ok = succeeded;
                                                 ++replies;
                                                 INFO("capture reply: " << payload.toStdString());
                                             });
    bridge.submitIfValid(QStringLiteral("CaptureConcentration"), QString::fromStdString(*body));
    const bool settled = pumpUntil([&] { return replies == 1; });
    QObject::disconnect(connection);
    return settled && ok;
}

/// @brief Registers a client and a sample through @p bridge the way the
///        shipped forms do — two `submitIfValid` calls — leaving the bridge
///        attached to the new sample.
///
/// Waits for `sampleChanged`, not just `RegisterSample`'s `replyReceived`:
/// the reply fires first, from inside `submitIfValid`'s success arm, but the
/// `sample` property is not populated until the `refresh()` that same arm
/// dispatches resolves in its own turn.
/// @param bridge The bridge to drive.
[[nodiscard]] bool registerSampleViaBridge(lims::gui::SampleBridge& bridge) {
    int clientReplies = 0;
    bool clientOk = false;
    const auto clientConn = QObject::connect(
        &bridge, &lims::gui::SampleBridge::replyReceived,
        [&](const QString& type, bool succeeded, const QString&) {
            if (type == QStringLiteral("RegisterClient")) {
                clientOk = succeeded;
                ++clientReplies;
            }
        });
    bridge.submitIfValid(QStringLiteral("RegisterClient"), QStringLiteral(R"({"name":"Waterworks Ltd"})"));
    const bool clientSettled = pumpUntil([&] { return clientReplies == 1; });
    QObject::disconnect(clientConn);
    if (!clientSettled || !clientOk) {
        return false;
    }

    bool sampleOk = false;
    const auto sampleConn = QObject::connect(
        &bridge, &lims::gui::SampleBridge::replyReceived,
        [&](const QString& type, bool succeeded, const QString&) {
            if (type == QStringLiteral("RegisterSample")) {
                sampleOk = succeeded;
            }
        });
    int changes = 0;
    const auto changedConn =
        QObject::connect(&bridge, &lims::gui::SampleBridge::sampleChanged, [&changes] { ++changes; });
    const auto body = QStringLiteral(R"({"clientId":%1,"reference":"WW-1"})").arg(bridge.clientId());
    bridge.submitIfValid(QStringLiteral("RegisterSample"), body);
    const bool sampleSettled = pumpUntil([&] { return changes == 1; });
    QObject::disconnect(sampleConn);
    QObject::disconnect(changedConn);
    return sampleSettled && sampleOk;
}

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

TEST_CASE("Every signal the QML files connect to exists on the bridge that emits it", "[lims][gui][qml-bridge]") {
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
    // Both views handle `onReplyReceived`: it is the only carrier a refused
    // schema-driven submission has.
    CHECK(hasSignalNamed(sampleBridge, "replyReceived"));

    CHECK(hasSignalNamed(resultBridge, "analysesListed"));
    CHECK(hasSignalNamed(resultBridge, "resultsListed"));
    CHECK(hasSignalNamed(resultBridge, "conflictsListed"));
    // ResultEntryView.qml: `function onResultVerified(verification)`, which is
    // how a recorded four-eyes verification reaches the table at all —
    // `verifyResult` is a typed call, so its success is not a `replyReceived`.
    CHECK(hasSignalNamed(resultBridge, "resultVerified"));
    CHECK(hasSignalNamed(resultBridge, "failed"));
    CHECK(hasSignalNamed(resultBridge, "replyReceived"));
}

TEST_CASE("The sample property bag carries every key the QML reads", "[lims][gui][qml-bridge]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    lims::gui::SampleBridge bridge{rig->bridge(0), rig->executor()};

    REQUIRE(registerSampleViaBridge(bridge));
    CHECK(bridge.clientId() >= 1);

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
    const auto nitrate =
        catalog.execute(lims::DefineAnalysis{.name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3});

    REQUIRE(registerSampleViaBridge(sampleBridge));

    Emissions changed;
    QObject::connect(&sampleBridge, &lims::gui::SampleBridge::sampleChanged, [&changed] { ++changed.count; });
    sampleBridge.receiveSample();
    REQUIRE(pumpUntil([&] { return changed.count == 1; }));
    sampleBridge.startWork();
    REQUIRE(pumpUntil([&] { return changed.count == 2; }));

    // Exactly Main.qml's own two lines: attach, then re-read. This surface
    // announces no attach of its own, so the listing arriving *is* the
    // evidence the attach landed -- and asserting it here is what keeps that
    // ordering honest.
    Emissions listed;
    QObject::connect(&resultBridge, &lims::gui::ResultBridge::resultsListed, [&listed] { ++listed.count; });
    Emissions failures;
    QObject::connect(&resultBridge, &lims::gui::ResultBridge::failed, [&failures] { ++failures.count; });
    resultBridge.openSample(sampleBridge.sample().value(QStringLiteral("id")).toLongLong());
    resultBridge.refreshResults();
    REQUIRE(pumpUntil([&] { return listed.count == 1 || failures.count > 0; }));
    INFO("attach or listing failed: " << resultBridge.lastError().toStdString());
    REQUIRE(failures.count == 0);

    // 2.4 mg/L, submitted the way the screen submits it: one schema-driven
    // form body through `submitIfValid`, so the reading crosses as the exact
    // rational the renderer builds and never as a `double`. It must come back
    // out as the exact decimal "2.4" — the value stored is 12/5, and a lab
    // report that showed anything else would be showing a different number
    // from the one on file.
    REQUIRE(submitCapture(resultBridge, lims::CaptureConcentration{.analysisVersionId = nitrate.versionId,
                                                                   .value = lims::Concentration{exact(12, 5, 3)}}));
    // `submitIfValid`'s success arm re-reads the listing itself, so a second
    // refresh here would be testing a call the screen never makes.
    REQUIRE(pumpUntil([&] { return listed.count == 2; }));

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

TEST_CASE("A no-number result names which claim it is, rather than showing a blank", "[lims][gui][qml-bridge]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    lims::gui::SampleBridge sampleBridge{rig->bridge(0), rig->executor()};
    lims::gui::ResultBridge resultBridge{rig->bridge(0), rig->executor()};

    lims::AnalysisCatalogModel catalog;
    morph::session::Context ctx;
    ctx.principal = "alice";
    const morph::session::detail::ScopedContext scope{ctx};
    const auto lead =
        catalog.execute(lims::DefineAnalysis{.name = "Lead", .canonicalUnit = "mg_per_L", .decimalPlaces = 3});

    REQUIRE(registerSampleViaBridge(sampleBridge));
    Emissions changed;
    QObject::connect(&sampleBridge, &lims::gui::SampleBridge::sampleChanged, [&changed] { ++changed.count; });
    sampleBridge.receiveSample();
    REQUIRE(pumpUntil([&] { return changed.count == 1; }));
    sampleBridge.startWork();
    REQUIRE(pumpUntil([&] { return changed.count == 2; }));

    Emissions listed;
    QObject::connect(&resultBridge, &lims::gui::ResultBridge::resultsListed, [&listed] { ++listed.count; });
    resultBridge.openSample(sampleBridge.sample().value(QStringLiteral("id")).toLongLong());
    resultBridge.refreshResults();
    REQUIRE(pumpUntil([&] { return listed.count == 1; }));

    // The other branch of the `exactlyOneOf` sum, through the same one form
    // the screen renders: an engaged `qualifier` and an empty `value`.
    REQUIRE(submitCapture(resultBridge, lims::CaptureConcentration{.analysisVersionId = lead.versionId,
                                                                   .qualifier = lims::QualifierChoice{
                                                                       std::string{lims::kQualifierBelowLod}}}));
    REQUIRE(pumpUntil([&] { return listed.count == 2; }));

    REQUIRE(resultBridge.results().size() == 1);
    const auto row = resultBridge.results().front().toMap();
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

TEST_CASE("The served schema document carries a form for every typed-field action", "[lims][gui][qml-bridge][forms]") {
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

TEST_CASE("submitIfValid refuses an action its surface does not own, loudly", "[lims][gui][qml-bridge][forms]") {
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

TEST_CASE("Registering a client and a sample through the form path leaves the shared handler "
          "attached and clientId populated",
          "[lims][gui][qml-bridge][forms]") {
    // `SamplePresenter::submitIfValid` routes `RegisterClient` to the plain
    // handler (the one action here with no key at all) and `RegisterSample` to
    // the shared one, decoding `RegisterClient`'s reply to emit
    // `clientRegistered` since the form path carries only raw JSON. This case
    // drives both through `submitIfValid` -- the only path a real
    // `DynamicForm` submits through -- and asserts both effects downstream of
    // the dispatch: `clientId` gets set, and the shared handler ends up
    // attached, so the follow-up `refresh()` (and the `sampleChanged` re-wiring
    // `Main.qml`'s `onSampleChanged` depends on) succeeds instead of failing
    // with "handler not bound".
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    lims::gui::SampleBridge bridge{rig->bridge(0), rig->executor()};

    Emissions registered;
    QObject::connect(&bridge, &lims::gui::SampleBridge::clientRegistered, [&registered] { ++registered.count; });
    QString clientPayload;
    bool clientOk = false;
    int clientReplies = 0;
    QObject::connect(&bridge, &lims::gui::SampleBridge::replyReceived,
                     [&](const QString& type, bool succeeded, const QString& text) {
                         if (type != QStringLiteral("RegisterClient")) {
                             return;
                         }
                         clientOk = succeeded;
                         clientPayload = text;
                         ++clientReplies;
                     });

    bridge.submitIfValid(QStringLiteral("RegisterClient"), QStringLiteral(R"({"name":"Waterworks Ltd"})"));
    REQUIRE(pumpUntil([&] { return clientReplies == 1; }));
    INFO("RegisterClient reply: " << clientPayload.toStdString());
    REQUIRE(clientOk);

    // `SampleView.qml:127`'s "Latest client id" label reads exactly this
    // property, so `clientRegistered` firing on the form path is what keeps it
    // off its `-1` default.
    REQUIRE(pumpUntil([&] { return registered.count == 1; }));
    CHECK(bridge.clientId() >= 1);

    Emissions changed;
    QObject::connect(&bridge, &lims::gui::SampleBridge::sampleChanged, [&changed] { ++changed.count; });
    QString samplePayload;
    bool sampleOk = false;
    int sampleReplies = 0;
    QObject::connect(&bridge, &lims::gui::SampleBridge::replyReceived,
                     [&](const QString& type, bool succeeded, const QString& text) {
                         if (type != QStringLiteral("RegisterSample")) {
                             return;
                         }
                         sampleOk = succeeded;
                         samplePayload = text;
                         ++sampleReplies;
                     });

    const auto body = QStringLiteral(R"({"clientId":%1,"reference":"WW-1"})").arg(bridge.clientId());
    bridge.submitIfValid(QStringLiteral("RegisterSample"), body);
    REQUIRE(pumpUntil([&] { return sampleReplies == 1; }));
    INFO("RegisterSample reply: " << samplePayload.toStdString());
    REQUIRE(sampleOk);

    // The success arm re-reads the attached sample (`SamplePresenter::
    // submitIfValid`'s `refresh()` call), which reaches the model only because
    // `RegisterSample` just attached the shared handler: `sampleChanged` firing
    // here is what `Main.qml`'s `onSampleChanged` cross-wiring needs to attach
    // the result surface.
    REQUIRE(pumpUntil([&] { return changed.count >= 1; }));
    CHECK(bridge.sample().value(QStringLiteral("state")).toString() == QStringLiteral("registered"));
    CHECK(bridge.lastError().isEmpty());
}

TEST_CASE("A model's refusal of a form body comes back as the model's own message", "[lims][gui][qml-bridge][forms]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    lims::gui::SampleBridge bridge{rig->bridge(0), rig->executor()};

    REQUIRE(registerSampleViaBridge(bridge));

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

    // The sample is `registered`; rework is a `ToBeVerified → InProgress`
    // edge, so the model refuses it. The point is not that the model refuses
    // -- test_sample_lifecycle.cpp owns that -- but that a refusal arriving on
    // the *schema* path carries the model's own words out to the surface. The
    // three-argument signal is the only carrier it has (`failed`/`lastError`
    // are the typed invokables' channel), which is why
    // test_lims_qml_surface.cpp separately proves gui/qml handles it.
    bridge.submitIfValid(QStringLiteral("ReturnForRework"), QStringLiteral(R"({"reason":"balance drifted"})"));
    REQUIRE(pumpUntil([&] { return replies == 1; }));
    CHECK(actionType == QStringLiteral("ReturnForRework"));
    CHECK_FALSE(ok);
    INFO("refusal: " << payload.toStdString());
    // Not a generic "failed": the message names the transition, which is the
    // only thing that tells an operator what to do next.
    CHECK(payload.contains(QStringLiteral("registered")));
    CHECK_FALSE(payload.isEmpty());
}
