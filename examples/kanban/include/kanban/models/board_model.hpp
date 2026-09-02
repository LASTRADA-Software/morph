// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <optional>
#include <string>

#include "kanban/core/errors.hpp"
#include "kanban/core/types.hpp"
#include "kanban/dto/activity_dto.hpp"
#include "kanban/dto/attachment_dto.hpp"
#include "kanban/dto/board_dto.hpp"
#include "kanban/dto/event_dto.hpp"
#include "kanban/dto/project_dto.hpp"
#include "kanban/dto/rule_dto.hpp"

/// @file
/// `BoardModel` -- this rung's shared/keyed board model (design spec §2).
/// Holds no database state itself: each `execute()` acquires a
/// `Lightweight::GlobalDataMapperPool()` connection for its own duration,
/// exactly like `bookmarks::BookmarkModel`/`polls::PollModel`.

namespace kanban {

class BoardModel {
public:
    /// @brief Attaches this handler to `action.projectId`'s board -- the
    ///        keyed attach action.
    /// @param action The project to attach to; `action.projectId` becomes
    ///        this handler's cached attach state on success.
    /// @return The freshly attached board's full state.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (an unset `projectId`).
    /// @throws NotFound if `action.projectId` names no project.
    /// @throws Forbidden if the caller has no role (at least `Role::Viewer`)
    ///         on `action.projectId` -- checked against the *target* project,
    ///         not this handler's (not-yet-set) attach state, so an
    ///         authenticated principal with no standing on the project
    ///         cannot read it merely by attaching.
    GetBoardResult execute(const OpenBoard& action);

    /// @brief Returns the current state of this handler's attached board.
    /// @param action Unused -- carries no fields.
    /// @return The attached board's full state.
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if the attached project no longer exists.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Viewer` (i.e. the caller has no role at all).
    GetBoardResult execute(const GetBoardState& action);

    /// @brief Creates a new column on this handler's attached board.
    /// @param action The column's name and WIP limit (`0` = unlimited).
    /// @return The board's full state after the column is created.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (an empty or over-length name).
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if the attached project no longer exists.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Member`.
    GetBoardResult execute(const CreateColumn& action);

    /// @brief Creates a new swimlane on this handler's attached board.
    /// @param action The swimlane's name.
    /// @return The board's full state after the swimlane is created.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (an empty or over-length name).
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if the attached project no longer exists.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Member`.
    GetBoardResult execute(const CreateSwimlane& action);

    /// @brief Creates a new task in the given column/swimlane on this
    ///        handler's attached board.
    /// @param action The task's target column id, target swimlane id, and
    ///        title.
    /// @return The board's full state after the task is created.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (an unset `columnId`/`swimlaneId`, or an empty or
    ///         over-length title).
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if the attached project no longer exists, or if
    ///         `action.columnId`/`action.swimlaneId` does not belong to the
    ///         attached project.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Member`.
    GetBoardResult execute(const CreateTask& action);

    /// @brief Appends a comment to the given task.
    /// @param action The target task id and comment body.
    /// @return The board's full state after the comment is appended.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (an unset `taskId` or an empty body).
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if the attached project no longer exists, or if
    ///         `action.taskId` does not belong to the attached project.
    /// @throws Forbidden if no principal is authenticated on the calling
    ///         session, or the caller's role on the attached project is
    ///         below `Role::Member`.
    GetBoardResult execute(const AddComment& action);

    /// @brief Design spec §1's exactly-once centerpiece -- added in Task 10.
    /// @param action The task's target position (column, swimlane,
    ///        position) and optional idempotency key.
    /// @return The board's full state after the move (or, for a
    ///         previously-applied `opId`, the replayed result).
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Member`. Checked before the idempotency-ledger
    ///         lookup, so a demoted caller replaying a known `opId` cannot
    ///         retrieve a stored result their current role could no longer
    ///         produce.
    /// @throws NotFound if `action.taskId` does not belong to the attached
    ///         project, or if `action.columnId`/`action.swimlaneId` does not.
    GetBoardResult execute(const MoveTaskPosition& action);

    /// @brief Design spec §1's polling read side -- lists every
    ///        `board_events` row after `action.lastEventId`, oldest first.
    /// @param action The cursor to list events after; `{}` (its default)
    ///        means "from the beginning".
    /// @return Every matching event, oldest first.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (a negative `lastEventId`).
    /// @throws NotFound if this handler was never attached via `OpenBoard`.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Viewer` (i.e. the caller has no role at all).
    GetEventsSinceResult execute(const GetEventsSince& action);

