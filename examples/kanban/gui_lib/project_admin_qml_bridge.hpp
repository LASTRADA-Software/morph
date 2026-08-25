// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

// Guarded exactly like project_admin_presenter.hpp's own includes: AUTOMOC
// runs moc over this header, and moc must not be pointed at morph's
// template-heavy bridge.hpp or at the model headers — see that header's own
// doc comment for the full rationale (mirrors
// bookmark_qml_bridges.hpp's identical guard).
#ifndef Q_MOC_RUN
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>

#include "project_admin_presenter.hpp"
#endif

namespace kanban::gui {

/// @brief QML-facing face of `kanban::gui::ProjectAdminPresenter`.
///
/// Turns the presenter's DTO-carrying signals into `QVariantMap`/
/// `QVariantList` property bags and its typed calls into `Q_INVOKABLE`s —
/// same shape as `bookmarks::gui::BookmarkBridge`/`TagBridge`: no decisions,
/// only translation (`examples/IMPLEMENTATION.md` rule 2). Login is folded
/// in here rather than given a class of its own, per this rung's GUI design
/// spec (`docs/superpowers/specs/2026-08-17-kanban-gui-design.md` §4.2):
/// `ProjectAdminPresenter` already owns both `AuthModel`'s and
/// `ProjectAdminModel`'s handlers, so this bridge is the one QML-facing
/// adapter over both.
class ProjectAdminBridge : public QObject {
    Q_OBJECT

    /// @brief The logged-in principal, or an empty string before `login`
    ///        succeeds.
    Q_PROPERTY(QString principal READ principal NOTIFY loggedIn)
    /// @brief The most recent `refreshProjects`/`createProject` result: every
    ///        project the caller belongs to, each a `{id, name, myRole}` map.
    Q_PROPERTY(QVariantList projects READ projects NOTIFY projectsListed)
    /// @brief The most recent `listRoles` result: every member's role on the
    ///        requested project, each a `{principal, role}` map.
    Q_PROPERTY(QVariantList roles READ roles NOTIFY rolesListed)

public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    ProjectAdminBridge(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    /// @brief The logged-in principal (see `principal` property).
    /// @return The principal, or an empty string before login.
    [[nodiscard]] QString principal() const { return _principal; }
    /// @brief The current project list (see `projects` property).
    /// @return The most recent listing's rows.
    [[nodiscard]] QVariantList projects() const { return _projects; }
    /// @brief The current role list (see `roles` property).
    /// @return The most recent role listing's rows.
    [[nodiscard]] QVariantList roles() const { return _roles; }

    /// @brief Mints a session token for `username` and installs it. Emits
    ///        `loggedIn`, or `failed`.
    /// @param username The identity to log in as.
    Q_INVOKABLE void login(const QString& username);

    /// @brief Fetches every project the caller belongs to. Emits
    ///        `projectsListed`, or `failed`.
    Q_INVOKABLE void refreshProjects();

    /// @brief Creates a project; the caller becomes its first Manager. Emits
    ///        `projectCreated`, or `failed`.
    /// @param name The new project's name.
    Q_INVOKABLE void createProject(const QString& name);

    /// @brief Lists every member's role on a project. Emits `rolesListed`,
    ///        or `failed`.
    /// @param projectId The project to list roles for, as its plain number.
    Q_INVOKABLE void listRoles(qlonglong projectId);

    /// @brief Sets (or changes) a member's role. Manager-only. Emits
    ///        `memberRoleSet`, or `failed`.
    /// @param projectId The project to act on, as its plain number.
    /// @param principal The member whose role to set.
    /// @param role      `"Viewer"`, `"Member"`, or `"Manager"`.
    Q_INVOKABLE void setMemberRole(qlonglong projectId, const QString& principal, const QString& role);

    /// @brief Removes a member's role entirely. Manager-only. Emits
    ///        `memberRemoved`, or `failed`.
    /// @param projectId The project to act on, as its plain number.
    /// @param principal The member to remove.
    Q_INVOKABLE void removeMember(qlonglong projectId, const QString& principal);

signals:
    /// @brief Emitted once the wrapped presenter's registration round trip
    ///        settles — successfully or not (`Presenter::bound()`,
    ///        `morph/core/bridge.hpp`'s `whenBound()`).
    void bound();
    /// @brief Emitted after a successful `login` — see `principal` property.
    /// @param principal The verified username the server echoed back.
    void loggedIn(const QString& principal);
    /// @brief A `refreshProjects` succeeded — see `projects` property.
    /// @param projects The listing's rows.
    void projectsListed(const QVariantList& projects);
    /// @brief A `createProject` succeeded.
    /// @param id   The new project's id, as its plain number.
    /// @param name The new project's name, echoed back.
    void projectCreated(qlonglong id, const QString& name);
    /// @brief A `listRoles` succeeded — see `roles` property.
    /// @param roles The listing's rows.
    void rolesListed(const QVariantList& roles);
    /// @brief A `setMemberRole` succeeded.
    void memberRoleSet();
    /// @brief A `removeMember` succeeded.
    void memberRemoved();
    /// @brief Any action's typed error, already rendered as a message.
    /// @param message The model's own `what()`.
    void failed(const QString& message);

private:
#ifndef Q_MOC_RUN
    ProjectAdminPresenter _presenter;
#endif
    QString _principal;
    QVariantList _projects;
    QVariantList _roles;
};

}  // namespace kanban::gui
