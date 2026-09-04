// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/forms/forms.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "kanban/core/types.hpp"

/// @file
/// `CreateRule`/`GetRules`/`DeleteRule` -- README build-order step 6's
/// event->condition->mutation automation rules (design spec §9). This file
/// is storage-and-DTO surface only: `kanban::db::RuleRecord` is where a rule
/// lives, and *evaluating* a rule (reading it back and acting on a trigger)
/// is a later task, not this one.
///
/// **Mutation scope, stated plainly**: `RuleMutationType` currently has
/// exactly two members, `AddTag`/`RemoveTag`. The README's own illustrative
/// example ("task moved to Done => assign to closer, add tag") needs an
/// "assign to closer" mutation kind that has no grounding anywhere in
/// kanban's schema/DTOs today (no "closer" concept exists) -- inventing one
/// ungrounded would be scope creep, so it is deliberately not supported.
/// `RuleMutationType` is still an open `enum class`, not a `bool
/// isAddNotRemove`-shaped hack, so a later, separately-justified mutation
/// kind is a one-line addition here rather than a reshape.
namespace kanban {

inline constexpr std::size_t kMaxRuleMutationValueBytes = 100;

/// @brief What kind of board event a rule watches for. Only one trigger
///        exists today -- a task moving into a particular column -- mirroring
///        `MoveTaskPosition`, the only mutating action this rung's rules
///        engine can observe.
enum class RuleTriggerEvent : std::uint8_t { TaskMovedToColumn };

/// @brief What a rule does when it fires. Scoped to tag add/remove for this
///        pass -- see this file's `@file` comment for why "assign to closer"
///        is out of scope.
enum class RuleMutationType : std::uint8_t { AddTag, RemoveTag };

/// @brief Renders @p event as its wire/storage string.
/// @param event Trigger event to render.
/// @return `"TaskMovedToColumn"`.
[[nodiscard]] constexpr std::string_view ruleTriggerEventToString(RuleTriggerEvent event) noexcept {
    switch (event) {
        case RuleTriggerEvent::TaskMovedToColumn:
            return "TaskMovedToColumn";
        default:
            // RuleTriggerEvent is a closed, 1-value uint8_t enum -- this arm
            // exists only to satisfy -Wswitch-default under -Weverything,
            // mirroring kanban::roleToString's identical accepted pattern.
            return "TaskMovedToColumn";
    }
}

/// @brief Parses @p text back into a `RuleTriggerEvent`.
/// @param text `"TaskMovedToColumn"`, or anything else.
/// @return The matching `RuleTriggerEvent`, or `RuleTriggerEvent::TaskMovedToColumn`
///         if @p text matches nothing (the only trigger this rung has, so it
///         is also the least-surprising fallback).
[[nodiscard]] constexpr RuleTriggerEvent ruleTriggerEventFromString(std::string_view text) noexcept {
    (void)text;
    return RuleTriggerEvent::TaskMovedToColumn;
}

/// @brief Renders @p type as its wire/storage string.
/// @param type Mutation type to render.
/// @return `"AddTag"` or `"RemoveTag"`.
[[nodiscard]] constexpr std::string_view ruleMutationTypeToString(RuleMutationType type) noexcept {
    switch (type) {
        case RuleMutationType::AddTag:
            return "AddTag";
        case RuleMutationType::RemoveTag:
            return "RemoveTag";
        default:
            // RuleMutationType is a closed, 2-value uint8_t enum -- every
            // value is handled above; this arm exists only to satisfy
            // -Wswitch-default under -Weverything (kanban::roleToString's
            // identical accepted pattern).
            return "AddTag";
    }
}

/// @brief Parses @p text back into a `RuleMutationType`.
/// @param text One of `"AddTag"`/`"RemoveTag"`.
/// @return The matching `RuleMutationType`, or `RuleMutationType::AddTag` if
///         @p text matches neither (mirrors `roleFromString`'s
///         least-surprising fallback convention).
[[nodiscard]] constexpr RuleMutationType ruleMutationTypeFromString(std::string_view text) noexcept {
    if (text == "RemoveTag") {
        return RuleMutationType::RemoveTag;
    }
    return RuleMutationType::AddTag;
}

/// @brief `forms::Choice` over a column id, backed by `GetBoardState`
///        (`board_dto.hpp`, whose `GetBoardResult` returns `columns` as its
///        first array member) -- the trigger-column picker
///        `examples/IMPLEMENTATION.md` rule 3 prescribes for a user-chosen
///        foreign key.
using TriggerColumnChoice = ::morph::forms::Choice<std::int64_t, "GetBoardState", "id", "name">;

/// @brief Creates an automation rule on `projectId`'s board: "when a task is
///        moved to `triggerColumnId`, apply `mutationType`/`mutationValue`."
///        `triggerColumnId` is this rung's only supported condition --
///        `RuleRecord`'s more general `conditionField`/`conditionValue`
///        storage shape is what the model maps this down to (`"columnId"` /
///        the column id, rendered as text), keeping the wire DTO concrete
///        and ergonomic while the entity stays general enough for a future
///        trigger/condition kind.
///
///        `projectId` must name the board the handler is attached to via
///        `OpenBoard` -- a `BoardModel` handler serves exactly one board, and
///        an argument naming another project is refused rather than silently
///        served from the attached one.
struct CreateRule {
    ProjectId projectId;
    TriggerColumnChoice triggerColumnId;
    RuleMutationType mutationType = RuleMutationType::AddTag;
    std::string mutationValue;

