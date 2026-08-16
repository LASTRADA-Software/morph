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
/// resolved design decision 1). The admin token `CreateBoard` generates is a
/// bare, server-generated random string, compared directly against a board
/// row's own `adminToken` column entirely inside `BoardModel::execute()`
/// (`requireAdmin()`, `board_model.cpp`) -- there is no framework-level
/// primitive for verifying a bare shared secret, so there is nothing for an
/// `IAuthorizer::authorize()` override to check here. `KanbanAuthorizer`
/// therefore leaves `authorize()` at `AllowAllAuthorizer`'s inherited
/// always-`true` and its whole body is the two instance-lifecycle hooks
/// below.
///
/// @par How this relates to `BookmarksAuthorizer`, precisely
/// The two share one idea -- both leave `authorizeRegister`/
/// `authorizeInstance` unconditionally permissive, by design rather than
/// necessity (the framework can gate both on identity now that `register`/
/// `attach` envelopes carry the caller's session; neither authorizer chooses
/// to) -- and nothing else. They are not
/// structurally alike: `BookmarksAuthorizer` derives from
/// `SigningAuthorizer`, overrides `authorize()` with a real carve-out on top
/// of genuine signed-token verification, ships principal-validation helpers,
/// and defines every body inline in its own header. `KanbanAuthorizer`
/// derives from `AllowAllAuthorizer`, overrides nothing that decides
/// anything, and splits a `.cpp` (`src/auth/kanban_authorizer.cpp`) for two
/// one-line `return true;` bodies -- a heavier file layout than bookmarks'
/// for a strictly smaller class. Read "mirrors bookmarks" claims about this
/// type as "reaches the same conclusion about those two hooks", never as
/// "is the same shape".
///
/// `register`/`attach`/`assign`/`deregister` envelopes now carry the
/// caller's authenticated session (both plain `wire::makeRegister` and
/// `wire::makeRegisterShared`, the keyed `OpenBoard{boardId}` attach
/// `BoardModel` uses), so `authorizeRegister` *could* gate a board attach by
/// admin/participant identity -- but this rung chooses not to: attaching to
/// a board by id is meant to be as open as knowing the shareable link, by
/// design (the rung README's resolved design decision 2). What actually
/// enforces admin-vs-participant is entirely inside `BoardModel::execute()`:
/// actions that require admin -- the model's token-gated actions -- call
/// `requireAdmin()` itself, re-checking the caller's token against the
/// board row's own stored column on every dispatch. This mirrors rung 2's
/// shape for a different reason, though: bookmarks' `authorizeInstance` is
/// now genuinely enforcing but checks *instance* ownership, which
/// `BoardModel` has no equivalent of at all (its instances are shared/keyed
/// by boardId, not owned by a caller) -- so the model's own re-check is not
/// standing in for a defeated framework hook, it is simply the only layer
/// that could ever express this rung's admin-vs-participant distinction.

namespace kanban::auth {

/// @brief This rung's `IAuthorizer`: unconditionally permissive on every
///        hook. See this file's `@file` comment for why that is the
///        correct, verified shape here rather than an oversight.
class KanbanAuthorizer : public ::morph::session::AllowAllAuthorizer {
  public:
    using AllowAllAuthorizer::AllowAllAuthorizer;

    /// @brief Admits every registration, by this rung's own design -- not
    ///        because identity is unavailable to gate on.
    ///
    /// Same conclusion as `BookmarksAuthorizer::authorizeRegister` (not the
    /// same shape -- see this file's `@file` comment), extended: this covers
    /// not only a plain `BoardModel` registration but also the keyed
    /// `OpenBoard` attach path (`registerModelShared`/`attachModel`'s wire
    /// form, which now carries a session too, exactly like plain
    /// `wire::makeRegister`). Admitting an unauthenticated attach gives away
    /// exactly what knowing the `boardId` already gives away, which by this
    /// rung's design is everything except actions requiring admin: certain
    /// actions are ones that re-check a token (`BoardModel::requireAdmin()`,
    /// against the board row's own `adminToken` column), and every other
    /// action is ungated on purpose -- see `board_model.hpp`'s "What is
    /// actually gated" section for the full, exact statement. This hook
    /// stays permissive regardless of whether @p ctx carries a real
    /// principal or not, since attaching to a board by id is meant to be as
    /// open as knowing the shareable link -- gating it now would change this
    /// rung's own product decision, not merely close a framework gap.
    /// @param ctx       Per-call session for the register envelope.
    ///                  Populated with the caller's verified principal when
    ///                  it holds a valid session, empty otherwise; ignored
    ///                  either way -- see above.
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
    /// principal against `ctx.principal`, and is now genuinely enforcing for
    /// bookmarks' plain-registered models. That comparison presumes a
    /// per-caller owner concept `BoardModel` never has in the first place:
    /// its instances are exclusively shared/keyed by `boardId`
    /// (`BRIDGE_MODEL_KEY`), which `RemoteServer` records ownerless by
    /// design (there is no single owning caller for a shared instance) --
    /// independent of, and unaffected by, whether register envelopes carry
    /// a session. This rung does not even attempt the comparison: the
    /// admin-vs-participant boundary this rung actually has lives entirely
    /// inside `BoardModel::execute()`, not at the instance-ownership layer.
    /// @param ctx            Per-call session. Ignored -- see above.
    /// @param modelType      Ignored: the same rule applies to every model.
    /// @param actionType     Ignored.
    /// @param modelId        Ignored: there is no per-instance owner to key on.
    /// @param ownerPrincipal Ignored -- always empty in practice: `BoardModel`
    ///                       instances are exclusively shared/keyed, and
    ///                       shared instances are recorded ownerless by
    ///                       design, not because owners can't be tracked.
    /// @return `true`, always -- see this function's own doc comment.
    [[nodiscard]] bool authorizeInstance([[maybe_unused]] const ::morph::session::Context& ctx,
                                         [[maybe_unused]] std::string_view modelType,
                                         [[maybe_unused]] std::string_view actionType,
                                         [[maybe_unused]] std::uint64_t modelId,
                                         [[maybe_unused]] std::string_view ownerPrincipal) const override;
};

}  // namespace kanban::auth
