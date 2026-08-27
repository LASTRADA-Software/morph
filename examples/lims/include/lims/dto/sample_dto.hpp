// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/tagged.hpp>
#include <string>
#include <string_view>

#include "lims/core/types.hpp"

/// @file
/// Sample registration and the lifecycle state machine (README build order
/// §2). Every legal edge of that machine is one action type, so the journal
/// records *which* transition was attempted rather than a generic "state was
/// set to 3" — an audit trail that cannot name the operation is not one.

namespace lims {

/// @brief A sample's monotonic base version.
///
/// Bumped by every mutating transition. This is the value an offline field
/// update carries and that replay compares against to detect a stale base
/// (README build order §7) — a protocol scalar, so a named opaque newtype
/// rather than a bare integer (examples/IMPLEMENTATION.md rule 3).
using SampleVersion = ::morph::util::Tagged<std::int64_t, "SampleVersion">;

/// @brief The lifecycle's legal edges, as a total function on state pairs.
///
/// Deliberately a free `constexpr` function rather than a method on a model:
/// the whole 6×6 matrix is then exhaustively testable without a database, and
/// the model has exactly one place to ask. Self-edges are illegal — receiving
/// an already-received sample is a mistake to reject, not a no-op to absorb,
/// because absorbing it would journal a transition that did not happen.
/// @param from The sample's current state.
/// @param to The state being requested.
/// @return `true` when the lifecycle permits `from → to`.
[[nodiscard]] constexpr bool isLegalTransition(SampleState from, SampleState to) noexcept {
    switch (from) {
        case SampleState::Registered:
            // Logged by the office: the container either turns up or is refused.
            return to == SampleState::Received || to == SampleState::Rejected;
        case SampleState::Received:
            // Still refusable at the bench (wrong preservative, broken seal).
            return to == SampleState::InProgress || to == SampleState::Rejected;
        case SampleState::InProgress:
            return to == SampleState::ToBeVerified;
        case SampleState::ToBeVerified:
            // Either the four-eyes check passes, or the sample goes back to
            // the bench for rework. Rejection is no longer available: a
            // sample that has been worked has results, and discarding those
            // silently is exactly what an audit trail exists to prevent.
            return to == SampleState::Published || to == SampleState::InProgress;
        case SampleState::Published:
        case SampleState::Rejected:
            return false;
        default:
            return false;
    }
}

/// @brief Human-readable name of @p state, for error messages and the journal.
/// @param state The state to name.
/// @return Its stable ascii name.
[[nodiscard]] constexpr std::string_view stateName(SampleState state) noexcept {
    switch (state) {
        case SampleState::Registered:
            return "registered";
        case SampleState::Received:
            return "received";
        case SampleState::InProgress:
            return "in-progress";
        case SampleState::ToBeVerified:
            return "to-be-verified";
        case SampleState::Published:
            return "published";
        case SampleState::Rejected:
            return "rejected";
        default:
            return "unknown";
    }
}

/// @brief Registers a client a sample can belong to.
///
/// Lives on `SampleModel` rather than a model of its own: in this rung a
/// client has no behaviour beyond being a sample's owner, and a model whose
/// only action is "insert a row" would be ceremony, not a stress test.
struct RegisterClient {
    /// @brief The client's name.
    std::string name;

    /// @brief Whether this registration is well-formed.
    /// @return `true` when the name is non-empty.
    [[nodiscard]] bool validate() const noexcept { return !name.empty(); }

    /// @brief Opts the generated form out of auto-submit-on-validity
    ///        (docs/spec/forms/forms.md, "Explicit submit mode"). Without
    ///        this, a form bound directly to a live controller would
    ///        register a client per keystroke.
    static constexpr bool explicitSubmit = true;
};

/// @brief The registered client's generated id.
struct RegisterClientResult {
    /// @brief The new client's id.
    ClientId clientId;
};

/// @brief Logs a new sample against a client. The lifecycle's entry point:
///        the created sample is `Registered`, at version 1.
struct RegisterSample {
    /// @brief The owning client.
    ClientId clientId;

    /// @brief The lab's own reference for the container (textual, free-form).
    std::string reference;

    /// @brief Whether this registration is well-formed.
    /// @return `true` when a client is named and a reference is given.
    [[nodiscard]] bool validate() const noexcept { return clientId.hasValue() && !reference.empty(); }

    /// @brief Same opt-out, same reason as `RegisterClient::explicitSubmit`.
    static constexpr bool explicitSubmit = true;
};

/// @brief One sample's full current state — the result of registering it,
///        attaching to it, or transitioning it.
struct SampleView {
    /// @brief The sample's id.
    SampleId id;

    /// @brief The owning client.
    ClientId clientId;

    /// @brief The lab's reference for the container.
    std::string reference;

    /// @brief Where the sample currently sits in the workflow.
    SampleState state = SampleState::Registered;

    /// @brief The base version an offline update must target to be applied.
    SampleVersion version;

    /// @brief When the sample was logged.
    ::morph::time::Timestamp registeredAt;
};

/// @brief Attaches this handler to one sample — the keyed action.
struct OpenSample {
    /// @brief The sample to attach to.
    SampleId sampleId;

    /// @brief Whether a sample is named.
    /// @return `true` when `sampleId` carries a value.
    [[nodiscard]] bool validate() const noexcept { return sampleId.hasValue(); }
};

/// @brief Re-reads the attached sample without changing it.
struct GetSample {
    /// @brief Always ready: the action names nothing of its own.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief `Registered → Received`: the container is physically in the lab.
struct ReceiveSample {
    /// @brief Always ready: the transition names nothing of its own.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief `Received → InProgress`: analysis work has started.
struct StartWork {
    /// @brief Always ready: the transition names nothing of its own.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief `InProgress → ToBeVerified`: results are captured and await the
///        four-eyes check.
struct SubmitForVerification {
    /// @brief Always ready: the transition names nothing of its own.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief `ToBeVerified → InProgress`: the verifier sent the sample back.
struct ReturnForRework {
    /// @brief Why the sample was returned; recorded in the journal.
    std::string reason;

    /// @brief Whether a reason was given.
    /// @return `true` when the reason is non-empty.
    [[nodiscard]] bool validate() const noexcept { return !reason.empty(); }

    /// @brief Same opt-out, same reason as `RegisterClient::explicitSubmit`.
    static constexpr bool explicitSubmit = true;
};

/// @brief `ToBeVerified → Published`: the report is released to the client.
struct PublishSample {
    /// @brief Always ready: the transition names nothing of its own.
    /// @return `true`.
    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief `Registered|Received → Rejected`: the container is refused.
struct RejectSample {
    /// @brief Why the sample was refused; recorded in the journal. Required:
    ///        a refusal with no stated reason is not auditable.
    std::string reason;

    /// @brief Whether a reason was given.
    /// @return `true` when the reason is non-empty.
    [[nodiscard]] bool validate() const noexcept { return !reason.empty(); }

    /// @brief Same opt-out, same reason as `RegisterClient::explicitSubmit`.
    static constexpr bool explicitSubmit = true;
};

}  // namespace lims

/// @brief Reflects `SampleState` as its stable name rather than an integer.
///
/// The journal payload *is* the audit record for this rung; an entry reading
/// `"state":"ToBeVerified"` stays legible after the enum gains a value,
/// whereas `"state":3` silently re-points at whatever now sits at index 3.
template <>
struct glz::meta<lims::SampleState> {
    using enum lims::SampleState;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Registered, Received, InProgress, ToBeVerified, Published, Rejected);
};
