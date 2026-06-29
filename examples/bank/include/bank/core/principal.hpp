// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/session.hpp>

#include <string>

/// @file
/// Helpers for reading the authenticated principal from the morph session
/// context. The bridge attaches its default session (set once at login via
/// `Bridge::setDefaultSession`) to every call, and the model reads it here
/// without changing its `execute()` signatures.

namespace bank {

/// @brief Returns the current session principal, or empty if none is attached.
[[nodiscard]] inline std::string sessionPrincipal() {
    if (const auto* ctx = morph::session::current(); ctx != nullptr) {
        return ctx->principal;
    }
    return {};
}

/// @brief Returns @p explicitOwner if non-empty, otherwise the session principal.
[[nodiscard]] inline std::string resolveOwner(const std::string& explicitOwner) {
    return explicitOwner.empty() ? sessionPrincipal() : explicitOwner;
}

}  // namespace bank
