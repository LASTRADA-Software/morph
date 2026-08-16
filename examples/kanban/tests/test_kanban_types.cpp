// SPDX-License-Identifier: Apache-2.0
#include "kanban/core/types.hpp"
#include "kanban/core/errors.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("ProjectId default-constructs empty and engages via explicit int64_t", "[kanban][types]") {
    kanban::ProjectId empty;
    CHECK_FALSE(empty.hasValue());

    kanban::ProjectId engaged{42};
    REQUIRE(engaged.hasValue());
    CHECK(*engaged == 42);
}

TEST_CASE("ProjectId::fromOptional adopts the payload as-is", "[kanban][types]") {
    auto engaged = kanban::ProjectId::fromOptional(std::optional<std::int64_t>{7});
    REQUIRE(engaged.hasValue());
    CHECK(*engaged == 7);

    auto empty = kanban::ProjectId::fromOptional(std::nullopt);
    CHECK_FALSE(empty.hasValue());
}

TEST_CASE("ProjectId equality/ordering compares the payload", "[kanban][types]") {
    CHECK(kanban::ProjectId{1} == kanban::ProjectId{1});
    CHECK(kanban::ProjectId{1} != kanban::ProjectId{2});
    CHECK(kanban::ProjectId{} == kanban::ProjectId{});
}

TEST_CASE("Role round-trips through roleToString/roleFromString", "[kanban][types]") {
    CHECK(kanban::roleToString(kanban::Role::Viewer) == "Viewer");
    CHECK(kanban::roleToString(kanban::Role::Member) == "Member");
    CHECK(kanban::roleToString(kanban::Role::Manager) == "Manager");
    CHECK(kanban::roleFromString("Viewer") == kanban::Role::Viewer);
    CHECK(kanban::roleFromString("Manager") == kanban::Role::Manager);
}

TEST_CASE("Every kanban error derives from KanbanError and carries its message", "[kanban][types]") {
    try {
        throw kanban::ValidationError{"bad input"};
    } catch (const kanban::KanbanError& e) {
        CHECK(std::string{e.what()} == "bad input");
    }
    try {
        throw kanban::NotFound{"missing"};
    } catch (const kanban::KanbanError& e) {
        CHECK(std::string{e.what()} == "missing");
    }
    try {
        throw kanban::Forbidden{"no"};
    } catch (const kanban::KanbanError& e) {
        CHECK(std::string{e.what()} == "no");
    }
    try {
        throw kanban::Conflict{"busy"};
    } catch (const kanban::KanbanError& e) {
        CHECK(std::string{e.what()} == "busy");
    }
}
