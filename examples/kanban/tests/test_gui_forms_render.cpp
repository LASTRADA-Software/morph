// SPDX-License-Identifier: Apache-2.0
//
// The seven schema-driven forms of this rung's GUI, driven through the *shipped*
// renderer in a real QML engine: `CreateProject` (ProjectListView.qml),
// `SetMemberRole` (MembersView.qml), `CreateColumn`/`CreateSwimlane`/
// `CreateTask`/`CreateRule` (BoardView.qml/RulesView.qml) and `AddComment`
// (TaskDetailPopup.qml).
//
// Why this exists as its own file, next to test_gui_qml_smoke.cpp rather than
// inside it. The smoke test is examples/TESTING.md presenter rule 6's
// engine-load check: it loads every root with all bridges null, so it proves
// only that the QML parses and every type resolves. It cannot prove that a
// `DynamicForm` bound to a real bridge produced the fields the schema
// describes, that the two *hidden* context fields (`CreateTask`'s
// columnId/swimlaneId, `AddComment`'s taskId) are actually engaged by the view
// that owns them, or that a submit reaches the model. Those are exactly the
// claims morph#344 turns on -- "schema generation working is necessary, not
// sufficient; nothing has been rendered on screen" -- so they get a test that
// renders on screen.
//
// It stays inside `ladder_kanban_tests` rather than becoming a third binary
// (bank's `bank_gui_qml_tests` is the ladder's precedent for that shape): this
// binary already links Qt Quick and already loads QML for the smoke test, so
// splitting would buy nothing. Like bank's, it is **not** Qt Quick Test and it
// synthesizes **no** mouse events (rule 6): the only thing it reproduces is the
// two calls the renderer's own Submit button makes -- `setFieldValue` for each
// typed field, then `submit()` -- both of them plain QML functions on
// DynamicForm, invoked through the metaobject.
//
// Runs under QT_QPA_PLATFORM=offscreen, against the QGuiApplication
// testkit_main.cpp owns when this rung's test binary is built. Compiled away
// entirely without MORPH_LADDER_QML_URI, i.e. in a configure with no
// MORPH_BUILD_FORMS_QML, exactly like the smoke test beside it.

#ifdef MORPH_LADDER_QML_URI

#include <QList>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <kanban/models/board_model.hpp>
#include <kanban/models/project_admin_model.hpp>
#include <memory>
#include <morph/session/session.hpp>
#include <string>

#include "board_qml_bridge.hpp"
#include "project_admin_qml_bridge.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

