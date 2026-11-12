// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lims/lab/dto/offline_dto.hpp"
#include "lims/lab/dto/result_dto.hpp"
#include "lims/lab/dto/sample_dto.hpp"
#include "lims/lab/dto/sample_entry_dto.hpp"
#include "lims/lab/dto/verification_dto.hpp"
#include "lims/org/authorization/declared.hpp"
#include "lims/org/models/org_model.hpp"

/// @file
/// The lab module's declared authorization: every write on a sample needs some role at the sample's site, and four
/// eyes on a reading.

namespace lims {

/// @brief The site of sample @p sample, as a scope.
/// @param sample The sample.
/// @return Its site; an empty site when the sample does not exist or has no site, which holds no role.
[[nodiscard]] Scope siteScopeOfSample(SampleId sample);

/// @brief The rule of a write on a sample the handler holds: some role at a site the model resolves from the sample
///        (or, for a sample entry, from its header), since the payload names none.
struct AnyRoleAtAttachedSite {
    /// @brief Any role is enough.
    static constexpr bool anyRole = true;
    /// @brief The model names the scope.
    static constexpr bool scopeFromModel = true;

    /// @brief The scope the model resolves: the site of the sample the handler is attached to.
    /// @tparam Model `SampleModel`.
    /// @tparam Action The action.
    /// @param model The instance.
    /// @return That site, alone.
    /// @throws NotFound when the handler is attached to no sample.
    template <typename Model, typename Action>
    [[nodiscard]] static std::vector<Scope> scopesIn(Model const& model, Action const& /*action*/) {
        return {model.attachedSiteScope()};
    }
};

/// @brief Every lifecycle transition needs some role at the attached sample's site.
template <>
struct Authorization<ReceiveSample> : AnyRoleAtAttachedSite {};
/// @copydoc Authorization<ReceiveSample>
template <>
struct Authorization<StartWork> : AnyRoleAtAttachedSite {};
/// @copydoc Authorization<ReceiveSample>
template <>
struct Authorization<SubmitForVerification> : AnyRoleAtAttachedSite {};
/// @copydoc Authorization<ReceiveSample>
template <>
struct Authorization<ReturnForRework> : AnyRoleAtAttachedSite {};
/// @copydoc Authorization<ReceiveSample>
template <>
struct Authorization<PublishSample> : AnyRoleAtAttachedSite {};
/// @copydoc Authorization<ReceiveSample>
template <>
struct Authorization<RejectSample> : AnyRoleAtAttachedSite {};

/// @brief Capturing a reading needs some role at the attached sample's site.
template <>
struct Authorization<CaptureConcentration> : AnyRoleAtAttachedSite {};

/// @brief Saving a sample entry needs some role at the header's site and, when it updates a stored sample, at that
///        sample's stored site too, so moving a sample needs a role at both ends.
template <>
struct Authorization<SaveSample> : AnyRoleAtAttachedSite {
    /// @brief The scopes the save writes to.
    /// @tparam Model `SampleModel`.
    /// @param model The instance.
    /// @return The header's site and, for an update, the stored site.
    template <typename Model>
    [[nodiscard]] static std::vector<Scope> scopesIn(Model const& model, SaveSample const& /*action*/) {
        return model.entryScopes();
    }
};

/// @brief Deleting a sample needs some role at the attached sample's site.
template <>
struct Authorization<DeleteSample> : AnyRoleAtAttachedSite {
    /// @brief The scope the delete acts in.
    /// @tparam Model `SampleModel`.
    /// @param model The instance.
    /// @return The stored sample's site, alone.
    /// @throws NotFound when the working copy was never saved.
    template <typename Model>
    [[nodiscard]] static std::vector<Scope> scopesIn(Model const& model, DeleteSample const& /*action*/) {
        return {model.storedSiteScope()};
    }
};

/// @brief Replaying a queued capture needs some role, held by the operator who captured it (the replay runs as that
///        operator), at the site of the sample the capture names.
template <>
struct Authorization<QueuedCapture> {
    /// @brief The handler judges the rule itself: it answers a redelivery before it asks for the role, so the action
    ///        declares no guard.
    static constexpr bool checkedInHandler = true;

    /// @brief Any role is enough.
    static constexpr bool anyRole = true;

    /// @brief The site of the sample the payload names.
    /// @param action The action.
    /// @return That site; an empty site when the sample does not exist.
    [[nodiscard]] static Scope scopeOf(QueuedCapture const& action) { return siteScopeOfSample(action.sampleId); }
};

/// @brief Resolving an offline-capture conflict needs some role at the site of the conflict's sample.
template <>
struct Authorization<ResolveConflict> {
    /// @brief Any role is enough.
    static constexpr bool anyRole = true;

    /// @brief The site of the sample the payload's conflict belongs to.
    /// @param action The action.
    /// @return That site; an empty site when the conflict or its sample does not exist, which holds no role.
    [[nodiscard]] static Scope scopeOf(ResolveConflict const& action);
};

/// @brief Registering a sample needs some role at the site the payload names.
template <>
struct Authorization<RegisterSample> {
    /// @brief Any role is enough.
    static constexpr bool anyRole = true;
    /// @brief The scope is spelled out in the payload.
    static constexpr bool scopeInPayload = true;

    /// @brief The site the payload names.
    /// @param action The action.
    /// @return That site.
    [[nodiscard]] static Scope scopeOf(RegisterSample const& action) { return Scope{action.siteId}; }
};

/// @brief Verifying a reading needs the verifier role at the sample's site, and the verifier must not be the
///        analyst who captured it: capture and verify are a separation-of-duties pair.
template <>
struct Authorization<VerifyResult> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Verifier;

    /// @brief The pair's first half: the registered id of `CaptureConcentration`. Spelled out because this header may
    ///        be read before the model that registers it; the declared-authorization suite compares it with the
    ///        registered id.
    static constexpr std::string_view separatedFrom = "CaptureConcentration";

    /// @brief The refusal when the verifier captured the reading.
    static constexpr std::string_view sameActorRefusal = "captured this result and cannot also verify it";

    /// @brief The site of the sample the payload's result belongs to.
    /// @param action The action.
    /// @return That site; an empty site when the result or its sample does not exist.
    [[nodiscard]] static Scope scopeOf(VerifyResult const& action);

    /// @brief Who captured the payload's result. A replayed or force-applied capture keeps its field operator here,
    ///        so the pair holds for those too.
    /// @param action The action.
    /// @return The capturing analyst; empty when the result does not exist.
    [[nodiscard]] static std::optional<std::string> firstPerformerOf(VerifyResult const& action);
};

}  // namespace lims

// The models are included last, after every declaration their registrations need.
#include "lims/lab/models/sample_model.hpp"
