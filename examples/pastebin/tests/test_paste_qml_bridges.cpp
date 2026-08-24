// SPDX-License-Identifier: Apache-2.0
//
// The QML-adapter layer's own suite: `PasteBridge` and `FormsBridge`
// (`gui_lib/paste_qml_bridges.hpp`), the two classes that stand between the
// Task 10 GUI classes and the QML shell.
//
// Why this file exists as a *separate* suite from test_paste_presenter.cpp:
// those adapters are the only place in the rung where a `PasteView` becomes a
// `QVariantMap` and a signal acquires the exact name and signature
// `gui/qml/Main.qml` and `gui/qml/PasteView.qml` bind against. QML binds by
// *string*, so a renamed key or a changed signal signature is not a compile
// error anywhere — it is a silently empty label at run time, and the offscreen
// engine-load smoke test (test_gui_qml_smoke.cpp) deliberately loads Main.qml
// with both controllers null, so it cannot catch it either. Every assertion
// below that names a string key or a signal signature is therefore a
// cross-check against a real binding site in those two QML files, cited
// inline.
//
// Both classes are Qt-Core-only (`QVariantMap` is Qt Core; the engine-facing
// side is `setInitialProperties` in each shell), so they instantiate under the
// testkit's owned application object exactly like `PastePresenter` does — no
// QML engine, no window. Domain rules (burn/expiry/visibility/pagination) are
// the model's and are covered in test_paste_model.cpp; routing and busy/idle
// are the presenter's and are covered in test_paste_presenter.cpp. This file
// only proves the translation.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "clock.hpp"
#include "paste_qml_bridges.hpp"
#include "paste_schemas.hpp"
#include "pastebin/models/paste_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"
#include "testkit/qml_surface.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <chrono>
#include <string>

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;
using morph::ladder::testkit::QmlSurfaceAudit;

/// @brief A `CreatePaste` body in the shape `DynamicForm.previewLine` hands
///        `FormsBridge::submitIfValid` — a fully-assembled JSON object, with
///        the optional members (`expiresAt`, `burnAfterReads`, `visibility`,
///        `editability`, per `CreatePaste::optionalFields`) left out exactly
///        as the form leaves them out when the user engages neither.
[[nodiscard]] QString createBody(const QString& content, const QString& syntax = QStringLiteral("text")) {
    return QStringLiteral(R"({"content":"%1","syntax":"%2"})").arg(content, syntax);
}

