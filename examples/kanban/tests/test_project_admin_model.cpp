// SPDX-License-Identifier: Apache-2.0
#include "kanban/models/project_admin_model.hpp"
#include "testkit/db_fixture.hpp"

#include "kanban/auth/kanban_authorizer.hpp"

#include <morph/session/session.hpp>

#include <catch2/catch_test_macros.hpp>

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
