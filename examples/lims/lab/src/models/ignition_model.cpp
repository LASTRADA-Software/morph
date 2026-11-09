// SPDX-License-Identifier: Apache-2.0
#include "lims/lab/models/ignition_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <morph/core/model.hpp>
#include <morph/core/registry.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"
#include "lims/lab/db/lims_entity.hpp"

namespace lims {

namespace {

// A value rounded to a fixed number of places, as the exact decimal the UI takes.
[[nodiscard]] std::optional<::morph::math::Rational> exact(std::optional<double> value, std::int64_t scale,
                                                           std::uint32_t places) {
    if (!value) {
        return std::nullopt;
    }
    return ::morph::math::Rational{::morph::math::Numerator{std::llround(*value * static_cast<double>(scale))},
                                   ::morph::math::Denominator{scale}, ::morph::math::DecimalPlaces{places}};
}

// Worked out here and not on the screen, so a person, a chat and any other client all read the same number.
[[nodiscard]] std::optional<double> resultOf(std::optional<double> container, std::optional<double> before,
                                             std::optional<double> after) {
    if (!container || !before || !after || *before == *container) {
        return std::nullopt;
    }
    return ((*before - *after) - *container) / (*before - *container);
}

[[nodiscard]] IgnitionTestView viewOf(db::IgnitionTestRecord const& row) {
    constexpr std::int64_t kMassScale = 1000;
    constexpr std::uint32_t kMassPlaces = 3;
    constexpr std::int64_t kResultScale = 10000;
    constexpr std::uint32_t kResultPlaces = 4;
    return IgnitionTestView{.id = static_cast<std::int64_t>(row.id.Value()),
                            .container = exact(row.container.Value(), kMassScale, kMassPlaces),
                            .before = exact(row.before.Value(), kMassScale, kMassPlaces),
                            .after = exact(row.after.Value(), kMassScale, kMassPlaces),
                            .result = exact(resultOf(row.container.Value(), row.before.Value(), row.after.Value()),
                                            kResultScale, kResultPlaces)};
}

[[nodiscard]] db::IgnitionTestRecord find(Lightweight::DataMapper& mapper, std::int64_t testId) {
    auto row = mapper.QuerySingle<db::IgnitionTestRecord>(static_cast<std::uint64_t>(testId));
    if (!row) {
        throw NotFound{"no ignition test " + std::to_string(testId)};
    }
    return *row;
}

}  // namespace

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
ListIgnitionTestsResult IgnitionModel::execute(const ListIgnitionTests& /*action*/) {
    requirePrincipal();
    Lightweight::DataMapper mapper;
    ListIgnitionTestsResult result;
    for (auto const& row : mapper.Query<db::IgnitionTestRecord>().All()) {
        result.tests.push_back(viewOf(row));
    }
    return result;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
IgnitionTestView IgnitionModel::execute(const AddIgnitionTest& /*action*/) {
    requirePrincipal();
    Lightweight::DataMapper mapper;
    db::IgnitionTestRecord row;
    mapper.Create(row);
    return viewOf(row);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
IgnitionTestView IgnitionModel::execute(const RecordIgnitionValues& action) {
    requirePrincipal();
    Lightweight::DataMapper mapper;
    db::IgnitionTestRecord row = find(mapper, action.id);
    if (action.container) {
        row.container = action.container->toDouble();
    }
    if (action.before) {
        row.before = action.before->toDouble();
    }
    if (action.after) {
        row.after = action.after->toDouble();
    }
    mapper.Update(row);
    return viewOf(row);
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
DeleteIgnitionTestResult IgnitionModel::execute(const DeleteIgnitionTest& action) {
    requirePrincipal();
    Lightweight::DataMapper mapper;
    db::IgnitionTestRecord const row = find(mapper, action.id);
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
