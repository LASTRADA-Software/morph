// SPDX-License-Identifier: Apache-2.0
//
// Sample entry: the working copy a record-editing screen edits section by section, its snapshots, the dilution
// series computed from it, the project lookup, and the save that commits it.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/session/session.hpp>
#include <morph/table/query.hpp>
#include <string>
#include <utility>
#include <vector>

#include "lims/core/errors.hpp"
#include "lims/core/model_support.hpp"
#include "lims/db/lims_entity.hpp"
#include "lims/models/sample_model.hpp"

namespace lims {

namespace {

/// @brief The projects a sample can be filed under. A fixed list: the lookup table is what the example exercises,
///        not project administration.
[[nodiscard]] std::vector<ProjectRow> const& projects() {
    static std::vector<ProjectRow> const rows{
        {.id = 1, .code = "WAT-24", .name = "Drinking water 2024"},
        {.id = 2, .code = "ENV-17", .name = "River monitoring"},
        {.id = 3, .code = "IND-03", .name = "Industrial effluent"},
        {.id = 4, .code = "GW-09", .name = "Groundwater survey"},
        {.id = 5, .code = "SOIL-11", .name = "Soil leachate"},
    };
    return rows;
}

/// @brief The project with @p id, or `nullptr`.
/// @param id The project's id.
/// @return The project.
[[nodiscard]] ProjectRow const* findProject(std::int64_t id) {
    auto const& rows = projects();
    auto const found = std::ranges::find(rows, id, &ProjectRow::id);
    return found == rows.end() ? nullptr : &*found;
}

/// @brief The stored entry row of @p sampleId, if it has one.
/// @param mapper   The open data mapper.
/// @param sampleId The sample.
/// @return The rows; at most one.
[[nodiscard]] std::vector<db::SampleEntryRecord> entryRows(Lightweight::DataMapper& mapper, std::int64_t sampleId) {
    return mapper.Query<db::SampleEntryRecord>()
        .Where(::Lightweight::FieldNameOf<&db::SampleEntryRecord::sampleId>, "=", sampleId)
        .All();
}

/// @brief The measurements view of a working copy.
/// @param draft The working copy.
/// @return The view.
[[nodiscard]] MeasurementsView measurementsOf(SampleDraft const& draft) {
    return MeasurementsView{.ntu = draft.ntu,
                            .nitrate = draft.nitrate,
                            .hasTurbidity = draft.ntu.hasValue(),
                            .hasNitrate = draft.nitrate.hasValue()};
}

/// @brief A whole number of percent, at the recovery's precision.
/// @param percent The percentage.
/// @return The recovery.
[[nodiscard]] RecoveryPercent percent(std::int64_t percent) {
    return RecoveryPercent{::morph::math::Rational{percent, ::morph::math::DecimalPlaces{1}}};
}

/// @brief The dilution series of a sampling section: each step's nominal concentration, and the recovery of the
///        steps measured so far, exactly.
/// @param sampling The sampling section.
/// @return The series; empty without a spike.
[[nodiscard]] DilutionSeries seriesOf(SamplingSection const& sampling) {
    DilutionSeries series;
    if (!sampling.spike.hasValue()) {
        return series;
    }
    std::array<Concentration const*, kDilutionFactors.size()> const measured{&sampling.measured1, &sampling.measured2,
                                                                             &sampling.measured3, &sampling.measured4};
    ::morph::math::Rational const hundred{100, ::morph::math::DecimalPlaces{0}};
    for (std::size_t step = 0; step < kDilutionFactors.size(); ++step) {
        std::int64_t const factor = kDilutionFactors.at(step);
        SeriesRow row{.step = static_cast<std::int64_t>(step) + 1,
                      .factor = factor,
                      .nominal = (sampling.spike / ::morph::math::Rational{factor, ::morph::math::DecimalPlaces{0}})
                                     .atDeclaredPrecision(),
                      .measured = *measured.at(step),
                      .recoveryPct = {},
                      .limitMin = percent(kRecoveryMinPercent),
                      .limitMax = percent(kRecoveryMaxPercent),
                      .outsideLimits = false};
        if (row.measured.hasValue() && *row.nominal != ::morph::math::Rational{}) {
            auto const share = *row.measured / *row.nominal;
            if (share) {
                row.recoveryPct = RecoveryPercent{*share * hundred}.atDeclaredPrecision();
                row.outsideLimits = *row.recoveryPct < *row.limitMin || *row.recoveryPct > *row.limitMax;
            }
        }
        series.rows.push_back(std::move(row));
    }
    return series;
}

}  // namespace

SampleDraft& SampleModel::draft() {
    if (!_draft) {
        throw NotFound{"no working copy: create or load a sample first"};
    }
    return *_draft;
}

HeaderView SampleModel::headerView() const {
    HeaderView view;
    if (_sampleId) {
        view.sampleId = SampleId{*_sampleId};
        view.sampleNumber = "S-" + std::to_string(*_sampleId);
    }
    if (_draft) {
        view.clientId = _draft->header.clientId;
        view.reference = _draft->header.reference;
        view.receivedOn = _draft->header.receivedOn;
        view.remarks = _draft->header.remarks;
        view.projectId = _draft->projectId;
        if (ProjectRow const* const project = findProject(_draft->projectId)) {
            view.projectName = project->name;
        }
    }
    return view;
}

HeaderView SampleModel::execute(const NewSample& action) {
    static_cast<void>(action);
    _draft = SampleDraft{};
    _sampleId.reset();
    return headerView();
}

HeaderView SampleModel::execute(const LoadSample& action) {
    if (!action.validate()) {
        throw ValidationError{"LoadSample: a sample id is required"};
    }
    Lightweight::DataMapper mapper;
    auto samples = mapper.Query<db::SampleRecord>()
                       .Where(::Lightweight::FieldNameOf<&db::SampleRecord::id>, "=", *action.id)
                       .All();
    if (samples.empty()) {
        throw NotFound{"sample " + std::to_string(*action.id) + " does not exist"};
    }
    db::SampleRecord const& row = samples.front();
    SampleDraft loaded;
    if (auto entries = entryRows(mapper, *action.id); !entries.empty()) {
        std::string const payload{entries.front().payload.Value().ToStringView()};
        if (auto const error = glz::read_json(loaded, payload); error) {
            throw ValidationError{"sample " + std::to_string(*action.id) + ": its stored entry does not decode"};
        }
    }
    // The sample row is the source of truth for who it belongs to and its reference.
    loaded.header.clientId = ClientId{static_cast<std::int64_t>(row.client.Value())};
    loaded.header.reference = std::string{row.reference.Value().ToStringView()};
    _draft = std::move(loaded);
    _sampleId = *action.id;
    _journal.rekey(std::to_string(*_sampleId));
    return headerView();
}

SaveSampleResult SampleModel::execute(const SaveSample& action) {
    if (!_draft) {
        return {.error = SaveError::noWorkingCopy, .sampleId = {}};
    }
    HeaderSection const& header = _draft->header;
    if (!header.clientId.hasValue()) {
        return {.error = SaveError::noClient, .sampleId = {}};
    }
    if (header.reference.empty()) {
        return {.error = SaveError::noReference, .sampleId = {}};
    }
    if (findProject(_draft->projectId) == nullptr) {
        return {.error = SaveError::noProject, .sampleId = {}};
    }
    requirePrincipal();

    Lightweight::DataMapper mapper;
    auto clients = mapper.Query<db::ClientRecord>()
                       .Where(::Lightweight::FieldNameOf<&db::ClientRecord::id>, "=", *header.clientId)
                       .All();
    if (clients.empty()) {
        throw NotFound{"SaveSample: no such client"};
    }
    std::string payload;
    if (auto const error = glz::write_json(*_draft, payload); error) {
        throw ValidationError{"SaveSample: the working copy does not encode"};
    }

    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    std::int64_t sampleId = 0;
    if (_sampleId) {
        auto samples = mapper.Query<db::SampleRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::SampleRecord::id>, "=", *_sampleId)
                           .All();
        if (samples.empty()) {
            throw NotFound{"sample " + std::to_string(*_sampleId) + " does not exist"};
        }
        db::SampleRecord& row = samples.front();
        row.client = clients.front();
        row.reference = Lightweight::SqlAnsiString<64>{header.reference};
        mapper.Update(row);
        sampleId = *_sampleId;
    } else {
        db::SampleRecord row;
        row.client = clients.front();
        row.reference = Lightweight::SqlAnsiString<64>{header.reference};
        row.state = static_cast<int>(SampleState::Registered);
        row.version = 1;
        row.registeredAt = nowMillis();
        mapper.Create(row);
        sampleId = static_cast<std::int64_t>(row.id.Value());
    }
    auto entries = entryRows(mapper, sampleId);
    if (entries.empty()) {
        db::SampleEntryRecord entry;
        entry.sampleId = sampleId;
        entry.payload = Lightweight::SqlDynamicAnsiString<4096>{payload};
        mapper.Create(entry);
    } else {
        entries.front().payload = Lightweight::SqlDynamicAnsiString<4096>{payload};
        mapper.Update(entries.front());
    }
    sqlTxn.Commit();

