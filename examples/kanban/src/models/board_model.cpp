// SPDX-License-Identifier: Apache-2.0
#include "kanban/models/board_model.hpp"

#include "kanban/db/kanban_entity.hpp"
#include "kanban/dto/project_dto.hpp"

#include "clock.hpp"

#include <morph/session/session.hpp>

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/DataMapper/Pool.hpp>
#include <Lightweight/SqlTransaction.hpp>

#include <glaze/glaze.hpp>

#include <algorithm>

namespace kanban {

static_assert(decltype(db::ProjectRecord::name)::ValueType{}.capacity() == kMaxProjectNameBytes,
              "kanban::kMaxProjectNameBytes must equal ProjectRecord::name's SqlAnsiString capacity -- otherwise "
              "CreateProject either rejects a name that would have fit, or accepts one that gets silently "
              "truncated on the way into the row.");
static_assert(decltype(db::ColumnRecord::name)::ValueType{}.capacity() == kMaxColumnNameBytes,
              "kanban::kMaxColumnNameBytes must equal ColumnRecord::name's SqlAnsiString capacity.");
static_assert(decltype(db::SwimlaneRecord::name)::ValueType{}.capacity() == kMaxSwimlaneNameBytes,
              "kanban::kMaxSwimlaneNameBytes must equal SwimlaneRecord::name's SqlAnsiString capacity.");
static_assert(decltype(db::TaskRecord::title)::ValueType{}.capacity() == kMaxTaskTitleBytes,
              "kanban::kMaxTaskTitleBytes must equal TaskRecord::title's SqlAnsiString capacity.");

namespace {

[[nodiscard]] std::int64_t nowMs() noexcept {
    return (*::morph::ladder::now().value).value.time_since_epoch().count();
}

[[nodiscard]] const std::string& requireOwner() {
    const auto* ctx = ::morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw Forbidden{"no authenticated principal"};
    }
    return ctx->principal;
}

[[nodiscard]] db::ProjectRecord loadProjectById(::Lightweight::DataMapper& mapper, std::uint64_t projectDbId) {
    auto rows = mapper.Query<db::ProjectRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::ProjectRecord::id>, "=", projectDbId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"project not found"};
    }
    return std::move(rows.front());
}

/// @brief The caller's own role on @p projectDbId, or `std::nullopt` if
///        they have none. Mirrors `ProjectAdminModel`'s identical helper
///        (design spec §3: not shared code, each model gets its own copy).
[[nodiscard]] std::optional<Role> loadCallerRole(::Lightweight::DataMapper& mapper, std::uint64_t projectDbId,
                                                  const std::string& principal) {
    auto rows = mapper.Query<db::ProjectRoleRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::ProjectRoleRecord::project>, "=", projectDbId)
                    .Where(::Lightweight::FieldNameOf<&db::ProjectRoleRecord::principal>, "=", principal)
                    .All();
    if (rows.empty()) {
        return std::nullopt;
    }
    return roleFromString(rows.front().role.Value().str());
}

/// @brief Confirms @p columnId names a real column belonging to @p project
///        -- design spec §2's cross-strand re-check: `ColumnRecord::project`
///        is FK-shaped but not FK-enforced by SQLite, and a column deleted
///        by `ProjectAdminModel` (a different strand) between `GetBoard` and
///        `MoveTaskPosition` must surface as a typed error here, not a
///        silent write into an orphaned row.
void requireColumnBelongsToProject(::Lightweight::DataMapper& mapper, const db::ProjectRecord& project,
                                    ColumnId columnId) {
    if (!columnId.hasValue() || *columnId < 0) {
        throw NotFound{"column does not belong to this project"};
    }
    auto rows = mapper.Query<db::ColumnRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::ColumnRecord::id>, "=",
                           static_cast<std::uint64_t>(*columnId))
                    .Where(::Lightweight::FieldNameOf<&db::ColumnRecord::project>, "=", project.id.Value())
                    .All();
    if (rows.empty()) {
        throw NotFound{"column does not belong to this project"};
    }
}

