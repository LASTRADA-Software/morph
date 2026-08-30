// SPDX-License-Identifier: Apache-2.0
#include "project_admin_presenter.hpp"

#include <glaze/glaze.hpp>
#include <morph/session/session.hpp>
#include <string>
#include <utility>

#include "gui/error_text.hpp"

namespace kanban::gui {

ProjectAdminPresenter::ProjectAdminPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                                             QObject* parent)
    : Presenter{parent}, _bridge{bridge}, _authHandler{bridge, executor}, _projectHandler{bridge, executor} {
    trackBound(_projectHandler.whenBound());
}

void ProjectAdminPresenter::reportError(const std::exception_ptr& err) {
    emit failed(::morph::ladder::gui::errorText(err));
}

void ProjectAdminPresenter::onLoginSucceeded(const LoginResult& result) {
    ::morph::session::Context session;
    session.principal = result.principal;
    session.token = result.token.hasValue() ? *result.token : std::string{};
    _bridge.setDefaultSession(session);
    emit loggedIn(QString::fromStdString(result.principal));
}

void ProjectAdminPresenter::login(const QString& username) {
    track<LoginResult>(
        _authHandler.execute(Login{.username = username.toStdString()}),
        [this](LoginResult result) { onLoginSucceeded(result); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void ProjectAdminPresenter::submitForm(const QString& actionType, const QString& bodyJson) {
    // `executeJson` is the type-erased counterpart of the typed `execute`
    // calls below: the schema renderer only ever knows an action by the string
    // the schema names it with. Routing is a single handler today because
    // `Login` is the only schema-driven form in this rung — see
    // `kanban_schemas.hpp` and morph#344. When a second model's action joins
    // the document, this becomes the same actionType->handler table
    // `bookmarks::gui::BookmarkFormsController::dispatch` already has, and the
    // unroutable case must report rather than silently drop: QML names types
    // as strings, so a typo has to arrive somewhere a human reads it.
    const std::string type = actionType.toStdString();
    if (type != "Login") {
        emit formReplyReceived(actionType, false,
                               QStringLiteral("no model in this client serves action '") + actionType + u'\'');
        return;
    }
    track<std::string>(
        _authHandler.executeJson(type, bodyJson.toStdString()),
        [this, actionType](std::string resultJson) {
            // A successful Login is the one reply this client reads rather
            // than merely displays: the token has to be installed before
            // anything else dispatches.
            // The same glaze reflection the wire used, so nothing here parses
            // JSON by hand; `read_json` returns a truthy error context on
            // failure. Named the same way, for the same reason, as
            // `bookmarks::gui::decodeLoginResult`.
            LoginResult result;
            if (glz::read_json(result, resultJson)) {
                emit formReplyReceived(actionType, false,
                                       QStringLiteral("login succeeded but its reply could not be decoded"));
                return;
            }
            onLoginSucceeded(result);
            // The token has already done its one job -- installed onto the
            // session above -- so it has no reason to leave this function.
            // `formReplyReceived` reaches every bound QML handler, and one
            // that rendered `payload` unconditionally would otherwise put a
            // live bearer credential on screen (and into any screenshot of
            // it). Re-encoding a redacted copy keeps the signal's shape
            // unchanged rather than making this a QML surface change. Same
            // reasoning, and the same redaction, as
            // `bookmarks::gui::FormsBridge::submitIfValid`.
            LoginResult redacted = result;
            redacted.token = AuthToken{};
            emit formReplyReceived(actionType, true,
                                   QString::fromStdString(glz::write_json(redacted).value_or("{}")));
        },
        [this, actionType](const std::exception_ptr& err) {
            emit formReplyReceived(actionType, false, ::morph::ladder::gui::errorText(err));
        });
}

void ProjectAdminPresenter::refreshProjects() {
    track<GetMyProjectsResult>(
        _projectHandler.execute(GetMyProjects{}),
        [this](GetMyProjectsResult result) { emit projectsListed(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void ProjectAdminPresenter::createProject(const QString& name) {
    // `name` is captured by this call's own lambda, not stashed on any shared
    // member: two overlapping createProject() calls each get their own
    // track() continuation with their own captured `name`, so the id/name
    // pairing on projectCreated() can never cross between them regardless of
    // completion order (see the signal's own doc comment).
    track<CreateProjectResult>(
        _projectHandler.execute(CreateProject{.name = name.toStdString()}),
        [this, name](CreateProjectResult result) { emit projectCreated(std::move(result), name); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void ProjectAdminPresenter::listRoles(ProjectId projectId) {
    track<GetProjectRolesResult>(
        _projectHandler.execute(GetProjectRoles{.projectId = projectId}),
        [this](GetProjectRolesResult result) { emit rolesListed(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void ProjectAdminPresenter::setMemberRole(ProjectId projectId, const QString& principal, Role role) {
    track<Ack>(
        _projectHandler.execute(
            SetMemberRole{.projectId = projectId, .principal = principal.toStdString(), .role = role}),
        [this](Ack) { emit memberRoleSet(); }, [this](const std::exception_ptr& err) { reportError(err); });
}

void ProjectAdminPresenter::removeMember(ProjectId projectId, const QString& principal) {
    track<Ack>(
        _projectHandler.execute(RemoveMember{.projectId = projectId, .principal = principal.toStdString()}),
        [this](Ack) { emit memberRemoved(); }, [this](const std::exception_ptr& err) { reportError(err); });
}

}  // namespace kanban::gui
