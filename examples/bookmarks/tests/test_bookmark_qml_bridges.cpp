// SPDX-License-Identifier: Apache-2.0
//
// The QML-adapter layer's own suite: `FormsBridge`, `BookmarkBridge`,
// `TagBridge` and `SharedFeedBridge` (`gui_lib/bookmark_qml_bridges.hpp`) plus
// the action-type routing in `BookmarkFormsController::dispatch`
// (`gui_lib/bookmark_forms_controller.cpp`) — everything that stands between
// the Task 17 presenters and the QML shell.
//
// Why this file exists as a *separate* suite from test_bookmark_presenter.cpp:
// those adapters are the only place in the rung where a `BookmarkView` becomes
// a `QVariantMap`, an action type becomes a routing-table string, and a signal
// acquires the exact name and signature `gui/qml/Main.qml`,
// `gui/qml/LoginView.qml` and `gui/qml/BookmarkListView.qml` bind against. QML
// binds by *string*, so a renamed key, a mistyped action id or a changed
// signal signature is not a compile error anywhere — it is a silently empty
// label at run time, and the offscreen engine-load smoke test
// (test_gui_qml_smoke.cpp) deliberately loads the QML with every controller
// null, so it cannot catch it either. Every assertion below that names a
// string key or an action id is therefore a cross-check against a real
// binding site in those three QML files, cited inline. Mirrors rung 1's own
// `examples/pastebin/tests/test_paste_qml_bridges.cpp`, which established
// this suite's shape.
//
// The metaobject half of that cross-check is no longer hand-written. The
// first case below points `morph::ladder::testkit::QmlSurfaceAudit`
// (`examples/common/testkit/qml_surface.hpp`) at `gui/qml/` and lets the QML
// itself be the expectation, in both directions at once — a name QML binds
// that no bridge has is a finding, and so is a bridge member no QML binds.
// The citations are computed, not maintained. What that audit cannot see
// (property storage kind, a signal parameter's C++ type) stays hand-written,
// in the case immediately after it.
//
// All four adapters are Qt-Core-only (`QVariantMap` is Qt Core; the
// engine-facing side is `setInitialProperties` in the shell), so they
// instantiate under the testkit's owned application object exactly like the
// presenters do — no QML engine, no window. Domain rules (ownership, tag
// diffing, archive filtering, bulk atomicity, the shared feed's query) are the
// models' and are covered in test_bookmark_model.cpp / test_tag_model.cpp /
// test_shared_feed_model.cpp; routing and busy/idle are the presenters' and are
// covered in their own suites. This file only proves the translation.
//
// ── Arms that are structurally unreachable, and are therefore not asserted ──
// Three of the private renderers in bookmark_qml_bridges.cpp have an arm no
// test in this file can reach, because nothing in the rung can *produce* the
// input:
//   * `readStateText(ReadState::Read)` — no action anywhere in the rung clears
//     `BookmarkRecord::isUnread` (it is `true` at construction and is only ever
//     read, in `bookmark_model.cpp` and `shared_feed_model.cpp`), so every row
//     any client can ever see is `Unread`. There is no "mark as read" action.
//   * `isoOrEmpty`'s empty arm — every `Timestamp` in a bookmark bag comes from
//     `bookmark_model.cpp`'s `fromEpochMs`, which always returns an engaged
//     `Timestamp`, and both `createdAtMs`/`updatedAtMs` are stamped on insert.
//   * `countText`'s `"N/A"` arm — every `Count` that reaches a bag is built by
//     `Count::fromDouble`, which is always engaged.
// They are defensive, not dead-by-mistake (each mirrors a shape rung 1 does
// reach), and reaching them from here would mean exposing the renderers
// themselves purely for a test. Stated rather than silently skipped; if a later
// rung adds the missing action, the arms become reachable and belong here.

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "bookmark_qml_bridges.hpp"
#include "bookmark_schemas.hpp"
#include "bookmarks/auth/bookmarks_authorizer.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"
#include "testkit/qml_surface.hpp"

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;
using morph::ladder::testkit::QmlSurfaceAudit;

constexpr std::string_view kSecret = "qml-bridges-test-secret";

/// @brief Builds a rig whose one bridge already carries a valid session for
///        @p principal — the state a client is in *after* login.
///
/// Every action in this rung needs a populated `session::current()->principal`
/// for the model's own scoping to succeed, even in `Mode::Local` (which runs no
/// authorizer at all) — the same recipe, and the same reason, as
/// test_bookmark_presenter.cpp's own helper.
/// @param principal The identity to install.
/// @return The rig, owning the bridge and executor the adapters take.
[[nodiscard]] std::unique_ptr<BackendRig> makeAuthedRig(std::string principal) {
    auto rig = std::make_unique<BackendRig>(Mode::Local, 1);
    const morph::session::TokenIssuer issuer{std::string{kSecret}, morph::session::hmacSha256};
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    // Every field named, not just the two that matter: `-Weverything` includes
    // `-Wmissing-designated-field-initializers`, which fires on a partial
    // designated-initializer list (see test_app.cpp's own note on this).
    ctx.token = issuer.issue(morph::session::SessionToken{
        .principal = ctx.principal,
        .issuedAtMs = 0,
        .expiresAtMs = 4102444800000,  // year 2100, far future
        .roles = {},
    });
    rig->bridge(0).setDefaultSession(ctx);
    return rig;
}

