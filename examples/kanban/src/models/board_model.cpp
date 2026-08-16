// SPDX-License-Identifier: Apache-2.0
#include "kanban/models/board_model.hpp"

#include "kanban/db/kanban_entity.hpp"
#include "kanban/dto/project_dto.hpp"

#include "clock.hpp"

#include <morph/session/session.hpp>

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/DataMapper/Pool.hpp>
#include <Lightweight/SqlTransaction.hpp>

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
    transaction.Commit();

    return buildState(mapper.Get(), project);
}

}  // namespace kanban
