// SPDX-License-Identifier: Apache-2.0
#include "lims/models/sample_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <morph/session/session.hpp>
#include <algorithm>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

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

/// @brief Maps a conflict row to its wire view.
/// @param row The conflict row.
/// @return The view a resolver sees.
[[nodiscard]] ConflictView toConflictView(const db::OfflineConflictRecord& row) {
    return ConflictView{
        .id = ConflictId{static_cast<std::int64_t>(row.id.Value())},
        .sampleId = SampleId{static_cast<std::int64_t>(row.sample.Value())},
        .baseVersion = SampleVersion{static_cast<std::int64_t>(row.baseVersion.Value())},
        .serverVersion = SampleVersion{static_cast<std::int64_t>(row.serverVersion.Value())},
        .reason = static_cast<ConflictReason>(row.reason.Value()),
        .status = static_cast<ConflictStatus>(row.status.Value()),
        .payload = std::string{row.payload.Value().ToStringView()},
        .detectedBy = std::string{row.detectedBy.Value().ToStringView()},
        .detectedAt = timestampFromMillis(row.detectedAt.Value()),
        .resolvedBy = std::string{row.resolvedBy.Value().ToStringView()},
        .resolutionNote = std::string{row.resolutionNote.Value().ToStringView()},
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

/// @brief Whether @p actionType is one of the lifecycle transitions, whose
///        recorded result is a `SampleView`.
/// @param actionType The recorded action id.
/// @return `true` for an action whose result carries a sample state.
[[nodiscard]] bool isLifecycleAction(std::string_view actionType) {
    return actionType == "RegisterSample" || actionType == "ReceiveSample" || actionType == "StartWork" ||
           actionType == "SubmitForVerification" || actionType == "ReturnForRework" ||
           actionType == "PublishSample" || actionType == "RejectSample";
}

/// @brief Reconstructs one audit step from one journal entry.
///
/// The whole of "reconstructible from the journal alone", and the reason it is
/// a `switch`-like chain over a *closed* set of action ids rather than a
/// generic decode: an entry this build does not recognise, or whose result
/// does not decode, becomes an `Unreadable` step. It is never skipped.
///
/// Skipping is the tempting implementation and the wrong one. An audit trail
/// that quietly omits what it could not read looks complete, so nobody
/// discovers the omission — which is exactly how the rung README's
/// "journal payload evolution" strain point turns "reconstructible from the
/// journal alone" into a false claim. An admitted gap can at least be
/// investigated.
/// @param entry The recorded entry.
/// @return The reconstructed step.
[[nodiscard]] AuditStep auditStepFor(const ::morph::journal::LogEntry& entry) {
    AuditStep step{
        .at = timestampFromMillis(entry.timestampMs),
        .principal = entry.principal,
        .action = entry.actionType,
        .kind = AuditStepKind::Unreadable,
        .outcome = entry.outcome == ::morph::journal::Outcome::Succeeded ? AuditOutcome::Succeeded
                                                                        : AuditOutcome::Refused,
        .state = SampleState::Registered,
        .version = SampleVersion{},
        .detail = entry.error,
    };

    // A refused attempt is fully reconstructible from the entry itself: the
    // action, the author, the instant, and why it was refused. There is no
    // result to decode, so the kind is settled here.
    if (step.outcome == AuditOutcome::Refused) {
        step.kind = isLifecycleAction(entry.actionType) ? AuditStepKind::LifecycleTransition
                                                        : AuditStepKind::Unreadable;
        if (step.kind == AuditStepKind::Unreadable && !entry.actionType.empty()) {
            // A recognised non-lifecycle action that was refused is still a
            // readable step, just not a lifecycle one.
            if (entry.actionType == "CaptureConcentration") {
                step.kind = AuditStepKind::ResultCaptured;
            } else if (entry.actionType == "VerifyResult") {
                step.kind = AuditStepKind::ResultVerified;
            } else if (entry.actionType == "QueuedCapture") {
                step.kind = AuditStepKind::OfflineReplay;
            } else if (entry.actionType == "ResolveConflict") {
                step.kind = AuditStepKind::ConflictResolved;
            } else if (entry.actionType == "GrantRole") {
                step.kind = AuditStepKind::RoleGranted;
            }
        }
        return step;
    }

    try {
        if (isLifecycleAction(entry.actionType)) {
            const auto view = ::morph::model::ActionTraits<ReceiveSample>::resultFromJson(entry.result);
            step.kind = AuditStepKind::LifecycleTransition;
            step.state = view.state;
            step.version = view.version;
        } else if (entry.actionType == "CaptureConcentration") {
            static_cast<void>(::morph::model::ActionTraits<CaptureConcentration>::resultFromJson(entry.result));
            step.kind = AuditStepKind::ResultCaptured;
        } else if (entry.actionType == "VerifyResult") {
            const auto view = ::morph::model::ActionTraits<VerifyResult>::resultFromJson(entry.result);
            step.kind = AuditStepKind::ResultVerified;
            step.detail = view.capturedBy;
        } else if (entry.actionType == "QueuedCapture") {
            const auto replay = ::morph::model::ActionTraits<QueuedCapture>::resultFromJson(entry.result);
            step.kind = AuditStepKind::OfflineReplay;
            step.version = replay.serverVersion;
            step.detail = replay.outcome == ReplayOutcome::Conflicted ? std::string{"conflicted"}
                                                                      : std::string{"applied"};
        } else if (entry.actionType == "ResolveConflict") {
            static_cast<void>(::morph::model::ActionTraits<ResolveConflict>::resultFromJson(entry.result));
            step.kind = AuditStepKind::ConflictResolved;
        } else if (entry.actionType == "GrantRole") {
            static_cast<void>(::morph::model::ActionTraits<GrantRole>::resultFromJson(entry.result));
            step.kind = AuditStepKind::RoleGranted;
        } else {
            step.detail = "unrecognised action id '" + entry.actionType + "'";
        }
    } catch (const std::exception& error) {
        step.kind = AuditStepKind::Unreadable;
        step.detail = std::string{"result payload did not decode: "} + error.what();
    }
    return step;
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
        if constexpr (std::is_same_v<Action, PublishSample>) {
            // A four-eyes control nothing depends on is theatre. Publishing is
            // the point at which the lab tells the client a number is true, so
            // it is the point that has to insist every number in the report
            // has been through a second pair of eyes.
            requireEveryResultVerified();
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

ResultView SampleModel::applyCapture(SampleId sampleId, const CaptureConcentration& capture,
                                     const std::string& author) {
    if (!capture.validate()) {
        // `exactlyOneOf` failing is the interesting half: it is what makes
        // "a number *and* a below-LOD flag" unrepresentable rather than
        // merely discouraged.
        throw ValidationError{"CaptureConcentration: name a version and exactly one of value / qualifier"};
    }

    Lightweight::DataMapper mapper;
    const auto sample = toView(loadSample(mapper, *sampleId));
    if (sample.state != SampleState::InProgress) {
        throw IllegalTransition{
            std::string{"results can only be captured while a sample is in-progress; this one is "} +
            std::string{stateName(sample.state)}};
    }

    auto versions = mapper.Query<db::AnalysisVersionRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::AnalysisVersionRecord::id>, "=",
                               *capture.analysisVersionId)
                        .All();
    if (versions.empty()) {
        throw NotFound{"CaptureConcentration: no such analysis version"};
    }
    const auto& version = versions.front();

    // The action's unit is a *compile-time* template parameter, so the only
    // way a concentration action can be wrong about its unit is if the
    // definition it names is not a concentration. Checked rather than
    // assumed: storing an amps reading in the mg/L column would be
    // undetectable afterwards.
    const auto canonicalUnit = std::string{version.canonicalUnit.Value().ToStringView()};
    if (canonicalUnit != std::string{Concentration::unitMeta().id}) {
        throw ValidationError{"CaptureConcentration: analysis version " + std::to_string(*capture.analysisVersionId) +
                              " is denominated in " + canonicalUnit + ", not " +
                              std::string{Concentration::unitMeta().id}};
    }

    // The conditional field, made load-bearing (README §5). `validate()` has
    // already established that a `diluted` preparation carries a factor -- the
    // client learns the same rule from `x-rules` in the served schema and the
    // server re-runs the compiled one -- so all that is left here is the
    // domain arithmetic and the one check a cross-field rule cannot express:
    // a factor must be a positive number.
    auto dilutionMultiplier = ::morph::math::Rational::one(::morph::math::DecimalPlaces{3});
    if (capture.dilution.hasValue() && *capture.dilution == std::string{kDilutionDiluted}) {
        if (!capture.dilutionFactor.hasValue() || (*capture.dilutionFactor).numerator <= 0) {
            throw ValidationError{"CaptureConcentration: a diluted aliquot needs a positive dilution factor"};
        }
        dilutionMultiplier = *capture.dilutionFactor;
    } else if (capture.dilution.hasValue() && *capture.dilution != std::string{kDilutionNeat}) {
        // Fail-closed on an unknown preparation code, for the same reason the
        // qualifier codes are: guessing turns "this client speaks a dialect we
        // do not know" into a silently undiluted reading.
        throw ValidationError{"CaptureConcentration: unknown dilution mode '" + *capture.dilution + "'"};
    }

    auto qualifier = ResultQualifier::Measured;
    std::optional<::morph::math::Rational> stored;
    if (capture.value.hasValue()) {
        // Exact: the reported concentration accounts for the dilution, and it
        // does so through `Rational`, never a `double`.
        const auto reading = *capture.value * dilutionMultiplier;
        if (!isExactAtDecimals(reading, version.decimalPlaces.Value())) {
            throw ValidationError{"CaptureConcentration: the reading carries more precision than analysis version " +
                                  std::to_string(*capture.analysisVersionId) + " declares"};
        }
        stored = reading;
    } else {
        const auto decoded = qualifierFromCode(*capture.qualifier);
        if (!decoded.has_value()) {
            // Fail-closed: an unknown code must not resolve to "we did not
            // look", which is a claim nobody made.
            throw ValidationError{"CaptureConcentration: unknown qualifier code '" + *capture.qualifier + "'"};
        }
        qualifier = *decoded;
    }

    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};

    // Re-capturing one analysis version replaces its previous answer; the
    // journal, not a second row, is where the superseded value lives.
    for (auto& existing : mapper.Query<db::ResultRecord>()
                              .Where(::Lightweight::FieldNameOf<&db::ResultRecord::sample>, "=", *sampleId)
                              .Where(::Lightweight::FieldNameOf<&db::ResultRecord::analysisVersion>, "=",
                                     *capture.analysisVersionId)
                              .All()) {
        mapper.Delete(existing);
    }

    db::ResultRecord row;
    row.sample = static_cast<std::uint64_t>(*sampleId);
    row.analysisVersion = static_cast<std::uint64_t>(*capture.analysisVersionId);
    row.qualifier = static_cast<int>(qualifier);
    row.valueNum = stored ? std::optional{stored->numerator} : std::nullopt;
    row.valueDen = stored ? std::optional{stored->denominator} : std::nullopt;
    row.valueDp = stored ? static_cast<int>(stored->decimalPlaces.value) : 0;
    row.capturedBy = Lightweight::SqlAnsiString<64>{author};
    row.capturedAt = nowMillis();
    mapper.Create(row);

    // A new result changes what the sample says, so it moves the base version
    // an offline update targets — same reason a transition does.
    auto sampleRow = loadSample(mapper, *sampleId);
    sampleRow.version = sampleRow.version.Value() + 1;
    mapper.Update(sampleRow);

    sqlTxn.Commit();
    return toResultView(row);
}

ListDilutionModesResult SampleModel::execute(const ListDilutionModes& action) {
    static_cast<void>(action);
    return ListDilutionModesResult{.modes = {
                                       {.id = std::string{kDilutionNeat}, .name = "Neat"},
                                       {.id = std::string{kDilutionDiluted}, .name = "Diluted"},
                                   }};
}

ResultView SampleModel::execute(const CaptureConcentration& action) {
    requirePrincipal();
    if (!_sampleId.has_value()) {
        throw NotFound{"this handler is not attached to a sample (execute OpenSample first)"};
    }
    try {
        auto view = applyCapture(SampleId{*_sampleId}, action, ::morph::session::current()->principal);
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


bool SampleModel::alreadyDecided(const std::string& opKey) {
    if (opKey.empty()) {
        return false;
    }
    Lightweight::DataMapper mapper;
    return !mapper.Query<db::ReplayedOpRecord>()
                .Where(::Lightweight::FieldNameOf<&db::ReplayedOpRecord::opKey>, "=", opKey)
                .All()
                .empty();
}

namespace {

/// @brief Records @p opKey as decided, so a redelivery is skipped.
///
/// A free function rather than a member: it touches no state of its own, and
/// keeping `Lightweight::DataMapper` out of `sample_model.hpp` is what lets
/// the client link. `ladder_lims_gui_lib` includes that header (through the
/// presenters) and is built for wasm under `MORPH_CLIENT_ONLY`, where the ORM
/// does not exist -- so a single mapper-typed declaration in the header broke
/// the WASM client build for the whole rung. Every other rung's model header
/// is ORM-free for the same reason.
void markDecided(Lightweight::DataMapper& mapper, const std::string& opKey) {
    db::ReplayedOpRecord row;
    row.opKey = Lightweight::SqlAnsiString<128>{opKey};
    row.decidedAt = nowMillis();
    mapper.Create(row);
}

}  // namespace

ReplayCaptureResult SampleModel::execute(const QueuedCapture& action) {
    requirePrincipal();
    try {
        if (!action.validate()) {
            throw ValidationError{"QueuedCapture: a sample, an author, an operation key and a usable capture are required"};
        }
        const auto& principal = ::morph::session::current()->principal;
        if (action.capturedBy != principal) {
            // A queued reading is replayed *as* the operator who took it, and
            // nobody else. Without this, any client able to reach the server
            // could file a lab result under a colleague's name -- which is the
            // one thing a 21 CFR Part 11-style trail exists to prevent.
            throw Forbidden{"QueuedCapture: replay must run as the capturing operator ('" + action.capturedBy +
                            "'), not '" + principal + "'"};
        }

        // At-most-once, enforced here because the queue is documented not to
        // enforce it -- and because the shipped queues disagree about whether
        // they do anyway (docs/findings/007).
        if (alreadyDecided(*action.operationKey)) {
            return ReplayCaptureResult{.outcome = ReplayOutcome::Skipped, .sampleId = action.sampleId};
        }

        Lightweight::DataMapper mapper;
        const auto sample = toView(loadSample(mapper, *action.sampleId));
        const auto serverVersion = SampleVersion{*sample.version};

        // The two ways a queued update can be out of date, in the order that
        // makes the flagged reason the *most specific* true one: a sample that
        // has moved on in the lifecycle has also moved on in version, and
        // "somebody published this while you were away" is a more actionable
        // thing to tell a human than "your base was stale".
        std::optional<ConflictReason> reason;
        if (sample.state != SampleState::InProgress) {
            reason = ConflictReason::LifecycleClosed;
        } else if (*action.baseVersion != *serverVersion) {
            reason = ConflictReason::StaleBase;
        }

        if (reason.has_value()) {
            Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
            db::OfflineConflictRecord row;
            row.sample = static_cast<std::uint64_t>(*action.sampleId);
            row.baseVersion = static_cast<std::int32_t>(*action.baseVersion);
            row.serverVersion = static_cast<std::int32_t>(*serverVersion);
            row.reason = static_cast<int>(*reason);
            row.status = static_cast<int>(ConflictStatus::Open);
            // Verbatim, not re-encoded from the decoded struct: re-encoding
            // would normalise away anything this build no longer understands,
            // which is exactly the journal-payload-evolution failure the rung
            // README warns about.
            row.payload = Lightweight::SqlDynamicAnsiString<4096>{
                ::morph::model::ActionTraits<QueuedCapture>::toJson(action)};
            row.detectedBy = Lightweight::SqlAnsiString<64>{principal};
            row.detectedAt = nowMillis();
            row.resolvedBy = Lightweight::SqlAnsiString<64>{std::string{}};
            row.resolvedAt = 0;
            row.resolutionNote = Lightweight::SqlAnsiString<255>{std::string{}};
            mapper.Create(row);
            // Flagging is terminal for the queued item: the conflict row owns
            // it now, so a redelivery must not raise a second flag about the
            // same edit.
            markDecided(mapper, *action.operationKey);
            sqlTxn.Commit();

            ReplayCaptureResult flagged{
                .outcome = ReplayOutcome::Conflicted,
                .sampleId = action.sampleId,
                .baseVersion = action.baseVersion,
                .serverVersion = serverVersion,
                .conflictId = ConflictId{static_cast<std::int64_t>(row.id.Value())},
                .reason = *reason,
            };
            _journal.recordSuccess<SampleModel>(action, flagged, nowMillis());
            return flagged;
        }

        static_cast<void>(applyCapture(action.sampleId, action.capture, action.capturedBy));
        Lightweight::DataMapper keyMapper;
        markDecided(keyMapper, *action.operationKey);

        ReplayCaptureResult applied{
            .outcome = ReplayOutcome::Applied,
            .sampleId = action.sampleId,
            .baseVersion = action.baseVersion,
            .serverVersion = SampleVersion{*serverVersion + 1},
        };
        _journal.recordSuccess<SampleModel>(action, applied, nowMillis());
        return applied;
    } catch (const LimsError& error) {
        _journal.recordFailure<SampleModel>(action, error.what(), nowMillis());
        throw;
    }
}

ListConflictsResult SampleModel::execute(const ListConflicts& action) {
    static_cast<void>(action);
    if (!_sampleId.has_value()) {
        throw NotFound{"this handler is not attached to a sample (execute OpenSample first)"};
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::OfflineConflictRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::OfflineConflictRecord::sample>, "=", *_sampleId)
                    .All();
    ListConflictsResult result;
    result.conflicts.reserve(rows.size());
    for (const auto& row : rows) {
        result.conflicts.push_back(toConflictView(row));
    }
    return result;
}

ConflictView SampleModel::execute(const ResolveConflict& action) {
    requirePrincipal();
    try {
        if (!action.validate()) {
            throw ValidationError{"ResolveConflict: a conflict and a stated reason are required"};
        }
        Lightweight::DataMapper mapper;
        auto rows = mapper.Query<db::OfflineConflictRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::OfflineConflictRecord::id>, "=", *action.conflictId)
                        .All();
        if (rows.empty()) {
            throw NotFound{"ResolveConflict: no such conflict"};
        }
        auto row = rows.front();
        // Same reason as `execute(VerifyResult)`: a resolution belongs to the
        // conflict's sample, whatever this handler is attached to.
        _journal.rekey(std::to_string(row.sample.Value()));
        if (static_cast<ConflictStatus>(row.status.Value()) != ConflictStatus::Open) {
            throw Conflict{"ResolveConflict: conflict " + std::to_string(*action.conflictId) +
                           " has already been resolved"};
        }

        if (action.resolution == ConflictResolution::ApplyAnyway) {
            // Rebase: apply the queued capture against whatever the sample
            // holds *now*. The reading's author stays the field operator who
            // took it; the resolver is recorded separately, just below. Any
            // rejection (the sample has since been published, say) propagates
            // and leaves the conflict open rather than half-resolved.
            const auto queued = ::morph::model::ActionTraits<QueuedCapture>::fromJson(
                std::string{row.payload.Value().ToStringView()});
            static_cast<void>(applyCapture(SampleId{static_cast<std::int64_t>(row.sample.Value())}, queued.capture,
                                           queued.capturedBy));
        }

        row.status = static_cast<int>(action.resolution == ConflictResolution::ApplyAnyway ? ConflictStatus::Applied
                                                                                           : ConflictStatus::Discarded);
        row.resolvedBy = Lightweight::SqlAnsiString<64>{::morph::session::current()->principal};
        row.resolvedAt = nowMillis();
        row.resolutionNote = Lightweight::SqlAnsiString<255>{action.note};
        mapper.Update(row);

        auto view = toConflictView(row);
        _journal.recordSuccess<SampleModel>(action, view, nowMillis());
        return view;
    } catch (const LimsError& error) {
        _journal.recordFailure<SampleModel>(action, error.what(), nowMillis());
        throw;
    }
}


std::vector<LimsRole> SampleModel::rolesOf(std::string_view principal) {
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::OperatorRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::OperatorRecord::principal>, "=", std::string{principal})
                    .All();
    std::vector<LimsRole> roles;
    roles.reserve(rows.size());
    for (const auto& row : rows) {
        roles.push_back(static_cast<LimsRole>(row.role.Value()));
    }
    return roles;
}

void SampleModel::requireEveryResultVerified() const {
    Lightweight::DataMapper mapper;
    for (const auto& result : mapper.Query<db::ResultRecord>()
                                  .Where(::Lightweight::FieldNameOf<&db::ResultRecord::sample>, "=", *_sampleId)
                                  .All()) {
        if (mapper.Query<db::VerificationRecord>()
                .Where(::Lightweight::FieldNameOf<&db::VerificationRecord::result>, "=", result.id.Value())
                .All()
                .empty()) {
            throw IllegalTransition{"result " + std::to_string(result.id.Value()) +
                                    " has not been verified; a sample cannot be published with unverified results"};
        }
    }
}

void SampleModel::requireRole(LimsRole role) {
    const auto& principal = ::morph::session::current()->principal;
    const auto held = rolesOf(principal);
    if (std::ranges::find(held, role) == held.end()) {
        throw Forbidden{"'" + principal + "' does not hold the " + std::string{roleName(role)} + " role"};
    }
}

RoleGrantResult SampleModel::execute(const GrantRole& action) {
    requirePrincipal();
    try {
        if (!action.validate()) {
            throw ValidationError{"GrantRole: a principal is required"};
        }

        Lightweight::DataMapper mapper;
        // The bootstrap carve-out, and its exact boundary: a lab with no
        // supervisor at all has no way to appoint its first one, so the first
        // grant is permitted. The moment *any* supervisor exists the exception
        // is gone -- and the bootstrap grant is journaled like every other, so
        // it is visible in the audit trail rather than a silent back door.
        const auto supervisors = mapper.Query<db::OperatorRecord>()
                                     .Where(::Lightweight::FieldNameOf<&db::OperatorRecord::role>, "=",
                                            static_cast<int>(LimsRole::Supervisor))
                                     .All();
        if (!supervisors.empty()) {
            requireRole(LimsRole::Supervisor);
        }

        // Idempotent: granting a role twice is not an error and does not
        // produce two rows a revoke would then have to chase.
        const auto existing = rolesOf(action.principal);
        if (std::ranges::find(existing, action.role) == existing.end()) {
            db::OperatorRecord row;
            row.principal = Lightweight::SqlAnsiString<64>{action.principal};
            row.role = static_cast<int>(action.role);
            row.grantedBy = Lightweight::SqlAnsiString<64>{::morph::session::current()->principal};
            row.grantedAt = nowMillis();
            mapper.Create(row);
        }

        RoleGrantResult result{.principal = action.principal, .roles = rolesOf(action.principal)};
        _journal.recordSuccess<SampleModel>(action, result, nowMillis());
        return result;
    } catch (const LimsError& error) {
        _journal.recordFailure<SampleModel>(action, error.what(), nowMillis());
        throw;
    }
}

VerificationView SampleModel::execute(const VerifyResult& action) {
    requirePrincipal();
    try {
        if (!action.validate()) {
            throw ValidationError{"VerifyResult: a resultId is required"};
        }
        // Half one of four eyes: the role. Re-checked here rather than left to
        // lims::auth::LimsAuthorizer, because no authorizer runs on the Local
        // dispatch path at all -- a client-side gate is UX, not security
        // (examples/IMPLEMENTATION.md rule 1).
        requireRole(LimsRole::Verifier);

        Lightweight::DataMapper mapper;
        auto results = mapper.Query<db::ResultRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::ResultRecord::id>, "=", *action.resultId)
                           .All();
        if (results.empty()) {
            throw NotFound{"VerifyResult: no such result"};
        }
        const auto& result = results.front();
        const auto capturedBy = std::string{result.capturedBy.Value().ToStringView()};
        const auto& principal = ::morph::session::current()->principal;

        // Half two: the second pair of eyes must belong to a second person.
        // This is the half `IAuthorizer` cannot express -- it is handed the
        // model and action type ids and never the row -- so it can only live
        // here.
        if (capturedBy == principal) {
            throw Forbidden{"VerifyResult: '" + principal +
                            "' captured this result and cannot also verify it"};
        }

        const auto sampleId = static_cast<std::int64_t>(result.sample.Value());
        // Journal this verification against the *sample* it belongs to, not
        // against whatever this handler happened to be attached to. A
        // verifier's handler is typically attached to nothing (it acts on a
        // result id), so without this the entry lands under an empty entity
        // key and the sample's own audit trail silently omits the second pair
        // of eyes -- which is the one thing a four-eyes record exists to show.
        _journal.rekey(std::to_string(sampleId));
        const auto sample = toView(loadSample(mapper, sampleId));
        if (sample.state != SampleState::ToBeVerified) {
            throw IllegalTransition{
                std::string{"results are verified while a sample is to-be-verified; this one is "} +
                std::string{stateName(sample.state)}};
        }

        if (!mapper.Query<db::VerificationRecord>()
                 .Where(::Lightweight::FieldNameOf<&db::VerificationRecord::result>, "=", *action.resultId)
                 .All()
                 .empty()) {
            throw Conflict{"VerifyResult: result " + std::to_string(*action.resultId) + " is already verified"};
        }

        db::VerificationRecord row;
        row.result = static_cast<std::uint64_t>(*action.resultId);
        row.verifiedBy = Lightweight::SqlAnsiString<64>{principal};
        row.verifiedAt = nowMillis();
        mapper.Create(row);

        VerificationView view{
            .id = VerificationId{static_cast<std::int64_t>(row.id.Value())},
            .resultId = action.resultId,
            .sampleId = SampleId{sampleId},
            .capturedBy = capturedBy,
            .verifiedBy = principal,
            .verifiedAt = timestampFromMillis(row.verifiedAt.Value()),
        };
        _journal.recordSuccess<SampleModel>(action, view, nowMillis());
        return view;
    } catch (const LimsError& error) {
        _journal.recordFailure<SampleModel>(action, error.what(), nowMillis());
        throw;
    }
}

ListVerificationsResult SampleModel::execute(const ListVerifications& action) {
    static_cast<void>(action);
    if (!_sampleId.has_value()) {
        throw NotFound{"this handler is not attached to a sample (execute OpenSample first)"};
    }
    Lightweight::DataMapper mapper;
    ListVerificationsResult result;
    for (const auto& resultRow : mapper.Query<db::ResultRecord>()
                                     .Where(::Lightweight::FieldNameOf<&db::ResultRecord::sample>, "=", *_sampleId)
                                     .All()) {
        for (const auto& row : mapper.Query<db::VerificationRecord>()
                                   .Where(::Lightweight::FieldNameOf<&db::VerificationRecord::result>, "=",
                                          resultRow.id.Value())
                                   .All()) {
            result.verifications.push_back(VerificationView{
                .id = VerificationId{static_cast<std::int64_t>(row.id.Value())},
                .resultId = ResultId{static_cast<std::int64_t>(resultRow.id.Value())},
                .sampleId = SampleId{*_sampleId},
                .capturedBy = std::string{resultRow.capturedBy.Value().ToStringView()},
                .verifiedBy = std::string{row.verifiedBy.Value().ToStringView()},
                .verifiedAt = timestampFromMillis(row.verifiedAt.Value()),
            });
        }
    }
    return result;
}

AuditTrailResult SampleModel::execute(const GetAuditTrail& action) {
    static_cast<void>(action);
    if (!_sampleId.has_value()) {
        throw NotFound{"this handler is not attached to a sample (execute OpenSample first)"};
    }

    // Deliberately no DataMapper in this method. "Reconstructible from the
    // journal alone" is only a claim worth making if the reconstruction
    // genuinely cannot consult the store, and the way to guarantee that is not
    // to open it. `test_verification_audit.cpp` deletes every row the sample
    // has and reconstructs the trail anyway.
    AuditTrailResult trail;
    for (const auto& entry : _journal.entries()) {
        trail.steps.push_back(auditStepFor(entry));
        const auto& step = trail.steps.back();
        if (step.kind == AuditStepKind::LifecycleTransition && step.outcome == AuditOutcome::Succeeded) {
            trail.states.push_back(step.state);
        }
    }
    return trail;
}

void SampleModel::attachOfflineQueue(std::shared_ptr<::morph::offline::IOfflineQueue> queue) {
    _queue = std::move(queue);
}

void SampleModel::onBackendChanged() {
    if (!_queue) {
        return;
    }
    for (const auto& item : _queue->drain()) {
        try {
            static_cast<void>(execute(::morph::model::ActionTraits<QueuedCapture>::fromJson(item.payload)));
        } catch (const std::exception& error) {
            // Unresolvable by definition: the payload does not decode, or it
            // names a sample this server has never heard of, or it claims an
            // author the replaying session is not. None of those get better by
            // waiting, and leaving the item queued would block every later
            // item behind it forever -- so it is journaled and dropped.
            _journal.recordRejectedPayload<SampleModel>("QueuedCapture", item.payload, error.what(), nowMillis());
        }
        // Every drained item is markDone()d whatever the outcome: this path
        // has no retry budget (docs/spec/offline/offline.md).
        _queue->markDone(item.id);
    }
}

void SampleModel::attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey) {
    _journal.attach(std::move(log), std::move(entityKey));
}

}  // namespace lims