namespace {

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

/// @brief A rig whose one bridge already carries a valid session for
///        @p principal — the same recipe test_board_qml_bridge.cpp uses.
/// @param principal The identity to install.
/// @return The rig, owning the bridge and executor the adapters take.
[[nodiscard]] std::unique_ptr<BackendRig> makeAuthedRig(std::string principal) {
    auto rig = std::make_unique<BackendRig>(Mode::Local, 1);
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    rig->bridge(0).setDefaultSession(ctx);
    return rig;
}

/// @brief Seeds one project (the rig's principal is its Manager) straight
///        through `ProjectAdminModel`'s own handler.
/// @param rig The rig whose bridge/executor to dispatch the seed through.
/// @return The new project's id, as its plain number.
[[nodiscard]] qlonglong seedProject(BackendRig& rig) {
    morph::bridge::BridgeHandler<kanban::ProjectAdminModel> creator{rig.bridge(0), rig.executor()};
    const auto id = morph::ladder::testkit::awaitQt(creator.execute(kanban::CreateProject{.name = "Sprint Board"})).id;
    return id.hasValue() ? static_cast<qlonglong>(*id) : -1;
}

/// @brief One field's worth of user input, as the renderer's own controls
///        record it: `DynamicForm.setFieldValue(name, text)`.
///
/// Every visible control in `DynamicForm.qml` funnels its edits through this
/// one function (its `TextField.onTextChanged`, its `ComboBox.onActivated`,
/// its `CheckBox.onToggled`), so calling it directly is the same state change
/// typing produces, minus the synthesized key events rule 6 forbids.
/// @param form  The `DynamicForm` instance.
/// @param field The field's wire name.
/// @param text  The text to record.
void type(QObject* form, const QString& field, const QString& text) {
    REQUIRE(form != nullptr);
    REQUIRE(QMetaObject::invokeMethod(form, "setFieldValue", Q_ARG(QVariant, QVariant{field}),
                                      Q_ARG(QVariant, QVariant{text})));
}

/// @brief Presses the renderer's own Submit button, without pressing it.
///
/// `DynamicForm.qml`'s Submit `Button.onClicked` is exactly `form.submit()`,
/// and that button is disabled while `!ready` — which is asserted separately at
/// each call site here, so the gate is checked rather than bypassed.
/// @param form The `DynamicForm` instance.
void pressSubmit(QObject* form) {
    REQUIRE(form != nullptr);
    REQUIRE(QMetaObject::invokeMethod(form, "submit"));
}

/// @brief The body `DynamicForm` would send right now (`previewLine`), or an
///        empty string while the form is not `ready`.
/// @param form The `DynamicForm` instance.
/// @return The assembled JSON body.
[[nodiscard]] QString bodyOf(const QObject* form) { return form->property("previewLine").toString(); }

/// @brief Whether the form's required-field gate is currently satisfied.
/// @param form The `DynamicForm` instance.
/// @return `DynamicForm.ready`.
[[nodiscard]] bool isReady(const QObject* form) { return form->property("ready").toBool(); }

/// @brief Depth-first search of the *visual* item tree under @p root for an
///        item with @p name as its `objectName`.
///
/// `QObject::findChild` is not enough for anything a `Repeater` created — a
/// delegate's visual parent is inside the tree while its `QObject` parent is
/// not — and BoardView's per-column `CreateTask` form is exactly that, two
/// Repeaters deep.
/// @param root The item to search under (searched itself first).
/// @param name The `objectName` to find.
/// @return The item, or `nullptr`.
[[nodiscard]] QQuickItem* findItem(QQuickItem* root, const QString& name) {
    if (root == nullptr) {
        return nullptr;
    }
    if (root->objectName() == name) {
        return root;
    }
    const QList<QQuickItem*> kids = root->childItems();
    for (QQuickItem* kid : kids) {
        if (QQuickItem* hit = findItem(kid, name); hit != nullptr) {
            return hit;
        }
    }
    return nullptr;
}

/// @brief One of the controls the renderer drew, by the `objectName` it gives
///        it: `field_<wire name>` for a scalar control, `column_<wire name>`
///        for the labelled column wrapping it.
///
/// Uses `DynamicForm`'s own `findControl`, not `QObject::findChild`: the
/// controls are `Repeater` delegates, whose *visual* parent is inside the form
/// while their `QObject` parent is not, so `findChild` sees none of them. That
/// is also why this is not a detail of the test -- it is the same lookup
/// `resetFields()` performs to clear a control.
/// @param form The `DynamicForm` instance.
/// @param name The control's `objectName`.
/// @return The control, or `nullptr` when the renderer drew none.
[[nodiscard]] QObject* control(QObject* form, const QString& name) {
    if (form == nullptr) {
        return nullptr;
    }
    QVariant found;
    if (!QMetaObject::invokeMethod(form, "findControl", Q_RETURN_ARG(QVariant, found),
                                   Q_ARG(QVariant, QVariant::fromValue(form)), Q_ARG(QVariant, QVariant{name}))) {
        return nullptr;
    }
    return found.value<QObject*>();
}

/// @brief Loads @p typeName from this rung's QML module with @p properties set
///        as initial properties, asserting the engine produced a root object
///        and emitted no warning this rung is responsible for.
///
/// Initial properties, not context properties, for the same reason
/// `gui/main.cpp` uses them: the root object declares what it needs, so the
/// same file also loads with nothing wired up.
///
/// @par The one tolerated warning
/// `DynamicForm.qml` declares `onOptionsReceived` in a `Connections` block
/// whose `target` is the controller, unconditionally. A controller that serves
/// no `morph::forms::Choice` field has no such signal — and deliberately so:
/// `bookmarks::gui::BookmarkFormsController`'s own "No `fetchOptions()`" note
/// records that adding one with nothing to call it would be a stub. So the
/// engine warns once per form, for every conforming controller in the ladder,
/// the moment a *real* controller is attached. The rule-6 smoke test never sees
/// it because it attaches none. Filed as morph#387; tolerated by exact text
/// here rather than by dropping the assertion, so any *other* warning — a
/// misspelled handler, a missing property, a broken binding — still fails.
/// @param engine     The engine to load into (kept alive by the caller).
/// @param typeName   Unqualified QML type name within `MORPH_LADDER_QML_URI`.
/// @param properties Initial properties for the root object.
/// @return The root object.
[[nodiscard]] QObject* loadRoot(QQmlApplicationEngine& engine, const char* typeName, const QVariantMap& properties) {
    QStringList unexpected;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, [&unexpected](const QList<QQmlError>& warnings) {
        for (const QQmlError& warning : warnings) {
            const QString text = warning.toString();
            if (text.contains(QStringLiteral("onOptionsReceived")) &&
                text.contains(QStringLiteral("MorphForms/qml/DynamicForm.qml"))) {
                continue;  // morph#387 — see this function's doc comment.
            }
            unexpected.append(text);
        }
    });
    engine.setInitialProperties(properties);
    engine.loadFromModule(MORPH_LADDER_QML_URI, typeName);
    INFO(unexpected.join(QStringLiteral("\n")).toStdString());
    CHECK(unexpected.isEmpty());
    REQUIRE_FALSE(engine.rootObjects().isEmpty());
    return engine.rootObjects().front();
}

}  // namespace

