// SPDX-License-Identifier: Apache-2.0
#include "project_admin_qml_bridge.hpp"
#include "gui/id_qml.hpp"

#include <QString>
#include <QVariant>

#include <utility>

namespace kanban::gui {

namespace {

// A `ProjectId` as the plain number QML rows and invokables carry, `kNoId`
// when unengaged.
using ::morph::ladder::gui::idNumber;

/// @brief `roleToString`'s output as a `QString` ("Viewer"/"Member"/"Manager").
[[nodiscard]] QString roleText(Role role) {
    const auto text = roleToString(role);
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// @brief One `MyProjectSummary` row as the property bag the project-list
///        view binds against.
[[nodiscard]] QVariantMap toVariantMap(const MyProjectSummary& summary) {
    return QVariantMap{
        {"id", idNumber(summary.id)},
        {"name", QString::fromStdString(summary.name)},
        {"myRole", roleText(summary.myRole)},
    };
}

/// @brief One `MemberRole` row as the property bag the members view binds
///        against.
[[nodiscard]] QVariantMap toVariantMap(const MemberRole& member) {
    return QVariantMap{
        {"principal", QString::fromStdString(member.principal)},
        {"role", roleText(member.role)},
    };
}

/// @brief Every row in @p rows as a `QVariantList` of property bags.
template <typename Rows>
[[nodiscard]] QVariantList toVariantList(const Rows& rows) {
    QVariantList out;
    out.reserve(static_cast<qsizetype>(rows.size()));
    for (const auto& row : rows) {
        out.append(toVariantMap(row));
    }
    return out;
}

}  // namespace

ProjectAdminBridge::ProjectAdminBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                                       QObject* parent)
    : QObject{parent}, _presenter{bridge, executor} {
    // Direct (same-thread) connections throughout — see
    // bookmark_qml_bridges.cpp's identical "Threading" note: no meta-type
    // registration is needed because every connection here is direct, not
    // queued.
    connect(&_presenter, &ProjectAdminPresenter::bound, this, &ProjectAdminBridge::bound);
    connect(&_presenter, &ProjectAdminPresenter::loggedIn, this, [this](QString principal) {
        _principal = std::move(principal);
        emit loggedIn(_principal);
    });
    connect(&_presenter, &ProjectAdminPresenter::projectsListed, this, [this](GetMyProjectsResult result) {
        _projects = toVariantList(result.projects);
        emit projectsListed(_projects);
    });
    connect(&_presenter, &ProjectAdminPresenter::projectCreated, this,
            [this](CreateProjectResult result, QString name) { emit projectCreated(idNumber(result.id), name); });
    connect(&_presenter, &ProjectAdminPresenter::rolesListed, this, [this](GetProjectRolesResult result) {
        _roles = toVariantList(result.roles);
        emit rolesListed(_roles);
    });
    connect(&_presenter, &ProjectAdminPresenter::memberRoleSet, this, &ProjectAdminBridge::memberRoleSet);
    connect(&_presenter, &ProjectAdminPresenter::memberRemoved, this, &ProjectAdminBridge::memberRemoved);
    connect(&_presenter, &ProjectAdminPresenter::failed, this, &ProjectAdminBridge::failed);
}

void ProjectAdminBridge::login(const QString& username) {
    _presenter.login(username);
}

void ProjectAdminBridge::refreshProjects() {
    _presenter.refreshProjects();
}

void ProjectAdminBridge::createProject(const QString& name) {
    _presenter.createProject(name);
}

void ProjectAdminBridge::listRoles(qlonglong projectId) {
    _presenter.listRoles(ProjectId{static_cast<std::int64_t>(projectId)});
}

void ProjectAdminBridge::setMemberRole(qlonglong projectId, const QString& principal, const QString& role) {
    _presenter.setMemberRole(ProjectId{static_cast<std::int64_t>(projectId)}, principal,
                             roleFromString(role.toStdString()));
}

void ProjectAdminBridge::removeMember(qlonglong projectId, const QString& principal) {
    _presenter.removeMember(ProjectId{static_cast<std::int64_t>(projectId)}, principal);
}

}  // namespace kanban::gui
