// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "lims/lab/dto/verification_dto.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/org/authorization/declared.hpp"

/// @file
/// The lab module's declared authorization: four eyes on a reading.

namespace lims {

/// @brief Verifying a reading needs the verifier role at the sample's site, and the verifier must not be the
///        analyst who captured it: capture and verify are a separation-of-duties pair.
template <>
struct Authorization<VerifyResult> {
    /// @brief The role.
    static constexpr LimsRole role = LimsRole::Verifier;

    /// @brief The pair's first half.
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
