// SPDX-License-Identifier: Apache-2.0
//
// The QML-adapter layer's own suite: `PollBridge` (`gui_lib/poll_qml_bridges.hpp`)
// and the `PollFormsController` it wraps (`gui_lib/poll_forms_controller.hpp`)
// — everything that stands between `PollPresenter`/`PollModel` and
// `gui/qml/{Main,CreatePollView,VoteView}.qml`. Mirrors
// examples/bookmarks/tests/test_bookmark_qml_bridges.cpp's shape and
// rationale (rung 2's Task 18) — read that file's own header comment for why
// this layer needs its own suite distinct from test_poll_presenter.cpp; the
// same reasoning applies verbatim here (QML binds by *string*, so a renamed
// key, a mistyped action id or a changed signal signature is not a compile
// error anywhere).
//
// One thing this suite proves that has no rung-2 analogue at all: that every
// already-open-poll action really does share PollFormsController's one
// `BridgeHandler<PollModel, AllowShared>` correctly. PollModel is this
// rung's shared/keyed model (rung 2's three models are all plain); a second,
// independently-attached handler for e.g. AddComment would fail "handler not
// bound" until it separately attached — the "openPoll then AddComment/
// FinalizePoll/UndoLastVoteChange/submitVotes/updateVotes/refresh/
// getEventsSince all succeed" cases below are the direct proof that never
// happens here (see poll_forms_controller.hpp's own doc comment for the full
// design rationale).

#include "poll_qml_bridges.hpp"
#include "poll_schemas.hpp"
#include "polls/auth/polls_authorizer.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMetaProperty>
#include <QMetaType>
#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

/// @brief Builds a rig with a fresh `PollsAuthorizer` — every polls test
///        file that touches `Mode::Socket` passes an explicit authorizer;
///        this suite stays on `Mode::Local` throughout (the presenter suite
///        already covers the full backend-mode matrix per action), but
///        matches the same construction shape for consistency.
[[nodiscard]] std::unique_ptr<BackendRig> makeRig() {
    return std::make_unique<BackendRig>(Mode::Local, 1, std::make_shared<polls::auth::PollsAuthorizer>());
}

/// @brief How many methods a class declares itself (signals + `Q_INVOKABLE`s),
///        i.e. excluding everything it inherits from `QObject`.
[[nodiscard]] int ownMethodCount(const QMetaObject* meta) { return meta->methodCount() - meta->methodOffset(); }

/// @brief One `pollBridge.createPoll(title, optionLabels)` round trip.
/// @param bridge       The bridge to create through.
/// @param title        The poll's title.
/// @param optionLabels Candidate option labels.
/// @return `{ok, bag-or-message}` from the single `created`/`failed` signal.
[[nodiscard]] std::pair<bool, QVariantMap> createVia(polls::gui::PollBridge& bridge, const QString& title,
                                                     const QVariantList& optionLabels) {
    QVariantMap bag;
    QString failure;
    bool settled = false;
    bool ok = false;
    const auto onCreated = QObject::connect(&bridge, &polls::gui::PollBridge::created, [&](const QVariantMap& result) {
        bag = result;
        ok = true;
        settled = true;
    });
    const auto onFailed = QObject::connect(&bridge, &polls::gui::PollBridge::failed, [&](const QString& message) {
        failure = message;
        ok = false;
        settled = true;
    });
    bridge.createPoll(title, optionLabels);
    REQUIRE(pumpUntil([&] { return settled; }));
    QObject::disconnect(onCreated);
    QObject::disconnect(onFailed);
    if (!ok) {
        bag.insert(QStringLiteral("__error"), failure);
    }
    return {ok, bag};
}

