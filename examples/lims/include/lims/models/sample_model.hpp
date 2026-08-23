// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/model_key.hpp>
#include <morph/core/registry.hpp>
#include <morph/journal/action_log.hpp>
#include <optional>
#include <string>

#include "lims/core/self_journal.hpp"
#include "lims/dto/result_dto.hpp"
#include "lims/dto/sample_dto.hpp"

/// @file
/// `SampleModel` — the sample lifecycle (README build order §2), keyed by
/// sample id so a bench client and an office client attach to the same
/// instance.

namespace lims {

/// @brief One sample: its registration and its position in the lab workflow.
///
/// Keyed by sample id (the hand-written `ModelKeyTraits`/`ActionKeyTraits`
/// specialisations below). Holds no connection of its own — each `execute()`
/// opens a `Lightweight::DataMapper` for its own duration, which is safe
/// because morph serialises every call on one instance onto its own strand.
///
/// **Every transition is guarded and journaled, including the ones that
/// fail.** An attempt to publish an unverified sample is rejected with
/// `IllegalTransition` *and* appended to the action log with
/// `Outcome::Failed`. A regulatory trail that records only the transitions
/// that succeeded cannot answer "who tried", which is half of what such a
/// trail is for.
class SampleModel {
public:
    /// @brief Registers a client samples can belong to.
    /// @param action The client's name.
    /// @return The new client's id.
    /// @throws ValidationError if the name is empty.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    RegisterClientResult execute(const RegisterClient& action);

    /// @brief Logs a new sample, `Registered` at version 1, and attaches this
    ///        handler to it.
    /// @param action The owning client and the lab's container reference.
    /// @return The new sample's full state.
    /// @throws ValidationError if the action is not well-formed.
    /// @throws NotFound if the named client does not exist.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SampleView execute(const RegisterSample& action);

    /// @brief Attaches this handler to the sample named by @p action.
    /// @param action The sample's id.
    /// @return That sample's full state.
    /// @throws ValidationError if no sample is named.
    /// @throws NotFound if the sample does not exist.
    SampleView execute(const OpenSample& action);

    /// @brief Re-reads the attached sample.
    /// @param action Carries no fields of its own.
    /// @return The attached sample's full state.
    /// @throws NotFound if this handler is not attached, or the sample is gone.
    SampleView execute(const GetSample& action);

    /// @brief `Registered → Received`.
    /// @param action Carries no fields of its own.
    /// @return The sample's state after the transition.
    /// @throws IllegalTransition if the sample is not `Registered`.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SampleView execute(const ReceiveSample& action);

    /// @brief `Received → InProgress`.
    /// @param action Carries no fields of its own.
    /// @return The sample's state after the transition.
    /// @throws IllegalTransition if the sample is not `Received`.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SampleView execute(const StartWork& action);

    /// @brief `InProgress → ToBeVerified`.
    /// @param action Carries no fields of its own.
    /// @return The sample's state after the transition.
    /// @throws IllegalTransition if the sample is not `InProgress`.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SampleView execute(const SubmitForVerification& action);

    /// @brief `ToBeVerified → InProgress`, with the verifier's stated reason.
    /// @param action The reason the sample went back to the bench.
    /// @return The sample's state after the transition.
    /// @throws ValidationError if no reason is given.
    /// @throws IllegalTransition if the sample is not `ToBeVerified`.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SampleView execute(const ReturnForRework& action);

    /// @brief `ToBeVerified → Published`.
    /// @param action Carries no fields of its own.
    /// @return The sample's state after the transition.
    /// @throws IllegalTransition if the sample is not `ToBeVerified`.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SampleView execute(const PublishSample& action);

    /// @brief `Registered|Received → Rejected`, with the stated reason.
    /// @param action Why the container is refused.
    /// @return The sample's state after the transition.
    /// @throws ValidationError if no reason is given.
    /// @throws IllegalTransition if the sample has already been worked.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    SampleView execute(const RejectSample& action);

    /// @brief Captures a concentration result against the attached sample.
    ///
    /// The submitted value is in the analysis version's canonical unit; the
    /// entry-unit conversion is the renderer's job, driven by the
    /// `x-unitAlternatives` this rung's `UnitTraits::relations` generate.
    ///
    /// Re-capturing the same analysis version replaces the previous result
    /// rather than appending a second row: a result is the lab's current
    /// answer for one analysis on one sample, and two live answers would make
    /// "the sample's results" ambiguous. The superseded value is not lost —
    /// the journal holds every capture.
    /// @param action The version, and exactly one of value/qualifier.
    /// @return The stored result.
    /// @throws ValidationError if the encoding is violated, the qualifier code
    ///         is unknown, the version's canonical unit is not mg/L, or the
    ///         value carries more precision than the version declares.
    /// @throws NotFound if the analysis version does not exist.
    /// @throws IllegalTransition if the sample is not `InProgress`.
    /// @throws EmptyPrincipalError if no principal is authenticated.
    ResultView execute(const CaptureConcentration& action);

    /// @brief Lists the attached sample's results, oldest first.
    /// @param action Carries no fields of its own.
    /// @return Every result captured against the sample.
    /// @throws NotFound if this handler is not attached to a sample.
    ListResultsResult execute(const ListResults& action);