/// @brief Confirms @p swimlaneId names a real swimlane belonging to
///        @p project -- sibling of `requireColumnBelongsToProject` above,
///        same design spec §2 "trust nothing read before this call,
///        re-check inside the transaction" discipline.
void requireSwimlaneBelongsToProject(::Lightweight::DataMapper& mapper, const db::ProjectRecord& project,
                                      SwimlaneId swimlaneId) {
    if (!swimlaneId.hasValue() || *swimlaneId < 0) {
        throw NotFound{"swimlane does not belong to this project"};
    }
    auto rows = mapper.Query<db::SwimlaneRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::SwimlaneRecord::id>, "=",
                           static_cast<std::uint64_t>(*swimlaneId))
                    .Where(::Lightweight::FieldNameOf<&db::SwimlaneRecord::project>, "=", project.id.Value())
                    .All();
    if (rows.empty()) {
        throw NotFound{"swimlane does not belong to this project"};
    }
}

/// @brief Confirms @p taskId names a real task belonging to @p project --
///        sibling of `requireColumnBelongsToProject` above, same design spec
///        §2 "trust nothing read before this call, re-check inside the
///        transaction" discipline: `TaskRecord::project` is FK-shaped but not
///        FK-enforced by SQLite, and a task from another project must
///        surface as a typed error here, not a silent cross-tenant read or
///        write into a foreign row.
void requireTaskBelongsToProject(::Lightweight::DataMapper& mapper, const db::ProjectRecord& project,
                                  TaskId taskId) {
    if (!taskId.hasValue() || *taskId < 0) {
        throw NotFound{"task does not belong to this project"};
    }
    auto rows = mapper.Query<db::TaskRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::TaskRecord::id>, "=",
                           static_cast<std::uint64_t>(*taskId))
                    .Where(::Lightweight::FieldNameOf<&db::TaskRecord::project>, "=", project.id.Value())
                    .All();
    if (rows.empty()) {
        throw NotFound{"task does not belong to this project"};
    }
}

[[nodiscard]] GetBoardResult buildState(::Lightweight::DataMapper& mapper, const db::ProjectRecord& project) {
    GetBoardResult result;
    result.projectId = ProjectId{static_cast<std::int64_t>(project.id.Value())};
    result.name = std::string{project.name.Value()};

    const std::uint64_t projectDbId = project.id.Value();
    auto columns = mapper.Query<db::ColumnRecord>()
                       .Where(::Lightweight::FieldNameOf<&db::ColumnRecord::project>, "=", projectDbId)
                       .OrderBy(::Lightweight::FieldNameOf<&db::ColumnRecord::sortOrder>)
                       .All();
    auto tasks = mapper.Query<db::TaskRecord>()
                     .Where(::Lightweight::FieldNameOf<&db::TaskRecord::project>, "=", projectDbId)
                     .All();
    for (const auto& col : columns) {
        ColumnView view;
        view.id = ColumnId{static_cast<std::int64_t>(col.id.Value())};
        view.name = std::string{col.name.Value()};
        view.wipLimit = col.wipLimit.Value();
        for (const auto& t : tasks) {
            if (t.column.Value() == col.id.Value()) {
                ++view.taskCount;
            }
        }
        result.columns.push_back(std::move(view));
    }

    auto swimlanes = mapper.Query<db::SwimlaneRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::SwimlaneRecord::project>, "=", projectDbId)
                          .OrderBy(::Lightweight::FieldNameOf<&db::SwimlaneRecord::sortOrder>)
                          .All();
    for (const auto& sw : swimlanes) {
        result.swimlanes.push_back(
            {.id = SwimlaneId{static_cast<std::int64_t>(sw.id.Value())}, .name = std::string{sw.name.Value()}});
    }

    for (const auto& t : tasks) {
        result.tasks.push_back({.id = TaskId{static_cast<std::int64_t>(t.id.Value())},
                                 .columnId = ColumnId{static_cast<std::int64_t>(t.column.Value())},
                                 .swimlaneId = SwimlaneId{static_cast<std::int64_t>(t.swimlane.Value())},
                                 .title = std::string{t.title.Value()},
                                 .position = t.position.Value()});
    }

    auto taskIds = std::vector<std::uint64_t>{};
    taskIds.reserve(tasks.size());
    for (const auto& t : tasks) {
        taskIds.push_back(t.id.Value());
    }
    if (!taskIds.empty()) {
        auto comments =
            mapper.Query<db::CommentRecord>().WhereIn(::Lightweight::FieldNameOf<&db::CommentRecord::task>, taskIds).All();
        for (const auto& c : comments) {
            result.comments.push_back({.taskId = TaskId{static_cast<std::int64_t>(c.task.Value())},
                                        .principal = std::string{c.principal.Value()},
                                        .body = std::string{c.body.Value()}});
        }
    }
    return result;
}

}  // namespace

