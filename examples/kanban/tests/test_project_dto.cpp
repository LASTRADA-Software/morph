// SPDX-License-Identifier: Apache-2.0
#include "kanban/dto/project_dto.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("CreateProject requires a non-empty, bounded name", "[kanban][dto]") {
    CHECK_FALSE(kanban::CreateProject{.name = ""}.validate());
    CHECK_FALSE(kanban::CreateProject{.name = std::string(201, 'x')}.validate());
    CHECK(kanban::CreateProject{.name = "Sprint Board"}.validate());
}

TEST_CASE("SetMemberRole requires an engaged projectId and non-empty principal", "[kanban][dto]") {
    CHECK_FALSE(kanban::SetMemberRole{.projectId = {}, .principal = "alice", .role = kanban::Role::Member}.validate());
    CHECK_FALSE(
        kanban::SetMemberRole{.projectId = kanban::ProjectId{1}, .principal = "", .role = kanban::Role::Member}
            .validate());
    CHECK(kanban::SetMemberRole{.projectId = kanban::ProjectId{1}, .principal = "alice", .role = kanban::Role::Member}
              .validate());
}

TEST_CASE("RemoveMember requires an engaged projectId and non-empty principal", "[kanban][dto]") {
    CHECK_FALSE(kanban::RemoveMember{.projectId = {}, .principal = "alice"}.validate());
    CHECK(kanban::RemoveMember{.projectId = kanban::ProjectId{1}, .principal = "alice"}.validate());
}

TEST_CASE("GetProjectRoles requires an engaged projectId", "[kanban][dto]") {
    CHECK_FALSE(kanban::GetProjectRoles{.projectId = {}}.validate());
    CHECK(kanban::GetProjectRoles{.projectId = kanban::ProjectId{1}}.validate());
}
