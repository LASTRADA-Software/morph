// SPDX-License-Identifier: Apache-2.0
#include "kanban/models/project_admin_model.hpp"
#include "testkit/db_fixture.hpp"

#include "kanban/auth/kanban_authorizer.hpp"

#include <morph/session/session.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using morph::ladder::testkit::DbFixture;

namespace {

/// @brief See `bookmarks::test_bookmark_model.cpp`'s identical
///        `contextFor`/`ScopedPrincipal` pair for why this is not a
///        designated initializer (`-Wmissing-designated-field-initializers`
///        under this target's strict warnings).
[[nodiscard]] morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

class ScopedPrincipal {
  public:
    explicit ScopedPrincipal(std::string principal) : _ctx{contextFor(std::move(principal))}, _scope{_ctx} {}

  private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};
}  // namespace

TEST_CASE("CreateProject makes the caller its first Manager", "[kanban][model]") {
    DbFixture fixture;
    kanban::ProjectAdminModel model;
    const ScopedPrincipal alice{"alice"};

    const auto result = model.execute(kanban::CreateProject{.name = "Sprint Board"});
    REQUIRE(result.id.hasValue());

    const auto roles = model.execute(kanban::GetProjectRoles{.projectId = result.id});
    REQUIRE(roles.roles.size() == 1);
    CHECK(roles.roles.front().principal == "alice");
    CHECK(roles.roles.front().role == kanban::Role::Manager);
}

TEST_CASE("SetMemberRole requires Manager; a Member cannot promote themselves", "[kanban][model]") {
    DbFixture fixture;
    kanban::ProjectAdminModel model;
    kanban::ProjectId projectId;
    {
        const ScopedPrincipal alice{"alice"};
        projectId = model.execute(kanban::CreateProject{.name = "Sprint Board"}).id;
        model.execute(kanban::SetMemberRole{.projectId = projectId, .principal = "bob", .role = kanban::Role::Member});
    }
    {
        const ScopedPrincipal bob{"bob"};
        CHECK_THROWS_AS(
            model.execute(
                kanban::SetMemberRole{.projectId = projectId, .principal = "bob", .role = kanban::Role::Manager}),
            kanban::Forbidden);
    }
}

TEST_CASE("RemoveMember deletes the role row; the removed principal can no longer be listed", "[kanban][model]") {
    DbFixture fixture;
    kanban::ProjectAdminModel model;
    const ScopedPrincipal alice{"alice"};
    const auto projectId = model.execute(kanban::CreateProject{.name = "Sprint Board"}).id;
    model.execute(kanban::SetMemberRole{.projectId = projectId, .principal = "bob", .role = kanban::Role::Member});
    model.execute(kanban::RemoveMember{.projectId = projectId, .principal = "bob"});

    const auto roles = model.execute(kanban::GetProjectRoles{.projectId = projectId});
    REQUIRE(roles.roles.size() == 1);
    CHECK(roles.roles.front().principal == "alice");
}

TEST_CASE("SetMemberRole rejects a principal over auth::kMaxPrincipalBytes rather than silently truncating it",
          "[kanban][model]") {
    // ProjectRoleRecord::principal is a SqlAnsiString<auth::kMaxPrincipalBytes>
    // column -- Light::SqlFixedString's constructor is noexcept and truncates
    // rather than throwing, so without this bound in validate(), a caller
    // could grant a role under an untruncated principal that gets silently
    // stored truncated. RemoveMember's lookup then queries by the full,
    // untruncated principal and never finds the (truncated) row -- the role
    // becomes un-removable through this action. This test proves the bound
    // itself; the un-removable consequence is exactly what it prevents.
    DbFixture fixture;
    kanban::ProjectAdminModel model;
    const ScopedPrincipal alice{"alice"};
    const auto projectId = model.execute(kanban::CreateProject{.name = "Sprint Board"}).id;

    const std::string overLong(kanban::auth::kMaxPrincipalBytes + 1, 'b');
    CHECK_THROWS_AS(
        model.execute(kanban::SetMemberRole{.projectId = projectId, .principal = overLong, .role = kanban::Role::Member}),
        kanban::ValidationError);

    const auto roles = model.execute(kanban::GetProjectRoles{.projectId = projectId});
    REQUIRE(roles.roles.size() == 1);
    CHECK(roles.roles.front().principal == "alice");
}

TEST_CASE("RemoveMember rejects a principal over auth::kMaxPrincipalBytes", "[kanban][model]") {
    DbFixture fixture;
    kanban::ProjectAdminModel model;
    const ScopedPrincipal alice{"alice"};
    const auto projectId = model.execute(kanban::CreateProject{.name = "Sprint Board"}).id;

    const std::string overLong(kanban::auth::kMaxPrincipalBytes + 1, 'b');
    CHECK_THROWS_AS(model.execute(kanban::RemoveMember{.projectId = projectId, .principal = overLong}),
                     kanban::ValidationError);
}

TEST_CASE("GetMyProjects lists every project the caller has a role on, with their own role",
          "[kanban][model]") {
    DbFixture fixture;
    kanban::ProjectAdminModel model;

    kanban::ProjectId p2;
    {
        const ScopedPrincipal alice{"alice"};
        // alice creates two projects (Manager on both); bob is added as
        // Viewer on the second only.
        model.execute(kanban::CreateProject{.name = "Alpha"});
        p2 = model.execute(kanban::CreateProject{.name = "Beta"}).id;
        model.execute(kanban::SetMemberRole{.projectId = p2, .principal = "bob", .role = kanban::Role::Viewer});
    }

    {
        const ScopedPrincipal alice{"alice"};
        const auto aliceProjects = model.execute(kanban::GetMyProjects{});
        REQUIRE(aliceProjects.projects.size() == 2);
        auto findByName = [&](const auto& projects, const std::string& name) {
            return std::ranges::find_if(projects, [&](const auto& p) { return p.name == name; });
        };
        const auto aliceAlpha = findByName(aliceProjects.projects, "Alpha");
        REQUIRE(aliceAlpha != aliceProjects.projects.end());
        CHECK(aliceAlpha->myRole == kanban::Role::Manager);
    }

    {
        const ScopedPrincipal bob{"bob"};
        const auto bobProjects = model.execute(kanban::GetMyProjects{});
        REQUIRE(bobProjects.projects.size() == 1);
        CHECK(bobProjects.projects.front().name == "Beta");
        CHECK(bobProjects.projects.front().myRole == kanban::Role::Viewer);
    }
}

TEST_CASE("GetMyProjects returns an empty list for a principal with no roles", "[kanban][model]") {
    DbFixture fixture;
    kanban::ProjectAdminModel model;
    const ScopedPrincipal carol{"carol"};
    const auto result = model.execute(kanban::GetMyProjects{});
    CHECK(result.projects.empty());
}