TEST_CASE("ProjectListView renders CreateProject through the shipped renderer and submits it",
          "[kanban][gui][qml-forms][issue344]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    kanban::gui::ProjectAdminBridge bridge{rig->bridge(0), rig->executor()};

    QQmlApplicationEngine engine;
    QObject* root =
        loadRoot(engine, "ProjectListView", {{QStringLiteral("projectAdminBridge"), QVariant::fromValue(&bridge)}});

    // 1. It rendered. The form exists, and the renderer drew a control for the
    //    one member `CreateProject` has -- named from the schema, not from
    //    anything this file or the .qml wrote by hand.
    QObject* form = root->findChild<QObject*>(QStringLiteral("createProjectForm"));
    REQUIRE(form != nullptr);
    CHECK(form->property("actionType").toString() == QStringLiteral("CreateProject"));
    REQUIRE(control(form, QStringLiteral("field_name")) != nullptr);

    // 2. Its explicit-submit gate is real: nothing has been typed, so the
    //    Submit button is disabled and `submit()` is a no-op.
    CHECK_FALSE(isReady(form));

    // 3. Typing satisfies the gate, and the body is the one the schema
    //    describes.
    type(form, QStringLiteral("name"), QStringLiteral("Sprint Board"));
    CHECK(isReady(form));
    CHECK(bodyOf(form) == QStringLiteral(R"({"name":"Sprint Board"})"));

    // 4. Submitting reaches the model: the project comes back in a listing the
    //    view never asked this test to fake.
    bool created = false;
    QObject::connect(&bridge, &kanban::gui::ProjectAdminBridge::projectCreated,
                     [&created](qlonglong, const QString&) { created = true; });
    pressSubmit(form);
    REQUIRE(pumpUntil([&created] { return created; }));

    bool listed = false;
    QVariantList rows;
    QObject::connect(&bridge, &kanban::gui::ProjectAdminBridge::projectsListed, [&](const QVariantList& projects) {
        rows = projects;
        listed = true;
    });
    bridge.refreshProjects();
    REQUIRE(pumpUntil([&listed] { return listed; }));
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Sprint Board"));

    // 5. And the view cleared the form on the reply, so the next create does
    //    not start with the last project's name still in the field.
    CHECK_FALSE(isReady(form));
}