void BoardModel::attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
    _log = std::move(log);
    _projectIdStr = std::move(entityKey);
}

template <typename Action, typename Result>
void BoardModel::logAction(const Action& action, const Result& result) const {
    if (!_log) {
        return;
    }
    ::morph::journal::LogEntry entry;
    entry.modelType = "BoardModel";
    entry.entityKey = _projectIdStr.value_or(std::string{});
    entry.actionType = std::string{::morph::model::ActionTraits<Action>::typeId()};
    entry.payload = ::morph::model::ActionTraits<Action>::toJson(action);
    entry.result = ::morph::model::ActionTraits<Action>::resultToJson(result);
    entry.outcome = ::morph::journal::Outcome::Succeeded;
    if (const auto* ctx = ::morph::session::current()) {
        entry.principal = ctx->principal;
    }
    entry.timestampMs = nowMs();
    _log->append(std::move(entry));
    // GetActivity reads this same log back via a fresh `entries()` call
    // (design spec §4), and `FileActionLog::entries()`'s own doc comment is
    // explicit that an unflushed `append()` is only visible "if the
    // platform's stdio buffering has already handed it to the OS" --
    // otherwise invisible to entries()'s separate ifstream, since append()
    // writes through buffered C stdio (`fwrite`) with no implicit flush.
    // Without this, a client polling GetActivity immediately after its own
    // mutating call would nondeterministically miss the entry it just
    // caused -- observed directly: `docs/spec/journal/journal.md`'s stated
    // contract is not something GetActivity can rely on without calling it.
    // `InMemoryActionLog::flush()` is a no-op, so this costs nothing for the
    // log type most non-App tests actually attach.
    _log->flush();
}

void BoardModel::requireRoleOn(std::uint64_t projectDbId, Role minimum) const {
    const auto& principal = requireOwner();
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    const auto role = loadCallerRole(mapper.Get(), projectDbId, principal);
    if (!role.has_value() || static_cast<std::uint8_t>(*role) < static_cast<std::uint8_t>(minimum)) {
        throw Forbidden{"caller's role does not permit this action"};
    }
}

void BoardModel::requireRole(Role minimum) const {
    const auto projectDbId = static_cast<std::uint64_t>(std::stoull(*_projectIdStr));
    requireRoleOn(projectDbId, minimum);
}

GetBoardResult BoardModel::execute(const OpenBoard& action) {
    if (!action.validate()) {
        throw ValidationError{"OpenBoard: projectId is required"};
    }
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    auto project = loadProjectById(mapper.Get(), static_cast<std::uint64_t>(*action.projectId));
    // C1 fix: gated against the *target* project (project.id), not
    // `_projectIdStr` -- that member is what this very call is about to set
    // on success, so `requireRole(Role minimum)`'s ambient-attach-state form
    // cannot be used here. Checked after `loadProjectById` resolves the
    // project (so a nonexistent project still reports NotFound, not
    // Forbidden) but before `buildState` returns any board contents, so a
    // principal with no standing on this project never observes its data by
    // attaching to it.
    requireRoleOn(project.id.Value(), Role::Viewer);
    _projectIdStr = std::to_string(project.id.Value());
    return buildState(mapper.Get(), project);
}

GetBoardResult BoardModel::execute(const GetBoardState& /*action*/) {
    if (!_projectIdStr.has_value()) {
        throw NotFound{"GetBoardState: handler was never attached via OpenBoard"};
    }
    requireRole(Role::Viewer);
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    const auto projectDbId = static_cast<std::uint64_t>(std::stoull(*_projectIdStr));
    return buildState(mapper.Get(), loadProjectById(mapper.Get(), projectDbId));
}

