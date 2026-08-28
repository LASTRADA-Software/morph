// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <utility>

/// @file
/// `Login`, and the opaque token it mints. Mirrors
/// `bookmarks::Login`/`bookmarks::LoginResult`
/// (`examples/bookmarks/include/bookmarks/dto/auth_dto.hpp`) exactly -- same
/// shape, same trust story, same dev-mode-login disclosure -- so this rung's
/// `AuthModel` and `LedgerAuthorizer` need no design of their own beyond
/// substituting the type name.
///
/// Every mutating action in this rung needs a signed token before it can do
/// anything over `RemoteServer`: `SigningAuthorizer::authorize()` is
/// consulted on every `execute` and rejects a caller with no valid token
/// outright. `Login` is how a caller gets one in the first place, which is
/// why `AuthModel` is the one model whose actions a not-yet-authenticated
/// caller can reach (`LedgerAuthorizer::authorize`'s carve-out).
///
/// **Dev-mode login, stated plainly, not smoothed over**: `Login` takes a
/// bare `username` with no password or other credential. This rung ships no
/// user registry, no password hashing and no account-recovery flow. What *is*
/// real and load-bearing is the **token**: a genuine, server-signed,
/// `SigningAuthorizer`-verified credential. Nothing downstream of `Login`
/// trusts a client's claimed identity un-verified -- `RemoteServer`
/// overwrites `Context::principal` with the value it recovers from the
/// token's signature before any model runs, so `OpenAccount`,
/// `StoreTransaction` and every other mutating action see an authenticated
/// identity or none at all (`docs/spec/security.md`). Only the *login step
/// itself* is a stand-in; a real deployment replaces it -- password
/// verification, OAuth, whatever -- by changing the body of
/// `AuthModel::execute(const Login&)` and nothing else.

namespace ledger {

/// @brief Opaque bearer-token newtype (`examples/IMPLEMENTATION.md` rule 3's
///        protocol-scalars row). Same shape as `bookmarks::AuthToken`. Named
///        `AuthToken`, not `SessionToken`, to avoid colliding with
///        `morph::session::SessionToken`, an unrelated type this DTO's own
///        model wraps rather than reuses.
struct AuthToken {
    /// @brief The payload; `std::nullopt` means "no token".
    std::optional<std::string> value;

    /// @brief Constructs the empty state.
    constexpr AuthToken() noexcept = default;

    /// @brief Engages with @p token.
    explicit AuthToken(std::string token) noexcept : value{std::move(token)} {}

    /// @brief Adopts an optional payload as-is.
    /// @param payload The optional payload to adopt as-is.
    /// @return An `AuthToken` wrapping @p payload directly.
    [[nodiscard]] static AuthToken fromOptional(std::optional<std::string> payload) noexcept {
        AuthToken result;
        result.value = std::move(payload);
        return result;
    }

    /// @brief Whether a value has been entered.
    /// @return `true` if the payload is engaged.
    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }

    /// @brief Unchecked access to the engaged value (UB when empty, exactly
    ///        like `std::optional::operator*`).
    /// @return The engaged value.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] const std::string& operator*() const noexcept { return *value; }

    /// @brief Equality/ordering on the payload; empty compares only equal to empty.
    [[nodiscard]] auto operator<=>(const AuthToken&) const noexcept = default;
};

/// @brief Dev-mode login: no password. See this file's `@file` comment for
///        exactly what that does and does not mean for this rung's security
///        posture.
struct Login {
    /// @brief The identity to mint a token for.
    std::string username;

    /// @brief Whether @p username is acceptable as a principal.
    ///
    /// Declared rather than defined inline because the check lives in
    /// `ledger/auth/ledger_authorizer.hpp`, and including that here would
    /// pull `morph/session/session_auth.hpp` -- and, transitively, its whole
    /// HMAC/base64 implementation -- into every translation unit that only
    /// wants the DTO shape. Mirrors `bookmarks::Login::validate`.
    /// @return `true` if `username` is a valid principal.
    [[nodiscard]] bool validate() const noexcept;
};

/// @brief What a successful `Login` returns.
struct LoginResult {
    /// @brief The freshly minted, server-signed bearer token. The client
    ///        installs this via `Bridge::setDefaultSession`.
    AuthToken token;
    /// @brief The verified username, echoed back for display. Equal to the
    ///        `Login`'s own `username` -- returned so a client need not keep
    ///        its own copy alongside the token.
    std::string principal;
};

}  // namespace ledger

/// @brief Reflects `AuthToken` as its bare payload -- same rationale and
///        shape as `glz::meta<ledger::LedgerId>`: the wire form of an opaque
///        scalar newtype is the scalar, not an object with a `value` member.
template <>
struct glz::meta<ledger::AuthToken> {
    static constexpr auto value = &ledger::AuthToken::value;
    static constexpr std::string_view name = "AuthToken";
};
