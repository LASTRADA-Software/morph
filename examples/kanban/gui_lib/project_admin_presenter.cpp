// SPDX-License-Identifier: Apache-2.0
#include "project_admin_presenter.hpp"

#include <morph/session/session.hpp>

#include <utility>

namespace kanban::gui {

ProjectAdminPresenter::ProjectAdminPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                                             QObject* parent)
    : Presenter{parent}, _bridge{bridge}, _authHandler{bridge, executor}, _projectHandler{bridge, executor} {
    trackBound(_projectHandler.whenBound());
}

void ProjectAdminPresenter::reportError(const std::exception_ptr& err) {
    try {
        std::rethrow_exception(err);
    } catch (const std::exception& ex) {
        emit failed(QString::fromStdString(ex.what()));
    }
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

void ProjectAdminPresenter::refreshProjects() {
    track<GetMyProjectsResult>(
        _projectHandler.execute(GetMyProjects{}),
        [this](GetMyProjectsResult result) { emit projectsListed(std::move(result)); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void ProjectAdminPresenter::createProject(const QString& name) {
    track<CreateProjectResult>(
        _projectHandler.execute(CreateProject{.name = name.toStdString()}),
        [this](CreateProjectResult result) { emit projectCreated(std::move(result)); },
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
