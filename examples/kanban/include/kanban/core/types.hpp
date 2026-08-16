// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <optional>
#include <string_view>

/// @file
/// Kanban's strong id types and the `Role` enum. Every id wraps an
/// auto-incrementing SQLite row id -- `BookmarkId`'s shape
/// (`std::optional<std::int64_t>` + `hasValue()` + `operator*()` +
/// `fromOptional()` + `operator<=>`), not `polls::OptionId`'s zero-sentinel
/// shape, since every one of these ids is returned fresh from a `Create*`
/// action rather than always looked up already-assigned (design spec §7).

namespace kanban {

#define KANBAN_DEFINE_STRONG_ID(Name)                                                                  \
    struct Name {                                                                                      \
        std::optional<std::int64_t> value;                                                             \
        constexpr Name() noexcept = default;                                                            \
        explicit Name(std::int64_t id) noexcept : value{id} {}                                           \
        [[nodiscard]] static Name fromOptional(std::optional<std::int64_t> payload) noexcept {            \
            Name result;                                                                                    \
            result.value = payload;                                                                          \
            return result;                                                                                     \
        }                                                                                                       \
        [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }                               \
        /* NOLINTNEXTLINE(bugprone-unchecked-optional-access) */                                                  \
        [[nodiscard]] std::int64_t operator*() const noexcept { return *value; }                                   \
        [[nodiscard]] auto operator<=>(const Name&) const noexcept = default;                                       \
    }

/// @brief Strong id for a project (a `projects` table surrogate key).
KANBAN_DEFINE_STRONG_ID(ProjectId);
/// @brief Strong id for a column (a `board_columns` table surrogate key).
KANBAN_DEFINE_STRONG_ID(ColumnId);
/// @brief Strong id for a task (a `tasks` table surrogate key).
KANBAN_DEFINE_STRONG_ID(TaskId);
/// @brief Strong id for a swimlane (a `swimlanes` table surrogate key).
KANBAN_DEFINE_STRONG_ID(SwimlaneId);
/// @brief Strong id for a tag (a `tags` table surrogate key).
KANBAN_DEFINE_STRONG_ID(TagId);

#undef KANBAN_DEFINE_STRONG_ID

/// @brief A project member's permission level (design spec §3): `Viewer`
///        reads only, `Member` votes/moves/comments, `Manager` additionally
///        administers structure (columns, WIP limits, roles) via
///        `ProjectAdminModel` and gates `FinalizePoll`-shaped actions.
enum class Role : std::uint8_t { Viewer, Member, Manager };

/// @brief Renders @p role as its wire/storage string.
/// @param role Role to render.
/// @return `"Viewer"`, `"Member"`, or `"Manager"`.
[[nodiscard]] constexpr std::string_view roleToString(Role role) noexcept {
    switch (role) {
        case Role::Viewer:
            return "Viewer";
        case Role::Member:
            return "Member";
        case Role::Manager:
            return "Manager";
    }
    return "Viewer";
}

/// @brief Parses @p text back into a `Role`.
/// @param text One of `"Viewer"`/`"Member"`/`"Manager"`.
/// @return The matching `Role`, or `Role::Viewer` if @p text matches none
///         (the least-privileged fallback -- never silently grants more
///         than the caller asked for on a malformed/unknown value).
[[nodiscard]] constexpr Role roleFromString(std::string_view text) noexcept {
    if (text == "Manager") {
        return Role::Manager;
    }
    if (text == "Member") {
        return Role::Member;
    }
    return Role::Viewer;
}

}  // namespace kanban

/// @brief On the wire a `ProjectId` is its nullable underlying integer.
template <>
struct glz::meta<kanban::ProjectId> {
    static constexpr auto value = &kanban::ProjectId::value;
    static constexpr std::string_view name = "ProjectId";
};
/// @brief On the wire a `ColumnId` is its nullable underlying integer.
template <>
struct glz::meta<kanban::ColumnId> {
    static constexpr auto value = &kanban::ColumnId::value;
    static constexpr std::string_view name = "ColumnId";
};
/// @brief On the wire a `TaskId` is its nullable underlying integer.
template <>
struct glz::meta<kanban::TaskId> {
    static constexpr auto value = &kanban::TaskId::value;
    static constexpr std::string_view name = "TaskId";
};
/// @brief On the wire a `SwimlaneId` is its nullable underlying integer.
template <>
struct glz::meta<kanban::SwimlaneId> {
    static constexpr auto value = &kanban::SwimlaneId::value;
    static constexpr std::string_view name = "SwimlaneId";
};
/// @brief On the wire a `TagId` is its nullable underlying integer.
template <>
struct glz::meta<kanban::TagId> {
    static constexpr auto value = &kanban::TagId::value;
    static constexpr std::string_view name = "TagId";
};

/// @brief On the wire a `Role` is its string name (`roleToString`).
template <>
struct glz::meta<kanban::Role> {
    using enum kanban::Role;
    static constexpr auto value = glz::enumerate(Viewer, Member, Manager);
};