/// @brief One `pollBridge.openPoll(pollId)` round trip.
/// @param bridge The bridge to open through.
/// @param pollId The poll to attach to.
/// @return `{ok, state-bag-or-message}` from the single `opened`/`failed` signal.
[[nodiscard]] std::pair<bool, QVariantMap> openVia(polls::gui::PollBridge& bridge, const QString& pollId) {
    QVariantMap bag;
    QString failure;
    bool settled = false;
    bool ok = false;
    const auto onOpened = QObject::connect(&bridge, &polls::gui::PollBridge::opened, [&](const QVariantMap& state) {
        bag = state;
        ok = true;
        settled = true;
    });
    const auto onFailed = QObject::connect(&bridge, &polls::gui::PollBridge::failed, [&](const QString& message) {
        failure = message;
        ok = false;
        settled = true;
    });
    bridge.openPoll(pollId);
    REQUIRE(pumpUntil([&] { return settled; }));
    QObject::disconnect(onOpened);
    QObject::disconnect(onFailed);
    if (!ok) {
        bag.insert(QStringLiteral("__error"), failure);
    }
    return {ok, bag};
}

/// @brief One `stateChanged`/`failed` round trip driven by @p act (e.g.
///        `refresh`, `submitVotes`, `updateVotes`).
/// @param bridge The bridge the action runs against.
/// @param act    Callable that triggers exactly one such round trip.
/// @return `{ok, state-bag-or-message}`.
template <typename Act>
[[nodiscard]] std::pair<bool, QVariantMap> stateChangeVia(polls::gui::PollBridge& bridge, Act act) {
    QVariantMap bag;
    QString failure;
    bool settled = false;
    bool ok = false;
    const auto onChanged =
        QObject::connect(&bridge, &polls::gui::PollBridge::stateChanged, [&](const QVariantMap& state) {
            bag = state;
            ok = true;
            settled = true;
        });
    const auto onFailed = QObject::connect(&bridge, &polls::gui::PollBridge::failed, [&](const QString& message) {
        failure = message;
        ok = false;
        settled = true;
    });
    act();
    REQUIRE(pumpUntil([&] { return settled; }));
    QObject::disconnect(onChanged);
    QObject::disconnect(onFailed);
    if (!ok) {
        bag.insert(QStringLiteral("__error"), failure);
    }
    return {ok, bag};
}

/// @brief One `pollBridge.submitIfValid(actionType, bodyJson)` round trip.
/// @param bridge     The bridge to submit through.
/// @param actionType The schema-driven action id.
/// @param bodyJson   The `DynamicForm`-shaped JSON body.
/// @return `{ok, payload}` from the single `replyReceived`.
[[nodiscard]] std::pair<bool, QString> submitVia(polls::gui::PollBridge& bridge, const QString& actionType,
                                                 const QString& bodyJson) {
    bool ok = false;
    QString payload;
    QString echoedType;
    bool replied = false;
    const auto connection = QObject::connect(&bridge, &polls::gui::PollBridge::replyReceived,
                                             [&](const QString& type, bool succeeded, const QString& body) {
                                                 echoedType = type;
                                                 ok = succeeded;
                                                 payload = body;
                                                 replied = true;
                                             });
    bridge.submitIfValid(actionType, bodyJson);
    REQUIRE(pumpUntil([&] { return replied; }));
    QObject::disconnect(connection);
    // VoteView.qml:98-106 dispatches on the echoed type, so a normalised or
    // empty echo would misroute every outcome on that screen.
    REQUIRE(echoedType == actionType);
    return {ok, payload};
}