    /// @brief Serves the qualifier picklist a `QualifierChoice` renders from.
    /// @param action Carries no fields of its own.
    /// @return The three "no number" codes, with display text.
    ListResultQualifiersResult execute(const ListResultQualifiers& action);

    /// @brief Attaches a durable action log, so every mutating `execute()`
    ///        records a `LogEntry`.
    ///
    /// Needed because a plain-constructed model never goes through the
    /// registry/dispatcher path the framework's own auto-append lives on —
    /// see `lims::SelfJournal`'s file comment.
    /// @param log Sink entries are forwarded to.
    /// @param entityKey Stable identity stamped onto every entry; re-stamped
    ///        with the sample's own id as soon as this handler attaches to one.
    void attachActionLog(std::shared_ptr<::morph::journal::IActionLog> log, std::string entityKey);

private:
    /// @brief Shared body of every transition action.
    ///
    /// Runs the guard, writes the new state and the bumped base version in one
    /// transaction, and journals the outcome — success or rejection.
    /// @tparam Action The transition action type (supplies the journal's
    ///         action id and payload).
    /// @param action The transition being attempted.
    /// @param target The state the lifecycle is being asked to move to.
    /// @return The sample's state after the transition.
    template <typename Action>
    SampleView transition(const Action& action, SampleState target);

    /// @brief Loads the attached sample and returns its view.
    /// @return The attached sample's full state.
    /// @throws NotFound if this handler is not attached, or the row is gone.
    [[nodiscard]] SampleView loadAttached() const;

    /// @brief Writes @p target and the bumped base version to the attached row.
    /// @param target The new state (already checked legal by the caller).
    /// @return The sample's state after the write.
    [[nodiscard]] SampleView writeState(SampleState target) const;

    /// @brief The sample this handler is attached to, set by `RegisterSample`
    ///        or `OpenSample`. Unset until then.
    std::optional<std::int64_t> _sampleId;

    /// @brief This instance's own journal (inert until `attachActionLog`).
    SelfJournal _journal;
};

}  // namespace lims

BRIDGE_REGISTER_MODEL(lims::SampleModel, "SampleModel")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::RegisterClient, "RegisterClient")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::RegisterSample, "RegisterSample")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::OpenSample, "OpenSample", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::GetSample, "GetSample", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ReceiveSample, "ReceiveSample")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::StartWork, "StartWork")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::SubmitForVerification, "SubmitForVerification")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ReturnForRework, "ReturnForRework")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::PublishSample, "PublishSample")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::RejectSample, "RejectSample")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::CaptureConcentration, "CaptureConcentration")
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ListResults, "ListResults", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::SampleModel, lims::ListResultQualifiers, "ListResultQualifiers",
                       ::morph::model::Loggable::No)

// Hand-written ModelKeyTraits/ActionKeyTraits rather than
// BRIDGE_MODEL_KEY/BRIDGE_KEY_FROM: those macros deduce the model's
// `PrimaryKey` as the *type* of the named member and route the value through
// `morph::model::keyToString`, whose `ModelKey` concept admits only
// `std::integral` or `std::string` (include/morph/core/model_key.hpp).
// `lims::SampleId` wraps `std::optional<std::int64_t>` and satisfies neither,
// so `BRIDGE_MODEL_KEY(SampleModel, OpenSample, &OpenSample::sampleId)` does
// not compile. `ledger::LedgerModel` and `kanban::BoardModel` already carry
// the identical hand-written workaround for the identical reason; this is the
// third rung to need it, which is exactly the trigger
// examples/IMPLEMENTATION.md's rule-of-three names — see the rung README's
// findings section.
template <>
struct morph::model::ActionKeyTraits<lims::OpenSample> {
    /// @brief This action carries its model's key.
    static constexpr bool hasKey = true;

    /// @brief The key is in the payload, not the result.
    static constexpr bool fromResult = false;

    /// @brief Unwraps `OpenSample::sampleId` to the wire-canonical integer.
    /// @param action The action carrying the key.
    /// @return The key's canonical string form.
    /// @throws std::bad_optional_access if the id is disengaged. Key
    ///         extraction runs *before* `SampleModel::execute`'s own
    ///         `validate()` check ever sees the action
    ///         (`BridgeHandler::execute`, include/morph/core/bridge.hpp), so
    ///         throwing here is the sanctioned rejection point — that call
    ///         site's own `catch (...)` resolves the `Completion` with the
    ///         error rather than letting it escape.
    static std::string key(const lims::OpenSample& action) { return morph::model::keyToString(*action.sampleId); }
};

template <>
struct morph::model::ModelKeyTraits<lims::SampleModel> {
    /// @brief The key type, in its unwrapped wire-canonical form.
    using PrimaryKey = std::int64_t;
};

template <>
struct morph::model::ActionKeyTraits<lims::RegisterSample> {
    /// @brief This action establishes its model's key.
    static constexpr bool hasKey = true;

    /// @brief The key is generated, so it comes from the result.
    static constexpr bool fromResult = true;

    /// @brief Reads the freshly generated sample id off the result.
    /// @tparam R The action's result type (`SampleView`).
    /// @param result The registration's result.
    /// @return The new key's canonical string form.
    template <typename R>
    static std::string keyOfResult(const R& result) {
        return morph::model::keyToString(*result.id);
    }
};
