// SPDX-License-Identifier: Apache-2.0
#include "crm/models/opportunity_model.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlTransaction.hpp>
#include <charconv>
#include <glaze/glaze.hpp>

#include "crm/core/errors.hpp"
#include "crm/core/model_support.hpp"
#include "crm/db/crm_entity.hpp"

namespace crm {

namespace {

/// @brief Parses a string-id `Choice` payload into an `Id`, or the
///        default-constructed (empty) id when the choice is unset.
///
/// Shared shape between `OpportunityAccountChoice`/`PrimaryContactChoice`
/// and `ContactModel::parseAccountChoice` — kept as a local template rather
/// than promoted to `core/model_support.hpp` for now, since only two models
/// need it; promote it if a third comes to need the same parse.
template <typename Id, typename Choice>
Id parseChoice(const Choice& choice, const char* fieldName) {
    if (!choice.hasValue()) {
        return Id{};
    }
    const std::string& text = *choice;
    std::int64_t parsed = 0;
    auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{}) {
        throw ValidationError{std::string{fieldName} + ": not a valid id"};
    }
    return Id{parsed};
}

Money toMoney(const std::optional<std::int64_t>& num, const std::optional<std::int64_t>& den) {
    if (!num.has_value() || !den.has_value()) {
        return Money{};  // empty: matches Quantity's own "not entered" state
    }
    return Money{morph::math::Rational{morph::math::Numerator{*num}, morph::math::Denominator{*den},
                                       morph::math::DecimalPlaces{2}}};
}

ConflictView toConflictView(const db::OpportunityConflictRecord& row) {
    return ConflictView{
        .id = ConflictId{static_cast<std::int64_t>(row.id.Value())},
        .opportunityId = OpportunityId{static_cast<std::int64_t>(row.opportunity.Value())},
        .baseVersion = row.baseVersion.Value(),
        .serverVersion = row.serverVersion.Value(),
        .reason = static_cast<ConflictReason>(row.reason.Value()),
        .status = static_cast<ConflictStatus>(row.status.Value()),
        .payload = std::string{row.payload.Value().ToStringView()},
        .detectedBy = std::string{row.detectedBy.Value().ToStringView()},
        .detectedAtMs = row.detectedAt.Value(),
        .resolvedBy = std::string{row.resolvedBy.Value().ToStringView()},
        .resolutionNote = std::string{row.resolutionNote.Value().ToStringView()},
    };
}

/// @brief `true` unless @p opKey has never been decided before — the
///        at-most-once check README §8 needs because the offline queue is
///        documented not to enforce it itself (`IOfflineQueue::enqueue`'s own
///        doc comment). Same shape as `lims::SampleModel::alreadyDecided`.
bool alreadyDecided(const std::string& opKey) {
    if (opKey.empty()) {
        return false;
    }
    Lightweight::DataMapper mapper;
    return !mapper.Query<db::OpportunityReplayedOpRecord>()
                .Where(::Lightweight::FieldNameOf<&db::OpportunityReplayedOpRecord::opKey>, "=", opKey)
                .All()
                .empty();
}

/// @brief Records @p opKey as decided, so a redelivery is skipped.
void markDecided(Lightweight::DataMapper& mapper, const std::string& opKey) {
    db::OpportunityReplayedOpRecord row;
    row.opKey = Lightweight::SqlAnsiString<128>{opKey};
    row.decidedAt = nowMillis();
    mapper.Create(row);
}

OpportunityView toView(const db::OpportunityRecord& row) {
    OpportunityView view{
        .id = OpportunityId{static_cast<std::int64_t>(row.id.Value())},
        .accountId = AccountId{static_cast<std::int64_t>(row.account.Value())},
        .primaryContactId = std::nullopt,
        .name = std::string{row.name.Value().ToStringView()},
        .stage = static_cast<OpportunityStage>(row.stage.Value()),
        .expectedCloseValue = toMoney(row.expectedCloseValueNum.Value(), row.expectedCloseValueDen.Value()),
        .version = row.version.Value(),
    };
    if (row.primaryContactId.Value().has_value()) {
        view.primaryContactId = ContactId{static_cast<std::int64_t>(*row.primaryContactId.Value())};
    }
    return view;
}

}  // namespace

