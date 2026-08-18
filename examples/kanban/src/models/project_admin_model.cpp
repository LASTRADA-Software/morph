// SPDX-License-Identifier: Apache-2.0
#include "kanban/models/project_admin_model.hpp"

#include "kanban/auth/kanban_authorizer.hpp"
#include "kanban/db/kanban_entity.hpp"

#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/DataMapper/Pool.hpp>
#include <Lightweight/SqlTransaction.hpp>

#include <algorithm>
#include <cstdint>

namespace kanban {

static_assert(decltype(db::ProjectRoleRecord::principal)::ValueType{}.capacity() == auth::kMaxPrincipalBytes,
              "kanban::auth::kMaxPrincipalBytes must equal ProjectRoleRecord::principal's SqlAnsiString capacity -- "
              "otherwise SetMemberRole/RemoveMember either reject a principal that would have fit, or accept one "
              "that gets silently truncated on the way into the row (Light::SqlFixedString's constructor is "
              "noexcept and truncates rather than throwing), desyncing the stored row from the untruncated "
              "principal RemoveMember's lookup queries by.");

namespace {

[[nodiscard]] const std::string& requireOwner() {
    const auto* ctx = ::morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw Forbidden{"no authenticated principal"};
    }
    return ctx->principal;
}

/// @brief Loads the project named by @p projectId, or throws `NotFound`.
[[nodiscard]] db::ProjectRecord loadProject(::Lightweight::DataMapper& mapper, std::uint64_t projectId) {
    auto rows = mapper.Query<db::ProjectRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::ProjectRecord::id>, "=", projectId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"project not found"};
    }
    return std::move(rows.front());
}

/// @brief The caller's own role on @p projectId, or `std::nullopt` if they
///        have none.
[[nodiscard]] std::optional<Role> loadCallerRole(::Lightweight::DataMapper& mapper, std::uint64_t projectId,
                                                  const std::string& principal) {
    auto rows = mapper.Query<db::ProjectRoleRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::ProjectRoleRecord::project>, "=", projectId)
                    .Where(::Lightweight::FieldNameOf<&db::ProjectRoleRecord::principal>, "=", principal)
                    .All();
    if (rows.empty()) {
        return std::nullopt;
    }
    return roleFromString(rows.front().role.Value().str());
}

/// @brief Every role row for @p projectId and @p principal (normally at
///        most one, since `SetMemberRole` always deletes-then-recreates
///        rather than updating in place -- but this loads *every* matching
///        row regardless, so a delete-then-recreate cleanly self-heals if
///        more than one ever existed).
[[nodiscard]] std::vector<db::ProjectRoleRecord> loadRoleRows(::Lightweight::DataMapper& mapper,
                                                               std::uint64_t projectId,
                                                               const std::string& principal) {
    return mapper.Query<db::ProjectRoleRecord>()
        .Where(::Lightweight::FieldNameOf<&db::ProjectRoleRecord::project>, "=", projectId)
        .Where(::Lightweight::FieldNameOf<&db::ProjectRoleRecord::principal>, "=", principal)
        .All();
}

/// @brief Expiry stamped into every minted token -- mirrors
///        `bookmarks::AuthModel`'s identical constant and rationale (see that
///        model's `.cpp` file comment): far enough out to be irrelevant,
///        since this rung ships no session-renewal path either.
constexpr std::int64_t kTokenExpiresAtMs = 4102444800000;

}  // namespace

void ProjectAdminModel::requireRole(ProjectId projectId, Role minimum) const {
    if (!projectId.hasValue()) {
        throw NotFound{"projectId is required"};
    }
    const auto& owner = requireOwner();
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    (void) loadProject(mapper.Get(), static_cast<std::uint64_t>(*projectId));  // throws NotFound
    const auto role = loadCallerRole(mapper.Get(), static_cast<std::uint64_t>(*projectId), owner);
    if (!role.has_value() || static_cast<std::uint8_t>(*role) < static_cast<std::uint8_t>(minimum)) {
        throw Forbidden{"caller's role does not permit this action"};
    }
}

CreateProjectResult ProjectAdminModel::execute(const CreateProject& action) {
    if (!action.validate()) {
        throw ValidationError{"CreateProject: name is required and bounded"};
    }
    const auto& owner = requireOwner();

    db::ProjectRecord project;
    project.name = Light::SqlAnsiString<200>{action.name};

    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    ::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
    mapper->Create(project);

    db::ProjectRoleRecord role;
    role.project = project;
    role.principal = Light::SqlAnsiString<64>{owner};
    role.role = Light::SqlAnsiString<16>{std::string{roleToString(Role::Manager)}};
    mapper->Create(role);

    transaction.Commit();

    return CreateProjectResult{.id = ProjectId{static_cast<std::int64_t>(project.id.Value())}};
}

