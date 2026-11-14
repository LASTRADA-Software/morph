// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/session/session.hpp>

#include "lims/org/core/errors.hpp"

/// @file
/// The principal check every mutating action makes. Kept apart from `model_support.hpp`, which includes the ORM, so
/// that the action guard, which a client build also reads, does not pull the ORM in.

namespace lims {

/// @brief Throws unless a non-empty principal is authenticated.
///
/// Called by every mutating action rather than assumed: the README names
/// empty-principal audit entries as disqualifying for this rung, and an
/// authorizer that ran is not the same fact as a principal that is present
/// (the authorize/authenticate TOCTOU it calls out).
/// @throws EmptyPrincipalError when no principal, or an empty one, is in scope.
inline void requirePrincipal() {
    const auto* ctx = ::morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw EmptyPrincipalError{};
    }
}

}  // namespace lims
