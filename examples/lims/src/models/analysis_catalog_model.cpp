// SPDX-License-Identifier: Apache-2.0
#include "lims/models/analysis_catalog_model.hpp"

#include "lims/core/errors.hpp"
#include "lims/core/model_support.hpp"
#include "lims/db/lims_entity.hpp"
#include "lims/dto/result_dto.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <morph/forms/forms.hpp>
#include <morph/session/session.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace lims {

namespace {

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


/// @brief Renders one bound as a `{"num":..,"den":..}` schema node.
/// @param bound The exact bound.
/// @return The DOM node.
[[nodiscard]] glz::generic_u64 boundNode(const AnalysisBound& bound) {
    glz::generic_u64 node{};
    node["num"] = bound.numerator;
    node["den"] = bound.denominator;
    return node;
}

/// @brief Merges @p version's own data into the compiled result-entry form.
///
/// This function is the whole of "clients render the version the result was
/// captured with", and its shape is the finding: the *structure* of the form
/// (which fields exist, which are required, the `x-rules` list, the field's
/// `x-decimalPlaces` and `ExtUnits`) comes from the compiled
/// `CaptureConcentration` struct and is the same for every version; only the
/// values patched in below vary. A version that wanted a different *shape* —
/// an extra field, a different rule — could not be served at all without
/// recompiling, which is exactly the "schemas become data" boundary the
/// README predicts.
///
/// The per-version precision is emitted as `x-versionDecimalPlaces` rather
/// than overwriting `x-decimalPlaces`: the latter is a contract the framework
/// itself enforces on dispatch (`docs/spec/forms/forms.md`, "Advertised
/// precision is enforced on dispatch") against the compiled
/// `Quantity`'s declared decimals, so rewriting it would advertise a promise
/// no code keeps. Both are served, and the disagreement is visible rather
/// than hidden.
/// @param version The version to render.
/// @return The schema text a client renders.
[[nodiscard]] std::string renderSchemaFor(const AnalysisVersionView& version) {
    const auto base = ::morph::forms::schemaJson<CaptureConcentration>();
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM requires operator[]
    glz::generic_u64 dom{};
    if (glz::read_json(dom, base)) {
        return base;  // not JSON we can patch; serve it verbatim rather than mangle it
    }
    auto& value = dom["properties"]["value"];
    value["x-versionDecimalPlaces"] = static_cast<std::int64_t>(version.decimalPlaces);
    if (version.specLow) {
        value["x-specLow"] = boundNode(*version.specLow);
    }
    if (version.specHigh) {
        value["x-specHigh"] = boundNode(*version.specHigh);
    }
    if (version.limitOfDetection) {
        value["x-limitOfDetection"] = boundNode(*version.limitOfDetection);
    }
    if (version.upperDetectionLimit) {
        value["x-upperDetectionLimit"] = boundNode(*version.upperDetectionLimit);
    }
    dom["x-analysisVersion"] = static_cast<std::int64_t>(version.version);
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return glz::write_json(dom).value_or(base);
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
    versionRow.createdAt = nowMillis();
    mapper.Create(versionRow);

    sqlTxn.Commit();

    DefineAnalysisResult result{
        .analysisId = AnalysisId{static_cast<std::int64_t>(analysisRow.id.Value())},
        .versionId = AnalysisVersionId{static_cast<std::int64_t>(versionRow.id.Value())},
        .version = 1,
    };
    _journal.recordSuccess<AnalysisCatalogModel>(action, result, nowMillis());
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
    versionRow.createdAt = nowMillis();
    mapper.Create(versionRow);
    sqlTxn.Commit();

    DefineAnalysisResult result{
        .analysisId = action.analysisId,
        .versionId = AnalysisVersionId{static_cast<std::int64_t>(versionRow.id.Value())},
        .version = highest + 1,
    };
    _journal.recordSuccess<AnalysisCatalogModel>(action, result, nowMillis());
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

AnalysisSchemaView AnalysisCatalogModel::execute(const GetAnalysisSchema& action) {
    if (!action.validate()) {
        throw ValidationError{"GetAnalysisSchema: versionId is required"};
    }
    const auto version = execute(GetAnalysisVersion{.versionId = action.versionId});
    if (version.canonicalUnit != std::string{Concentration::unitMeta().id}) {
        // Honest refusal rather than a plausible-looking wrong form. There is
        // one compiled result-entry action per *unit family*, not per
        // analysis, so a version denominated in anything but mg/L has no form
        // to serve — see the rung README's findings section.
        throw ValidationError{"GetAnalysisSchema: no compiled result-entry action for unit '" +
                              version.canonicalUnit + "'"};
    }
    return AnalysisSchemaView{
        .versionId = version.id,
        .version = version.version,
        .schemaJson = renderSchemaFor(version),
    };
}

void AnalysisCatalogModel::attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
    _journal.attach(std::move(log), std::move(entityKey));
}

}  // namespace lims
