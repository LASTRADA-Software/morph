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

/// @brief Maps a result row to its wire view.
/// @param row The result row.
/// @return The view a client sees.
[[nodiscard]] ResultView toResultView(const db::ResultRecord& row) {
    const auto num = row.valueNum.Value();
    const auto den = row.valueDen.Value();
    Concentration value;
    if (num.has_value() && den.has_value()) {
        value = Concentration{::morph::math::Rational{
            ::morph::math::Numerator{*num}, ::morph::math::Denominator{*den},
            ::morph::math::DecimalPlaces{static_cast<std::uint32_t>(row.valueDp.Value())}}};
    }
    return ResultView{
        .id = ResultId{static_cast<std::int64_t>(row.id.Value())},
        .sampleId = SampleId{static_cast<std::int64_t>(row.sample.Value())},
        .analysisVersionId = AnalysisVersionId{static_cast<std::int64_t>(row.analysisVersion.Value())},
        .qualifier = static_cast<ResultQualifier>(row.qualifier.Value()),
        .value = value,
        .capturedBy = std::string{row.capturedBy.Value().ToStringView()},
        .capturedAt = timestampFromMillis(row.capturedAt.Value()),
    };
}

/// @brief 10^@p places, for the precision check below.
///
/// `Rational` bounds its own `decimalPlaces` to `kMaxDecimalPlaces` (18, the
/// largest power of ten that fits an `int64_t`), so this cannot overflow for
/// any tag a decoded value can carry.
/// @param places Decimal places, in `[0, 18]`.
/// @return `10^places`.
[[nodiscard]] std::int64_t powerOfTen(std::uint32_t places) noexcept {
    std::int64_t result = 1;
    for (std::uint32_t i = 0; i < places; ++i) {
        result *= 10;
    }
    return result;
}