GetBoardResult BoardModel::execute(const CreateColumn& action) {
    if (!action.validate()) {
        throw ValidationError{"CreateColumn: a bounded, non-empty name is required"};
    }
    if (!_projectIdStr.has_value()) {
        throw NotFound{"CreateColumn: handler was never attached via OpenBoard"};
    }
    requireRole(Role::Member);
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    const auto projectDbId = static_cast<std::uint64_t>(std::stoull(*_projectIdStr));
    auto project = loadProjectById(mapper.Get(), projectDbId);

    auto existing = mapper->Query<db::ColumnRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::ColumnRecord::project>, "=", projectDbId)
                        .All();

    ::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
    db::ColumnRecord rec;
    rec.project = project;
    rec.name = action.name;
    rec.wipLimit = action.wipLimit;
    rec.sortOrder = static_cast<std::int64_t>(existing.size());
    mapper->Create(rec);

    db::BoardEventRecord event;
    event.project = project;
    event.kind = "column";
    event.summary = "column created";
    event.createdAtMs = nowMs();
    mapper->Create(event);

    transaction.Commit();

    auto result = buildState(mapper.Get(), project);
    logAction(action, result);
    return result;
}

GetBoardResult BoardModel::execute(const CreateSwimlane& action) {
    if (!action.validate()) {
        throw ValidationError{"CreateSwimlane: a bounded, non-empty name is required"};
    }
    if (!_projectIdStr.has_value()) {
        throw NotFound{"CreateSwimlane: handler was never attached via OpenBoard"};
    }
    requireRole(Role::Member);
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    const auto projectDbId = static_cast<std::uint64_t>(std::stoull(*_projectIdStr));
    auto project = loadProjectById(mapper.Get(), projectDbId);

    auto existing = mapper->Query<db::SwimlaneRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::SwimlaneRecord::project>, "=", projectDbId)
                        .All();

    ::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
    db::SwimlaneRecord rec;
    rec.project = project;
    rec.name = action.name;
    rec.sortOrder = static_cast<std::int64_t>(existing.size());
    mapper->Create(rec);

    db::BoardEventRecord event;
    event.project = project;
    event.kind = "swimlane";
    event.summary = "swimlane created";
    event.createdAtMs = nowMs();
    mapper->Create(event);

    transaction.Commit();

    auto result = buildState(mapper.Get(), project);
    logAction(action, result);
    return result;
}

GetBoardResult BoardModel::execute(const CreateTask& action) {
    if (!action.validate()) {
        throw ValidationError{"CreateTask: engaged columnId/swimlaneId and a bounded title are required"};
    }
    if (!_projectIdStr.has_value()) {
        throw NotFound{"CreateTask: handler was never attached via OpenBoard"};
    }
    requireRole(Role::Member);
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    const auto projectDbId = static_cast<std::uint64_t>(std::stoull(*_projectIdStr));
    auto project = loadProjectById(mapper.Get(), projectDbId);

    // C2 fix: re-check both destination FKs belong to this project before
    // trusting them -- a Member of a different project must not be able to
    // create a task pointing at this project's column/swimlane by id
    // (design spec §2's "trust nothing read before this call" discipline,
    // already applied to MoveTaskPosition's destination but previously
    // missing here).
    requireColumnBelongsToProject(mapper.Get(), project, action.columnId);
    requireSwimlaneBelongsToProject(mapper.Get(), project, action.swimlaneId);

    auto existing = mapper->Query<db::TaskRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::TaskRecord::column>, "=",
                               static_cast<std::uint64_t>(*action.columnId))
                        .Where(::Lightweight::FieldNameOf<&db::TaskRecord::swimlane>, "=",
                               static_cast<std::uint64_t>(*action.swimlaneId))
                        .All();

    ::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
    db::TaskRecord rec;
    rec.project = project;
    rec.column = static_cast<std::uint64_t>(*action.columnId);
    rec.swimlane = static_cast<std::uint64_t>(*action.swimlaneId);
    rec.title = action.title;
    rec.position = static_cast<std::int64_t>(existing.size());
    rec.createdAtMs = nowMs();
    mapper->Create(rec);

    db::BoardEventRecord event;
    event.project = project;
    event.kind = "task";
    event.summary = "task created";
    event.createdAtMs = nowMs();
    mapper->Create(event);

    transaction.Commit();

    auto result = buildState(mapper.Get(), project);
    logAction(action, result);
    return result;
}