    /// @brief Design spec §4's activity stream -- derived from `IActionLog::
    ///        entries(entityKey)`, not a parallel table.
    /// @param action Unused -- carries no fields.
    /// @return Every activity entry for this handler's attached board,
    ///         oldest first. Empty (not an error) if this handler has no log
    ///         attached.
    /// @throws NotFound if this handler was never attached via `OpenBoard`.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Viewer` (i.e. the caller has no role at all).
    GetActivityResult execute(const GetActivity& action);

    /// @brief Creates a new automation rule on this handler's attached board
    ///        (design spec §9, README build-order step 6): "when a task
    ///        moves into `action.triggerColumnId`, apply
    ///        `action.mutationType`/`action.mutationValue`."
    ///        `action.triggerColumnId` is mapped down to `RuleRecord`'s
    ///        general `conditionField = "columnId"` / `conditionValue`
    ///        storage shape (Task 13's deliberately-left-undone mapping).
    /// @param action The rule's trigger column, mutation type, and mutation
    ///        value (a tag name).
    /// @return The rule's freshly assigned id.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (an unset `projectId`/`triggerColumnId`, or an empty or
    ///         over-length `mutationValue`).
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if `action.projectId` names a project other than the
    ///         attached one (the rule is created on the attached board, so an
    ///         argument naming a different project is refused rather than
    ///         written onto this one), or if the attached project no longer
    ///         exists, or if `action.triggerColumnId` does not belong to the
    ///         attached project.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Manager` -- rule creation is a structural,
    ///         board-policy change, the same gate `ProjectAdminModel` applies
    ///         to column/role administration, not `Role::Member`'s
    ///         day-to-day-use bar.
    CreateRuleResult execute(const CreateRule& action);

    /// @brief Lists every automation rule on this handler's attached board.
    /// @param action Carries no fields beyond `projectId`, which must name
    ///        the attached board: the handler's own attach state selects the
    ///        board, and the argument is checked against it.
    /// @return Every rule on the attached board, in creation order.
    /// @throws ValidationError if `action.validate()` rejects the request (an
    ///         unset `projectId`).
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if `action.projectId` names a project other than the
    ///         attached one, or if the attached project no longer exists.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Viewer` (i.e. the caller has no role at all).
    GetRulesResult execute(const GetRules& action);

    /// @brief Deletes one automation rule.
    /// @param action The rule id to delete.
    /// @return An acknowledgement carrying no data.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (an unset `ruleId`).
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if the attached project no longer exists, or if
    ///         `action.ruleId` does not belong to the attached project.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Manager` (same gate as `CreateRule`).
    Ack execute(const DeleteRule& action);

    /// @brief Applies one rule's `AddTag`/`RemoveTag` mutation to a task --
    ///        `evaluateRules`' own cascade action (see `rule_dto.hpp`'s
    ///        `ApplyTagMutation` doc comment for why this is a real,
    ///        registered action rather than a bare private helper: its
    ///        `LogEntry` must independently replay via `dispatcher.dispatch`).
    /// @param action The target task, mutation kind, and tag name.
    /// @return An acknowledgement carrying no data.
    /// @throws ValidationError if `action.validate()` rejects the request.
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if the attached project no longer exists, or if
    ///         `action.taskId` does not belong to the attached project.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Member` (same gate as `AddComment`/
    ///         `MoveTaskPosition`).
    ApplyTagMutationResult execute(const ApplyTagMutation& action);

    /// @brief Records an attachment's metadata against a task -- README
    ///        build-order step 8. Called **after** a separate HTTP side
    ///        channel (a later task) has already uploaded the file's bytes
    ///        and returned `action.storageKey`; this call never touches
    ///        bytes, only the metadata row.
    /// @param action The target task id, filename, content type, size, and
    ///        the side channel's opaque `storageKey`.
    /// @return An acknowledgement carrying no data.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (an unset `taskId`, an empty `filename`/`contentType`/
    ///         `storageKey`, or a negative `sizeBytes`).
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if the attached project no longer exists, or if
    ///         `action.taskId` does not belong to the attached project.
    /// @throws Forbidden if no principal is authenticated, or the caller's
    ///         role on the attached project is below `Role::Member` -- same
    ///         gate as `AddComment`, the more directly analogous precedent
    ///         (task-content, not board administration).
    Ack execute(const AddAttachment& action);

