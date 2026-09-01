// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>
#include <exception>

#include "gui/presenter.hpp"
#include "kanban/dto/auth_dto.hpp"
#include "kanban/dto/project_dto.hpp"

// See bookmarks::gui::BookmarkPresenter's identical guard and doc comment
// (examples/bookmarks/gui_lib/bookmark_presenter.hpp) for why moc must never
// see morph/core/bridge.hpp or this rung's model headers: moc is not a C++
// front end and mis-parses their template machinery.
#ifndef Q_MOC_RUN
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>

#include "kanban/models/project_admin_model.hpp"
#endif

namespace kanban::gui {

/// @brief Drives `kanban::AuthModel` and `kanban::ProjectAdminModel` for the
///        login and project-list/member-management views. Routes every
///        action through its own `BridgeHandler` and translates the typed
///        result into a Qt signal — no QML dependency, no domain logic
///        (`examples/IMPLEMENTATION.md` rule 2's "translates and routes; it
///        never decides").
///
/// One presenter for two models, not two: `Login` is the one action that
/// must run before every other action in this rung can succeed at all
/// (`AuthModel`'s own scope), and this rung's GUI design spec
/// (`docs/superpowers/specs/2026-08-17-kanban-gui-design.md` §4.2) keeps the
/// login step alongside the project-list/member-management surface it
/// unlocks rather than splitting it into a third presenter/bridge pair.
class ProjectAdminPresenter : public ::morph::ladder::gui::Presenter {
    Q_OBJECT
public:
    /// @param bridge   The shared `Bridge` `AppContext` owns.
    /// @param executor The executor `Completion` callbacks land on.
    /// @param parent   Optional `QObject` parent.
    ProjectAdminPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                          QObject* parent = nullptr);

    /// @brief Mints a session token for `username`. Emits `loggedIn` on
    ///        success (after installing the token as the bridge's default
    ///        session — see `onLoginSucceeded`'s own doc comment), `failed`
    ///        on error.
    /// @param username The identity to log in as.
    void login(const QString& username);

    /// @brief Dispatches @p bodyJson as @p actionType's body through the
    ///        type-erased `executeJson` path, for the schema-driven forms
    ///        `DynamicForm` renders.
    ///
    /// The schema renderer names action types as strings and hands back an
    /// assembled JSON body, so this is the entry point it needs — the typed
    /// `login()`/`createProject()` calls stay for callers that already hold
    /// typed arguments. A successful `Login` still installs its token here
    /// rather than in the QML bridge, so the one place this rung learns an
    /// identity is unchanged by which of the two paths dispatched it.
    ///
    /// Serves `Login` (`AuthModel`) and `CreateProject` (`ProjectAdminModel`);
    /// any other action type is reported back through `formReplyReceived` as
    /// unroutable rather than dropped.
    ///
    /// @param actionType Registered action type id, as the schema names it.
    /// @param bodyJson   Fully-assembled JSON body, as `DynamicForm` builds it.
    void submitForm(const QString& actionType, const QString& bodyJson);

    /// @brief Lists every project the caller has any role on. Emits
    ///        `projectsListed` on success, `failed` on error.
    void refreshProjects();

    /// @brief Creates a project; the caller becomes its first Manager.
    ///        Emits `projectCreated` on success, `failed` on error.
    /// @param name The new project's name.
    void createProject(const QString& name);

    /// @brief Lists every member's role on a project. Emits `rolesListed` on
    ///        success, `failed` on error.
    /// @param projectId The project to list roles for.
    void listRoles(ProjectId projectId);

    /// @brief Sets (or changes) a member's role. Manager-only. Emits
    ///        `memberRoleSet` on success, `failed` on error.
    /// @param projectId The project to act on.
    /// @param principal The member whose role to set.
    /// @param role      The role to grant.
    void setMemberRole(ProjectId projectId, const QString& principal, Role role);

    /// @brief Removes a member's role entirely. Manager-only. Emits
    ///        `memberRemoved` on success, `failed` on error.
    /// @param projectId The project to act on.
    /// @param principal The member to remove.
    void removeMember(ProjectId projectId, const QString& principal);

signals:
    /// @brief Emitted after a successful `Login` has been *applied* — i.e.
    ///        after the token is installed, so a slot may dispatch straight
    ///        away.
    /// @param principal The verified username the server echoed back.
    void loggedIn(QString principal);
    /// @brief `GetMyProjects` succeeded.
    /// @param result Every project the caller belongs to, with their own role.
    void projectsListed(kanban::GetMyProjectsResult result);
    /// @brief `CreateProject` succeeded.
    /// @param result The new project's id.
    /// @param name   The name this specific `createProject()` call was given
    ///        — carried alongside @p result (not read back from any shared
    ///        member) so two overlapping `createProject()` calls can never
    ///        cross-contaminate: `CreateProjectResult` itself only carries
    ///        the new id, so the name has to travel with its own call's
    ///        completion, captured in that call's own `track()` continuation
    ///        (see `createProject()`'s definition).
    void projectCreated(kanban::CreateProjectResult result, QString name);
    /// @brief `GetProjectRoles` succeeded.
    /// @param result Every member's role on the requested project.
    void rolesListed(kanban::GetProjectRolesResult result);
    /// @brief `SetMemberRole` succeeded.
    void memberRoleSet();
    /// @brief `RemoveMember` succeeded.
    void memberRemoved();
    /// @brief Emitted for any action's typed error — @p message is
    ///        `std::exception::what()`, ready for direct display.
    void failed(QString message);

    /// @brief Emitted once per `submitForm`, carrying that form's outcome.
    ///
    /// Separate from `failed` because a form's own view shows its own result
    /// inline, next to the fields that produced it, rather than through the
    /// shell's shared error surface.
    /// @param actionType The action the reply belongs to.
    /// @param ok         Whether the dispatch succeeded.
    /// @param payload    Result JSON on success, the error message otherwise.
    ///                   A successful `Login`'s token is redacted first — see
    ///                   `submitForm`'s definition.
    void formReplyReceived(QString actionType, bool ok, QString payload);

private:
    /// @brief Installs @p result's token as the shared `Bridge`'s default
    ///        session, so every subsequent action from every presenter
    ///        carries it, then announces the new identity.
    ///
    /// The whole of this client's authentication handling, and deliberately
    /// so: this is infrastructure wiring, not business logic
    /// (`examples/IMPLEMENTATION.md` rule 2's "(b) pure glue" clause). It
    /// decides nothing — the token is the server's, minted and signed by
    /// it, and `principal` is the server's echo of the identity it
    /// verified, not the client's claim. Mirrors
    /// `bookmarks::gui::FormsBridge::onLoginSucceeded` exactly.
    /// @param result The decoded `LoginResult` the server returned.
    void onLoginSucceeded(const LoginResult& result);

    /// @brief Shared error-display body passed as every `track()` call's
    ///        third argument — see `Presenter::track()`'s doc comment
    ///        (`examples/common/gui/presenter.hpp`).
    /// @param err The failed completion's exception.
    void reportError(const std::exception_ptr& err);

    ::morph::bridge::Bridge& _bridge;
    ::morph::bridge::BridgeHandler<AuthModel> _authHandler;
    ::morph::bridge::BridgeHandler<ProjectAdminModel> _projectHandler;
};

}  // namespace kanban::gui