GetBoardResult BoardModel::execute(const AddComment& action) {
    if (!action.validate()) {
        throw ValidationError{"AddComment: an engaged taskId and non-empty body are required"};
    }
    if (!_projectIdStr.has_value()) {
        throw NotFound{"AddComment: handler was never attached via OpenBoard"};
    }
    requireRole(Role::Member);
    const auto& principal = requireOwner();
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    const auto projectDbId = static_cast<std::uint64_t>(std::stoull(*_projectIdStr));
    auto project = loadProjectById(mapper.Get(), projectDbId);

    // C2 fix: without this, a Member of a different project could attach a
    // comment to any task on the server by id, which then surfaces in that
    // task's *other* project's board view (buildState's comment list).
    requireTaskBelongsToProject(mapper.Get(), project, action.taskId);

    ::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
    db::CommentRecord rec;
    rec.task = static_cast<std::uint64_t>(*action.taskId);
    rec.principal = principal;
    rec.body = action.body;
    rec.createdAtMs = nowMs();
    mapper->Create(rec);

    db::BoardEventRecord event;
    event.project = project;
    event.kind = "comment";
    event.summary = "comment added";
    event.createdAtMs = nowMs();
    mapper->Create(event);

    transaction.Commit();

    auto result = buildState(mapper.Get(), project);
    logAction(action, result);
    return result;
}