/// @brief Installs a process-global `TokenIssuer` for a scope and clears it
///        again on the way out — `AuthModel::execute(const Login&)` throws
///        without one. Same shape, and the same
///        failing-REQUIRE-must-not-leak-it rationale, as
///        test_bookmarks_authorizer.cpp's own.
class ScopedTokenIssuer {
public:
    explicit ScopedTokenIssuer(std::shared_ptr<morph::session::TokenIssuer> issuer) {
        bookmarks::auth::setTokenIssuer(std::move(issuer));
    }
    ~ScopedTokenIssuer() { bookmarks::auth::setTokenIssuer(nullptr); }
    ScopedTokenIssuer(const ScopedTokenIssuer&) = delete;
    ScopedTokenIssuer& operator=(const ScopedTokenIssuer&) = delete;
    ScopedTokenIssuer(ScopedTokenIssuer&&) = delete;
    ScopedTokenIssuer& operator=(ScopedTokenIssuer&&) = delete;
};

/// @brief One `submitIfValid` round trip, exactly as a `DynamicForm`'s submit
///        button performs it.
/// @param forms      The bridge to submit through.
/// @param actionType The action id QML names as a string literal.
/// @param bodyJson   Fully-assembled JSON body, as `DynamicForm` builds it.
/// @return `{ok, payload}` from the single `replyReceived` the submit produces.
[[nodiscard]] std::pair<bool, QString> submit(bookmarks::gui::FormsBridge& forms, const QString& actionType,
                                              const QString& bodyJson) {
    bool replied = false;
    bool ok = false;
    QString payload;
    QString echoedType;
    const auto connection = QObject::connect(&forms, &bookmarks::gui::FormsBridge::replyReceived,
                                             [&](const QString& type, bool succeeded, const QString& body) {
                                                 echoedType = type;
                                                 ok = succeeded;
                                                 payload = body;
                                                 replied = true;
                                             });
    forms.submitIfValid(actionType, bodyJson);
    const bool settled = pumpUntil([&] { return replied; });
    QObject::disconnect(connection);
    REQUIRE(settled);
    // BookmarkListView.qml:190 dispatches on the echoed type (it returns early
    // for "Login" and resets a different form for each of the others), so a
    // normalised or empty echo would misroute every outcome on that screen.
    REQUIRE(echoedType == actionType);
    return {ok, payload};
}

/// @brief Creates one bookmark through the schema-driven form path and returns
///        its id in the `qlonglong` shape list rows and invokables use.
///
/// This is the composition the shell actually performs: `BookmarkListView.qml`
/// creates through `formsController.submitIfValid` (:249) and reads the outcome
/// in `onReplyReceived` (:190), never through `bookmarkController` —
/// `BookmarkBridge` relays no `created` signal at all (see
/// bookmark_qml_bridges.hpp's comment on why that is deliberate). The id comes
/// out of the reply payload, a `CreateBookmarkResult` (`{"id": …}`).
/// @param forms    The bridge to submit through.
/// @param bodyJson A `CreateBookmark` body.
/// @return The new bookmark's id.
[[nodiscard]] qlonglong createVia(bookmarks::gui::FormsBridge& forms, const QString& bodyJson) {
    const auto [ok, payload] = submit(forms, QStringLiteral("CreateBookmark"), bodyJson);
    REQUIRE(ok);
    const QJsonDocument reply = QJsonDocument::fromJson(payload.toUtf8());
    REQUIRE(reply.isObject());
    const auto id = reply.object().value(QStringLiteral("id")).toVariant().toLongLong();
    REQUIRE(id > 0);
    return id;
}

/// @brief `BookmarkBridge::open`'s one bag.
/// @param bridge The bridge to read through.
/// @param id     The bookmark to open.
/// @return The property bag `loaded` carried.
[[nodiscard]] QVariantMap openBag(bookmarks::gui::BookmarkBridge& bridge, qlonglong id) {
    QVariantMap bag;
    bool loaded = false;
    const auto connection =
        QObject::connect(&bridge, &bookmarks::gui::BookmarkBridge::loaded, [&](const QVariantMap& bookmark) {
            bag = bookmark;
            loaded = true;
        });
    bridge.open(id);
    const bool settled = pumpUntil([&] { return loaded; });
    QObject::disconnect(connection);
    REQUIRE(settled);
    return bag;
}

/// @brief The rows `BookmarkBridge::refresh` (or `refreshIncludingArchived`)
///        hands the list delegate.
/// @tparam Refresh Callable invoked to start the listing.
/// @param bridge  The bridge to list through.
/// @param refresh Which listing to start.
/// @return The page's rows.
template <typename Refresh>
[[nodiscard]] QVariantList listRows(bookmarks::gui::BookmarkBridge& bridge, Refresh refresh) {
    QVariantList rows;
    bool listed = false;
    const auto connection =
        QObject::connect(&bridge, &bookmarks::gui::BookmarkBridge::listed, [&](const QVariantList& page) {
            rows = page;
            listed = true;
        });
    refresh();
    const bool settled = pumpUntil([&] { return listed; });
    QObject::disconnect(connection);
    REQUIRE(settled);
    return rows;
}