TEST_CASE("MembersView renders SetMemberRole through the shipped renderer and submits it",
          "[kanban][gui][qml-forms][issue344][issue393]") {
    // Loaded directly (not via ProjectListView's "Members" button, which this
    // rule-6 file cannot click): MembersView.qml's own projectAdminBridge/
    // projectId initial properties are exactly what ProjectListView.qml wires
    // into it, and SetMemberRole::role is the closed-set field morph#386 used
    // to force to a free-text field.
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    const qlonglong projectId = seedProject(*rig);
    REQUIRE(projectId > 0);

    kanban::gui::ProjectAdminBridge bridge{rig->bridge(0), rig->executor()};

    QQmlApplicationEngine engine;
    QObject* root = loadRoot(engine, "MembersView",
                             {{QStringLiteral("projectAdminBridge"), QVariant::fromValue(&bridge)},
                              {QStringLiteral("projectId"), projectId}});

    QObject* form = root->findChild<QObject*>(QStringLiteral("addMemberForm"));
    REQUIRE(form != nullptr);
    CHECK(form->property("actionType").toString() == QStringLiteral("SetMemberRole"));

    // `role` is a closed `oneOf`-of-`const`s (Role's glz::meta/glz::enumerate)
    // -- the renderer draws it as a ComboBox with the three named rows, not
    // a free-text field, which is exactly the gap morph#386 closed.
    QObject* roleControl = control(form, QStringLiteral("field_role"));
    REQUIRE(roleControl != nullptr);
    const QVariantList roleOptions = form->property("fields").toList();
    bool foundRoleField = false;
    for (const QVariant& entry : roleOptions) {
        const QVariantMap field = entry.toMap();
        if (field.value(QStringLiteral("name")).toString() != QStringLiteral("role")) continue;
        foundRoleField = true;
        const QVariantList enumOptions = field.value(QStringLiteral("enumOptions")).toList();
        REQUIRE(enumOptions.size() == 3);
        QStringList labels;
        for (const QVariant& option : enumOptions) {
            labels.append(option.toMap().value(QStringLiteral("label")).toString());
        }
        CHECK(labels.contains(QStringLiteral("Viewer")));
        CHECK(labels.contains(QStringLiteral("Member")));
        CHECK(labels.contains(QStringLiteral("Manager")));
    }
    CHECK(foundRoleField);

    // `projectId` is hidden context, engaged from the view -- not typed here.
    QObject* hiddenProject = control(form, QStringLiteral("column_projectId"));
    REQUIRE(hiddenProject != nullptr);
    CHECK_FALSE(hiddenProject->property("visible").toBool());

    // Unlike a boolean's seeded "false", an enum ComboBox starts at
    // currentIndex -1 -- "no selection" -- so the gate needs `role` engaged
    // too, not just `principal` (DynamicForm.qml's resetFields()/currentIndex
    // comments). Membership is decidable client-side once the schema states
    // the closed set (morph#386): an out-of-set value here would leave the
    // field's own JSON literal null and the gate unsatisfied, which is a
    // stronger property than the free-text field this form replaced ever had.
    CHECK_FALSE(isReady(form));
    type(form, QStringLiteral("principal"), QStringLiteral("bob"));
    CHECK_FALSE(isReady(form));
    type(form, QStringLiteral("role"), QStringLiteral(R"("Member")"));
    CHECK(isReady(form));
    CHECK(bodyOf(form) == QStringLiteral(R"({"projectId":%1,"principal":"bob","role":"Member"})").arg(projectId));

    bool roleSet = false;
    QObject::connect(&bridge, &kanban::gui::ProjectAdminBridge::memberRoleSet, [&roleSet] { roleSet = true; });
    pressSubmit(form);
    REQUIRE(pumpUntil([&roleSet] { return roleSet; }));

    bool listed = false;
    QVariantList rows;
    QObject::connect(&bridge, &kanban::gui::ProjectAdminBridge::rolesListed, [&](const QVariantList& roles) {
        rows = roles;
        listed = true;
    });
    bridge.listRoles(projectId);
    REQUIRE(pumpUntil([&listed] { return listed; }));
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().toMap().value(QStringLiteral("principal")).toString() == QStringLiteral("bob"));
    CHECK(rows.front().toMap().value(QStringLiteral("role")).toString() == QStringLiteral("Member"));

    // The view cleared and re-seeded the form on the reply, so a second add
    // starts with the hidden projectId already engaged and nothing else.
    CHECK_FALSE(isReady(form));
}