    if (!_sampleId) {
        _sampleId = sampleId;
        _journal.rekey(std::to_string(sampleId));
    }
    SaveSampleResult result{.error = SaveError::none, .sampleId = SampleId{sampleId}};
    _journal.recordSuccess<SampleModel>(action, result, nowMillis());
    return result;
}

DeleteSampleResult SampleModel::execute(const DeleteSample& action) {
    requirePrincipal();
    if (!_sampleId) {
        throw NotFound{"DeleteSample: the working copy was never saved"};
    }
    Lightweight::DataMapper mapper;
    auto samples = mapper.Query<db::SampleRecord>()
                       .Where(::Lightweight::FieldNameOf<&db::SampleRecord::id>, "=", *_sampleId)
                       .All();
    if (samples.empty()) {
        throw NotFound{"sample " + std::to_string(*_sampleId) + " does not exist"};
    }
    // A sample that has been received is part of the lab's record; only a registration can be withdrawn.
    if (static_cast<SampleState>(samples.front().state.Value()) != SampleState::Registered) {
        std::string const error = "DeleteSample: only a registered sample can be deleted";
        _journal.recordFailure<SampleModel>(action, error, nowMillis());
        throw IllegalTransition{error};
    }
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    mapper.Query<db::SampleEntryRecord>()
        .Where(::Lightweight::FieldNameOf<&db::SampleEntryRecord::sampleId>, "=", *_sampleId)
        .Delete();
    mapper.Delete(samples.front());
    sqlTxn.Commit();

    DeleteSampleResult result{.sampleId = SampleId{*_sampleId}};
    _journal.recordSuccess<SampleModel>(action, result, nowMillis());
    _sampleId.reset();
    _draft.reset();
    return result;
}

