// SPDX-License-Identifier: Apache-2.0
#include "lims/models/analysis_catalog_model.hpp"

#include "clock.hpp"
#include "lims/core/errors.hpp"
#include "lims/db/lims_entity.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <morph/session/session.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace lims {

namespace {

/// @brief The authenticated principal, or `EmptyPrincipalError`.
///
/// Checked on every mutating action rather than assumed: the README names
/// empty-principal audit entries as disqualifying for this rung, and an
/// authorizer that ran is not the same fact as a principal that is present.
[[nodiscard]] const std::string& requirePrincipal() {
    const auto* ctx = ::morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw EmptyPrincipalError{};
    }
    return ctx->principal;
}

[[nodiscard]] std::int64_t nowMs() {
    return (*morph::ladder::now().value).value.time_since_epoch().count();
}

/// @brief Copies the optional rational bounds off an action onto a row.
template <typename Action>
void applyBounds(db::AnalysisVersionRecord& row, const Action& action) {
    row.specLowNum = action.specLow ? std::optional{action.specLow->numerator} : std::nullopt;
    row.specLowDen = action.specLow ? std::optional{action.specLow->denominator} : std::nullopt;
    row.specHighNum = action.specHigh ? std::optional{action.specHigh->numerator} : std::nullopt;
    row.specHighDen = action.specHigh ? std::optional{action.specHigh->denominator} : std::nullopt;
    row.lodNum = action.limitOfDetection ? std::optional{action.limitOfDetection->numerator} : std::nullopt;
    row.lodDen = action.limitOfDetection ? std::optional{action.limitOfDetection->denominator} : std::nullopt;
    row.udlNum = action.upperDetectionLimit ? std::optional{action.upperDetectionLimit->numerator} : std::nullopt;
    row.udlDen = action.upperDetectionLimit ? std::optional{action.upperDetectionLimit->denominator} : std::nullopt;
}

/// @brief Reads a nullable rational pair back off a row.
[[nodiscard]] std::optional<AnalysisBound> readBound(const std::optional<std::int64_t>& num,
                                                      const std::optional<std::int64_t>& den) {
    if (!num.has_value() || !den.has_value()) {
        return std::nullopt;
    }
    return AnalysisBound{.numerator = *num, .denominator = *den};
}

[[nodiscard]] AnalysisVersionView toView(const db::AnalysisVersionRecord& row, std::string name) {
    return AnalysisVersionView{
        .id = AnalysisVersionId{static_cast<std::int64_t>(row.id.Value())},
        .analysisId = AnalysisId{static_cast<std::int64_t>(row.analysis.Value())},
        .name = std::move(name),
        .version = row.version.Value(),
        .canonicalUnit = std::string{row.canonicalUnit.Value().ToStringView()},
        .decimalPlaces = static_cast<std::int32_t>(row.decimalPlaces.Value()),
        .specLow = readBound(row.specLowNum.Value(), row.specLowDen.Value()),
        .specHigh = readBound(row.specHighNum.Value(), row.specHighDen.Value()),
        .limitOfDetection = readBound(row.lodNum.Value(), row.lodDen.Value()),
        .upperDetectionLimit = readBound(row.udlNum.Value(), row.udlDen.Value()),
    };
}

}  // namespace

DefineAnalysisResult AnalysisCatalogModel::execute(const DefineAnalysis& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"DefineAnalysis: name, canonicalUnit and usable bounds are required"};
    }

    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};

    db::AnalysisRecord analysisRow;
    analysisRow.name = Lightweight::SqlAnsiString<128>{action.name};
    mapper.Create(analysisRow);

    db::AnalysisVersionRecord versionRow;
    versionRow.analysis = analysisRow;
    versionRow.version = 1;
    versionRow.canonicalUnit = Lightweight::SqlAnsiString<32>{action.canonicalUnit};
    versionRow.decimalPlaces = static_cast<int>(action.decimalPlaces);
    applyBounds(versionRow, action);
    versionRow.createdAt = nowMs();
    mapper.Create(versionRow);

    sqlTxn.Commit();

    DefineAnalysisResult result{
        .analysisId = AnalysisId{static_cast<std::int64_t>(analysisRow.id.Value())},
        .versionId = AnalysisVersionId{static_cast<std::int64_t>(versionRow.id.Value())},
        .version = 1,
    };
    logAction(action, result);
    return result;
}