    /// @brief Lists every attachment recorded against a task.
    /// @param action The task id to list attachments for.
    /// @return Every attachment on `action.taskId`, in upload order.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (an unset `taskId`).
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if the attached project no longer exists, or if
    ///         `action.taskId` does not belong to the attached project.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Viewer` (i.e. the caller has no role at all) --
    ///         same read bar as `GetBoardState`/`GetRules`.
    GetAttachmentsResult execute(const GetAttachments& action);

    /// @brief Deletes one attachment's metadata row. Does not delete the
    ///        underlying bytes the side channel stored (out of scope --
    ///        see `attachment_dto.hpp`'s `RemoveAttachment` doc comment).
    /// @param action The attachment id to delete.
    /// @return An acknowledgement carrying no data.
    /// @throws ValidationError if `action.validate()` rejects the request
    ///         (an unset `attachmentId`).
    /// @throws NotFound if this handler was never attached via `OpenBoard`,
    ///         or if the attached project no longer exists, or if
    ///         `action.attachmentId` does not name an attachment belonging
    ///         to a task on the attached project.
    /// @throws Forbidden if the caller's role on the attached project is
    ///         below `Role::Member` -- same gate as `AddAttachment`.
    Ack execute(const RemoveAttachment& action);

    /// @brief Attaches a durable action log and this instance's stable
    ///        identity, so every subsequent mutating `execute()` records a
    ///        `morph::journal::LogEntry` that `execute(GetActivity)` can
    ///        later read back.
    ///
    /// This is a **model-level** mirror of `morph::model::detail::
    /// IModelHolder::attachActionLog` -- not a call into that framework
    /// method. `IModelHolder::attachActionLog`/`recordIfAttached` live on the
    /// type-erased holder that wraps a *registry-constructed* model (created
    /// via `ModelFactory::create<Model>()` and dispatched through
    /// `ActionDispatcher`/`Bridge::executeVia`); a `BoardModel` a unit test
    /// (or any caller) constructs directly with `kanban::BoardModel model;`
    /// has no such holder wrapping it; `model.execute(action)` calls
    /// `BoardModel::execute` straight, never touching `IModelHolder` or the
    /// dispatcher's runner, so `recordIfAttached`'s auto-append never fires
    /// for this path. `BoardModel` therefore keeps its own
    /// `shared_ptr<IActionLog>` and appends its own `LogEntry` at the end of
    /// every successful mutating `execute()` (see `logAction` below) --
    /// functionally the same effect `recordIfAttached` gives a
    /// holder-wrapped instance, achieved without one.
    /// @param log Sink entries are forwarded to.
    /// @param entityKey Stable identity stamped onto every `LogEntry` this
    ///        instance produces (this rung's project id, as a string), and --
    ///        because it names a project -- also adopted as the board this
    ///        handler is attached to. **An empty key is ignored for that
    ///        second purpose**: it identifies no project, so it neither
    ///        attaches an unattached handler nor un-attaches an open one.
    ///        `ModelFactory::create` passes exactly that when it hands the
    ///        process-wide default log to a newly constructed holder, and
    ///        storing it used to leave `_projectIdStr` engaged-but-empty --
    ///        which made every attach guard in this class pass and every
    ///        action answer `stoull` (#368). The log itself is attached
    ///        either way.
    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey);

private:
    /// @brief Records @p action/@p result as a `LogEntry` if a log is
    ///        attached; no-op otherwise. Called at the end of every
    ///        successful mutating `execute()` -- the model-level equivalent
    ///        of `IModelHolder::recordIfAttached` for a plain, non-holder-
    ///        wrapped `BoardModel` instance (see `attachActionLog`'s doc
    ///        comment for why this instance cannot rely on the framework's
    ///        own auto-append instead). Flushes `_log` after appending, so a
    ///        `GetActivity` call immediately afterward (the common case: a
    ///        client polls right after its own mutating call) reliably sees
    ///        the entry even when `_log` is a `FileActionLog` -- `append()`
    ///        writes through buffered C stdio with no implicit flush, and
    ///        `entries()` reads through a separate `ifstream` that cannot
    ///        see unflushed bytes still sitting in that buffer.
    /// @tparam Action Concrete action type; used to look up
    ///         `morph::model::ActionTraits<Action>::typeId()`/`toJson()`.
    /// @tparam Result Concrete result type; used to look up
    ///         `morph::model::ActionTraits<Action>::resultToJson()`.
    /// @param action The executed action, for its type-id and JSON payload.
    /// @param result The action's result, for its JSON encoding.
    /// @param causalParentId Design spec §9's cascade-journaling field --
    ///        empty (the default) for every ordinary, non-cascaded call site
    ///        this file already had before Task 14; `evaluateRules` is the
    ///        only caller that passes a non-empty value, set to the
    ///        triggering `MoveTaskPosition` entry's own stable identity.
    template <typename Action, typename Result>
    void logAction(const Action& action, const Result& result, std::string causalParentId = {}) const;

