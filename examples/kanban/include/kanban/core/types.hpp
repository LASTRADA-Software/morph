// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/core/payload_shape_tag.hpp>
#include <optional>
#include <string_view>

#include "errors.hpp"

/// @file
/// Kanban's strong id types and the `Role` enum. Every id wraps an
/// auto-incrementing SQLite row id -- `BookmarkId`'s shape
/// (`std::optional<std::int64_t>` + `hasValue()` + `operator*()` +
/// `fromOptional()` + `operator<=>`), not `polls::OptionId`'s zero-sentinel
/// shape, since every one of these ids is returned fresh from a `Create*`
/// action rather than always looked up already-assigned (design spec §7).

namespace kanban {

#define KANBAN_DEFINE_STRONG_ID(Name)                                                          \
    struct Name {                                                                              \
        std::optional<std::int64_t> value;                                                     \
        constexpr Name() noexcept = default;                                                   \
        explicit Name(std::int64_t id) noexcept : value{id} {}                                 \
        [[nodiscard]] static Name fromOptional(std::optional<std::int64_t> payload) noexcept { \
            Name result;                                                                       \
            result.value = payload;                                                            \
            return result;                                                                     \
        }                                                                                      \
        [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }             \
        /* NOLINTNEXTLINE(bugprone-unchecked-optional-access) */                               \
        [[nodiscard]] std::int64_t operator*() const noexcept { return *value; }               \
        [[nodiscard]] auto operator<=>(const Name&) const noexcept = default;                  \
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
/// @brief Strong id for an automation rule (a `rules` table surrogate key).
KANBAN_DEFINE_STRONG_ID(RuleId);
/// @brief Strong id for a task attachment (an `attachments` table surrogate
///        key).
KANBAN_DEFINE_STRONG_ID(AttachmentId);

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
        default:
            // Role is a closed, 3-value uint8_t enum -- every value is
            // handled above. This arm exists only to satisfy
            // -Wswitch-default under -Weverything (the switch is already
            // exhaustive; see examples/pastebin/units.hpp's identical
            // UnitTraits<Unit>::meta for the same accepted pattern).
            return "Viewer";
    }
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

/// @brief Strong identifier for one row in the `board_events` append-only
///        log. Zero-sentinel shape (not `fromOptional`'s optional shape) --
///        it is always looked up already-assigned, per `polls::PollEventId`'s
///        identical precedent.
///
/// The shape carries a constraint the type cannot enforce on its own: because
/// `value == 0` *is* the "not entered" state, **an event id of `0` is
/// unrepresentable** -- construct one and it reports `hasValue() == false`,
/// so a real event would read as "no event" (morph#215). The constraint holds
/// because these ids are SQLite row ids, which start at 1. `fromRowId()` is
/// the enforcement; use it for every conversion from a stored value.
struct BoardEventId {
    std::int64_t value{0};
    [[nodiscard]] constexpr bool hasValue() const { return value != 0; }
    [[nodiscard]] constexpr std::int64_t operator*() const { return value; }
    [[nodiscard]] constexpr bool operator==(const BoardEventId&) const = default;