/// @brief Finds the first option's `id` in a `GetPollStateResult` bag's
///        `options` list.
/// @param stateBag A bag as `opened`/`stateChanged` carries it.
/// @return The first option's numeric id.
[[nodiscard]] qlonglong firstOptionId(const QVariantMap& stateBag) {
    const QVariantList options = stateBag.value(QStringLiteral("options")).toList();
    REQUIRE_FALSE(options.isEmpty());
    return options.front().toMap().value(QStringLiteral("id")).toLongLong();
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════
// The QML-visible surface: names and signatures QML binds by string
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("PollBridge exposes exactly the surface Main.qml/CreatePollView.qml/VoteView.qml bind against",
          "[polls][gui][qml-bridges]") {
    DbFixture fixture;
    auto rig = makeRig();
    polls::gui::PollBridge bridge{rig->bridge(0), rig->executor()};

    const QMetaObject* meta = bridge.metaObject();

    // `root.pollBridge.schemasJson` — Main.qml.
    REQUIRE(meta->indexOfProperty("schemasJson") >= 0);
    CHECK(meta->property(meta->indexOfProperty("schemasJson")).isConstant());
    CHECK(meta->propertyCount() - meta->propertyOffset() == 1);

    // `page.pollBridge.createPoll(...)` — CreatePollView.qml.
    REQUIRE(meta->indexOfMethod("createPoll(QString,QVariantList)") >= 0);
    // `page.pollBridge.openPoll(...)` — VoteView.qml (Component.onCompleted)
    // and Main.qml's landing screen.
    REQUIRE(meta->indexOfMethod("openPoll(QString)") >= 0);
    REQUIRE(meta->indexOfMethod("refresh()") >= 0);
    REQUIRE(meta->indexOfMethod("submitVotes(QString,QVariantList)") >= 0);
    REQUIRE(meta->indexOfMethod("updateVotes(QString,QVariantList)") >= 0);
    REQUIRE(meta->indexOfMethod("setAdminToken(QString)") >= 0);
    REQUIRE(meta->indexOfMethod("submitIfValid(QString,QString)") >= 0);
    REQUIRE(meta->indexOfMethod("stopPolling()") >= 0);

    REQUIRE(meta->indexOfSignal("created(QVariantMap)") >= 0);
    REQUIRE(meta->indexOfSignal("opened(QVariantMap)") >= 0);
    REQUIRE(meta->indexOfSignal("stateChanged(QVariantMap)") >= 0);
    REQUIRE(meta->indexOfSignal("eventReceived(QVariantMap)") >= 0);
    REQUIRE(meta->indexOfSignal("replyReceived(QString,bool,QString)") >= 0);
    REQUIRE(meta->indexOfSignal("pollingStopped(QString)") >= 0);
    REQUIRE(meta->indexOfSignal("failed(QString)") >= 0);

    // Nothing else: an adapter method with no binding site is a stub, and one
    // removed from under a binding is a silent runtime gap.
    CHECK(ownMethodCount(meta) == 15);

    // The property's value is the shared schema document, verbatim — the
    // same one every shell builds (poll_schemas.hpp exists so they cannot
    // diverge), and `JSON.parse`-able, since Main.qml does exactly that.
    CHECK(bridge.schemasJson().toStdString() == polls::gui::pollSchemasJson());
    const QJsonDocument schemas = QJsonDocument::fromJson(bridge.schemasJson().toUtf8());
    REQUIRE(schemas.isObject());
    for (const char* actionType : {"AddComment", "FinalizePoll", "UndoLastVoteChange"}) {
        INFO("missing schema: " << actionType);
        CHECK(schemas.object().contains(QString::fromLatin1(actionType)));
    }
    CHECK(schemas.object().size() == 3);
}

// ═════════════════════════════════════════════════════════════════════════
// createPoll / openPoll
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("PollBridge::createPoll emits a {pollId, adminToken, participantToken} bag with no leaked field",
          "[polls][gui][qml-bridges]") {
    DbFixture fixture;
    auto rig = makeRig();
    polls::gui::PollBridge bridge{rig->bridge(0), rig->executor()};

    const auto [ok, bag] =
        createVia(bridge, QStringLiteral("Team offsite"), QVariantList{QStringLiteral("2026-09-01"), QStringLiteral("2026-09-02")});
    REQUIRE(ok);
    for (const char* key : {"pollId", "adminToken", "participantToken"}) {
        INFO("missing key: " << key);
        REQUIRE(bag.contains(QString::fromLatin1(key)));
    }
    CHECK(bag.size() == 3);
    CHECK_FALSE(bag.value(QStringLiteral("pollId")).toString().isEmpty());
    CHECK_FALSE(bag.value(QStringLiteral("adminToken")).toString().isEmpty());
    CHECK_FALSE(bag.value(QStringLiteral("participantToken")).toString().isEmpty());
}

TEST_CASE("PollBridge::createPoll with fewer than two options emits failed, not a crash",
          "[polls][gui][qml-bridges]") {
    DbFixture fixture;
    auto rig = makeRig();
    polls::gui::PollBridge bridge{rig->bridge(0), rig->executor()};

    const auto [ok, bag] = createVia(bridge, QStringLiteral("T"), QVariantList{QStringLiteral("only one")});
    CHECK_FALSE(ok);
    CHECK_FALSE(bag.value(QStringLiteral("__error")).toString().isEmpty());
}

TEST_CASE("PollBridge::openPoll emits the poll's full state, and a bad pollId emits failed",
          "[polls][gui][qml-bridges]") {
    DbFixture fixture;
    auto rig = makeRig();
    polls::gui::PollBridge bridge{rig->bridge(0), rig->executor()};

    const auto [created, createdBag] =
        createVia(bridge, QStringLiteral("Lunch spot"), QVariantList{QStringLiteral("Cafe"), QStringLiteral("Diner")});
    REQUIRE(created);
    const QString pollId = createdBag.value(QStringLiteral("pollId")).toString();

    const auto [ok, state] = openVia(bridge, pollId);
    REQUIRE(ok);
    for (const char* key : {"pollId", "title", "finalized", "finalizedOptionId", "options", "votes", "comments",
                            "lastEventId"}) {
        INFO("missing key: " << key);
        REQUIRE(state.contains(QString::fromLatin1(key)));
    }
    CHECK(state.size() == 8);
    CHECK(state.value(QStringLiteral("pollId")).toString() == pollId);
    CHECK(state.value(QStringLiteral("title")).toString() == QStringLiteral("Lunch spot"));
    CHECK_FALSE(state.value(QStringLiteral("finalized")).toBool());
    // Unengaged (no finalize yet, freshly opened -- lastEventId not yet
    // advanced): both render as -1, this rung's "not entered" sentinel.
    CHECK(state.value(QStringLiteral("finalizedOptionId")).toLongLong() == -1);
    CHECK(state.value(QStringLiteral("lastEventId")).toLongLong() == -1);
    const QVariantList options = state.value(QStringLiteral("options")).toList();
    REQUIRE(options.size() == 2);
    CHECK(options[0].toMap().value(QStringLiteral("label")).toString() == QStringLiteral("Cafe"));
    CHECK(options[0].toMap().value(QStringLiteral("yesCount")).toString() == QStringLiteral("0"));

    const auto [badOk, badBag] = openVia(bridge, QStringLiteral("no-such-poll-id"));
    CHECK_FALSE(badOk);
    CHECK_FALSE(badBag.value(QStringLiteral("__error")).toString().isEmpty());
}

// ═════════════════════════════════════════════════════════════════════════
// The shared-handler proof: openPoll, then every other action on the same
// poll, all through PollFormsController's one BridgeHandler
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("PollBridge threads openPoll's attach through every later action on the same poll",
          "[polls][gui][qml-bridges]") {
    DbFixture fixture;
    auto rig = makeRig();
    polls::gui::PollBridge bridge{rig->bridge(0), rig->executor()};

    const auto [created, createdBag] =
        createVia(bridge, QStringLiteral("T"), QVariantList{QStringLiteral("1"), QStringLiteral("2")});
    REQUIRE(created);
    const QString pollId = createdBag.value(QStringLiteral("pollId")).toString();
    const QString adminToken = createdBag.value(QStringLiteral("adminToken")).toString();

    const auto [opened, openedState] = openVia(bridge, pollId);
    REQUIRE(opened);
    const qlonglong optionId = firstOptionId(openedState);

    // submitVotes -- would fail "handler not bound" if PollFormsController
    // used a second, independently-attached handler instead of reusing the
    // one openPoll() just attached.
    const auto [votedOk, votedState] = stateChangeVia(bridge, [&] {
        bridge.submitVotes(QStringLiteral("alice"),
                           QVariantList{QVariantMap{{"optionId", optionId}, {"choice", QStringLiteral("Yes")}}});
    });
    REQUIRE(votedOk);
    const QVariantList votedOptions = votedState.value(QStringLiteral("options")).toList();
    CHECK(votedOptions.front().toMap().value(QStringLiteral("yesCount")).toString() == QStringLiteral("1"));

    // updateVotes -- same handler, different action.
    const auto [updatedOk, updatedState] = stateChangeVia(bridge, [&] {
        bridge.updateVotes(QStringLiteral("alice"),
                           QVariantList{QVariantMap{{"optionId", optionId}, {"choice", QStringLiteral("No")}}});
    });
    REQUIRE(updatedOk);
    const QVariantList updatedOptions = updatedState.value(QStringLiteral("options")).toList();
    CHECK(updatedOptions.front().toMap().value(QStringLiteral("yesCount")).toString() == QStringLiteral("0"));
    CHECK(updatedOptions.front().toMap().value(QStringLiteral("noCount")).toString() == QStringLiteral("1"));

    // AddComment -- schema-driven, via submitIfValid.
    const auto [commentOk, commentPayload] =
        submitVia(bridge, QStringLiteral("AddComment"),
                 QStringLiteral(R"({"participantName":"alice","body":"works for me"})"));
    REQUIRE(commentOk);
    CHECK(commentPayload.contains(QStringLiteral("works for me")));

    // refresh -- a plain GetPollState against the same attached handler.
    const auto [refreshedOk, refreshedState] = stateChangeVia(bridge, [&] { bridge.refresh(); });
    REQUIRE(refreshedOk);
    CHECK(refreshedState.value(QStringLiteral("comments")).toList().size() == 1);

    // UndoLastVoteChange -- schema-driven; its result is UndoLastVoteChangeResult,
    // not GetPollStateResult, so the payload shape differs from the others.
    const auto [undoOk, undoPayload] =
        submitVia(bridge, QStringLiteral("UndoLastVoteChange"), QStringLiteral(R"({"participantName":"alice"})"));
    REQUIRE(undoOk);
    // `"Yes"`, not `true`: `UndoLastVoteChangeResult::restored` is the
    // two-enumerator `polls::Restored`, reflected by its own `glz::meta`
    // as the enumerator name (IMPLEMENTATION.md rule 3 -- no bare bools
    // in DTO fields, on the wire or off it).
    CHECK(undoPayload.contains(QStringLiteral("\"restored\":\"Yes\"")));

    // FinalizePoll -- admin-token-gated; fails without the token, succeeds
    // once PollBridge::setAdminToken installs it, and both dispatch through
    // the same attached handler as everything above.
    const auto [deniedOk, deniedPayload] =
        submitVia(bridge, QStringLiteral("FinalizePoll"), QStringLiteral(R"({"optionId":%1})").arg(optionId));
    CHECK_FALSE(deniedOk);
    CHECK_FALSE(deniedPayload.isEmpty());

    bridge.setAdminToken(adminToken);
    const auto [finalizedOk, finalizedPayload] =
        submitVia(bridge, QStringLiteral("FinalizePoll"), QStringLiteral(R"({"optionId":%1})").arg(optionId));
    REQUIRE(finalizedOk);
    CHECK(finalizedPayload.contains(QStringLiteral("\"finalized\":\"Yes\"")));
}

// ═════════════════════════════════════════════════════════════════════════
// submitIfValid's allow-list (finding 034's guard)
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("PollBridge::submitIfValid refuses an action outside the schema document instead of mis-dispatching it",
          "[polls][gui][qml-bridges]") {
    // OpenPoll in particular: dispatching it through executeJson on an
    // AllowShared handler silently skips the payload-keyed attach step
    // (docs/findings/034) -- PollFormsController::submitIfValid refuses it
    // by name before that path is ever reached.
    DbFixture fixture;
    auto rig = makeRig();
    polls::gui::PollBridge bridge{rig->bridge(0), rig->executor()};

    for (const auto& actionType : {QStringLiteral("OpenPoll"), QStringLiteral("SubmitVotes"),
                                   QStringLiteral("CreatePoll"), QStringLiteral("NotEvenReal")}) {
        const auto [ok, payload] = submitVia(bridge, actionType, QStringLiteral("{}"));
        INFO(actionType.toStdString());
        CHECK_FALSE(ok);
        CHECK(payload.contains(QStringLiteral("not a schema-driven action")));
    }
}

// ═════════════════════════════════════════════════════════════════════════
// The live, event-driven results display -- EventPoller wired to a real view
// ═════════════════════════════════════════════════════════════════════════

TEST_CASE("PollBridge's EventPoller applies a live event and refreshes state, end to end",
          "[polls][gui][qml-bridges][event-poller]") {
    // The one genuinely slow case in this suite, deliberately: it proves the
    // *real* production wiring (PollBridge's Dispatch closure over
    // PollFormsController::getEventsSince, ticking on EventPoller's real
    // default 3s interval -- see event_poller.hpp's own "Default poll
    // interval" section) rather than a manually-driven pollOnce(), which
    // PollBridge does not expose (it owns the poller privately, matching a
    // real view). test_event_poller.cpp already covers the class's own
    // mechanics exhaustively with an artificial long interval + manual
    // ticks; this is the one place in the whole ladder that proves the
    // *wiring* to a real screen's adapter actually ticks on its own.
    DbFixture fixture;
    auto rig = makeRig();
    polls::gui::PollBridge bridge{rig->bridge(0), rig->executor()};

    const auto [created, createdBag] =
        createVia(bridge, QStringLiteral("T"), QVariantList{QStringLiteral("1"), QStringLiteral("2")});
    REQUIRE(created);
    const QString pollId = createdBag.value(QStringLiteral("pollId")).toString();

    const auto [opened, openedState] = openVia(bridge, pollId);
    REQUIRE(opened);
    const qlonglong optionId = firstOptionId(openedState);

    // A vote after openPoll writes one PollEvent (kind "vote") -- the
    // increment the next tick should pick up.
    const auto [votedOk, votedState] = stateChangeVia(bridge, [&] {
        bridge.submitVotes(QStringLiteral("alice"),
                           QVariantList{QVariantMap{{"optionId", optionId}, {"choice", QStringLiteral("Yes")}}});
    });
    REQUIRE(votedOk);
    static_cast<void>(votedState);

    QVariantMap event;
    bool eventSeen = false;
    QVariantMap resynced;
    bool resyncSeen = false;
    const auto onEvent =
        QObject::connect(&bridge, &polls::gui::PollBridge::eventReceived, [&](const QVariantMap& e) {
            event = e;
            eventSeen = true;
        });
    const auto onResync =
        QObject::connect(&bridge, &polls::gui::PollBridge::stateChanged, [&](const QVariantMap& s) {
            resynced = s;
            resyncSeen = true;
        });

    // kDefaultInterval is 3000ms; a 6s budget comfortably covers one real
    // tick plus dispatch/round-trip overhead without hardcoding a tighter
    // margin that would make this test flaky on a loaded CI runner.
    REQUIRE(pumpUntil([&] { return eventSeen; }, std::chrono::milliseconds{6000}));
    QObject::disconnect(onEvent);

    CHECK(event.value(QStringLiteral("kind")).toString() == QStringLiteral("vote"));
    CHECK_FALSE(event.value(QStringLiteral("summary")).toString().isEmpty());
    CHECK(event.value(QStringLiteral("id")).toLongLong() > 0);

    // onEventApplied schedules a debounced refresh() right after -- give it
    // a further short budget on the same event loop.
    REQUIRE(pumpUntil([&] { return resyncSeen; }, std::chrono::milliseconds{2000}));
    QObject::disconnect(onResync);
    CHECK(resynced.value(QStringLiteral("pollId")).toString() == pollId);
}