    /// @brief Records a rejected @p action as a `LogEntry` with
    ///        `Outcome::Failed` and @p error, if a log is attached; no-op
    ///        otherwise. The refused-attempt counterpart to `logAction`
    ///        above: every mutating `execute()` overload catches its own
    ///        `KanbanError` hierarchy around the whole body and calls this
    ///        before rethrowing, so a `Forbidden`, `NotFound`,
    ///        `ValidationError`, or `Conflict` refusal leaves the same
    ///        audit trace a success does -- see
    ///        `lims::SelfJournal::recordFailure` for the identical rationale
    ///        this mirrors (`include/lims/core/self_journal.hpp`).
    /// @tparam Action Concrete action type.
    /// @param action The rejected action.
    /// @param error The rejecting exception's `what()`.
    template <typename Action>
    void logFailure(const Action& action, const std::string& error) const;

    /// @brief Throws `Forbidden` unless the calling principal's role on
    ///        this handler's attached project is at least `minimum`. Same
    ///        shape as `ProjectAdminModel::requireRole` -- not shared code
    ///        (design spec §3): `BoardModel` and `ProjectAdminModel` are
    ///        separate classes with separate mapper/entity access, so each
    ///        gets its own copy. Delegates to `requireRoleOn` using this
    ///        handler's own `_projectIdStr` as the target project -- every
    ///        call site attached via `OpenBoard` keeps working unchanged.
    /// @param minimum The minimum role the caller must hold.
    /// @throws Forbidden if no principal is authenticated, or the caller
    ///         has no role on the attached project, or a role below
    ///         `minimum`.
    void requireRole(Role minimum) const;

    /// @brief Throws `Forbidden` unless the calling principal's role on
    ///        @p projectDbId is at least `minimum`. The explicit-project
    ///        variant `requireRole(Role)` cannot use: `execute(OpenBoard)`
    ///        must gate access to the project it is *attaching to*, before
    ///        `_projectIdStr` (which `OpenBoard` itself sets) is available
    ///        to read.
    /// @param projectDbId The project to check the caller's role against.
    /// @param minimum The minimum role the caller must hold.
    /// @throws Forbidden if no principal is authenticated, or the caller
    ///         has no role on @p projectDbId, or a role below `minimum`.
    void requireRoleOn(std::uint64_t projectDbId, Role minimum) const;

    /// @brief Design spec §9's automation-rules cascade: fires every rule on
    ///        the attached project whose `triggerColumnId` equals
    ///        @p newColumn, applying each one's `AddTag`/`RemoveTag`
    ///        mutation to @p movedTask.
    ///
    /// Called at the end of `execute(const MoveTaskPosition&)`'s successful
    /// body, after the move's own transaction has committed. Returns
    /// immediately, evaluating no rules, if `morph::journal::isReplaying()`
    /// -- Phase 5's Option A decision (design spec §9): a replayed
    /// `MoveTaskPosition` entry must not re-fire a rule whose own cascade
    /// entry is *also* being replayed from its own recorded `LogEntry`,
    /// which would double-apply the mutation.
    ///
    /// Each fired mutation is journaled as its own `LogEntry` (via
    /// `logAction`-shaped hand construction, since a rule's cascade is not
    /// itself one of `BoardModel`'s registered wire actions) with
    /// `causalParentId` set to @p triggerCausalId -- the triggering move's
    /// own opaque, `LogEntry::seq`-independent identity (design spec §9;
    /// `docs/spec/journal/journal.md`'s Invariants section).
    /// @param movedTask The task that was just moved.
    /// @param newColumn The column it was moved into -- matched against
    ///        every rule's `triggerColumnId`.
    /// @param triggerCausalId The triggering `MoveTaskPosition` entry's own
    ///        stable identity, reused verbatim as every fired cascade
    ///        entry's `causalParentId`.
    void evaluateRules(TaskId movedTask, ColumnId newColumn, const std::string& triggerCausalId);