GetBoardResult BoardModel::execute(const MoveTaskPosition& action) {
    if (!action.validate()) {
        throw ValidationError{"MoveTaskPosition: engaged taskId/columnId/swimlaneId and a non-negative position "
                               "are required"};
    }
    if (!_projectIdStr.has_value()) {
        throw NotFound{"MoveTaskPosition: handler was never attached via OpenBoard"};
    }
    // Design spec §1: the role gate must run unconditionally, before the
    // ledger lookup below -- a demoted caller replaying a known opId must
    // not retrieve the stored result their current role could no longer
    // produce.
    requireRole(Role::Member);
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    const auto projectDbId = static_cast<std::uint64_t>(std::stoull(*_projectIdStr));
    auto project = loadProjectById(mapper.Get(), projectDbId);

    // Design spec §1: ledger lookup, after the role gate above, before any
    // re-validation.
    if (!action.opId.empty()) {
        auto existingOp = mapper
                              ->Query<db::AppliedOpRecord>()
                              .Where(::Lightweight::FieldNameOf<&db::AppliedOpRecord::project>, "=", projectDbId)
                              .Where(::Lightweight::FieldNameOf<&db::AppliedOpRecord::opId>, "=", action.opId)
                              .All();
        if (!existingOp.empty()) {
            GetBoardResult replayed;
            if (auto err = glz::read_json(replayed, std::string{existingOp.front().resultJson.Value()}); err) {
                throw ::kanban::KanbanError{"MoveTaskPosition: corrupt ledger entry"};
            }
            // Design spec §4 (corrected): a ledger hit means this call
            // performed nothing new -- it only returned a previously-stored
            // result -- so there is nothing to journal here. Verified against
            // a live `FileActionLog` capture during real `RemoteServer`
            // dispatch: the framework's own auto-append does not produce a
            // second entry on this path, so logging a replay unconditionally
            // was `BoardModel`'s own self-inflicted duplicate, not something
            // the framework required compensating for.
            return replayed;
        }
    }

    // C2 fix: without this, a Member of a different project could move any
    // task on the server by id -- the checks below only ever verified the
    // *destination* column/swimlane belong to this project, never the task
    // being moved. Checked right after the ledger-hit branch and before the
    // destination checks, so a Member of another project cannot move this
    // project's task at all, regardless of what destination they name.
    requireTaskBelongsToProject(mapper.Get(), project, action.taskId);

    requireColumnBelongsToProject(mapper.Get(), project, action.columnId);

    // WIP-limit check: count tasks already in the target column, excluding
    // this task itself (a same-column reorder must not count against its
    // own limit).
    auto targetColumnRows = mapper->Query<db::ColumnRecord>()
                                 .Where(::Lightweight::FieldNameOf<&db::ColumnRecord::id>, "=",
                                        static_cast<std::uint64_t>(*action.columnId))
                                 .All();
    auto targetColumn = targetColumnRows.front();
    if (targetColumn.wipLimit.Value() > 0) {
        auto currentInColumn =
            mapper->Query<db::TaskRecord>()
                .Where(::Lightweight::FieldNameOf<&db::TaskRecord::column>, "=",
                       static_cast<std::uint64_t>(*action.columnId))
                .Where(::Lightweight::FieldNameOf<&db::TaskRecord::id>, "!=",
                       static_cast<std::uint64_t>(*action.taskId))
                .All();
        if (static_cast<std::int64_t>(currentInColumn.size()) + 1 > targetColumn.wipLimit.Value()) {
            throw Conflict{"MoveTaskPosition: target column is at its WIP limit"};
        }
    }

    auto swimlaneRows = mapper->Query<db::SwimlaneRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::SwimlaneRecord::id>, "=",
                                   static_cast<std::uint64_t>(*action.swimlaneId))
                            .Where(::Lightweight::FieldNameOf<&db::SwimlaneRecord::project>, "=", projectDbId)
                            .All();
    if (swimlaneRows.empty()) {
        // Same cross-strand re-check as requireColumnBelongsToProject, for
        // the swimlane half of the destination -- design spec §2's "trust
        // nothing read before this call, re-check inside the transaction"
        // discipline applies equally to both halves of (columnId, swimlaneId).
        throw NotFound{"swimlane does not belong to this project"};
    }
    auto targetSwimlane = swimlaneRows.front();

    auto taskRows = mapper->Query<db::TaskRecord>()
                         .Where(::Lightweight::FieldNameOf<&db::TaskRecord::id>, "=",
                                static_cast<std::uint64_t>(*action.taskId))
                         .All();
    if (taskRows.empty()) {
        throw NotFound{"MoveTaskPosition: task not found"};
    }
    auto task = taskRows.front();

    ::Lightweight::SqlTransaction transaction{mapper->Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};

    // The task's (column, swimlane) before this move -- captured before
    // `task.column`/`task.swimlane` are overwritten below, so the source-side
    // renumbering query a few lines down can still tell which pair to
    // re-tighten. A same-(column, swimlane) reorder has source == destination
    // and needs no separate pass (the destination pass below already covers
    // it, and re-running an identical renumber over the same rows would be
    // redundant, not incorrect, but is skipped entirely for clarity).
    const auto sourceColumnId = task.column.Value();
    const auto sourceSwimlaneId = task.swimlane.Value();
    const bool movesAcrossColumnOrSwimlane =
        sourceColumnId != static_cast<std::uint64_t>(*action.columnId) ||
        sourceSwimlaneId != static_cast<std::uint64_t>(*action.swimlaneId);

    // Position renumbering (design spec §2): delete-then-recreate every task
    // in the destination (column, swimlane), never an in-place index shift
    // -- mirrors polls::PollModel::applyVotes()'s vote-replacement idiom.
    auto destinationTasks =
        mapper->Query<db::TaskRecord>()
            .Where(::Lightweight::FieldNameOf<&db::TaskRecord::column>, "=",
                   static_cast<std::uint64_t>(*action.columnId))
            .Where(::Lightweight::FieldNameOf<&db::TaskRecord::swimlane>, "=",
                   static_cast<std::uint64_t>(*action.swimlaneId))
            .Where(::Lightweight::FieldNameOf<&db::TaskRecord::id>, "!=",
                   static_cast<std::uint64_t>(*action.taskId))
            .OrderBy(::Lightweight::FieldNameOf<&db::TaskRecord::position>)
            .All();

    // Assigning the loaded parent records here (rather than the raw FK
    // integers) is a style choice, not a correctness requirement: both
    // `targetColumn`/`targetSwimlane` are already loaded in scope from the
    // ownership checks above, so reusing them avoids a redundant re-fetch a
    // bare-integer assignment would otherwise need to name the same rows by
    // id. `Light::BelongsTo::operator=(S&&)` (a bare key value) now marks
    // the field modified correctly, same as `operator=(ReferencedRecord&)` --
    // LASTRADA-Software/Lightweight#551 fixed the prior silent-modification-
    // loss bug on that path (its variadic converting-constructor overload
    // previously left `_modified` false, so `mapper->Update(task)` below
    // would have silently omitted `column_id`/`swimlane_id` from its `SET`
    // clause and the move would not have persisted). `CreateTask`'s
    // `rec.column = static_cast<std::uint64_t>(...)` a few lines up uses the
    // bare-integer form directly, since it has no already-loaded record to
    // reuse and feeds a fresh `Create()` call either way.
    task.column = targetColumn;
    task.swimlane = targetSwimlane;
    std::int64_t pos = 0;
    for (auto& t : destinationTasks) {
        if (pos == action.position) {
            ++pos;
        }
        t.position = pos++;
        mapper->Update(t);
    }
    task.position = std::min(action.position, pos);
    mapper->Update(task);

    // Source-side renumbering: a cross-(column, swimlane) move leaves a gap
    // behind in the pair the task departed -- the destination-only pass above
    // never touches those rows, since they never match its
    // (columnId, swimlaneId) WHERE clause. Without this, design spec §2's
    // "position is dense within its (columnId, swimlaneId) pair" invariant
    // holds for the destination but silently drifts for the source (e.g.
    // moving the task that sat at position 2 out of a 5-task column leaves
    // the other four at {0, 1, 3, 4} forever, not renumbered to {0, 1, 2, 3},
    // until some *other* move happens to touch that same pair again). A
    // same-(column, swimlane) reorder has source == destination, so this
    // pass is skipped for it -- the destination pass already renumbered every
    // row in that pair, including what would otherwise be a redundant second
    // pass over the identical rows.
    if (movesAcrossColumnOrSwimlane) {
        auto sourceTasks = mapper->Query<db::TaskRecord>()
                                .Where(::Lightweight::FieldNameOf<&db::TaskRecord::column>, "=", sourceColumnId)
                                .Where(::Lightweight::FieldNameOf<&db::TaskRecord::swimlane>, "=", sourceSwimlaneId)
                                .Where(::Lightweight::FieldNameOf<&db::TaskRecord::id>, "!=",
                                       static_cast<std::uint64_t>(*action.taskId))
                                .OrderBy(::Lightweight::FieldNameOf<&db::TaskRecord::position>)
                                .All();
        std::int64_t sourcePos = 0;
        for (auto& t : sourceTasks) {
            t.position = sourcePos++;
            mapper->Update(t);
        }
    }

    db::BoardEventRecord event;
    event.project = project;
    event.kind = "move";
    event.summary = "task moved";
    event.createdAtMs = nowMs();
    mapper->Create(event);

    auto result = buildState(mapper.Get(), project);

    if (!action.opId.empty()) {
        std::string resultJson;
        if (auto err = glz::write_json(result, resultJson); err) {
            throw KanbanError{"MoveTaskPosition: failed to serialize result for the applied-ops ledger"};
        }
        db::AppliedOpRecord op;
        op.project = project;
        op.opId = action.opId;
        op.resultJson = resultJson;
        op.createdAtMs = nowMs();
        mapper->Create(op);
    }

    transaction.Commit();
    logAction(action, result);
    return result;
}