/// @brief `TagBridge::refresh`'s rows.
/// @param tags The bridge to list through.
/// @return The tag rows.
[[nodiscard]] QVariantList tagRows(bookmarks::gui::TagBridge& tags) {
    QVariantList rows;
    bool listed = false;
    const auto connection = QObject::connect(&tags, &bookmarks::gui::TagBridge::listed, [&](const QVariantList& page) {
        rows = page;
        listed = true;
    });
    tags.refresh();
    const bool settled = pumpUntil([&] { return listed; });
    QObject::disconnect(connection);
    REQUIRE(settled);
    return rows;
}

/// @brief The id of the tag named @p name in @p rows.
/// @param rows Tag rows from `TagBridge::listed`.
/// @param name The tag name to find.
/// @return Its id, or `-1` if absent.
[[nodiscard]] qlonglong tagIdNamed(const QVariantList& rows, const QString& name) {
    for (const QVariant& row : rows) {
        const QVariantMap bag = row.toMap();
        if (bag.value(QStringLiteral("name")).toString() == name) {
            return bag.value(QStringLiteral("id")).toLongLong();
        }
    }
    return -1;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════
// The QML-visible surface: names and signatures QML binds by string
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("Every bookmarks bridge exposes exactly the surface gui/qml binds, and nothing more",
          "[bookmarks][gui][qml-bridges]") {
    // What replaced ~110 lines of hand-written `indexOfMethod`/`indexOfSignal`
    // REQUIREs here, and why.
    //
    // The old shape enumerated each bridge's metaobject against a transcription
    // of the QML — every expected name spelled out in C++, each one carrying a
    // `BookmarkListView.qml:301`-style citation in a comment, plus an
    // `ownMethodCount(meta) == 15` to catch anything added. It worked in one
    // direction only: it proved every name a human had *transcribed* was still
    // there. A QML file that binds a name the bridge never had — `onListd` for
    // `listed`, `refreshAll()` for `refresh()` — satisfied it by construction,
    // because the transcription was the expectation. The engine-load smoke test
    // (test_gui_qml_smoke.cpp) cannot catch that either, and says so in its own
    // file comment: it loads every root with every controller null, so no
    // `Connections` handler name is ever resolved against a real signal.
    //
    // `QmlSurfaceAudit` (testkit/qml_surface.hpp) reads gui/qml/*.qml as the
    // expectation instead, so the citations are computed rather than
    // maintained, and both directions are covered. See that header for the
    // exact list of what it does and does not catch.
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    bookmarks::gui::FormsBridge forms{rig->bridge(0), rig->executor()};
    bookmarks::gui::BookmarkBridge bookmarkBridge{rig->bridge(0), rig->executor()};
    bookmarks::gui::TagBridge tags{rig->bridge(0), rig->executor()};
    bookmarks::gui::SharedFeedBridge feed{rig->bridge(0), rig->executor()};

    // The alias names are exactly the four `setInitialProperties` keys
    // gui/main.cpp supplies, and are the one per-rung fact the audit cannot
    // derive: it checks that each alias's *surface* agrees with the QML, not
    // that the shell wires that alias to this class.
    QmlSurfaceAudit audit{QStringLiteral(MORPH_LADDER_SOURCE_ROOT "/examples/bookmarks/gui/qml")};
    audit.bind(QStringLiteral("formsController"), forms);
    audit.bind(QStringLiteral("bookmarkController"), bookmarkBridge);
    audit.bind(QStringLiteral("tagController"), tags);
    audit.bind(QStringLiteral("feedController"), feed);

    const QStringList findings = audit.run();
    INFO(findings.join(QStringLiteral("\n")).toStdString());
    CHECK(findings.isEmpty());

    // The audit is only as good as the files it found: three, the same three
    // gui/main.cpp's QML module ships.
    CHECK(audit.scannedFiles() == QStringList{QStringLiteral("BookmarkListView.qml"), QStringLiteral("LoginView.qml"),
                                              QStringLiteral("Main.qml")});
}

TEST_CASE("The bookmarks bridges' surface carries the metaobject facts QML cannot state",
          "[bookmarks][gui][qml-bridges]") {
    // Everything the audit above structurally cannot see, because QML's own
    // text does not record it: property storage kind, and the C++ type a
    // signal parameter actually carries. Both are load-bearing here.
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    bookmarks::gui::FormsBridge forms{rig->bridge(0), rig->executor()};
    bookmarks::gui::BookmarkBridge bookmarkBridge{rig->bridge(0), rig->executor()};

    // `schemasJson` is CONSTANT, which is why Main.qml:35 parses it exactly
    // once into `root.schemas` instead of re-parsing on every change.
    const QMetaObject* formsMeta = forms.metaObject();
    const int schemasJson = formsMeta->indexOfProperty("schemasJson");
    REQUIRE(schemasJson >= 0);
    CHECK(formsMeta->property(schemasJson).isConstant());

    // `bulkEdited` carries an already-rendered *string*, not a number:
    // BookmarkListView.qml concatenates it straight into a status line.
    const QMetaObject* bookmarkMeta = bookmarkBridge.metaObject();
    const int bulkEdited = bookmarkMeta->indexOfSignal("bulkEdited(QString)");
    REQUIRE(bulkEdited >= 0);
    CHECK(bookmarkMeta->method(bulkEdited).parameterMetaType(0).id() == QMetaType::QString);

    // The property's value is the shared schema document, verbatim — the same
    // one every shell builds (bookmark_schemas.hpp exists so they cannot
    // diverge), and `JSON.parse`-able, since Main.qml:35 does exactly that.
    CHECK(forms.schemasJson().toStdString() == bookmarks::gui::bookmarkSchemasJson());
    const QJsonDocument schemas = QJsonDocument::fromJson(forms.schemasJson().toUtf8());
    REQUIRE(schemas.isObject());
    // The six action ids QML passes to `submitIfValid` as string literals must
    // each have a schema to render from, or the form is blank.
    for (const char* actionType :
         {"Login", "CreateBookmark", "EditBookmark", "ImportBookmarks", "RenameTag", "MergeTags"}) {
        INFO("missing schema: " << actionType);
        CHECK(schemas.object().contains(QString::fromLatin1(actionType)));
    }
    CHECK(schemas.object().size() == 6);
}

// ═════════════════════════════════════════════════════════════════════════
// The property-bag shapes: exactly N keys, no leaked field
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("BookmarkBridge::open emits a bookmark bag carrying every key BookmarkListView.qml reads",
          "[bookmarks][gui][qml-bridges]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    bookmarks::gui::FormsBridge forms{rig->bridge(0), rig->executor()};
    bookmarks::gui::BookmarkBridge bookmarkBridge{rig->bridge(0), rig->executor()};

    const qlonglong id = createVia(
        forms,
        QStringLiteral(R"({"url":"https://bag.example","title":"Bag","description":"desc","notes":"private note",)"
                       R"("tags":["work","home"]})"));
    const QVariantMap bag = openBag(bookmarkBridge, id);

    // Every key below is read by name in QML: `title`/`url` from
    // BookmarkListView.qml:345-347, `description`/`notes`/`tags`/`visibility`/
    // `readState`/`archiveState`/`createdAt`/`updatedAt` from the detail
    // Repeater's model (:352-360), `id` from :377, :383, :389.
    for (const char* key : {"id", "url", "title", "description", "notes", "tags", "createdAt", "updatedAt",
                            "readState", "archiveState", "visibility"}) {
        INFO("missing key: " << key);
        REQUIRE(bag.contains(QString::fromLatin1(key)));
    }
    // Nothing extra: the bag is exactly these eleven, so a key added here
    // without a QML binding (or removed from under one) shows up as a failure
    // rather than as dead weight.
    CHECK(bag.size() == 11);

    CHECK(bag.value(QStringLiteral("id")).toLongLong() == id);
    CHECK(bag.value(QStringLiteral("url")).toString() == QStringLiteral("https://bag.example"));
    CHECK(bag.value(QStringLiteral("title")).toString() == QStringLiteral("Bag"));
    CHECK(bag.value(QStringLiteral("description")).toString() == QStringLiteral("desc"));
    CHECK(bag.value(QStringLiteral("notes")).toString() == QStringLiteral("private note"));

    // `id` is a *number*, not a string: `open`/`archive`/`unarchive`/`remove`
    // all take `qlonglong`, and BookmarkListView.qml feeds them straight from
    // this bag (:377) and from a list row (:301).
    CHECK(bag.value(QStringLiteral("id")).typeId() == QMetaType::LongLong);
    // `tags` is a list, because :355 calls `.join(", ")` on it.
    REQUIRE(bag.value(QStringLiteral("tags")).typeId() == QMetaType::QVariantList);
    const QVariantList tags = bag.value(QStringLiteral("tags")).toList();
    CHECK(tags.size() == 2);
    // Every *other* value is already a display string — the detail pane
    // concatenates them into a Label with no formatting of its own (rule 2's
    // "pure glue" allowance depends on this being true here).
    for (auto it = bag.cbegin(); it != bag.cend(); ++it) {
        if (it.key() == QStringLiteral("id") || it.key() == QStringLiteral("tags")) {
            continue;
        }
        INFO("non-string value for key: " << it.key().toStdString());
        CHECK(it.value().typeId() == QMetaType::QString);
    }

    // The three enum renderers, in their default arms, rendered as the words
    // the detail pane displays verbatim.
    CHECK(bag.value(QStringLiteral("visibility")).toString() == QStringLiteral("Private"));
    CHECK(bag.value(QStringLiteral("readState")).toString() == QStringLiteral("Unread"));
    CHECK(bag.value(QStringLiteral("archiveState")).toString() == QStringLiteral("Active"));

    // `isoOrEmpty`'s engaged arm — a real ISO-8601 instant, shown verbatim.
    const QString created = bag.value(QStringLiteral("createdAt")).toString();
    CHECK(created.contains(QLatin1Char('T')));
    CHECK(created.endsWith(QLatin1Char('Z')));
    CHECK_FALSE(bag.value(QStringLiteral("updatedAt")).toString().isEmpty());
}

TEST_CASE("BookmarkBridge::refresh emits rows in the narrower summary shape, with no notes key",
          "[bookmarks][gui][qml-bridges]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    bookmarks::gui::FormsBridge forms{rig->bridge(0), rig->executor()};
    bookmarks::gui::BookmarkBridge bookmarkBridge{rig->bridge(0), rig->executor()};

    static_cast<void>(
        createVia(forms, QStringLiteral(R"({"url":"https://row.example","title":"Row","notes":"must not leak"})")));

    const QVariantList rows = listRows(bookmarkBridge, [&bookmarkBridge] { bookmarkBridge.refresh(); });
    REQUIRE(rows.size() == 1);
    const QVariantMap bag = rows.front().toMap();

    // `id`/`title`/`url`/`visibility`/`archiveState` are read off `modelData`
    // at BookmarkListView.qml:290, :296-298, :301, :307; the remaining four are
    // the summary shape the shared-feed delegate also reads (:516-517).
    for (const char* key :
         {"id", "url", "title", "tags", "createdAt", "updatedAt", "readState", "archiveState", "visibility"}) {
        INFO("missing key: " << key);
        REQUIRE(bag.contains(QString::fromLatin1(key)));
    }
    // Narrower than the `loaded` bag *on purpose*: a listing must not leak
    // `notes` (`bookmarks/dto/bookmark_dto.hpp`'s `BookmarkSummary`). This
    // assertion is the one that would catch a well-meaning widening of the
    // summary bag into a full `BookmarkView` map.
    CHECK(bag.size() == 9);
    CHECK_FALSE(bag.contains(QStringLiteral("notes")));
    CHECK_FALSE(bag.contains(QStringLiteral("description")));

    CHECK(bag.value(QStringLiteral("id")).typeId() == QMetaType::LongLong);
    CHECK(bag.value(QStringLiteral("title")).toString() == QStringLiteral("Row"));
    CHECK(bag.value(QStringLiteral("visibility")).toString() == QStringLiteral("Private"));
    CHECK(bag.value(QStringLiteral("archiveState")).toString() == QStringLiteral("Active"));
}

TEST_CASE("TagBridge::refresh emits {id, name, bookmarkCount} rows and nothing else",
          "[bookmarks][gui][qml-bridges]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    bookmarks::gui::FormsBridge forms{rig->bridge(0), rig->executor()};
    bookmarks::gui::TagBridge tags{rig->bridge(0), rig->executor()};

    static_cast<void>(createVia(forms, QStringLiteral(R"({"url":"https://tagged.example","tags":["work"]})")));

    const QVariantList rows = tagRows(tags);
    REQUIRE(rows.size() == 1);
    const QVariantMap bag = rows.front().toMap();

    // `modelData.id` / `.name` / `.bookmarkCount` — BookmarkListView.qml:455-456.
    for (const char* key : {"id", "name", "bookmarkCount"}) {
        INFO("missing key: " << key);
        REQUIRE(bag.contains(QString::fromLatin1(key)));
    }
    CHECK(bag.size() == 3);
    CHECK(bag.value(QStringLiteral("name")).toString() == QStringLiteral("work"));
    // The id is a number the rename/merge forms are filled in with by hand
    // (":455" prints it after a '#'); the count is already a display string,
    // concatenated straight into the same label.
    CHECK(bag.value(QStringLiteral("id")).typeId() == QMetaType::LongLong);
    CHECK(bag.value(QStringLiteral("id")).toLongLong() > 0);
    REQUIRE(bag.value(QStringLiteral("bookmarkCount")).typeId() == QMetaType::QString);
    const QString count = bag.value(QStringLiteral("bookmarkCount")).toString();
    CHECK(count.startsWith(QStringLiteral("1")));
    CHECK(count != QStringLiteral("N/A"));
}

TEST_CASE("SharedFeedBridge::refresh emits the same summary shape, and only Shared bookmarks",
          "[bookmarks][gui][qml-bridges]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    bookmarks::gui::FormsBridge forms{rig->bridge(0), rig->executor()};
    bookmarks::gui::SharedFeedBridge feed{rig->bridge(0), rig->executor()};

    static_cast<void>(createVia(
        forms, QStringLiteral(R"({"url":"https://shared.example","title":"Shared one","notes":"must not leak",)"
                              R"("visibility":"Shared"})")));
    static_cast<void>(createVia(forms, QStringLiteral(R"({"url":"https://private.example"})")));

    QVariantList rows;
    bool listed = false;
    QObject::connect(&feed, &bookmarks::gui::SharedFeedBridge::listed, [&](const QVariantList& page) {
        rows = page;
        listed = true;
    });
    feed.refresh();
    REQUIRE(pumpUntil([&] { return listed; }));

    REQUIRE(rows.size() == 1);
    const QVariantMap bag = rows.front().toMap();
    // Same nine keys as BookmarkBridge::listed — the shared feed reuses
    // `BookmarkSummary`, so the same non-leak rule applies here too.
    CHECK(bag.size() == 9);
    CHECK_FALSE(bag.contains(QStringLiteral("notes")));
    // `modelData.title` / `.url` / `.createdAt` — BookmarkListView.qml:516-517.
    CHECK(bag.value(QStringLiteral("title")).toString() == QStringLiteral("Shared one"));
    CHECK_FALSE(bag.value(QStringLiteral("createdAt")).toString().isEmpty());
    // `visibilityText`'s *other* arm: the feed only ever carries Shared rows.
    CHECK(bag.value(QStringLiteral("visibility")).toString() == QStringLiteral("Shared"));
}

// ═════════════════════════════════════════════════════════════════════════
// The renderers' second arms, and bulkArchive's bool -> BulkArchiveOp map
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("BookmarkBridge renders the second arm of the visibility and archive-state renderers",
          "[bookmarks][gui][qml-bridges]") {
    // The bag cases above exercise each renderer's *default* arm (Private,
    // Unread, Active). This one exercises the other arm of the two that a
    // client can actually reach, which is where a formatting regression would
    // be visible: BookmarkListView.qml:307 shows
    // `visibility + " · " + archiveState` on every row, verbatim.
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    bookmarks::gui::FormsBridge forms{rig->bridge(0), rig->executor()};
    bookmarks::gui::BookmarkBridge bookmarkBridge{rig->bridge(0), rig->executor()};

    const qlonglong id = createVia(forms, QStringLiteral(R"({"url":"https://arms.example","visibility":"Shared"})"));
    CHECK(openBag(bookmarkBridge, id).value(QStringLiteral("visibility")).toString() == QStringLiteral("Shared"));

    bool archived = false;
    QObject::connect(&bookmarkBridge, &bookmarks::gui::BookmarkBridge::archived, [&] { archived = true; });
    bookmarkBridge.archive(id);
    REQUIRE(pumpUntil([&] { return archived; }));
    CHECK(openBag(bookmarkBridge, id).value(QStringLiteral("archiveState")).toString() == QStringLiteral("Archived"));

    // The archived row is gone from the default listing and back in the
    // archive-inclusive one — the two `refresh` invokables the toggle at
    // BookmarkListView.qml:66-68 switches between.
    CHECK(listRows(bookmarkBridge, [&bookmarkBridge] { bookmarkBridge.refresh(); }).isEmpty());
    CHECK(listRows(bookmarkBridge, [&bookmarkBridge] { bookmarkBridge.refreshIncludingArchived(); }).size() == 1);

    bool unarchived = false;
    QObject::connect(&bookmarkBridge, &bookmarks::gui::BookmarkBridge::unarchived, [&] { unarchived = true; });
    bookmarkBridge.unarchive(id);
    REQUIRE(pumpUntil([&] { return unarchived; }));
    CHECK(openBag(bookmarkBridge, id).value(QStringLiteral("archiveState")).toString() == QStringLiteral("Active"));
}

TEST_CASE("BookmarkBridge::bulkArchive maps true to BulkArchiveOp::Archive and false to Unarchive",
          "[bookmarks][gui][qml-bridges]") {
    // The one place in the client where a QML `bool` becomes a domain enum
    // (`bulkArchive(page.selectedIds, true)` at BookmarkListView.qml:323, and
    // `false` at :329). Inverting the ternary would archive on "Unarchive" and
    // vice versa, with no compile error and no visible difference until a user
    // pressed the wrong-behaving button.
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    bookmarks::gui::FormsBridge forms{rig->bridge(0), rig->executor()};
    bookmarks::gui::BookmarkBridge bookmarkBridge{rig->bridge(0), rig->executor()};

    const qlonglong first = createVia(forms, QStringLiteral(R"({"url":"https://bulk-one.example"})"));
    const qlonglong second = createVia(forms, QStringLiteral(R"({"url":"https://bulk-two.example"})"));
    const QVariantList ids{QVariant{first}, QVariant{second}};

    QString affected;
    int bulkEdits = 0;
    QObject::connect(&bookmarkBridge, &bookmarks::gui::BookmarkBridge::bulkEdited, [&](const QString& count) {
        affected = count;
        ++bulkEdits;
    });

    bookmarkBridge.bulkArchive(ids, true);
    REQUIRE(pumpUntil([&] { return bulkEdits == 1; }));
    // `affected` reaches QML already rendered ("bulk edit affected N
    // bookmark(s)", :148).
    CHECK(affected.startsWith(QStringLiteral("2")));
    CHECK(listRows(bookmarkBridge, [&bookmarkBridge] { bookmarkBridge.refresh(); }).isEmpty());
    for (const QVariant& row :
         listRows(bookmarkBridge, [&bookmarkBridge] { bookmarkBridge.refreshIncludingArchived(); })) {
        CHECK(row.toMap().value(QStringLiteral("archiveState")).toString() == QStringLiteral("Archived"));
    }

    // ...and the other direction, on the same two rows.
    bookmarkBridge.bulkArchive(ids, false);
    REQUIRE(pumpUntil([&] { return bulkEdits == 2; }));
    const QVariantList active = listRows(bookmarkBridge, [&bookmarkBridge] { bookmarkBridge.refresh(); });
    REQUIRE(active.size() == 2);
    for (const QVariant& row : active) {
        CHECK(row.toMap().value(QStringLiteral("archiveState")).toString() == QStringLiteral("Active"));
    }
}

// ═════════════════════════════════════════════════════════════════════════
// BookmarkFormsController::dispatch — the six-entry routing table
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("BookmarkFormsController::dispatch routes every one of the six form actions to the model that serves it",
          "[bookmarks][gui][qml-bridges]") {
    // `dispatch()` maps an action-type *string* to one of three
    // `BridgeHandler`s. A typo, or a new action added to bookmark_schemas.hpp
    // and forgotten here, is not a compile error: the form renders, the button
    // submits, and the reply is an error message. This case submits all six
    // ids exactly as the QML string literals spell them.
    DbFixture fixture;
    const ScopedTokenIssuer issuer{
        std::make_shared<morph::session::TokenIssuer>(std::string{kSecret}, morph::session::hmacSha256)};
    // Deliberately *not* pre-authenticated: the Login route below is what
    // installs the session the other five need, which is the real client's own
    // startup order.
    BackendRig rig{Mode::Local, 1};
    bookmarks::gui::FormsBridge forms{rig.bridge(0), rig.executor()};
    bookmarks::gui::TagBridge tags{rig.bridge(0), rig.executor()};

    // 1/6 — Login -> AuthModel.
    {
        const auto [ok, payload] = submit(forms, QStringLiteral("Login"), QStringLiteral(R"({"username":"alice"})"));
        REQUIRE(ok);
        CHECK(payload.contains(QStringLiteral("\"principal\"")));
    }

    // 2/6 — CreateBookmark -> BookmarkModel. Reaching the model at all proves
    // Login's reply was decoded and installed as the bridge's default session.
    const qlonglong id = createVia(forms, QStringLiteral(R"({"url":"https://route.example","tags":["work","home"]})"));

    // 3/6 — EditBookmark -> BookmarkModel.
    {
        const auto [ok, payload] =
            submit(forms, QStringLiteral("EditBookmark"),
                   QStringLiteral(R"({"id":%1,"url":"https://edited.example","title":"Edited"})").arg(id));
        INFO(payload.toStdString());
        REQUIRE(ok);
    }

    // 4/6 — ImportBookmarks -> BookmarkModel.
    {
        const auto [ok, payload] =
            submit(forms, QStringLiteral("ImportBookmarks"),
                   QStringLiteral(R"({"chunk":"<DT><A HREF=\"https://imported.example\">Imported</A>",)"
                                  R"("opId":"import-op-1"})"));
        INFO(payload.toStdString());
        REQUIRE(ok);
        CHECK(payload.contains(QStringLiteral("\"imported\"")));
    }

    // 5/6 — RenameTag -> TagModel. The ids come from the tag list, exactly as
    // the user reads them off BookmarkListView.qml:455 before typing them in.
    const QVariantList before = tagRows(tags);
    REQUIRE(before.size() == 2);
    const qlonglong workId = tagIdNamed(before, QStringLiteral("work"));
    const qlonglong homeId = tagIdNamed(before, QStringLiteral("home"));
    REQUIRE(workId > 0);
    REQUIRE(homeId > 0);
    {
        const auto [ok, payload] =
            submit(forms, QStringLiteral("RenameTag"), QStringLiteral(R"({"id":%1,"name":"office"})").arg(workId));
        INFO(payload.toStdString());
        REQUIRE(ok);
    }
    CHECK(tagIdNamed(tagRows(tags), QStringLiteral("office")) == workId);

    // 6/6 — MergeTags -> TagModel.
    {
        const auto [ok, payload] = submit(forms, QStringLiteral("MergeTags"),
                                          QStringLiteral(R"({"sourceId":%1,"targetId":%2})").arg(homeId).arg(workId));
        INFO(payload.toStdString());
        REQUIRE(ok);
    }
    const QVariantList after = tagRows(tags);
    CHECK(after.size() == 1);
    CHECK(tagIdNamed(after, QStringLiteral("home")) == -1);
}

TEST_CASE("BookmarkFormsController::dispatch reports an unrouted action type instead of dropping it",
          "[bookmarks][gui][qml-bridges]") {
    // The exact failure mode the routing table risks: a QML string literal
    // that no `if` in `dispatch()` matches. It must surface as a message in
    // the status line (BookmarkListView.qml:193 renders `actionType + ": " +
    // payload` on `!ok`), never as a submit that silently does nothing.
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    bookmarks::gui::FormsBridge forms{rig->bridge(0), rig->executor()};

    // A plausible typo of a real id, and a name from a model this client does
    // not serve forms for at all.
    for (const auto& actionType : {QStringLiteral("CreateBookmarks"), QStringLiteral("ListSharedFeed")}) {
        const auto [ok, payload] = submit(forms, actionType, QStringLiteral(R"({"url":"https://typo.example"})"));
        INFO(actionType.toStdString());
        CHECK_FALSE(ok);
        CHECK(payload.contains(QStringLiteral("no model in this client serves action")));
        CHECK(payload.contains(actionType));
    }

    // A *routed* action whose body the model refuses still comes back on the
    // same `!ok` arm, with the model's own message — the two failures are
    // indistinguishable to QML by design, and both must be non-empty.
    const auto [ok, payload] = submit(forms, QStringLiteral("CreateBookmark"), QStringLiteral(R"({"url":""})"));
    CHECK_FALSE(ok);
    CHECK_FALSE(payload.isEmpty());
    CHECK(payload.contains(QStringLiteral("CreateBookmark")));
}

// ═════════════════════════════════════════════════════════════════════════
// Login: the session-installing seam, and both arms of the reply decode
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("FormsBridge installs the returned token and announces loggedIn before replyReceived",
          "[bookmarks][gui][qml-bridges]") {
    // `onLoginSucceeded` is the whole of this client's authentication
    // handling. Main.qml:47 pushes BookmarkListView on `loggedIn`, and that
    // screen dispatches immediately (:66-76), so the token must already be
    // installed when the signal fires — the ordering asserted below is load
    // bearing, not cosmetic.
    DbFixture fixture;
    const ScopedTokenIssuer issuer{
        std::make_shared<morph::session::TokenIssuer>(std::string{kSecret}, morph::session::hmacSha256)};
    BackendRig rig{Mode::Local, 1};
    bookmarks::gui::FormsBridge forms{rig.bridge(0), rig.executor()};
    bookmarks::gui::BookmarkBridge bookmarkBridge{rig.bridge(0), rig.executor()};

    // Before login the bridge carries no session at all, so a domain action is
    // refused — the state a just-launched client is in.
    {
        QString message;
        bool failed = false;
        const auto connection =
            QObject::connect(&bookmarkBridge, &bookmarks::gui::BookmarkBridge::failed, [&](const QString& text) {
                message = text;
                failed = true;
            });
        bookmarkBridge.refresh();
        REQUIRE(pumpUntil([&] { return failed; }));
        QObject::disconnect(connection);
        CHECK_FALSE(message.isEmpty());
    }

    QString announced;
    int order = 0;
    int loggedInAt = 0;
    int replyAt = 0;
    QObject::connect(&forms, &bookmarks::gui::FormsBridge::loggedIn, [&](const QString& principal) {
        announced = principal;
        loggedInAt = ++order;
    });
    QObject::connect(&forms, &bookmarks::gui::FormsBridge::replyReceived,
                     [&](const QString&, bool, const QString&) { replyAt = ++order; });

    forms.submitIfValid(QStringLiteral("Login"), QStringLiteral(R"({"username":"alice"})"));
    REQUIRE(pumpUntil([&] { return replyAt != 0; }));

    // The server's echo of the identity it verified, not the client's claim.
    CHECK(announced == QStringLiteral("alice"));
    REQUIRE(loggedInAt != 0);
    CHECK(loggedInAt < replyAt);

    // ...and the same bridge now works, which is the only observable proof
    // that `setDefaultSession` was called with the returned token.
    QVariantList rows;
    bool listed = false;
    QObject::connect(&bookmarkBridge, &bookmarks::gui::BookmarkBridge::listed, [&](const QVariantList& page) {
        rows = page;
        listed = true;
    });
    bookmarkBridge.refresh();
    REQUIRE(pumpUntil([&] { return listed; }));
    CHECK(rows.isEmpty());  // a real, empty collection — not an error
}

TEST_CASE("decodeLoginResult accepts a real Login reply and rejects anything that is not one",
          "[bookmarks][gui][qml-bridges]") {
    // The failure arm's *caller* — `FormsBridge::submitIfValid`'s
    // "login succeeded but its reply could not be decoded" branch — cannot be
    // reached through any backend the ladder ships, because the reply is
    // always written by `resultToJson` from the same reflected type this reads
    // back. See `decodeLoginResult`'s own doc comment: the decision was split
    // out precisely so both arms are testable without a fake backend.
    const auto decoded = bookmarks::gui::decodeLoginResult(R"({"token":"signed.token.value","principal":"alice"})");
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->token.hasValue());
    CHECK(*decoded->token == "signed.token.value");
    CHECK(decoded->principal == "alice");

    // Everything a peer could hand back that is *not* a LoginResult. Each must
    // yield nullopt rather than a default-constructed result, which is what
    // would otherwise be installed as a tokenless session under an empty
    // principal — a client that believes it is logged in and is not.
    for (const char* body : {"", "not json at all", "[1,2,3]", "null", R"({"token":123,"principal":"alice"})",
                             R"({"principal":"alice")"}) {
        INFO("unexpectedly decoded: " << body);
        CHECK_FALSE(bookmarks::gui::decodeLoginResult(body).has_value());
    }
}

TEST_CASE("decodeLoginResult reads back exactly what a real Login dispatch produced, redacted",
          "[bookmarks][gui][qml-bridges]") {
    // Pins the assumption the case above rests on: the reply shape asserted
    // there by hand is the shape the wire really carries. If `LoginResult`'s
    // reflection ever changed, this fails here rather than silently making the
    // hand-written literals above test nothing.
    //
    // `payload` here is `FormsBridge::submitIfValid`'s emitted
    // `replyReceived` argument, not the model's raw wire reply -- and for a
    // successful `Login` those two are deliberately different
    // (`bookmark_qml_bridges.cpp`'s own comment on the `Login` branch): the
    // token has already been installed onto the session by the time this
    // signal fires, so the QML-facing payload has it redacted rather than
    // broadcasting a live bearer credential to every bound handler.
    DbFixture fixture;
    const ScopedTokenIssuer issuer{
        std::make_shared<morph::session::TokenIssuer>(std::string{kSecret}, morph::session::hmacSha256)};
    BackendRig rig{Mode::Local, 1};
    bookmarks::gui::FormsBridge forms{rig.bridge(0), rig.executor()};
    bookmarks::gui::BookmarkBridge bookmarkBridge{rig.bridge(0), rig.executor()};

    const auto [ok, payload] = submit(forms, QStringLiteral("Login"), QStringLiteral(R"({"username":"alice"})"));
    REQUIRE(ok);

    const auto decoded = bookmarks::gui::decodeLoginResult(payload.toStdString());
    REQUIRE(decoded.has_value());
    CHECK(decoded->principal == "alice");
    CHECK_FALSE(decoded->token.hasValue());

    // ...and the same bridge now works, which is the only observable proof
    // that `setDefaultSession` was called with the *real* token -- the one
    // this test just confirmed never left the process via `payload`.
    QVariantList rows;
    bool listed = false;
    QObject::connect(&bookmarkBridge, &bookmarks::gui::BookmarkBridge::listed, [&](const QVariantList& page) {
        rows = page;
        listed = true;
    });
    bookmarkBridge.refresh();
    REQUIRE(pumpUntil([&] { return listed; }));
    CHECK(rows.isEmpty());  // a real, empty collection — not an error
}