/// @brief Whether @p value is *exactly* representable at @p places decimals.
///
/// `Rational` keeps `gcd(|num|, den) == 1`, so `num * 10^places` is divisible
/// by `den` exactly when `10^places` is — no multiplication, and therefore no
/// overflow, is needed to decide it.
///
/// This is the app-level answer to the README's D1 (retag-vs-round,
/// upstream issue #159): `x-decimalPlaces` "enforcement" retags a value's
/// precision tag without changing the value, so a hand-built over-precise
/// payload would be *stored* as 1.23456 while every display of it said 1.235.
/// Display disagreeing with storage is disqualifying in a LIMS, so this rung
/// rejects the payload instead of retagging it. Rejecting is the one option
/// that cannot produce a stored number nobody ever sees.
/// @param value The decoded reading.
/// @param places The analysis version's declared decimal places.
/// @return `true` when the value needs no rounding at that precision.
[[nodiscard]] bool isExactAtDecimals(const ::morph::math::Rational& value, std::int32_t places) noexcept {
    if (places < 0 || static_cast<std::uint32_t>(places) > ::morph::math::kMaxDecimalPlaces) {
        return false;
    }
    return powerOfTen(static_cast<std::uint32_t>(places)) % value.denominator == 0;
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

ListResultQualifiersResult SampleModel::execute(const ListResultQualifiers& action) {
    static_cast<void>(action);
    // Served, not hardcoded in a client: the renderer discovers these through
    // the `x-optionsAction` the `QualifierChoice` field's own type declares.
    return ListResultQualifiersResult{.qualifiers = {
                                          {.id = std::string{kQualifierNotMeasured}, .name = "Not measured"},
                                          {.id = std::string{kQualifierBelowLod}, .name = "< LOD"},
                                          {.id = std::string{kQualifierAboveUdl}, .name = "> UDL"},
                                      }};
}

ResultView SampleModel::execute(const CaptureConcentration& action) {
    requirePrincipal();
    try {
        if (!action.validate()) {
            // `exactlyOneOf` failing is the interesting half: it is what makes
            // "a number *and* a below-LOD flag" unrepresentable rather than
            // merely discouraged.
            throw ValidationError{
                "CaptureConcentration: name a version and exactly one of value / qualifier"};
        }

        const auto sample = loadAttached();
        if (sample.state != SampleState::InProgress) {
            throw IllegalTransition{std::string{"results can only be captured while a sample is in-progress; this one is "} +
                                    std::string{stateName(sample.state)}};
        }

        Lightweight::DataMapper mapper;
        auto versions = mapper.Query<db::AnalysisVersionRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::AnalysisVersionRecord::id>, "=",
                                   *action.analysisVersionId)
                            .All();
        if (versions.empty()) {
            throw NotFound{"CaptureConcentration: no such analysis version"};
        }
        const auto& version = versions.front();

        // The action's unit is a *compile-time* template parameter, so the
        // only way a concentration action can be wrong about its unit is if
        // the definition it names is not a concentration. Checked rather than
        // assumed: storing an amps reading in the mg/L column would be
        // undetectable afterwards.
        const auto canonicalUnit = std::string{version.canonicalUnit.Value().ToStringView()};
        if (canonicalUnit != std::string{Concentration::unitMeta().id}) {
            throw ValidationError{"CaptureConcentration: analysis version " + std::to_string(*action.analysisVersionId) +
                                  " is denominated in " + canonicalUnit + ", not " +
                                  std::string{Concentration::unitMeta().id}};
        }

        auto qualifier = ResultQualifier::Measured;
        std::optional<::morph::math::Rational> stored;
        if (action.value.hasValue()) {
            const auto& reading = *action.value;
            if (!isExactAtDecimals(reading, version.decimalPlaces.Value())) {
                throw ValidationError{"CaptureConcentration: the reading carries more precision than analysis version " +
                                      std::to_string(*action.analysisVersionId) + " declares"};
            }
            stored = reading;
        } else {
            const auto decoded = qualifierFromCode(*action.qualifier);
            if (!decoded.has_value()) {
                // Fail-closed: an unknown code must not resolve to "we did not
                // look", which is a claim nobody made.
                throw ValidationError{"CaptureConcentration: unknown qualifier code '" + *action.qualifier + "'"};
            }
            qualifier = *decoded;
        }

        Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};

        // Re-capturing one analysis version replaces its previous answer; the
        // journal, not a second row, is where the superseded value lives.
        for (auto& existing : mapper.Query<db::ResultRecord>()
                                  .Where(::Lightweight::FieldNameOf<&db::ResultRecord::sample>, "=", *_sampleId)
                                  .Where(::Lightweight::FieldNameOf<&db::ResultRecord::analysisVersion>, "=",
                                         *action.analysisVersionId)
                                  .All()) {
            mapper.Delete(existing);
        }

        db::ResultRecord row;
        row.sample = static_cast<std::uint64_t>(*_sampleId);
        row.analysisVersion = static_cast<std::uint64_t>(*action.analysisVersionId);
        row.qualifier = static_cast<int>(qualifier);
        row.valueNum = stored ? std::optional{stored->numerator} : std::nullopt;
        row.valueDen = stored ? std::optional{stored->denominator} : std::nullopt;
        row.valueDp = stored ? static_cast<int>(stored->decimalPlaces.value) : 0;
        row.capturedBy = Lightweight::SqlAnsiString<64>{::morph::session::current()->principal};
        row.capturedAt = nowMillis();
        mapper.Create(row);

        // A new result changes what the sample says, so it moves the base
        // version an offline update targets — same reason a transition does.
        auto sampleRow = loadSample(mapper, *_sampleId);
        sampleRow.version = sampleRow.version.Value() + 1;
        mapper.Update(sampleRow);

        sqlTxn.Commit();

        auto view = toResultView(row);
        _journal.recordSuccess<SampleModel>(action, view, nowMillis());
        return view;
    } catch (const LimsError& error) {
        _journal.recordFailure<SampleModel>(action, error.what(), nowMillis());
        throw;
    }
}

ListResultsResult SampleModel::execute(const ListResults& action) {
    static_cast<void>(action);
    if (!_sampleId.has_value()) {
        throw NotFound{"this handler is not attached to a sample (execute OpenSample first)"};
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::ResultRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::ResultRecord::sample>, "=", *_sampleId)
                    .All();
    ListResultsResult result;
    result.results.reserve(rows.size());
    for (const auto& row : rows) {
        result.results.push_back(toResultView(row));
    }
    return result;
}

void SampleModel::attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
    _journal.attach(std::move(log), std::move(entityKey));
}

}  // namespace lims