    /// @brief Wraps a stored row id, rejecting the one value this type cannot
    ///        represent.
    ///
    /// Never fires in practice (row ids start at 1); it exists so a seeded
    /// row, a migrated dataset, or a sequence reset fails loudly at the
    /// boundary rather than collapsing into the empty state one layer below
    /// the surface, where no conversion helper can restore the distinction.
    /// @param rowId Stored row id; must be non-zero.
    /// @return An engaged `BoardEventId` wrapping @p rowId.
    /// @throws KanbanError if @p rowId is `0`.
    [[nodiscard]] static BoardEventId fromRowId(std::int64_t rowId) {
        if (rowId == 0) {
            throw KanbanError{
                "BoardEventId::fromRowId: an event row id of 0 is unrepresentable -- 0 is this "
                "type's \"not entered\" sentinel (morph#215)"};
        }
        return BoardEventId{.value = rowId};
    }
};

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
/// @brief On the wire a `RuleId` is its nullable underlying integer.
template <>
struct glz::meta<kanban::RuleId> {
    static constexpr auto value = &kanban::RuleId::value;
    static constexpr std::string_view name = "RuleId";
};
/// @brief On the wire an `AttachmentId` is its nullable underlying integer.
template <>
struct glz::meta<kanban::AttachmentId> {
    static constexpr auto value = &kanban::AttachmentId::value;
    static constexpr std::string_view name = "AttachmentId";
};

/// @brief On the wire a `BoardEventId` is its underlying integer.
template <>
struct glz::meta<kanban::BoardEventId> {
    static constexpr auto value = &kanban::BoardEventId::value;
    static constexpr std::string_view name = "BoardEventId";
};

/// @brief On the wire a `Role` is its string name (`roleToString`).
template <>
struct glz::meta<kanban::Role> {
    using enum kanban::Role;
    static constexpr auto value = glz::enumerate(Viewer, Member, Manager);
};

/// @brief Stable payload-shape tag for each strong id above, so a journal
///        fingerprint can tell one id from another.
///
/// The `glz::meta` specialisations above are what make these types
/// serialisable at all, and they are also what makes them *opaque* to
/// `morph::model::payloadShape`: a type whose meta names a value rather than an
/// object has no reflected members to decompose, so it renders as the bare `x`
/// and every id in a payload looks like every other one
/// (`morph/core/payload_shape_tag.hpp`; `docs/spec/journal/journal.md`,
/// "Custom-codec types name themselves").
///
/// This rung's centrepiece is made of ids. `MoveTaskPosition{TaskId, ColumnId,
/// SwimlaneId}` -- design spec §1's exactly-once action, and the one the
/// offline queue replays -- fingerprinted as three interchangeable `x`s, and
/// `CreateTask{ColumnId, SwimlaneId}` as two, so exchanging two of them --
/// what an id rename or a copy-paste between adjacent lines produces -- left
/// the fingerprint untouched and `journal::replay()`'s mismatch gate with
/// nothing to fire on, while the recorded integers decoded into the wrong
/// slots. Every one of these ids is `std::optional<std::int64_t>` on the wire,
/// so the JSON is byte-identical across such a swap and no decode, on any
/// path, can catch it: the tag is the only place it is visible at all.
///
/// The tag text is spelled here rather than derived from `glz::name_v`, which
/// is compiler-dependent -- a journal readable only by the build that wrote it
/// is the worse failure. It is part of the on-disk fingerprint of every entry
/// this rung records, so it is an interface: renaming a tag invalidates every
/// retained journal entry carrying that id, exactly as renaming a field does.
/// `kanban.`-namespaced so it cannot collide with another rung's tag.
///
/// One specialisation per id type, generated the same way the structs and
/// their `glz::meta`s are, then undefined immediately after -- the same shape
/// as `LEDGER_DEFINE_STRONG_ID_SHAPE_TAG`
/// (`examples/ledger/include/ledger/core/types.hpp`), which this mirrors.
#define KANBAN_DEFINE_STRONG_ID_SHAPE_TAG(Name, Tag)                      \
    template <>                                                           \
    struct morph::model::PayloadShapeTag<kanban::Name> {                  \
        /** @brief This id's stable shape name. @return The tag. */       \
        static constexpr std::string_view name() noexcept { return Tag; } \
    }

KANBAN_DEFINE_STRONG_ID_SHAPE_TAG(ProjectId, "kanban.projectId");
KANBAN_DEFINE_STRONG_ID_SHAPE_TAG(ColumnId, "kanban.columnId");
KANBAN_DEFINE_STRONG_ID_SHAPE_TAG(TaskId, "kanban.taskId");
KANBAN_DEFINE_STRONG_ID_SHAPE_TAG(SwimlaneId, "kanban.swimlaneId");
KANBAN_DEFINE_STRONG_ID_SHAPE_TAG(TagId, "kanban.tagId");
KANBAN_DEFINE_STRONG_ID_SHAPE_TAG(RuleId, "kanban.ruleId");
KANBAN_DEFINE_STRONG_ID_SHAPE_TAG(AttachmentId, "kanban.attachmentId");
KANBAN_DEFINE_STRONG_ID_SHAPE_TAG(BoardEventId, "kanban.boardEventId");

#undef KANBAN_DEFINE_STRONG_ID_SHAPE_TAG