TEST_CASE("BoardView renders CreateColumn/CreateSwimlane/CreateTask through the shipped renderer and submits them",
          "[kanban][gui][qml-forms][issue344]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    const qlonglong projectId = seedProject(*rig);
    REQUIRE(projectId > 0);

    kanban::gui::BoardBridge bridge{rig->bridge(0), rig->executor()};
    bool opened = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::boardChanged, [&opened] { opened = true; });
    bridge.openBoard(QString::number(projectId));
    REQUIRE(pumpUntil([&opened] { return opened; }));
    bridge.stopPolling();  // Deterministic: nothing may re-enter while forms drive.

    QQmlApplicationEngine engine;
    QObject* root = loadRoot(engine, "BoardView", {{QStringLiteral("boardBridge"), QVariant::fromValue(&bridge)}});

    // ── CreateColumn ────────────────────────────────────────────────────────
    QObject* columnForm = root->findChild<QObject*>(QStringLiteral("createColumnForm"));
    REQUIRE(columnForm != nullptr);
    // Both members rendered, and the optional one is visibly optional: it is a
    // drawn control the gate does not wait for (`optionalFields{"wipLimit"}`).
    REQUIRE(control(columnForm, QStringLiteral("field_name")) != nullptr);
    REQUIRE(control(columnForm, QStringLiteral("field_wipLimit")) != nullptr);
    CHECK_FALSE(isReady(columnForm));
    type(columnForm, QStringLiteral("name"), QStringLiteral("To Do"));
    CHECK(isReady(columnForm));
    // wipLimit left empty: the key is absent from the body entirely, so the
    // model's own default (0 == unlimited) applies. This is the assertion that
    // the `optionalFields` opt-out is load-bearing rather than decorative.
    CHECK(bodyOf(columnForm) == QStringLiteral(R"({"name":"To Do"})"));

    bool boardChanged = false;
    auto watchBoard = [&] {
        boardChanged = false;
        return QObject::connect(&bridge, &kanban::gui::BoardBridge::boardChanged,
                                [&boardChanged] { boardChanged = true; });
    };
    auto connection = watchBoard();
    pressSubmit(columnForm);
    REQUIRE(pumpUntil([&boardChanged] { return boardChanged; }));
    QObject::disconnect(connection);

    const QVariantList columns = bridge.board().value(QStringLiteral("columns")).toList();
    REQUIRE(columns.size() == 1);
    CHECK(columns.front().toMap().value(QStringLiteral("name")).toString() == QStringLiteral("To Do"));
    CHECK(columns.front().toMap().value(QStringLiteral("wipLimit")).toLongLong() == 0);
    const QString columnId = columns.front().toMap().value(QStringLiteral("id")).toString();

    // ── CreateSwimlane ──────────────────────────────────────────────────────
    QObject* swimlaneForm = root->findChild<QObject*>(QStringLiteral("createSwimlaneForm"));
    REQUIRE(swimlaneForm != nullptr);
    type(swimlaneForm, QStringLiteral("name"), QStringLiteral("Default"));
    CHECK(isReady(swimlaneForm));
    connection = watchBoard();
    pressSubmit(swimlaneForm);
    REQUIRE(pumpUntil([&boardChanged] { return boardChanged; }));
    QObject::disconnect(connection);

    const QVariantList swimlanes = bridge.board().value(QStringLiteral("swimlanes")).toList();
    REQUIRE(swimlanes.size() == 1);
    const QString swimlaneId = swimlanes.front().toMap().value(QStringLiteral("id")).toString();

    // ── CreateTask, with its two hidden context fields ──────────────────────
    // The per-column form only exists once the board has a column to render a
    // delegate for, which the two submits above just created.
    QQuickItem* rootItem = qobject_cast<QQuickItem*>(root);
    REQUIRE(rootItem != nullptr);
    QObject* taskForm = nullptr;
    REQUIRE(pumpUntil([&] {
        taskForm = findItem(rootItem, QStringLiteral("createTaskForm_") + columnId);
        return taskForm != nullptr;
    }));

    // `columnId`/`swimlaneId` are declared `hidden` in CreateTask's
    // fieldMetadata, so the renderer draws their column but keeps it invisible.
    // This is what proves x-hidden reached the screen rather than only the
    // schema string.
    QObject* hiddenColumn = control(taskForm, QStringLiteral("column_columnId"));
    QObject* hiddenSwimlane = control(taskForm, QStringLiteral("column_swimlaneId"));
    QObject* titleColumn = control(taskForm, QStringLiteral("column_title"));
    REQUIRE(hiddenColumn != nullptr);
    REQUIRE(hiddenSwimlane != nullptr);
    REQUIRE(titleColumn != nullptr);
    CHECK_FALSE(hiddenColumn->property("visible").toBool());
    CHECK_FALSE(hiddenSwimlane->property("visible").toBool());
    CHECK(titleColumn->property("visible").toBool());

    // ...and the view engaged both of them from the delegate that owns the
    // form, which is the only way a hidden required field is ever satisfied.
    // Nothing is typed here but the title.
    type(taskForm, QStringLiteral("title"), QStringLiteral("Fix bug"));
    CHECK(isReady(taskForm));
    // Both ids ride out as JSON *numbers*, not quoted strings: the strong-id
    // `$ref` into `$defs` resolved to `{"type":["integer","null"]}` and the
    // renderer typed the field from it. A quoted id here is what the server
    // rejects with parse_number_failure (morph#189's shape).
    CHECK(bodyOf(taskForm) ==
          QStringLiteral(R"({"columnId":%1,"swimlaneId":%2,"title":"Fix bug"})").arg(columnId, swimlaneId));

    connection = watchBoard();
    pressSubmit(taskForm);
    REQUIRE(pumpUntil([&boardChanged] { return boardChanged; }));
    QObject::disconnect(connection);

    const QVariantList tasks = bridge.board().value(QStringLiteral("tasks")).toList();
    REQUIRE(tasks.size() == 1);
    const QVariantMap task = tasks.front().toMap();
    CHECK(task.value(QStringLiteral("title")).toString() == QStringLiteral("Fix bug"));
    CHECK(task.value(QStringLiteral("columnId")).toString() == columnId);
    CHECK(task.value(QStringLiteral("swimlaneId")).toString() == swimlaneId);
}