CreateOpportunityResult OpportunityModel::execute(const CreateOpportunity& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"CreateOpportunity: account and name are required"};
    }
    const AccountId accountId = parseChoice<AccountId>(action.account, "account");
    const ContactId primaryContactId = parseChoice<ContactId>(action.primaryContact, "primaryContact");

    Lightweight::DataMapper mapper;
    auto accountRows = mapper.Query<db::AccountRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *accountId)
                           .All();
    if (accountRows.empty()) {
        throw NotFound{"CreateOpportunity: no such account"};
    }
    if (primaryContactId.hasValue()) {
        auto contactRows = mapper.Query<db::ContactRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::ContactRecord::id>, "=", *primaryContactId)
                               .All();
        if (contactRows.empty()) {
            throw NotFound{"CreateOpportunity: no such primary contact"};
        }
    }

    db::OpportunityRecord row;
    row.account = accountRows.front();
    if (primaryContactId.hasValue()) {
        row.primaryContactId = static_cast<std::uint64_t>(*primaryContactId);
    }
    row.name = Lightweight::SqlAnsiString<128>{action.name};
    row.stage = static_cast<int>(OpportunityStage::Prospecting);
    if (action.expectedCloseValue.hasValue()) {
        row.expectedCloseValueNum = (*action.expectedCloseValue).numerator;
        row.expectedCloseValueDen = (*action.expectedCloseValue).denominator;
    }
    row.createdAt = nowMillis();
    row.version = 1;
    mapper.Create(row);

    CreateOpportunityResult result{.opportunityId = OpportunityId{static_cast<std::int64_t>(row.id.Value())}};
    _journal.recordSuccess<OpportunityModel>(action, result, nowMillis());
    return result;
}

UpdateOpportunityResult OpportunityModel::execute(const UpdateOpportunity& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"UpdateOpportunity: opportunityId, account and name are required"};
    }
    const AccountId accountId = parseChoice<AccountId>(action.account, "account");
    const ContactId primaryContactId = parseChoice<ContactId>(action.primaryContact, "primaryContact");

    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::OpportunityRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::OpportunityRecord::id>, "=", *action.opportunityId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"UpdateOpportunity: no such opportunity"};
    }
    auto& row = rows.front();
    if (row.version.Value() != action.expectedVersion) {
        throw Conflict{"UpdateOpportunity: version mismatch — record was edited concurrently"};
    }

    auto accountRows = mapper.Query<db::AccountRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *accountId)
                           .All();
    if (accountRows.empty()) {
        throw NotFound{"UpdateOpportunity: no such account"};
    }
    if (primaryContactId.hasValue()) {
        auto contactRows = mapper.Query<db::ContactRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::ContactRecord::id>, "=", *primaryContactId)
                               .All();
        if (contactRows.empty()) {
            throw NotFound{"UpdateOpportunity: no such primary contact"};
        }
    }

    row.account = accountRows.front();
    row.primaryContactId = primaryContactId.hasValue()
                               ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(*primaryContactId)}
                               : std::nullopt;
    row.name = Lightweight::SqlAnsiString<128>{action.name};
    if (action.expectedCloseValue.hasValue()) {
        row.expectedCloseValueNum = (*action.expectedCloseValue).numerator;
        row.expectedCloseValueDen = (*action.expectedCloseValue).denominator;
    } else {
        row.expectedCloseValueNum = std::nullopt;
        row.expectedCloseValueDen = std::nullopt;
    }
    row.version = row.version.Value() + 1;
    mapper.Update(row);

    UpdateOpportunityResult result{.opportunity = toView(row)};
    _journal.recordSuccess<OpportunityModel>(action, result, nowMillis());
    return result;
}

OpportunityView OpportunityModel::execute(const GetOpportunity& action) {
    if (!action.validate()) {
        throw ValidationError{"GetOpportunity: opportunityId is required"};
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::OpportunityRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::OpportunityRecord::id>, "=", *action.opportunityId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"GetOpportunity: no such opportunity"};
    }
    return toView(rows.front());
}

ListOpportunitiesResult OpportunityModel::execute(const ListOpportunities& action) {
    Lightweight::DataMapper mapper;
    const bool hasAccountFilter = action.accountId.has_value() && action.accountId->hasValue();
    const bool hasStageFilter = action.stage.has_value();

    // Four filter combinations, branched explicitly rather than reassigning
    // an intermediate query-builder value — matches ListContacts/
    // ListContactOptions's own established idiom for a single optional
    // filter (query.Where(...).All() called directly, never stored
    // mid-chain), extended here to two independent filters that may combine.
    std::vector<db::OpportunityRecord> rows;
    if (hasAccountFilter && hasStageFilter) {
        rows =
            mapper.Query<db::OpportunityRecord>()
                .Where(::Lightweight::FieldNameOf<&db::OpportunityRecord::account>, "=", **action.accountId)
                .Where(::Lightweight::FieldNameOf<&db::OpportunityRecord::stage>, "=", static_cast<int>(*action.stage))
                .All();
    } else if (hasAccountFilter) {
        rows = mapper.Query<db::OpportunityRecord>()
                   .Where(::Lightweight::FieldNameOf<&db::OpportunityRecord::account>, "=", **action.accountId)
                   .All();
    } else if (hasStageFilter) {
        rows =
            mapper.Query<db::OpportunityRecord>()
                .Where(::Lightweight::FieldNameOf<&db::OpportunityRecord::stage>, "=", static_cast<int>(*action.stage))
                .All();
    } else {
        rows = mapper.Query<db::OpportunityRecord>().All();
    }

    ListOpportunitiesResult result;
    result.opportunities.reserve(rows.size());
    for (const auto& row : rows) {
        result.opportunities.push_back(toView(row));
    }
    return result;
}

