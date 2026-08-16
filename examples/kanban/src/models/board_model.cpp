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
            result.comments.push_back(
                {.principal = std::string{c.principal.Value()}, .body = std::string{c.body.Value()}});
        }
    }
    return result;
}

}  // namespace

GetBoardResult BoardModel::execute(const OpenBoard& action) {
    if (!action.validate()) {
        throw ValidationError{"OpenBoard: projectId is required"};
    }
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    auto project = loadProjectById(mapper.Get(), static_cast<std::uint64_t>(*action.projectId));
    _projectIdStr = std::to_string(project.id.Value());
    return buildState(mapper.Get(), project);
}

GetBoardResult BoardModel::execute(const GetBoardState& /*action*/) {
    if (!_projectIdStr.has_value()) {
        throw NotFound{"GetBoardState: handler was never attached via OpenBoard"};
    }
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

    return buildState(mapper.Get(), project);
}

GetBoardResult BoardModel::execute(const CreateSwimlane& action) {
    if (!action.validate()) {
        throw ValidationError{"CreateSwimlane: a bounded, non-empty name is required"};
    }
    if (!_projectIdStr.has_value()) {
        throw NotFound{"CreateSwimlane: handler was never attached via OpenBoard"};
    }
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

    return buildState(mapper.Get(), project);
}

GetBoardResult BoardModel::execute(const CreateTask& action) {
    if (!action.validate()) {
        throw ValidationError{"CreateTask: engaged columnId/swimlaneId and a bounded title are required"};
    }
    if (!_projectIdStr.has_value()) {
        throw NotFound{"CreateTask: handler was never attached via OpenBoard"};
    }
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    const auto projectDbId = static_cast<std::uint64_t>(std::stoull(*_projectIdStr));
    auto project = loadProjectById(mapper.Get(), projectDbId);

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

    return buildState(mapper.Get(), project);
}

GetBoardResult BoardModel::execute(const AddComment& action) {
    if (!action.validate()) {
        throw ValidationError{"AddComment: an engaged taskId and non-empty body are required"};
    }
    if (!_projectIdStr.has_value()) {
        throw NotFound{"AddComment: handler was never attached via OpenBoard"};
    }
    const auto& principal = requireOwner();
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    const auto projectDbId = static_cast<std::uint64_t>(std::stoull(*_projectIdStr));
    auto project = loadProjectById(mapper.Get(), projectDbId);

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

    return buildState(mapper.Get(), project);
}

GetBoardResult BoardModel::execute(const MoveTaskPosition& action) {
    if (!action.validate()) {
        throw ValidationError{"MoveTaskPosition: engaged taskId/columnId/swimlaneId and a non-negative position "
                               "are required"};
    }
    if (!_projectIdStr.has_value()) {
        throw NotFound{"MoveTaskPosition: handler was never attached via OpenBoard"};
    }
    auto mapper = ::Lightweight::GlobalDataMapperPool().Acquire();
    const auto projectDbId = static_cast<std::uint64_t>(std::stoull(*_projectIdStr));
    auto project = loadProjectById(mapper.Get(), projectDbId);

    // Design spec §1: ledger lookup, after any identity gate (none exists
    // on this action -- MoveTaskPosition is not role-gated, per the README's
    // "What is actually gated" convention any un-mentioned action inherits
    // from polls' equivalent statement: only structural/admin actions are
    // role-gated, ordinary board moves are not), before any re-validation.
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
            return replayed;
        }
    }

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

    // Assigning the loaded parent records here -- not the raw FK integers --
    // is load-bearing, not stylistic: `Light::BelongsTo::operator=` has no
    // overload for a bare integral key, only `operator=(ReferencedRecord&)`
    // (which marks the field `_modified`) and the copy/move-assignment
    // overloads. A bare `task.column = static_cast<std::uint64_t>(...)` on
    // an already-loaded record compiles (it implicitly constructs a
    // temporary `BelongsTo` and copy-assigns it), but that path never sets
    // `_modified`, so the subsequent `mapper->Update(task)` below would
    // silently omit `column_id`/`swimlane_id` from its `SET` clause and the
    // move would not persist -- verified against
    // `Lightweight/DataMapper/BelongsTo.hpp` and `DataMapper::Update()`'s
    // `field.IsModified()` gate. `rec.project = project;` elsewhere in this
    // file relies on exactly the same `operator=(ReferencedRecord&)` path
    // for the identical reason.
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
    return result;
}

GetEventsSinceResult BoardModel::execute(const GetEventsSince& action) {
    if (!action.validate()) {
        throw ValidationError{"GetEventsSince: malformed request"};
    }
    if (!_projectIdStr.has_value()) {
        throw NotFound{"GetEventsSince: handler was never attached via OpenBoard"};
    }
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

}  // namespace kanban