/// @brief Creates one paste through `FormsBridge` and returns its id, so the
///        `PasteBridge` cases below have a real row to act on without reaching
///        past the adapters into the model.
///
/// This is the composition the shell actually performs: `Main.qml` creates
/// through `formsController.submitIfValid` and reads the outcome in
/// `onReplyReceived`, never through `pasteController` — `PasteBridge` relays no
/// `created` signal at all (see paste_qml_bridges.cpp's comment on why that is
/// deliberate). The id comes out of the reply payload, which is a
/// `CreatePasteResult` (`{"id": ...}`) — not out of a follow-up listing, whose
/// order is descending by id and so identifies "the paste just created" only
/// by accident when exactly one exists.
/// @param forms   The bridge to submit through.
/// @param content Paste body.
/// @param syntax  Syntax label.
/// @return The new paste's id.
[[nodiscard]] QString createPasteVia(pastebin::gui::FormsBridge& forms, const QString& content,
                                     const QString& syntax = QStringLiteral("text")) {
    bool replied = false;
    bool ok = false;
    QString payload;
    QObject::connect(&forms, &pastebin::gui::FormsBridge::replyReceived,
                     [&](const QString&, bool succeeded, const QString& body) {
                         ok = succeeded;
                         payload = body;
                         replied = true;
                     });
    forms.submitIfValid(QStringLiteral("CreatePaste"), createBody(content, syntax));
    REQUIRE(pumpUntil([&] { return replied; }));
    REQUIRE(ok);
    QObject::disconnect(&forms, &pastebin::gui::FormsBridge::replyReceived, nullptr, nullptr);

    const QJsonDocument reply = QJsonDocument::fromJson(payload.toUtf8());
    REQUIRE(reply.isObject());
    const QString id = reply.object().value(QStringLiteral("id")).toString();
    REQUIRE_FALSE(id.isEmpty());
    return id;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════
// The QML-visible surface: names and signatures QML binds by string
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("Both pastebin bridges expose exactly the surface gui/qml binds, and nothing more",
          "[pastebin][gui][qml-bridges]") {
    // Was two cases of hand-written `indexOfMethod`/`indexOfSignal` REQUIREs,
    // each carrying a `Main.qml:169`-style citation in a comment. Those
    // proved only that every name a human had transcribed was still present:
    // the QML was never read, so a `Connections` handler bound to a signal
    // that no longer exists — or one that never existed — passed. (The rung's
    // engine-load smoke test cannot see it either; it loads Main.qml with
    // both controllers null, so no handler name is ever resolved.) There was
    // also no count assertion here at all, so a bridge member with no binding
    // site was not checked in the other direction either.
    //
    // `QmlSurfaceAudit` (testkit/qml_surface.hpp) reads gui/qml/ as the
    // expectation and covers both directions. See that header for its limits.
    DbFixture fixture;
    BackendRig rig{Mode::Local, 1};
    pastebin::gui::FormsBridge forms{rig.bridge(0), rig.executor()};
    pastebin::gui::PasteBridge pastes{rig.bridge(0), rig.executor()};

    // The two `setInitialProperties` keys gui/main.cpp supplies.
    QmlSurfaceAudit audit{QStringLiteral(MORPH_LADDER_SOURCE_ROOT "/examples/pastebin/gui/qml")};
    audit.bind(QStringLiteral("formsController"), forms);
    audit.bind(QStringLiteral("pasteController"), pastes);

    const QStringList findings = audit.run();
    INFO(findings.join(QStringLiteral("\n")).toStdString());
    CHECK(findings.isEmpty());
    CHECK(audit.scannedFiles() == QStringList{QStringLiteral("Main.qml"), QStringLiteral("PasteView.qml")});

    // What the audit cannot see, because QML's text does not record it:
    // `schemasJson` is CONSTANT, which is why Main.qml parses it exactly once.
    const QMetaObject* meta = forms.metaObject();
    const int schemasJson = meta->indexOfProperty("schemasJson");
    REQUIRE(schemasJson >= 0);
    CHECK(meta->property(schemasJson).isConstant());

    // The property's value is the shared schema document, verbatim — the same
    // one both shells build (paste_schemas.hpp exists so they cannot diverge),
    // and `JSON.parse`-able, since Main.qml does exactly that to it.
    CHECK(forms.schemasJson().toStdString() == pastebin::gui::pasteSchemasJson());
    CHECK(forms.schemasJson().contains(QStringLiteral("\"CreatePaste\"")));
}

// ═════════════════════════════════════════════════════════════════════════
// FormsBridge: both arms of its one reply signal
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("FormsBridge::submitIfValid relays a successful create as replyReceived(type, true, resultJson), "
          "all three backend modes",
          "[pastebin][gui][qml-bridges]") {
    auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    DbFixture fixture;
    BackendRig rig{mode, 1};
    pastebin::gui::FormsBridge forms{rig.bridge(0), rig.executor()};

    QString actionType;
    bool ok = false;
    QString payload;
    bool replied = false;
    QObject::connect(&forms, &pastebin::gui::FormsBridge::replyReceived,
                     [&](const QString& type, bool succeeded, const QString& body) {
                         actionType = type;
                         ok = succeeded;
                         payload = body;
                         replied = true;
                     });

    forms.submitIfValid(QStringLiteral("CreatePaste"), createBody(QStringLiteral("through the form")));
    REQUIRE(pumpUntil([&] { return replied; }));

    // Main.qml:118 renders `actionType + " ok: " + payload`, so the echoed type
    // must be the one submitted, not a normalised or empty string.
    CHECK(actionType == QStringLiteral("CreatePaste"));
    CHECK(ok);
    // `CreatePasteResult` is `{id}`; the shell displays the JSON verbatim.
    CHECK(payload.contains(QStringLiteral("\"id\"")));
}

TEST_CASE("FormsBridge::submitIfValid relays a rejected create as replyReceived(type, false, message)",
          "[pastebin][gui][qml-bridges]") {
    DbFixture fixture;
    BackendRig rig{Mode::Local, 1};
    pastebin::gui::FormsBridge forms{rig.bridge(0), rig.executor()};

    QString actionType;
    bool ok = true;
    QString payload;
    bool replied = false;
    QObject::connect(&forms, &pastebin::gui::FormsBridge::replyReceived,
                     [&](const QString& type, bool succeeded, const QString& body) {
                         actionType = type;
                         ok = succeeded;
                         payload = body;
                         replied = true;
                     });

    // Empty content fails `CreatePaste::validate()` — the model's own rule,
    // reached through the generic executeJson path the form uses.
    forms.submitIfValid(QStringLiteral("CreatePaste"), createBody(QString{}));
    REQUIRE(pumpUntil([&] { return replied; }));

    CHECK(actionType == QStringLiteral("CreatePaste"));
    CHECK_FALSE(ok);
    // Main.qml:116 shows `payload` as the error text, so it must be the
    // exception's own `what()`, not an empty string or a generic placeholder.
    CHECK_FALSE(payload.isEmpty());
    CHECK(payload.contains(QStringLiteral("CreatePaste")));

    // Nothing was stored by the rejected submit.
    pastebin::gui::PasteBridge pastes{rig.bridge(0), rig.executor()};
    QVariantList rows;
    bool listed = false;
    QObject::connect(&pastes, &pastebin::gui::PasteBridge::listed, [&](const QVariantList& page) {
        rows = page;
        listed = true;
    });
    pastes.refresh();
    REQUIRE(pumpUntil([&] { return listed; }));
    CHECK(rows.isEmpty());
}

// ═════════════════════════════════════════════════════════════════════════
// PasteBridge: the property-bag shapes
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("PasteBridge::open emits a paste bag carrying every key PasteView.qml reads, "
          "all three backend modes",
          "[pastebin][gui][qml-bridges]") {
    auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    DbFixture fixture;
    BackendRig rig{mode, 1};
    pastebin::gui::FormsBridge forms{rig.bridge(0), rig.executor()};
    pastebin::gui::PasteBridge pastes{rig.bridge(0), rig.executor()};

    const QString id = createPasteVia(forms, QStringLiteral("bag contents"), QStringLiteral("cpp"));

    QVariantMap bag;
    bool loaded = false;
    QObject::connect(&pastes, &pastebin::gui::PasteBridge::loaded, [&](const QVariantMap& paste) {
        bag = paste;
        loaded = true;
    });
    pastes.open(id);
    REQUIRE(pumpUntil([&] { return loaded; }));

    // Every key below is read by name in QML. `id`/`readCount` from
    // Main.qml:88; `content` from PasteView.qml:72; `syntax`, `visibility`,
    // `editability`, `createdAt`, `expiresAt`, `readCount`, `burnAfterReads`
    // from PasteView.qml:29-35; `id` again from PasteView.qml:46, :79.
    for (const char* key : {"id", "content", "syntax", "createdAt", "expiresAt", "burnAfterReads", "readCount",
                            "visibility", "editability"}) {
        INFO("missing key: " << key);
        REQUIRE(bag.contains(QString::fromLatin1(key)));
    }
    // Nothing extra: the bag is exactly these nine, so a key added here without
    // a QML binding (or removed from under one) shows up as a failure rather
    // than as dead weight.
    CHECK(bag.size() == 9);

    CHECK(bag.value(QStringLiteral("id")).toString() == id);
    CHECK(bag.value(QStringLiteral("content")).toString() == QStringLiteral("bag contents"));
    CHECK(bag.value(QStringLiteral("syntax")).toString() == QStringLiteral("cpp"));
    // Every value is already a display *string* — PasteView.qml concatenates
    // them straight into a Label with no formatting of its own (rule 2's
    // "pure glue" allowance depends on this being true here).
    for (auto it = bag.cbegin(); it != bag.cend(); ++it) {
        INFO("non-string value for key: " << it.key().toStdString());
        CHECK(it.value().typeId() == QMetaType::QString);
    }

    // The two enums render as the words PasteView.qml displays verbatim.
    CHECK(bag.value(QStringLiteral("visibility")).toString() == QStringLiteral("Public"));
    CHECK(bag.value(QStringLiteral("editability")).toString() == QStringLiteral("Immutable"));

    // Two sentinel conventions PasteView.qml compares against *literally*
    // (PasteView.qml:33 and :35) — if either renderer ever changed, the pane
    // would silently start showing the raw sentinel instead of "never"/"no
    // limit". This create engaged neither `expiresAt` nor `burnAfterReads`.
    CHECK(bag.value(QStringLiteral("expiresAt")).toString().isEmpty());
    CHECK(bag.value(QStringLiteral("burnAfterReads")).toString() == QStringLiteral("N/A"));

    // A read is a mutation at this rung: the count is real state, rendered as
    // text. Main.qml:88 shows it as "read N time(s)".
    CHECK(bag.value(QStringLiteral("readCount")).toString().startsWith(QStringLiteral("1")));
    CHECK_FALSE(bag.value(QStringLiteral("createdAt")).toString().isEmpty());
}

TEST_CASE("PasteBridge::refresh emits list rows in the narrower summary shape, and only public pastes",
          "[pastebin][gui][qml-bridges]") {
    DbFixture fixture;
    BackendRig rig{Mode::Local, 1};
    pastebin::gui::FormsBridge forms{rig.bridge(0), rig.executor()};
    pastebin::gui::PasteBridge pastes{rig.bridge(0), rig.executor()};

    (void) createPasteVia(forms, QStringLiteral("first"), QStringLiteral("text"));
    (void) createPasteVia(forms, QStringLiteral("second"), QStringLiteral("md"));

    // A private paste, submitted through the same form path with the optional
    // `visibility` member engaged — it must not appear in the listing.
    {
        bool replied = false;
        QObject::connect(&forms, &pastebin::gui::FormsBridge::replyReceived,
                         [&](const QString&, bool ok, const QString&) {
                             CHECK(ok);
                             replied = true;
                         });
        forms.submitIfValid(QStringLiteral("CreatePaste"),
                            QStringLiteral(R"({"content":"hidden","syntax":"text","visibility":"Private"})"));
        REQUIRE(pumpUntil([&] { return replied; }));
        QObject::disconnect(&forms, &pastebin::gui::FormsBridge::replyReceived, nullptr, nullptr);
    }

    QVariantList rows;
    bool listed = false;
    QObject::connect(&pastes, &pastebin::gui::PasteBridge::listed, [&](const QVariantList& page) {
        rows = page;
        listed = true;
    });
    pastes.refresh();
    REQUIRE(pumpUntil([&] { return listed; }));

    REQUIRE(rows.size() == 2);
    for (const QVariant& row : rows) {
        const QVariantMap bag = row.toMap();
        // Main.qml:197 reads exactly these four off `modelData`.
        for (const char* key : {"id", "syntax", "createdAt", "visibility"}) {
            INFO("missing key: " << key);
            REQUIRE(bag.contains(QString::fromLatin1(key)));
        }
        // Narrower than the `loaded` bag *on purpose*: a listing must not leak
        // paste content (`pastebin/dto/paste_dto.hpp`'s `PasteSummary`). This
        // assertion is the one that would catch a well-meaning widening of the
        // summary bag into a full `PasteView` map.
        CHECK(bag.size() == 4);
        CHECK_FALSE(bag.contains(QStringLiteral("content")));
        CHECK(bag.value(QStringLiteral("visibility")).toString() == QStringLiteral("Public"));
        CHECK_FALSE(bag.value(QStringLiteral("id")).toString().isEmpty());
    }
}

TEST_CASE("PasteBridge renders the engaged arm of every formatted field, and the second arm of both enums",
          "[pastebin][gui][qml-bridges]") {
    // The `loaded`-bag case above exercises each renderer's *empty/default*
    // arm (`isoOrEmpty` with no instant -> "", `readsText` with no budget ->
    // "N/A", Public, Immutable). This one exercises the other arm of all four,
    // which is where a formatting regression would actually be visible in the
    // pane: an engaged expiry, an engaged burn budget, Private and Editable.
    //
    // The row is seeded through `PasteModel` directly rather than through
    // `FormsBridge`, deliberately: engaging `burnAfterReads` over the wire
    // means hand-writing a `Rational`'s `{num,den,dp}` wire object, which
    // pins this file to a codec detail it is not about. Seeding in C++ is the
    // convention the sibling model suite already uses, and the subject under
    // test — the adapter's rendering — is unaffected by how the row got there.
    // `Mode::Local`, so the bridge and the seeding model share one process and
    // one database.
    DbFixture fixture;
    BackendRig rig{Mode::Local, 1};
    pastebin::gui::PasteBridge pastes{rig.bridge(0), rig.executor()};

    pastebin::PasteId seededId;
    {
        pastebin::PasteModel seed;
        pastebin::CreatePaste create;
        create.content = "fully engaged";
        create.syntax = "cpp";
        create.expiresAt = ::morph::time::Timestamp{*morph::ladder::now() + std::chrono::hours{24}};
        create.burnAfterReads = pastebin::Reads{::morph::math::Rational{9, pastebin::Reads::declaredPrecision()}};
        create.visibility = pastebin::Visibility::Private;
        create.editability = pastebin::Editability::Editable;
        seededId = seed.execute(create).id;
    }
    REQUIRE(seededId.hasValue());

    QVariantMap bag;
    bool loaded = false;
    QObject::connect(&pastes, &pastebin::gui::PasteBridge::loaded, [&](const QVariantMap& paste) {
        bag = paste;
        loaded = true;
    });
    pastes.open(QString::fromStdString(*seededId));
    REQUIRE(pumpUntil([&] { return loaded; }));

    // Both enum ternaries' second branch (paste_qml_bridges.cpp's
    // `toVariantMap`), rendered as the words PasteView.qml:30-31 display.
    CHECK(bag.value(QStringLiteral("visibility")).toString() == QStringLiteral("Private"));
    CHECK(bag.value(QStringLiteral("editability")).toString() == QStringLiteral("Editable"));

    // `isoOrEmpty`'s engaged arm. PasteView.qml:33 shows this verbatim unless
    // it is exactly "", so it must be a real ISO-8601 instant.
    const QString expires = bag.value(QStringLiteral("expiresAt")).toString();
    CHECK(expires.contains(QLatin1Char('T')));
    CHECK(expires.endsWith(QLatin1Char('Z')));

    // `readsText`'s engaged arm. PasteView.qml:35 shows this verbatim unless
    // it is exactly "N/A", so an engaged budget must render as something else.
    const QString burn = bag.value(QStringLiteral("burnAfterReads")).toString();
    CHECK(burn != QStringLiteral("N/A"));
    CHECK(burn.startsWith(QStringLiteral("9")));

    // The paste is private, so it is absent from the public listing — the
    // `PasteSummary` visibility rule, seen from the adapter's side.
    QVariantList rows;
    bool listed = false;
    QObject::connect(&pastes, &pastebin::gui::PasteBridge::listed, [&](const QVariantList& page) {
        rows = page;
        listed = true;
    });
    pastes.refresh();
    REQUIRE(pumpUntil([&] { return listed; }));
    CHECK(rows.isEmpty());
}


TEST_CASE("PasteBridge::remove emits removed(), and a follow-up open emits failed() with the model's message",
          "[pastebin][gui][qml-bridges]") {
    auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    DbFixture fixture;
    BackendRig rig{mode, 1};
    pastebin::gui::FormsBridge forms{rig.bridge(0), rig.executor()};
    pastebin::gui::PasteBridge pastes{rig.bridge(0), rig.executor()};

    const QString id = createPasteVia(forms, QStringLiteral("doomed"));

    bool removed = false;
    QObject::connect(&pastes, &pastebin::gui::PasteBridge::removed, [&] { removed = true; });
    pastes.remove(id);
    REQUIRE(pumpUntil([&] { return removed; }));

    QString message;
    bool failed = false;
    QObject::connect(&pastes, &pastebin::gui::PasteBridge::failed, [&](const QString& text) {
        message = text;
        failed = true;
    });
    pastes.open(id);
    REQUIRE(pumpUntil([&] { return failed; }));
    // Main.qml:103 shows this string as the error banner, so it must be the
    // model's own `what()`.
    CHECK_FALSE(message.isEmpty());
    CHECK(message.contains(QStringLiteral("GetPaste")));
}

TEST_CASE("PasteBridge::open against an unknown id emits failed(), not loaded()",
          "[pastebin][gui][qml-bridges]") {
    DbFixture fixture;
    BackendRig rig{Mode::Local, 1};
    pastebin::gui::PasteBridge pastes{rig.bridge(0), rig.executor()};

    bool loaded = false;
    QObject::connect(&pastes, &pastebin::gui::PasteBridge::loaded, [&](const QVariantMap&) { loaded = true; });
    QString message;
    bool failed = false;
    QObject::connect(&pastes, &pastebin::gui::PasteBridge::failed, [&](const QString& text) {
        message = text;
        failed = true;
    });

    pastes.open(QStringLiteral("no-such-paste"));
    REQUIRE(pumpUntil([&] { return failed; }));
    CHECK_FALSE(loaded);
    CHECK_FALSE(message.isEmpty());
}