    /// `projectId` is **context, not input**: a rule is created on whichever
    /// board is open, and `gui/qml/RulesView.qml` supplies it from the
    /// attached `BoardBridge`. `hidden` says exactly that to every renderer --
    /// the field still travels in the payload, and `BoardModel::execute()`
    /// still re-checks it names the attached board
    /// (`requireProjectMatchesAttachedBoard`), so this is presentation and
    /// never a security control (`docs/spec/forms/forms.md`, "Field metadata
    /// is not a security control"). Same shape as `CreateTask::columnId`/
    /// `swimlaneId` (board_dto.hpp) and `SetMemberRole::projectId`
    /// (project_dto.hpp).
    static constexpr std::array<::morph::forms::FieldMeta, 1> fieldMetadata{
        ::morph::forms::FieldMeta{.field = "projectId", .hidden = true},
    };

    /// Side-effectful, so the renderer draws its own Submit button rather
    /// than firing the moment the draft is ready -- same declaration, for the
    /// same reason, as every other mutating board form (`CreateColumn`,
    /// `CreateSwimlane`, `CreateTask`, board_dto.hpp).
    static constexpr bool explicitSubmit = true;

    [[nodiscard]] bool validate() const noexcept {
        return projectId.hasValue() && triggerColumnId.value.has_value() && !mutationValue.empty() &&
               mutationValue.size() <= kMaxRuleMutationValueBytes;
    }
};

/// @brief What a successful `CreateRule` returns.
struct CreateRuleResult {
    RuleId ruleId;
};

/// @brief Lists every automation rule on `projectId`'s board. As with
///        `CreateRule`, `projectId` must be the board the handler is attached
///        to.
struct GetRules {
    ProjectId projectId;

    [[nodiscard]] bool validate() const noexcept { return projectId.hasValue(); }
};

/// @brief One rule, as returned by `GetRules`.
struct RuleView {
    RuleId id;
    ColumnId triggerColumnId;
    RuleMutationType mutationType = RuleMutationType::AddTag;
    std::string mutationValue;
};

/// @brief `GetRules`' result: every rule on the board.
struct GetRulesResult {
    std::vector<RuleView> rules;
};

/// @brief Deletes one automation rule.
struct DeleteRule {
    RuleId ruleId;

    [[nodiscard]] bool validate() const noexcept { return ruleId.hasValue(); }
};

/// @brief `BoardModel`'s own registered action for applying one rule's
///        `AddTag`/`RemoveTag` mutation to a task -- the cascade `evaluateRules`
///        fires (design spec §9). Not part of the rung's GUI-facing API
///        surface (no presenter/QML bridge ever constructs one directly);
///        it exists as a real, `BRIDGE_REGISTER_ACTION`-registered action
///        purely so its own `LogEntry` is independently replayable via
///        `morph::journal::replay()`'s `dispatcher.dispatch()` -- a cascade
///        entry's `actionType` must name a registered action or replay
///        throws "unknown action". Carries the same `Role::Member` gate as
///        every other task-mutating action (`AddComment`, `MoveTaskPosition`),
///        so a client that dispatches this directly (bypassing
///        `evaluateRules`) is bound by the same RBAC a rule's own cascade
///        already implies its triggering caller passed.
struct ApplyTagMutation {
    TaskId taskId;
    RuleMutationType mutationType = RuleMutationType::AddTag;
    std::string tag;

    [[nodiscard]] bool validate() const noexcept {
        return taskId.hasValue() && !tag.empty() && tag.size() <= kMaxRuleMutationValueBytes;
    }
};

/// @brief What a successful `ApplyTagMutation` returns -- an
///        acknowledgement, mirroring `kanban::Ack`'s "nothing else to
///        return" shape but defined locally so this file does not need to
///        pull in `project_dto.hpp` for one bare struct.
struct ApplyTagMutationResult {};

}  // namespace kanban

/// @brief On the wire a `RuleTriggerEvent` is its string name
///        (`ruleTriggerEventToString`) -- same convention as `glz::meta<kanban::Role>`.
template <>
struct glz::meta<kanban::RuleTriggerEvent> {
    using enum kanban::RuleTriggerEvent;
    static constexpr auto value = glz::enumerate(TaskMovedToColumn);
};

/// @brief On the wire a `RuleMutationType` is its string name
///        (`ruleMutationTypeToString`) -- same convention as `glz::meta<kanban::Role>`.
template <>
struct glz::meta<kanban::RuleMutationType> {
    using enum kanban::RuleMutationType;
    static constexpr auto value = glz::enumerate(AddTag, RemoveTag);
};