    /// @brief The actual add/remove-tag database work behind `ApplyTagMutation`
    ///        -- factored out of `execute(const ApplyTagMutation&)` so
    ///        `evaluateRules` can perform the same mutation and journal it
    ///        itself (with `causalParentId` set) without going through
    ///        `execute()`'s own unconditional `logAction` call, which would
    ///        otherwise record the same fired mutation as two separate
    ///        `LogEntry` rows -- one causal-linked, one not -- and `morph::
    ///        journal::replay()` would then dispatch both, double-applying
    ///        the tag on replay. Performs no RBAC check and no `validate()`
    ///        call of its own: both call sites (`execute(ApplyTagMutation)`,
    ///        already role-gated, and `evaluateRules`, itself only reachable
    ///        from `execute(MoveTaskPosition)`'s own `Role::Member` gate)
    ///        have already authorized the caller before reaching here.
    /// @param action The mutation to apply -- assumed already validated.
    /// @throws NotFound if `action.taskId` does not belong to the attached
    ///         project.
    void applyTagMutationImpl(const ApplyTagMutation& action);

    /// @brief The project this handler is attached to, set on the first
    ///        successful `execute(OpenBoard)` and, independently, by
    ///        `attachActionLog` from a **non-empty** `entityKey` (the keyed
    ///        registration path, where `Remote::attachLogIfConfigured`
    ///        supplies the client's `contextKey` -- the same project id
    ///        `OpenBoard` would set). Disengaged until one of those happens.
    ///
    ///        The invariant every attach guard in this class relies on:
    ///        **engaged implies non-empty implies attached.** It is what
    ///        makes `has_value()` a sufficient test before the
    ///        `std::stoull(*_projectIdStr)` that follows it in every
    ///        `execute()` overload, and it holds only because
    ///        `attachActionLog` declines an empty key -- see #368 for what
    ///        the fifteen guards did while it did not.
    std::optional<std::string> _projectIdStr;

    /// @brief Durable action log this instance appends to, if any -- set by
    ///        `attachActionLog`. Null (the default) for a handler that never
    ///        had one attached; every `logAction` call is then a no-op and
    ///        `execute(GetActivity)` returns an empty stream rather than
    ///        throwing (design spec §4: "Local-mode-without-attach is a
    ///        stated limitation", not an error).
    std::shared_ptr<::morph::journal::IActionLog> _log;
};

}  // namespace kanban

BRIDGE_REGISTER_MODEL(kanban::BoardModel, "BoardModel")
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::OpenBoard, "OpenBoard", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::GetBoardState, "GetBoardState", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::CreateColumn, "CreateColumn")
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::CreateSwimlane, "CreateSwimlane")
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::CreateTask, "CreateTask")
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::AddComment, "AddComment")
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::MoveTaskPosition, "MoveTaskPosition")
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::GetEventsSince, "GetEventsSince", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::GetActivity, "GetActivity", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::CreateRule, "CreateRule")
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::GetRules, "GetRules", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::DeleteRule, "DeleteRule")
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::ApplyTagMutation, "ApplyTagMutation")
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::AddAttachment, "AddAttachment")
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::GetAttachments, "GetAttachments", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(kanban::BoardModel, kanban::RemoveAttachment, "RemoveAttachment")

// `BoardModel` is keyed on the project the board belongs to, and `OpenBoard`
// is the action that names it. `BRIDGE_MODEL_KEY` deduces the key *type* from
// the member it is handed, so `PrimaryKeyOf<BoardModel>` is `kanban::ProjectId`
// itself -- the strong id examples/IMPLEMENTATION.md rule 3 requires -- rather
// than the unwrapped `std::int64_t` this rung declared while
// `morph::model::ModelKey` still admitted only raw scalars (morph#163 widened
// it; morph#183 migrated this rung off the hand-written specialisations).
//
// The disengaged-`projectId` rejection the hand-written `key()` spelled out is
// now `morph::model::keyToString`'s own: it throws for a strong id with no
// value instead of dereferencing an empty optional, which is what makes
// `BoardBridge::openBoard("not-a-number")` (parsed into a default-constructed
// `ProjectId{}` by board_qml_bridge.cpp's `parseId`) a rejected `Completion`
// rather than undefined behaviour. `BridgeHandler::execute`'s
// `try { key = ActionKeyTraits<Action>::key(action); } catch (...)` block
// still turns the throw into `.onError()`, unchanged. What *did* change is the
// exception's type: morph's own `std::runtime_error`, no longer
// `kanban::ValidationError` -- nothing in this rung catches the key rejection
// by type (`morph::ladder::gui::errorText` reads `what()` off any
// `std::exception`), and test_board_model.cpp pins the new type.
BRIDGE_MODEL_KEY(kanban::BoardModel, kanban::OpenBoard, &kanban::OpenBoard::projectId);