GetEventsSinceResult BoardModel::execute(const GetEventsSince& action) {
    if (!action.validate()) {
        throw ValidationError{"GetEventsSince: malformed request"};
    }
    if (!_projectIdStr.has_value()) {
        throw NotFound{"GetEventsSince: handler was never attached via OpenBoard"};
    }
    requireRole(Role::Viewer);
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    const auto projectDbId = static_cast<std::uint64_t>(std::stoull(*_projectIdStr));

    auto rows = mapper
                    ->Query<db::BoardEventRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::BoardEventRecord::project>, "=", projectDbId)
                    .Where(::Lightweight::FieldNameOf<&db::BoardEventRecord::id>, ">",
                           static_cast<std::uint64_t>(*action.lastEventId))
                    .OrderBy(::Lightweight::FieldNameOf<&db::BoardEventRecord::id>)
                    .All();

    GetEventsSinceResult result;
    result.events.reserve(rows.size());
    for (const auto& row : rows) {
        result.events.push_back({.id = BoardEventId{.value = static_cast<std::int64_t>(row.id.Value())},
                                  .kind = std::string{row.kind.Value()},
                                  .summary = std::string{row.summary.Value()}});
    }
    return result;
}

GetActivityResult BoardModel::execute(const GetActivity& /*action*/) {
    if (!_projectIdStr.has_value()) {
        throw NotFound{"GetActivity: handler was never attached via OpenBoard"};
    }
    requireRole(Role::Viewer);
    GetActivityResult result;
    if (!_log) {
        return result;  // no log attached (design spec §4: Local-mode-without-attach is a stated limitation)
    }
    // FileActionLog re-reads its whole backing file per call (LADDER.md's own
    // journal-honesty note) -- acceptable at this rung's per-board scale
    // (design spec §4), but not a pattern to copy at bigger scale without
    // re-checking that cost.
    auto entries = _log->entries(*_projectIdStr);
    for (const auto& entry : entries) {
        result.events.push_back({.actionType = entry.actionType,
                                  .principal = entry.principal,
                                  .timestampMs = entry.timestampMs,
                                  .summary = entry.actionType + " by " + entry.principal});
    }
    return result;
}

}  // namespace kanban