TEST_CASE("RulesView renders CreateRule through the shipped renderer and submits it",
          "[kanban][gui][qml-forms][issue344][issue393]") {
    // Loaded directly (not via BoardView's "Rules" popup button, which this
    // rule-6 file cannot click): RulesView.qml's own boardBridge initial
    // property is exactly what BoardView.qml wires into it.
    // `CreateRule::mutationType` is the closed-set field morph#386 used to
    // force to a free-text field; `triggerColumnId` is the Choice field this
    // rung's first server-fetched combo box (morph#393).
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    const qlonglong projectId = seedProject(*rig);
    REQUIRE(projectId > 0);

    kanban::gui::BoardBridge bridge{rig->bridge(0), rig->executor()};
    bool changed = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::boardChanged, [&changed] { changed = true; });
    bridge.openBoard(QString::number(projectId));
    REQUIRE(pumpUntil([&changed] { return changed; }));
    bridge.stopPolling();

    changed = false;
    bridge.createColumn(QStringLiteral("To Do"), 0);
    REQUIRE(pumpUntil([&changed] { return changed; }));
    const QString columnId =
        bridge.board().value(QStringLiteral("columns")).toList().front().toMap().value("id").toString();

    QQmlApplicationEngine engine;
    QObject* root = loadRoot(engine, "RulesView", {{QStringLiteral("boardBridge"), QVariant::fromValue(&bridge)}});

    QObject* form = root->findChild<QObject*>(QStringLiteral("createRuleForm"));
    REQUIRE(form != nullptr);
    CHECK(form->property("actionType").toString() == QStringLiteral("CreateRule"));

    // `mutationType` is a closed `oneOf`-of-`const`s (RuleMutationType's
    // glz::meta/glz::enumerate) -- a ComboBox, not the free-text field
    // morph#386 used to force.
    REQUIRE(control(form, QStringLiteral("field_mutationType")) != nullptr);

    // `triggerColumnId` is a Choice (`x-optionsAction: "GetBoardState"`), so
    // the renderer fetches its own options through the controller on load --
    // DynamicForm.qml's own Component.onCompleted, no explicit call from
    // this test -- rather than reading them off `board.columns` by hand. This
    // is this rung's first Choice field, so it is also the first thing to
    // exercise the fetchOptions/optionsReceived seam added for it.
    bool optionsFetched = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::optionsReceived,
                     [&](const QString& optionsAction, bool ok, const QString&) {
                         if (optionsAction == QStringLiteral("GetBoardState") && ok) optionsFetched = true;
                     });
    REQUIRE(pumpUntil([&optionsFetched] { return optionsFetched; }));

    QVariantMap triggerColumnField;
    for (const QVariant& entry : form->property("fields").toList()) {
        const QVariantMap field = entry.toMap();
        if (field.value(QStringLiteral("name")).toString() == QStringLiteral("triggerColumnId")) {
            triggerColumnField = field;
            break;
        }
    }
    CHECK(triggerColumnField.value(QStringLiteral("isChoice")).toBool());
    CHECK(triggerColumnField.value(QStringLiteral("optionsAction")).toString() == QStringLiteral("GetBoardState"));

    // `projectId` is hidden context, engaged from the view -- not typed here.
    QObject* hiddenProject = control(form, QStringLiteral("column_projectId"));
    REQUIRE(hiddenProject != nullptr);
    CHECK_FALSE(hiddenProject->property("visible").toBool());

    CHECK_FALSE(isReady(form));
    type(form, QStringLiteral("triggerColumnId"), columnId);
    type(form, QStringLiteral("mutationType"), QStringLiteral(R"("AddTag")"));
    type(form, QStringLiteral("mutationValue"), QStringLiteral("urgent"));
    CHECK(isReady(form));
    CHECK(bodyOf(form) == QStringLiteral(R"({"projectId":%1,"triggerColumnId":%2,"mutationType":"AddTag",)"
                                         R"("mutationValue":"urgent"})")
                              .arg(projectId)
                              .arg(columnId));

    bool ruleCreated = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::ruleCreated, [&ruleCreated] { ruleCreated = true; });
    pressSubmit(form);
    REQUIRE(pumpUntil([&ruleCreated] { return ruleCreated; }));

    bool listed = false;
    QVariantList rows;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::rulesListed, [&](const QVariantList& rules) {
        rows = rules;
        listed = true;
    });
    bridge.getRules();
    REQUIRE(pumpUntil([&listed] { return listed; }));
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().toMap().value(QStringLiteral("triggerColumnId")).toString() == columnId);
    CHECK(rows.front().toMap().value(QStringLiteral("mutationType")).toString() == QStringLiteral("AddTag"));
    CHECK(rows.front().toMap().value(QStringLiteral("mutationValue")).toString() == QStringLiteral("urgent"));

    // The view cleared and re-seeded the form on the reply, so a second add
    // starts with the hidden projectId already engaged and nothing else.
    CHECK_FALSE(isReady(form));
}

