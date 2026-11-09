// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "lims/lab/dto/offline_dto.hpp"
#include "lims/lab/dto/sample_dto.hpp"
#include "lims/lab/dto/verification_dto.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/org/authorization/declared.hpp"

/// @file
/// The lab module's declared authorization: every write on a sample needs some role at the sample's site, and four
/// eyes on a reading.

namespace lims {

/// @brief The site of sample @p sample, as a scope.
/// @param sample The sample.
/// @return Its site; an empty site when the sample does not exist or has no site, which holds no role.
[[nodiscard]] Scope siteScopeOfSample(SampleId sample);

/// @brief The rule of a write on an existing sample: some role at the sample's site, which the model resolves from
///        the sample it is attached to, since the payload names none.
struct AnyRoleAtAttachedSite {
    /// @brief Any role is enough.
    static constexpr bool anyRole = true;
    /// @brief The model names the scope.
    static constexpr bool scopeFromModel = true;
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

/// @brief Replaying a queued capture needs some role, held by the operator who captured it (the replay runs as that
///        operator), at the site of the sample the capture names.
template <>
struct Authorization<QueuedCapture> {
    /// @brief Any role is enough.
    static constexpr bool anyRole = true;

    /// @brief The site of the sample the payload names.
    /// @param action The action.
    /// @return That site; an empty site when the sample does not exist.
    [[nodiscard]] static Scope scopeOf(QueuedCapture const& action) { return siteScopeOfSample(action.sampleId); }
};

/// @brief Registering a sample needs some role at the site the payload names.
template <>
struct Authorization<RegisterSample> {
    /// @brief Any role is enough.
    static constexpr bool anyRole = true;

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

    /// @brief The pair's first half: the registered id of `CaptureConcentration`, so the pair names an action that
    ///        exists.
    static constexpr std::string_view separatedFrom = ::morph::model::ActionTraits<CaptureConcentration>::typeId();

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