Ack ProjectAdminModel::execute(const SetMemberRole& action) {
    if (!action.validate()) {
        throw ValidationError{"SetMemberRole: projectId is required and principal must be a valid, bounded "
                               "principal"};
    }
    requireRole(action.projectId, Role::Manager);

    const auto projectId = static_cast<std::uint64_t>(*action.projectId);
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    ::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};

    auto existing = loadRoleRows(mapper.Get(), projectId, action.principal);
    for (auto& row : existing) {
        mapper->Delete(row);
    }

    db::ProjectRoleRecord role;
    role.project = loadProject(mapper.Get(), projectId);
    role.principal = Light::SqlAnsiString<64>{action.principal};
    role.role = Light::SqlAnsiString<16>{std::string{roleToString(action.role)}};
    mapper->Create(role);

    transaction.Commit();
    return Ack{};
}

Ack ProjectAdminModel::execute(const RemoveMember& action) {
    if (!action.validate()) {
        throw ValidationError{"RemoveMember: projectId is required and principal must be a valid, bounded "
                               "principal"};
    }
    requireRole(action.projectId, Role::Manager);

    const auto projectId = static_cast<std::uint64_t>(*action.projectId);
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    ::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};

    auto existing = loadRoleRows(mapper.Get(), projectId, action.principal);
    for (auto& row : existing) {
        mapper->Delete(row);
    }

    transaction.Commit();
    return Ack{};
}

GetProjectRolesResult ProjectAdminModel::execute(const GetProjectRoles& action) {
    if (!action.validate()) {
        throw ValidationError{"GetProjectRoles: projectId is required"};
    }
    requireRole(action.projectId, Role::Viewer);

    const auto projectId = static_cast<std::uint64_t>(*action.projectId);
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    auto rows = mapper->Query<db::ProjectRoleRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::ProjectRoleRecord::project>, "=", projectId)
                    .All();

    GetProjectRolesResult result;
    result.roles.reserve(rows.size());
    for (const auto& row : rows) {
        result.roles.push_back(
            MemberRole{.principal = std::string{row.principal.Value().str()}, .role = roleFromString(row.role.Value().str())});
    }
    return result;
}

GetMyProjectsResult ProjectAdminModel::execute(const GetMyProjects&) {
    const auto& principal = requireOwner();

    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    auto roleRows = mapper->Query<db::ProjectRoleRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::ProjectRoleRecord::principal>, "=", principal)
                        .All();

    GetMyProjectsResult result;
    result.projects.reserve(roleRows.size());
    for (const auto& roleRow : roleRows) {
        const auto projectId = roleRow.project.Value();
        auto projectRows = mapper->Query<db::ProjectRecord>()
                                .Where(::Lightweight::FieldNameOf<&db::ProjectRecord::id>, "=", projectId)
                                .All();
        if (projectRows.empty()) {
            continue;  // Project deleted underneath a stale role row; skip.
        }
        result.projects.push_back(MyProjectSummary{
            .id = ProjectId{static_cast<std::int64_t>(projectId)},
            .name = std::string{projectRows.front().name.Value().str()},
            .myRole = roleFromString(roleRow.role.Value().str()),
        });
    }
    std::ranges::sort(result.projects, {}, &MyProjectSummary::name);
    return result;
}

LoginResult AuthModel::execute(const Login& action) {
    if (!action.validate()) {
        throw ValidationError{"Login: username must be a valid principal"};
    }
    if (auth::isReservedPrincipal(action.username)) {
        // See isReservedPrincipal's doc comment: minting one of these on
        // request would hand any caller the internal worker's authority.
        throw ValidationError{"Login: the 'system:' principal namespace is reserved"};
    }
    auto issuer = auth::tokenIssuer();
    if (!issuer) {
        // No App has installed one -- e.g. a test that constructs AuthModel
        // directly, or a server bootstrap that forgot. A clear, typed
        // failure, not a null dereference.
        throw ValidationError{"Login: no token issuer installed"};
    }
    auto token = issuer->issue(::morph::session::SessionToken{
        .principal = action.username,
        // 0 disables TokenVerifier's not-before check, which this rung has
        // no use for: there is no scenario here where a token is minted
        // against a clock ahead of the verifier's, since the issuer and the
        // verifier are the same process.
        .issuedAtMs = 0,
        .expiresAtMs = kTokenExpiresAtMs,
        .roles = {},
    });
    return LoginResult{.token = AuthToken{std::move(token)}, .principal = action.username};
}

}  // namespace kanban