TEST_CASE("TaskDetailPopup renders AddComment through the shipped renderer and submits it",
          "[kanban][gui][qml-forms][issue344]") {
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    const qlonglong projectId = seedProject(*rig);
    REQUIRE(projectId > 0);

    kanban::gui::BoardBridge bridge{rig->bridge(0), rig->executor()};
    bool changed = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::boardChanged, [&changed] { changed = true; });
    bridge.openBoard(QString::number(projectId));
    REQUIRE(pumpUntil([&changed] { return changed; }));
    bridge.stopPolling();

    // Seeded through the typed path, not the forms path: this case is about
    // AddComment, and a task has to exist for it to comment on.
    changed = false;
    bridge.createColumn(QStringLiteral("To Do"), 0);
    REQUIRE(pumpUntil([&changed] { return changed; }));
    const QString columnId =
        bridge.board().value(QStringLiteral("columns")).toList().front().toMap().value("id").toString();
    changed = false;
    bridge.createSwimlane(QStringLiteral("Default"));
    REQUIRE(pumpUntil([&changed] { return changed; }));
    const QString swimlaneId =
        bridge.board().value(QStringLiteral("swimlanes")).toList().front().toMap().value("id").toString();
    changed = false;
    bridge.createTask(columnId, swimlaneId, QStringLiteral("Fix bug"));
    REQUIRE(pumpUntil([&changed] { return changed; }));
    const QString taskId =
        bridge.board().value(QStringLiteral("tasks")).toList().front().toMap().value("id").toString();

    QQmlApplicationEngine engine;
    QObject* root = loadRoot(engine, "BoardView", {{QStringLiteral("boardBridge"), QVariant::fromValue(&bridge)}});

    QObject* popup = root->findChild<QObject*>(QStringLiteral("taskDetailPopup"));
    REQUIRE(popup != nullptr);
    QObject* form = root->findChild<QObject*>(QStringLiteral("addCommentForm"));
    REQUIRE(form != nullptr);

    // `taskId` is hidden context, exactly as in CreateTask: the renderer draws
    // a column for it and keeps it out of the layout, while `body` is an
    // ordinary visible field.
    //
    // Asserted off the renderer's own parsed field model rather than off
    // `Item.visible`, and only here: `Item.visible` is *effective* visibility,
    // and everything inside a closed `Popup` reports `false` whatever its own
    // flag says. The visible/invisible pair is checked for real in the
    // CreateTask case above, whose form is in an open tree. Both forms reach
    // `hidden` through the same `x-hidden` path.
    QObject* hidden = control(form, QStringLiteral("column_taskId"));
    REQUIRE(hidden != nullptr);
    REQUIRE(control(form, QStringLiteral("field_body")) != nullptr);
    QVariantMap hiddenFlags;
    for (const QVariant& entry : form->property("fields").toList()) {
        const QVariantMap field = entry.toMap();
        hiddenFlags.insert(field.value(QStringLiteral("name")).toString(), field.value(QStringLiteral("hidden")));
    }
    CHECK(hiddenFlags.value(QStringLiteral("taskId")).toBool());
    CHECK_FALSE(hiddenFlags.value(QStringLiteral("body")).toBool());

    popup->setProperty("taskId", taskId);
    REQUIRE(QMetaObject::invokeMethod(popup, "bindCommentContext"));

    // The body alone completes the form; the task id came from the view.
    CHECK_FALSE(isReady(form));
    type(form, QStringLiteral("body"), QStringLiteral("looks good"));
    CHECK(isReady(form));
    CHECK(bodyOf(form) == QStringLiteral(R"({"taskId":%1,"body":"looks good"})").arg(taskId));

    bool commented = false;
    QObject::connect(&bridge, &kanban::gui::BoardBridge::commentAdded,
                     [&commented](const QString&) { commented = true; });
    pressSubmit(form);
    REQUIRE(pumpUntil([&commented] { return commented; }));

    const QVariantList comments = bridge.board().value(QStringLiteral("comments")).toList();
    REQUIRE(comments.size() == 1);
    CHECK(comments.front().toMap().value(QStringLiteral("body")).toString() == QStringLiteral("looks good"));
    CHECK(comments.front().toMap().value(QStringLiteral("taskId")).toString() == taskId);

    // The view cleared the submitted text and re-seeded the hidden task id, so
    // a second comment on the same task needs only a second body.
    CHECK_FALSE(isReady(form));
    type(form, QStringLiteral("body"), QStringLiteral("shipping it"));
    CHECK(isReady(form));
    CHECK(bodyOf(form) == QStringLiteral(R"({"taskId":%1,"body":"shipping it"})").arg(taskId));
}

