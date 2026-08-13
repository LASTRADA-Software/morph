// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/session/session.hpp>

#include <cstdint>
#include <string_view>

/// @file
/// This rung's one `IAuthorizer`. Narrower than
/// `bookmarks::auth::BookmarksAuthorizer` (`examples/bookmarks/include/bookmarks/auth/bookmarks_authorizer.hpp`)
/// by design, not by omission: this rung has no signed-token mechanism at
/// all -- no `SigningAuthorizer`, no `TokenIssuer` (see the rung README's
/// resolved design decision 1). The admin token `CreatePoll` generates is a
/// bare, server-generated random string, compared directly against a poll
/// row's own `adminToken` column entirely inside `PollModel::execute()`
/// (`requireAdmin()`, `poll_model.cpp`) -- there is no framework-level
/// primitive for verifying a bare shared secret, so there is nothing for an
/// `IAuthorizer::authorize()` override to check here. `PollsAuthorizer`
/// therefore leaves `authorize()` at `AllowAllAuthorizer`'s inherited
/// always-`true` and its whole body is the two instance-lifecycle hooks
/// below.
///
/// @par How this relates to `BookmarksAuthorizer`, precisely
/// The two share one idea -- both leave `authorizeRegister`/
/// `authorizeInstance` unconditionally permissive because finding 027 makes
/// any identity check there unenforceable -- and nothing else. They are not
/// structurally alike: `BookmarksAuthorizer` derives from
/// `SigningAuthorizer`, overrides `authorize()` with a real carve-out on top
/// of genuine signed-token verification, ships principal-validation helpers,
/// and defines every body inline in its own header. `PollsAuthorizer`
/// derives from `AllowAllAuthorizer`, overrides nothing that decides
/// anything, and splits a `.cpp` (`src/auth/polls_authorizer.cpp`) for two
/// one-line `return true;` bodies -- a heavier file layout than bookmarks'
/// for a strictly smaller class. Read "mirrors bookmarks" claims about this
/// type as "reaches the same conclusion about those two hooks", never as
/// "is the same shape".
///
/// @warning Both of those two hooks are limited by
/// `docs/findings/027-register-envelope-carries-no-session.md`, exactly as
/// `BookmarksAuthorizer`'s own `@file` comment documents: morph's
/// `register` envelope carries no session, so `RemoteServer` sees an empty,
/// unauthenticated `Context` on every registration a `Bridge` client makes.
/// The rung README's resolved design decision 2 extends that finding's
/// scope explicitly to `registerModelShared`/`attachModel` (the keyed
/// `OpenPoll{pollId}` attach `PollModel` uses): `wire::makeRegisterShared`
/// carries no session either, exactly like plain `wire::makeRegister`, so
/// `authorizeRegister` cannot gate a poll attach by admin/participant token
/// -- and is not meant to; attaching to a poll by id is meant to be as open
/// as knowing the shareable link, by this rung's own design. What actually
/// enforces admin-vs-participant is entirely inside `PollModel::execute()`:
/// `FinalizePoll` -- the model's *only* token-gated action -- calls
/// `requireAdmin()` itself, re-checking the caller's token against the
/// poll row's own stored column on every dispatch, mirroring rung 2's
/// "`authorizeInstance` is inert, the model re-checks ownership" pattern.

namespace polls::auth {

/// @brief This rung's `IAuthorizer`: unconditionally permissive on every
///        hook. See this file's `@file` comment for why that is the
///        correct, verified shape here rather than an oversight.
class PollsAuthorizer : public ::morph::session::AllowAllAuthorizer {
  public:
    using AllowAllAuthorizer::AllowAllAuthorizer;

    /// @brief Admits every registration -- the only decision finding 027
    ///        (extended to shared/keyed registration by this rung's own
    ///        design decision 2) leaves this hook able to make.
    ///
    /// Same reasoning as `BookmarksAuthorizer::authorizeRegister` (not the
    /// same shape -- see this file's `@file` comment), extended: this covers
    /// not only a plain `PollModel` registration but also the keyed
    /// `OpenPoll` attach path (`registerModelShared`/`attachModel`'s wire
    /// form, which is still a session-less `register` envelope per design
    /// decision 2). Admitting an unauthenticated attach gives away exactly
    /// what knowing the `pollId` already gives away, which by this rung's
    /// design is everything except finalizing: `FinalizePoll` is the one
    /// action that re-checks a token (`PollModel::requireAdmin()`, against
    /// the poll row's own `adminToken` column), and every other action is
    /// ungated on purpose -- see `poll_model.hpp`'s "What is actually gated"
    /// section for the full, exact statement. Requiring an identity that
    /// cannot be presented (finding 027's `ctx.principal` is always empty here) would
    /// not be security, it would be an outage that rejects every real
    /// client's first `BridgeHandler` construction -- including one that
    /// goes on to present a perfectly valid admin token to `FinalizePoll`.
    /// @param ctx       Per-call session for the register envelope. Empty
    ///                  in practice -- see this file's `@file` warning.
    /// @param modelType Target model type id. `RemoteServer` has already
    ///                  rejected a type its registry does not know by the
    ///                  time this runs.
    /// @return `true`, always -- see this function's own doc comment.
    [[nodiscard]] bool authorizeRegister([[maybe_unused]] const ::morph::session::Context& ctx,
                                         [[maybe_unused]] std::string_view modelType) const override;

    /// @brief Admits every per-instance operation -- there is no owner
    ///        principal to check against here.
    ///
    /// `BookmarksAuthorizer::authorizeInstance` compares a recorded owner
    /// principal against `ctx.principal`; that comparison presumes a
    /// registration-time identity finding 027 never actually supplies (see
    /// its own `@warning`). This rung does not even attempt it: `PollModel`
    /// instances are shared/keyed by `pollId` (`BRIDGE_MODEL_KEY`, not
    /// per-caller ownership), so there is no "owner" concept for this hook
    /// to enforce in the first place -- the admin-vs-participant boundary
    /// this rung actually has lives entirely inside `PollModel::execute()`,
    /// not at the instance-ownership layer.
    /// @param ctx            Per-call session. Ignored -- see above.
    /// @param modelType      Ignored: the same rule applies to every model.
    /// @param actionType     Ignored.
    /// @param modelId        Ignored: there is no per-instance owner to key on.
    /// @param ownerPrincipal Ignored -- always empty in practice (finding 027).
    /// @return `true`, always -- see this function's own doc comment.
    [[nodiscard]] bool authorizeInstance([[maybe_unused]] const ::morph::session::Context& ctx,
                                         [[maybe_unused]] std::string_view modelType,
                                         [[maybe_unused]] std::string_view actionType,
                                         [[maybe_unused]] std::uint64_t modelId,
                                         [[maybe_unused]] std::string_view ownerPrincipal) const override;
};

}  // namespace polls::auth