HeaderView SampleModel::execute(const HeaderSection& action) {
    draft().header = action;
    return headerView();
}

DilutionSeries SampleModel::execute(const SamplingSection& action) {
    draft().sampling = action;
    return seriesOf(action);
}

MeasurementsView SampleModel::execute(const TurbiditySection& action) {
    draft().ntu = action.ntu;
    return measurementsOf(*_draft);
}

MeasurementsView SampleModel::execute(const NitrateSection& action) {
    draft().nitrate = action.nitrate;
    return measurementsOf(*_draft);
}

MeasurementsView SampleModel::execute(const ClearOptionalMeasurement& action) {
    if (!action.validate()) {
        throw ValidationError{"ClearOptionalMeasurement: `" + action.which + "` is not an optional measurement"};
    }
    SampleDraft& copy = draft();
    if (action.which == kTurbidity) {
        copy.ntu = Turbidity{};
    } else {
        copy.nitrate = Concentration{};
    }
    return measurementsOf(copy);
}

HeaderView SampleModel::execute(const GetHeader& action) {
    static_cast<void>(action);
    static_cast<void>(draft());
    return headerView();
}

MeasurementsView SampleModel::execute(const GetMeasurements& action) {
    static_cast<void>(action);
    return measurementsOf(draft());
}

DilutionSeries SampleModel::execute(const GetDilutionSeries& action) {
    static_cast<void>(action);
    return seriesOf(draft().sampling);
}

::morph::table::Page<ProjectRow> SampleModel::execute(const ListProjects& action) {
    auto page = ::morph::table::apply<ProjectRow>(
        projects(), [](ProjectRow const& row) { return ::morph::table::RowId{row.id}; }, action.query);
    if (!page) {
        throw ValidationError{"ListProjects: " + page.error().message};
    }
    return std::move(*page);
}

HeaderView SampleModel::execute(const SelectProject& action) {
    SampleDraft& copy = draft();
    if (findProject(action.projectId) == nullptr) {
        throw NotFound{"project " + std::to_string(action.projectId) + " does not exist"};
    }
    copy.projectId = action.projectId;
    return headerView();
}

}  // namespace lims
