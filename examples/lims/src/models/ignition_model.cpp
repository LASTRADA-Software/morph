// SPDX-License-Identifier: Apache-2.0
#include "lims/models/ignition_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/model.hpp>
#include <morph/core/registry.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lims/core/errors.hpp"
#include "lims/core/model_support.hpp"
#include "lims/db/lims_entity.hpp"

namespace lims {

namespace {

[[nodiscard]] IgnitionTestView viewOf(db::IgnitionTestRecord const& row) {
    return IgnitionTestView{.id = static_cast<std::int64_t>(row.id.Value()),
                            .container = row.container.Value(),
                            .before = row.before.Value(),
                            .after = row.after.Value()};
}

[[nodiscard]] db::IgnitionTestRecord find(Lightweight::DataMapper& mapper, std::int64_t testId) {
    auto row = mapper.QuerySingle<db::IgnitionTestRecord>(static_cast<std::uint64_t>(testId));
    if (!row) {
        throw NotFound{"no ignition test " + std::to_string(testId)};
    }
    return std::move(*row);
}

}  // namespace

ListIgnitionTestsResult IgnitionModel::execute(const ListIgnitionTests& /*action*/) {
    requirePrincipal();
    Lightweight::DataMapper mapper;
    ListIgnitionTestsResult result;
    for (auto const& row : mapper.Query<db::IgnitionTestRecord>().All()) {
        result.tests.push_back(viewOf(row));
    }
    return result;
}

IgnitionTestView IgnitionModel::execute(const AddIgnitionTest& /*action*/) {
    requirePrincipal();
    Lightweight::DataMapper mapper;
    db::IgnitionTestRecord row;
    mapper.Create(row);
    return viewOf(row);
}

IgnitionTestView IgnitionModel::execute(const RecordIgnitionValues& action) {
    requirePrincipal();
    Lightweight::DataMapper mapper;
    db::IgnitionTestRecord row = find(mapper, action.id);
    if (action.container) {
        row.container = action.container;
    }
    if (action.before) {
        row.before = action.before;
    }
    if (action.after) {
        row.after = action.after;
    }
    mapper.Update(row);
    return viewOf(row);
}

DeleteIgnitionTestResult IgnitionModel::execute(const DeleteIgnitionTest& action) {
    requirePrincipal();
    Lightweight::DataMapper mapper;
    db::IgnitionTestRecord row = find(mapper, action.id);
    mapper.Delete(row);
    return DeleteIgnitionTestResult{.id = action.id};
}

void attachIgnitionLog(std::shared_ptr<::morph::journal::IActionLog> log) {
    ::morph::model::detail::defaultRegistry().registerModel<IgnitionModel>("IgnitionModel", [log = std::move(log)] {
        auto holder = std::make_unique<::morph::model::detail::ModelHolder<IgnitionModel>>();
        holder->attachActionLog(log, {});
        return holder;
    });
}

}  // namespace lims