MoveOpportunityStageResult OpportunityModel::execute(const MoveOpportunityStage& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"MoveOpportunityStage: opportunityId is required"};
    }

    Lightweight::DataMapper mapper;

    // Idempotency-ledger lookup (LADDER.md strain 5), before any
    // re-validation — kanban::BoardModel::execute(MoveTaskPosition)'s exact
    // ordering. An empty opId opts out (test/replay callers that don't need
    // dedup), matching kanban's `if (!action.opId.empty())` guard.
    if (!action.opId.empty()) {
        auto existingOp =
            mapper.Query<db::AppliedOpRecord>()
                .Where(::Lightweight::FieldNameOf<&db::AppliedOpRecord::opportunity>, "=", *action.opportunityId)
                .Where(::Lightweight::FieldNameOf<&db::AppliedOpRecord::opId>, "=", action.opId)
                .All();
        if (!existingOp.empty()) {
            MoveOpportunityStageResult replayed;
            if (auto err = glz::read_json(replayed, std::string{existingOp.front().resultJson.Value()}); err) {
                throw CrmError{"MoveOpportunityStage: corrupt ledger entry"};
            }
            // A ledger hit performed nothing new — nothing to journal here,
            // matching kanban's corrected §4 (a ledger hit only returns a
            // previously-stored result; the framework's own auto-append does
            // not produce a second entry on this path).
            return replayed;
        }
    }

    auto rows = mapper.Query<db::OpportunityRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::OpportunityRecord::id>, "=", *action.opportunityId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"MoveOpportunityStage: no such opportunity"};
    }
    auto& row = rows.front();

    const auto currentStage = static_cast<OpportunityStage>(row.stage.Value());
    if (!isLegalStageTransition(currentStage, action.stage)) {
        throw IllegalTransition{"MoveOpportunityStage: opportunity is in a terminal stage (Won or Lost)"};
    }

    Lightweight::SqlTransaction transaction{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};

    row.stage = static_cast<int>(action.stage);
    row.version = row.version.Value() + 1;
    mapper.Update(row);

    MoveOpportunityStageResult result{
        .opportunityId = OpportunityId{static_cast<std::int64_t>(row.id.Value())},
        .stage = action.stage,
        .version = row.version.Value(),
    };

    if (!action.opId.empty()) {
        db::AppliedOpRecord opRow;
        opRow.opportunity = row;
        opRow.opId = Lightweight::SqlAnsiString<128>{action.opId};
        opRow.resultJson = glz::write_json(result).value_or(std::string{});
        opRow.createdAt = nowMillis();
        mapper.Create(opRow);
    }

    transaction.Commit();

    _journal.recordSuccess<OpportunityModel>(action, result, nowMillis());
    return result;
}