TEST_CASE("A board form refuses an action its controller does not serve", "[kanban][gui][qml-forms][issue344]") {
    // The renderer names action types as strings, so a typo in a QML
    // `actionType:` -- or a form pointed at the wrong bridge -- must arrive
    // somewhere a human reads it rather than silently doing nothing. Both
    // controllers report; neither drops.
    DbFixture fixture;
    auto rig = makeAuthedRig("alice");
    kanban::gui::BoardBridge board{rig->bridge(0), rig->executor()};
    kanban::gui::ProjectAdminBridge admin{rig->bridge(0), rig->executor()};

    QString boardReply;
    bool boardOk = true;
    bool boardReplied = false;
    QObject::connect(&board, &kanban::gui::BoardBridge::replyReceived,
                     [&](const QString&, bool ok, const QString& payload) {
                         boardOk = ok;
                         boardReply = payload;
                         boardReplied = true;
                     });
    // `MoveTaskPosition` is a registered BoardModel action, and deliberately
    // still refused here: it is not one of the five forms this bridge renders,
    // and reaching a model action through the form seam just because the model
    // serves it is the hole the literal action list closes.
    board.submitIfValid(QStringLiteral("MoveTaskPosition"), QStringLiteral("{}"));
    REQUIRE(pumpUntil([&boardReplied] { return boardReplied; }));
    CHECK_FALSE(boardOk);
    CHECK(boardReply.contains(QStringLiteral("MoveTaskPosition")));

    QString adminReply;
    bool adminOk = true;
    bool adminReplied = false;
    QObject::connect(&admin, &kanban::gui::ProjectAdminBridge::replyReceived,
                     [&](const QString&, bool ok, const QString& payload) {
                         adminOk = ok;
                         adminReply = payload;
                         adminReplied = true;
                     });
    admin.submitIfValid(QStringLiteral("CreateColumn"), QStringLiteral("{}"));
    REQUIRE(pumpUntil([&adminReplied] { return adminReplied; }));
    CHECK_FALSE(adminOk);
    CHECK(adminReply.contains(QStringLiteral("CreateColumn")));
}

#endif  // MORPH_LADDER_QML_URI