DefineAnalysisResult AnalysisCatalogModel::execute(const ReviseAnalysis& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"ReviseAnalysis: analysisId, canonicalUnit and usable bounds are required"};
    }

    Lightweight::DataMapper mapper;
    auto analysisRows = mapper.Query<db::AnalysisRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::AnalysisRecord::id>, "=", *action.analysisId)
                            .All();
    if (analysisRows.empty()) {
        throw NotFound{"ReviseAnalysis: no such analysis"};
    }

    // The new version number is one past the highest existing one, read
    // rather than counted: a gap (however it arose) must not cause a
    // duplicate version number on an append-only table.
    auto existing = mapper.Query<db::AnalysisVersionRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::AnalysisVersionRecord::analysis>, "=",
                               *action.analysisId)
                        .All();
    std::int32_t highest = 0;
    for (const auto& row : existing) {
        highest = std::max(highest, row.version.Value());
    }

    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    db::AnalysisVersionRecord versionRow;
    versionRow.analysis = analysisRows.front();
    versionRow.version = highest + 1;
    versionRow.canonicalUnit = Lightweight::SqlAnsiString<32>{action.canonicalUnit};
    versionRow.decimalPlaces = static_cast<int>(action.decimalPlaces);
    applyBounds(versionRow, action);
    versionRow.createdAt = nowMs();
    mapper.Create(versionRow);
    sqlTxn.Commit();

    DefineAnalysisResult result{
        .analysisId = action.analysisId,
        .versionId = AnalysisVersionId{static_cast<std::int64_t>(versionRow.id.Value())},
        .version = highest + 1,
    };
    logAction(action, result);
    return result;
}

ListAnalysesResult AnalysisCatalogModel::execute(const ListAnalyses& action) {
    (void)action;
    Lightweight::DataMapper mapper;
    auto analysisRows = mapper.Query<db::AnalysisRecord>().All();

    ListAnalysesResult result;
    result.analyses.reserve(analysisRows.size());
    for (const auto& analysisRow : analysisRows) {
        auto versions = mapper.Query<db::AnalysisVersionRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::AnalysisVersionRecord::analysis>, "=",
                                   analysisRow.id.Value())
                            .All();
        if (versions.empty()) {
            continue;
        }
        const auto* current = &versions.front();
        for (const auto& candidate : versions) {
            if (candidate.version.Value() > current->version.Value()) {
                current = &candidate;
            }
        }
        result.analyses.push_back(toView(*current, std::string{analysisRow.name.Value().ToStringView()}));
    }
    return result;
}

AnalysisVersionView AnalysisCatalogModel::execute(const GetAnalysisVersion& action) {
    if (!action.validate()) {
        throw ValidationError{"GetAnalysisVersion: versionId is required"};
    }
    Lightweight::DataMapper mapper;
    auto versions = mapper.Query<db::AnalysisVersionRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::AnalysisVersionRecord::id>, "=", *action.versionId)
                        .All();
    if (versions.empty()) {
        throw NotFound{"GetAnalysisVersion: no such analysis version"};
    }
    auto analysisRows = mapper.Query<db::AnalysisRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::AnalysisRecord::id>, "=",
                                   versions.front().analysis.Value())
                            .All();
    if (analysisRows.empty()) {
        throw NotFound{"GetAnalysisVersion: version's analysis is missing"};
    }
    return toView(versions.front(), std::string{analysisRows.front().name.Value().ToStringView()});
}

void AnalysisCatalogModel::attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
    _log = std::move(log);
    _entityKeyStr = std::move(entityKey);
}

template <typename Action, typename Result>
void AnalysisCatalogModel::logAction(const Action& action, const Result& result) const {
    if (!_log) {
        return;
    }
    ::morph::journal::LogEntry entry;
    entry.modelType = "AnalysisCatalogModel";
    entry.entityKey = _entityKeyStr.value_or(std::string{});
    entry.actionType = std::string{::morph::model::ActionTraits<Action>::typeId()};
    entry.payload = ::morph::model::ActionTraits<Action>::toJson(action);
    entry.result = ::morph::model::ActionTraits<Action>::resultToJson(result);
    entry.outcome = ::morph::journal::Outcome::Succeeded;
    if (const auto* ctx = ::morph::session::current()) {
        entry.principal = ctx->principal;
    }
    entry.timestampMs = nowMs();
    _log->append(std::move(entry));
    _log->flush();
}

}  // namespace lims