ReplayOpportunityUpdateResult OpportunityModel::execute(const QueuedOpportunityUpdate& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{
            "QueuedOpportunityUpdate: an opportunity, an author, an operation key and a name are required"};
    }
    const auto& principal = ::morph::session::current()->principal;
    if (action.capturedBy != principal) {
        // A queued edit is replayed *as* the operator who made it, and nobody
        // else — same author-integrity guard as
        // lims::SampleModel::execute(QueuedCapture).
        throw Forbidden{"QueuedOpportunityUpdate: replay must run as the capturing operator ('" + action.capturedBy +
                        "'), not '" + principal + "'"};
    }

    // At-most-once, enforced here because the queue is documented not to
    // enforce it (docs/spec/offline/offline.md).
    if (alreadyDecided(*action.operationKey)) {
        return ReplayOpportunityUpdateResult{.outcome = ReplayOutcome::Skipped, .opportunityId = action.opportunityId};
    }

    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::OpportunityRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::OpportunityRecord::id>, "=", *action.opportunityId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"QueuedOpportunityUpdate: no such opportunity"};
    }
    auto& row = rows.front();
    const auto serverVersion = row.version.Value();
    const auto currentStage = static_cast<OpportunityStage>(row.stage.Value());

    // The two ways a queued update can be out of date, in the order that
    // makes the flagged reason the *most specific* true one — same ordering
    // rationale as lims::SampleModel::execute(QueuedCapture): a deal that has
    // closed has also moved on in version, and "this deal closed while you
    // were away" is more actionable than "your base was stale".
    std::optional<ConflictReason> reason;
    if (currentStage == OpportunityStage::Won || currentStage == OpportunityStage::Lost) {
        reason = ConflictReason::LifecycleClosed;
    } else if (action.baseVersion != serverVersion) {
        reason = ConflictReason::StaleBase;
    }

    if (reason.has_value()) {
        Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
        db::OpportunityConflictRecord conflictRow;
        conflictRow.opportunity = row;
        conflictRow.baseVersion = action.baseVersion;
        conflictRow.serverVersion = serverVersion;
        conflictRow.reason = static_cast<int>(*reason);
        conflictRow.status = static_cast<int>(ConflictStatus::Open);
        // Verbatim, not re-encoded from the decoded struct — see
        // ConflictView::payload's own doc comment for why.
        conflictRow.payload = Lightweight::SqlDynamicAnsiString<4096>{
            ::morph::model::ActionTraits<QueuedOpportunityUpdate>::toJson(action)};
        conflictRow.detectedBy = Lightweight::SqlAnsiString<64>{principal};
        conflictRow.detectedAt = nowMillis();
        conflictRow.resolvedBy = Lightweight::SqlAnsiString<64>{std::string{}};
        conflictRow.resolvedAt = 0;
        conflictRow.resolutionNote = Lightweight::SqlAnsiString<255>{std::string{}};
        mapper.Create(conflictRow);
        // Flagging is terminal for the queued item: the conflict row owns it
        // now, so a redelivery must not raise a second flag about the same edit.
        markDecided(mapper, *action.operationKey);
        sqlTxn.Commit();

        ReplayOpportunityUpdateResult flagged{
            .outcome = ReplayOutcome::Conflicted,
            .opportunityId = action.opportunityId,
            .baseVersion = action.baseVersion,
            .serverVersion = serverVersion,
            .conflictId = ConflictId{static_cast<std::int64_t>(conflictRow.id.Value())},
            .reason = *reason,
        };
        _journal.recordSuccess<OpportunityModel>(action, flagged, nowMillis());
        return flagged;
    }

    const AccountId accountId = parseChoice<AccountId>(action.account, "account");
    const ContactId primaryContactId = parseChoice<ContactId>(action.primaryContact, "primaryContact");
    auto accountRows = mapper.Query<db::AccountRecord>()
                           .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *accountId)
                           .All();
    if (accountRows.empty()) {
        throw NotFound{"QueuedOpportunityUpdate: no such account"};
    }
    row.account = accountRows.front();
    row.primaryContactId = primaryContactId.hasValue()
                               ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(*primaryContactId)}
                               : std::nullopt;
    row.name = Lightweight::SqlAnsiString<128>{action.name};
    if (action.expectedCloseValue.hasValue()) {
        row.expectedCloseValueNum = (*action.expectedCloseValue).numerator;
        row.expectedCloseValueDen = (*action.expectedCloseValue).denominator;
    } else {
        row.expectedCloseValueNum = std::nullopt;
        row.expectedCloseValueDen = std::nullopt;
    }
    row.version = serverVersion + 1;
    // One transaction over both writes, for the same reason the conflict
    // branch above uses one: an apply that commits without its op-key beside
    // it is an apply this server can no longer recognise as its own, and the
    // redelivery it invites is then decided on stale-base grounds instead of
    // skipped. Exactly-once is a claim about the *pair*, not about either
    // statement.
    Lightweight::SqlTransaction sqlTxn{mapper.Connection(), Lightweight::SqlTransactionMode::ROLLBACK};
    mapper.Update(row);
    markDecided(mapper, *action.operationKey);
    sqlTxn.Commit();

    ReplayOpportunityUpdateResult applied{
        .outcome = ReplayOutcome::Applied,
        .opportunityId = action.opportunityId,
        .baseVersion = action.baseVersion,
        .serverVersion = serverVersion + 1,
    };
    _journal.recordSuccess<OpportunityModel>(action, applied, nowMillis());
    return applied;
}

