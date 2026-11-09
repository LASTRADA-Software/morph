// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

/// @file
/// lims' development sign-in: a username in, a signed session token out. There is no password: the rung has no user
/// store, and a person's roles come from `GrantRole`, which the authoriser reads on every role-gated call.

namespace lims {

/// @brief Signs in as @p username.
struct LimsLogin {
    /// @brief The principal to sign in as.
    std::string username;

    /// @brief Explicit: a sign-in is sent by its button, never on every keystroke.
    static constexpr bool explicitSubmit = true;

    /// @brief Whether `username` is a principal this rung accepts.
    /// @return It is.
    [[nodiscard]] bool validate() const noexcept;
};

/// @brief A sign-in's answer: the session token the server verifies, and the principal it names.
struct LimsLoginResult {
    /// @brief The signed bearer token.
    std::string token;
    /// @brief The principal signed in.
    std::string principal;
};

}  // namespace lims
