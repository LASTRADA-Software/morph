// SPDX-License-Identifier: Apache-2.0
#include "lims/models/sample_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <morph/session/session.hpp>
#include <string>
#include <utility>

#include "lims/core/errors.hpp"
#include "lims/core/model_support.hpp"
#include "lims/db/lims_entity.hpp"

namespace lims {

namespace {

/// @brief Maps a row to its wire view.
/// @param row The sample row.
/// @return The view a client sees.
[[nodiscard]] SampleView toView(const db::SampleRecord& row) {
    return SampleView{
        .id = SampleId{static_cast<std::int64_t>(row.id.Value())},
        .clientId = ClientId{static_cast<std::int64_t>(row.client.Value())},
        .reference = std::string{row.reference.Value().ToStringView()},
        .state = static_cast<SampleState>(row.state.Value()),
        .version = SampleVersion{static_cast<std::int64_t>(row.version.Value())},
        .registeredAt = timestampFromMillis(row.registeredAt.Value()),
    };
}

/// @brief Loads one sample row by id.
/// @param mapper The open data mapper.
/// @param sampleId The row's primary key.
/// @return The row.
/// @throws NotFound if no such row exists.
[[nodiscard]] db::SampleRecord loadSample(Lightweight::DataMapper& mapper, std::int64_t sampleId) {
    auto rows =
        mapper.Query<db::SampleRecord>().Where(::Lightweight::FieldNameOf<&db::SampleRecord::id>, "=", sampleId).All();
    if (rows.empty()) {
        throw NotFound{"sample " + std::to_string(sampleId) + " does not exist"};
    }
    return rows.front();
}

}  // namespace

RegisterClientResult SampleModel::execute(const RegisterClient& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"RegisterClient: a name is required"};
    }

    Lightweight::DataMapper mapper;
    db::ClientRecord row;
    row.name = Lightweight::SqlAnsiString<128>{action.name};
    mapper.Create(row);

    RegisterClientResult result{.clientId = ClientId{static_cast<std::int64_t>(row.id.Value())}};
    _journal.recordSuccess<SampleModel>(action, result, nowMillis());
    return result;
}

SampleView SampleModel::execute(const RegisterSample& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"RegisterSample: a client and a reference are required"};
    }

    Lightweight::DataMapper mapper;
    auto clients = mapper.Query<db::ClientRecord>()
                       .Where(::Lightweight::FieldNameOf<&db::ClientRecord::id>, "=", *action.clientId)
                       .All();
    if (clients.empty()) {
        throw NotFound{"RegisterSample: no such client"};
    }

    db::SampleRecord row;
    row.client = clients.front();
    row.reference = Lightweight::SqlAnsiString<64>{action.reference};
    row.state = static_cast<int>(SampleState::Registered);
    row.version = 1;
    row.registeredAt = nowMillis();
    mapper.Create(row);

    // Attach to what we just created, and re-stamp the journal's identity:
    // entries written before this point could not name a sample that did not
    // exist yet.
    _sampleId = static_cast<std::int64_t>(row.id.Value());
    _journal.rekey(std::to_string(*_sampleId));

    auto view = toView(row);
    _journal.recordSuccess<SampleModel>(action, view, nowMillis());
    return view;
}

SampleView SampleModel::execute(const OpenSample& action) {
    if (!action.validate()) {
        throw ValidationError{"OpenSample: a sampleId is required"};
    }
    Lightweight::DataMapper mapper;
    auto row = loadSample(mapper, *action.sampleId);
    _sampleId = *action.sampleId;
    _journal.rekey(std::to_string(*_sampleId));
    return toView(row);
}

SampleView SampleModel::execute(const GetSample& action) {
    static_cast<void>(action);
    return loadAttached();
}

SampleView SampleModel::execute(const ReceiveSample& action) { return transition(action, SampleState::Received); }

SampleView SampleModel::execute(const StartWork& action) { return transition(action, SampleState::InProgress); }

SampleView SampleModel::execute(const SubmitForVerification& action) {
    return transition(action, SampleState::ToBeVerified);
}

SampleView SampleModel::execute(const ReturnForRework& action) { return transition(action, SampleState::InProgress); }

SampleView SampleModel::execute(const PublishSample& action) { return transition(action, SampleState::Published); }

SampleView SampleModel::execute(const RejectSample& action) { return transition(action, SampleState::Rejected); }

SampleView SampleModel::loadAttached() const {
    if (!_sampleId.has_value()) {
        throw NotFound{"this handler is not attached to a sample (execute OpenSample first)"};
    }
    Lightweight::DataMapper mapper;
    return toView(loadSample(mapper, *_sampleId));
}

SampleView SampleModel::writeState(SampleState target) const {
    Lightweight::DataMapper mapper;
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    auto row = loadSample(mapper, *_sampleId);
    row.state = static_cast<int>(target);
    // The base version is what an offline update targets (README §7), so it
    // moves with the state and inside the same transaction: a reader must
    // never see the new state at the old version.
    row.version = row.version.Value() + 1;
    mapper.Update(row);
    sqlTxn.Commit();
    return toView(row);
}

template <typename Action>
SampleView SampleModel::transition(const Action& action, SampleState target) {
    // Outside the try, deliberately: an attempt with no authenticated
    // principal must produce **no** journal entry at all. An audit entry
    // naming nobody is exactly what this rung's README calls disqualifying,
    // so the one failure class that cannot be attributed is also the one
    // failure class that is not recorded.
    requirePrincipal();
    try {
        if (!action.validate()) {
            throw ValidationError{std::string{::morph::model::ActionTraits<Action>::typeId()} +
                                  ": the action is not well-formed"};
        }
        const auto before = loadAttached();
        if (!isLegalTransition(before.state, target)) {
            throw IllegalTransition{std::string{"a "} + std::string{stateName(before.state)} +
                                    " sample cannot become " + std::string{stateName(target)}};
        }
        auto after = writeState(target);
        _journal.recordSuccess<SampleModel>(action, after, nowMillis());
        return after;
    } catch (const LimsError& error) {
        // The rejected attempt is itself audit-worthy: "who tried to publish
        // an unverified sample" is precisely the question a 21 CFR Part
        // 11-style trail exists to answer, and an entry that is never written
        // cannot answer it.
        _journal.recordFailure<SampleModel>(action, error.what(), nowMillis());
        throw;
    }
}

void SampleModel::attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
    _journal.attach(std::move(log), std::move(entityKey));
}

}  // namespace lims