ListConflictsResult OpportunityModel::execute(const ListConflicts& action) {
    if (!action.validate()) {
        throw ValidationError{"ListConflicts: opportunityId is required"};
    }
    Lightweight::DataMapper mapper;
    auto rows =
        mapper.Query<db::OpportunityConflictRecord>()
            .Where(::Lightweight::FieldNameOf<&db::OpportunityConflictRecord::opportunity>, "=", *action.opportunityId)
            .All();
    ListConflictsResult result;
    result.conflicts.reserve(rows.size());
    for (const auto& row : rows) {
        result.conflicts.push_back(toConflictView(row));
    }
    return result;
}

ConflictView OpportunityModel::execute(const ResolveConflict& action) {
    requirePrincipal();
    if (!action.validate()) {
        throw ValidationError{"ResolveConflict: a conflict and a stated reason are required"};
    }
    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<db::OpportunityConflictRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::OpportunityConflictRecord::id>, "=", *action.conflictId)
                    .All();
    if (rows.empty()) {
        throw NotFound{"ResolveConflict: no such conflict"};
    }
    auto row = rows.front();
    if (static_cast<ConflictStatus>(row.status.Value()) != ConflictStatus::Open) {
        throw Conflict{"ResolveConflict: conflict " + std::to_string(*action.conflictId) +
                       " has already been resolved"};
    }

    if (action.resolution == ConflictResolution::ApplyAnyway) {
        // Rebase: apply the queued edit against whatever the opportunity
        // holds *now*. Any rejection (the deal has since closed, say)
        // propagates and leaves the conflict open rather than half-resolved.
        const auto queued = ::morph::model::ActionTraits<QueuedOpportunityUpdate>::fromJson(
            std::string{row.payload.Value().ToStringView()});
        auto opportunityRows =
            mapper.Query<db::OpportunityRecord>()
                .Where(::Lightweight::FieldNameOf<&db::OpportunityRecord::id>, "=", row.opportunity.Value())
                .All();
        if (opportunityRows.empty()) {
            throw NotFound{"ResolveConflict: opportunity no longer exists"};
        }
        auto& opportunityRow = opportunityRows.front();
        const AccountId accountId = parseChoice<AccountId>(queued.account, "account");
        const ContactId primaryContactId = parseChoice<ContactId>(queued.primaryContact, "primaryContact");
        auto accountRows = mapper.Query<db::AccountRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::AccountRecord::id>, "=", *accountId)
                               .All();
        if (accountRows.empty()) {
            throw NotFound{"ResolveConflict: no such account"};
        }
        opportunityRow.account = accountRows.front();
        opportunityRow.primaryContactId =
            primaryContactId.hasValue() ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(*primaryContactId)}
                                        : std::nullopt;
        opportunityRow.name = Lightweight::SqlAnsiString<128>{queued.name};
        if (queued.expectedCloseValue.hasValue()) {
            opportunityRow.expectedCloseValueNum = (*queued.expectedCloseValue).numerator;
            opportunityRow.expectedCloseValueDen = (*queued.expectedCloseValue).denominator;
        } else {
            opportunityRow.expectedCloseValueNum = std::nullopt;
            opportunityRow.expectedCloseValueDen = std::nullopt;
        }
        opportunityRow.version = opportunityRow.version.Value() + 1;
        mapper.Update(opportunityRow);
    }

    row.status = static_cast<int>(action.resolution == ConflictResolution::ApplyAnyway ? ConflictStatus::Applied
                                                                                       : ConflictStatus::Discarded);
    row.resolvedBy = Lightweight::SqlAnsiString<64>{::morph::session::current()->principal};
    row.resolvedAt = nowMillis();
    row.resolutionNote = Lightweight::SqlAnsiString<255>{action.note};
    mapper.Update(row);

    auto view = toConflictView(row);
    _journal.recordSuccess<OpportunityModel>(action, view, nowMillis());
    return view;
}

void OpportunityModel::attachOfflineQueue(std::shared_ptr<::morph::offline::IOfflineQueue> queue) {
    _queue = std::move(queue);
}

void OpportunityModel::onBackendChanged() {
    if (!_queue) {
        return;
    }
    for (const auto& item : _queue->drain()) {
        try {
            static_cast<void>(execute(::morph::model::ActionTraits<QueuedOpportunityUpdate>::fromJson(item.payload)));
        } catch (const std::exception& error) {
            // Unresolvable by definition: the payload does not decode, or it
            // names an opportunity or account this server has never heard of,
            // or it claims an author the replaying session is not. None of
            // those get better by waiting, and leaving the item queued would
            // block every later item behind it forever — so it is journaled
            // and dropped, same as lims::SampleModel::onBackendChanged.
            _journal.recordRejectedPayload<OpportunityModel>("QueuedOpportunityUpdate", item.payload, error.what(),
                                                             nowMillis());
        }
        _queue->markDone(item.id);
    }
}

}  // namespace crm
