# Ladder Rung 2 (Bookmarks) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build rung 2 of the [application ladder](../../../examples/LADDER.md) —
**bookmarks**: three models (`BookmarkModel`, `TagModel`, `SharedFeedModel`),
a real bookmark↔tag many-to-many, the ladder's first genuine multi-user
authorization, its first background job, and its first multi-row
(outbox-managed) journal writes — per
[`examples/bookmarks/README.md`](../../../examples/bookmarks/README.md)
(design questions resolved in that file — read it first, it is this plan's
design authority, alongside two corrections this plan's own research made
to it; see "Corrections to the README" below).

**Architecture:** `ladder_bookmarks_lib` (STATIC: DTOs, entities, migration,
three models, app bootstrap, the rung's `IAuthorizer` — morph + Lightweight,
no Qt-Widgets/Catch2), `ladder_bookmarks_gui_lib` (STATIC: presenters +
forms-controller glue — `Qt6::Core` only), `ladder_bookmarks_gui` (EXE:
desktop client), `ladder_bookmarks_gui_wasm` (EXE, Emscripten only), a
standalone `ladder_bookmarks_server` (EXE: hosts all three models over
`QtWebSocketServer` with the real `SigningAuthorizer`-derived authorizer
installed), and `ladder_bookmarks_tests` (EXE: Catch2 model + presenter
tests, full `BackendRig` mode matrix). `morph_add_rung()`
(`cmake/morph_add_rung.cmake`) needs **no changes** — confirmed by reading
it: it globs `src/models/*.cpp` with no per-model target logic, so three
models' `.cpp` files fold into one `ladder_bookmarks_lib` exactly like
`pastebin`'s one model does, and `bookmarks` is already listed in
`examples/CMakeLists.txt`'s `_morph_known_rungs`. Task 13 is therefore
small: one `CMakeLists.txt` calling `morph_add_rung(NAME bookmarks)`.

**Tech Stack:** C++23, Qt6 (Core, WebSockets, Quick/QuickControls2), Catch2 v3,
Lightweight ORM (SQLite/ODBC), CMake 3.25+, `morph::forms` +
`MorphForms` QML module, `morph::journal::FileActionLog` +
`morph::journal::OutboxRelay`, `morph::session::SigningAuthorizer`.

## Corrections to the README (found during this plan's research, not yet
## written back into `examples/bookmarks/README.md` — apply them as this
## plan's authority where the two disagree; a follow-up task should fold
## these into the README itself, see the Self-Review section)

Two claims in the README's "Design decisions" and "morph subsystems
exercised" sections do not survive contact with `RemoteServer`'s actual
source and are corrected here, with citations. Nothing below is guesswork —
every claim cites the exact line read.

1. **`BookmarkModel`/`TagModel` must NOT be registered as framework-`shared`
   instances.** `include/morph/core/remote.hpp:800` —
   `_owners[fresh] = std::string{};  // shared instances are ownerless, by
   design` — inside `RemoteServer::acquireSharedInstance()`. The surrounding
   doc comment (`remote.hpp:714-722`) spells out why: *"A shared instance is
   recorded with an empty owner principal: `IAuthorizer::authorizeInstance`'s
   documented `ownerPrincipal == ctx.principal` policy would otherwise reject
   every client but the one that created it, defeating cross-client sharing
   outright."* This means `authorizeInstance`'s ownership check is a **no-op**
   for any `AllowShared`/`BRIDGE_MODEL_KEY` model — `ownerPrincipal` is
   *always* empty for it, so `ownerPrincipal.empty() || ownerPrincipal ==
   ctx.principal` is always `true`. The README's "keyed by principal... via
   `authorizeInstance`" design would give `BookmarkModel`/`TagModel` **zero**
   real per-instance protection from the framework.

   The working mechanism is the *other* registration path: plain
   (non-shared) `register` genuinely records the authenticated caller as the
   instance's owner — `remote.hpp:962-966,1011`: *"Record the owner
   principal for per-instance authorization: `env.session`'s principal is
   already the verified identity stamped above... This is what lets
   `authorizeInstance` later deny a different principal,"* followed by
   `_owners[mid] = std::move(env.session.principal);`. So: **`BookmarkModel`
   and `TagModel` are registered plain — no `BRIDGE_MODEL_KEY`/`AllowShared`
   — exactly like `pastebin::PasteModel`.** Each client's own `register`
   calls gets its own fresh instance, `authorizeInstance` genuinely denies
   any *other* principal from touching that specific `modelId`, and — since
   a model instance carries no meaningful in-memory state anyway (all real
   state is the database, partitioned by an `ownerPrincipal` column) —
   nothing about "one instance per user" is lost: every registration by the
   same user, from any device, reads and writes the identical rows.

   `SharedFeedModel` is **also registered plain**, for a different reason:
   `AllowShared` requires a keyed action (`BRIDGE_MODEL_KEY`, an
   `ActionKeyTraits<Action>::key(action)` extracted from a client-supplied
   action field, `include/morph/core/bridge.hpp:1036-1048,1131-1139`) to
   attach — machinery built for "many clients converge on the *same named*
   instance," which buys `SharedFeedModel` nothing: it has no per-user state
   to converge on, every instance reads the identical `WHERE shared = 1`
   rows regardless of how many separate instances exist, and
   [`LADDER.md`](../../../examples/LADDER.md)'s own cross-cutting stress map
   assigns "Shared instances" coverage to rungs 3/4/6/8, not rung 2 — so
   there is no rung-2 obligation to exercise `AllowShared` at all. Plain
   registration is simpler and sufficient: `authorizeRegister`'s "must be
   authenticated" gate is the real policy (Task 1), and
   `authorizeInstance`'s per-instance check, while it does apply, is
   incidental — `SharedFeedModel::execute()` never consults `ownerPrincipal`
   itself, so it does not matter that each user's own handle to it is
   technically "owned" by them alone.

2. **The model itself does not need to "remember" an owner across calls.**
   Since `BookmarkModel`/`TagModel` are plain-registered (point 1), and
   `session::current()` is repopulated by the framework on **every**
   dispatched action (`session::detail::ScopedContext`,
   `include/morph/session/session.hpp:249-264`, installed around each
   `execute()` by `RemoteServer::dispatchExecute`/`LocalBackend::execute`),
   the model reads `session::current()->principal` fresh on every call and
   uses it directly as the `WHERE owner_principal = ?` filter value — no
   per-instance mutable "captured on first use" state is needed anywhere.
   This is simpler than the README's "captures the calling principal at
   first use" framing implies (that framing does not appear verbatim in the
   README, but is the natural reading of "per-user shared instances" and is
   corrected here for clarity).

One authorizer implements both models' real ownership check and
`SharedFeedModel`'s "any authenticated principal" policy **without any
model-type branching** — see Task 1: `ownerPrincipal.empty() ||
ownerPrincipal == ctx.principal` is simultaneously the correct policy for
plain-registered instances (real, non-empty owner) and shared instances
(always-empty owner, so always permissive) — the same one-line check
`tests/test_policy_hardening.cpp`'s `OwnershipAuthorizer` already
demonstrates, applied uniformly.

## Global Constraints

- C++23 throughout (`target_compile_features(... PUBLIC cxx_std_23)`).
- **DTO type discipline** ([`IMPLEMENTATION.md`](../../../examples/IMPLEMENTATION.md)
  rule 3): the only plain type permitted in an action/result field is
  `std::string` (URLs, titles, descriptions, notes, tag names, HTML
  fragments). Everything else is a strong type — `BookmarkId`, `TagId`,
  `Cursor`, `ImportOpId`, `morph::time::Timestamp`, `enum class`, a
  dimensionless `Count` quantity. **No `int`/`int64_t`/`double`/`float`/
  `bool`/raw enum in any DTO field.**
- **Persistence exclusively through Lightweight**
  ([`IMPLEMENTATION.md`](../../../examples/IMPLEMENTATION.md) rule 4). No
  new sanctioned-escape-tier entry is needed this rung — confirmed in Task 5:
  `HasManyThrough` exists but is incompatible with `DataMapper::Update`
  (below), so this plan avoids it entirely rather than fighting it; tag
  associations are read via a plain `Query<BookmarkTagRecord>().Where(...)`,
  which is ordinary `DataMapper` usage, not an escape. `BulkEdit`/tag
  merge use `Lightweight::SqlTransaction` wrapping N ordinary
  `DataMapper`/`SqlStatement` calls, the same pattern rung 1's
  `EditPaste`/`GetPaste` already proved (`examples/pastebin/src/models/paste_model.cpp`).
- **`HasMany`/`HasManyThrough` incompatibility with `Update()`** (verified
  against Lightweight's vendored source this plan's research read directly,
  `build/*/​_deps/lightweight-src/src/Lightweight/DataMapper/DataMapper.hpp:1974-1985`
  and `Description.hpp:181-187`): `DataMapper::Update()`'s non-reflection
  path calls `field.IsModified()` on **every** record member via
  `EnumerateRecordMembers` (which does not filter by field kind), and
  neither `HasMany<T>` nor `HasManyThrough<T,U>` declares an `IsModified()`
  method — so a record type that embeds either as a member fails to compile
  the moment `Update()` is instantiated for it. `examples/bank/include/bank/db/account_entity.hpp`'s
  own doc comment independently confirms this for `HasMany` ("`DataMapper::Update`
  cannot be instantiated for a record that has a `HasMany` member... Children
  are reached via their `account_id` foreign key instead"). **Rule for this
  rung: `BookmarkRecord`/`TagRecord` carry zero relation-typed members.**
  Tag reads go through explicit `Query<BookmarkTagRecord>()` calls in the
  model, never through an embedded `HasManyThrough` field. `BookmarkTagRecord`
  itself never needs `Update()` (only `Create`/delete), so its `BelongsTo<>`
  members are unaffected (`BelongsTo` **does** support `Update()` — bank's
  own `AccountRecord::user` is a `BelongsTo` field on a record that *is*
  updated elsewhere in bank).
- **Auth**: every model-bearing action requires a valid signed token
  (`morph::session::SigningAuthorizer`, default `hmacSha256` MAC — this
  rung's dev/test posture, not `MORPH_REQUIRE_VETTED_HMAC`, per the README).
  One `BookmarksAuthorizer` (Task 1) covers all three models — see
  "Corrections" above. `BookmarkModel`/`TagModel`/`SharedFeedModel` are
  **all registered plain** — no `BRIDGE_MODEL_KEY`/`AllowShared` anywhere in
  this rung (Task 10 confirms `SharedFeedModel`'s reasoning: no per-user
  state to converge on, so `AllowShared`'s keying machinery buys nothing).
  A restricted principal charset (ASCII, no control
  bytes) is enforced by this rung's own registration/login DTO `validate()`
  as defense-in-depth against finding 026's unescaped-`glz::write_json` gap
  in `TokenIssuer::issue()` (`include/morph/session/session_auth.hpp:346`,
  `docs/findings/026-control-byte-escaping-missing-in-three-sibling-writers.md`)
  — this rung does not fix core, only guards its own input at the boundary
  where it feeds that code path.
- **Journal — split by blast radius** (README, resolved): `BulkEdit` and
  `RenameTag`/`MergeTags` (multi-row) use `IModelHolder::setOutboxManaged(true)`
  + `journal::OutboxRelay`, with the model's own SQL-backed outbox table
  written inside the same `SqlTransaction` as the mutation (Task 8/9). Every
  other action (single-row CRUD, archive/unarchive, the background fetch's
  `RecordMetadata`) keeps the framework's default two-independent-write
  auto-append — explicit, not the implicit choice rung 1 made.
- **No generic undo** (README, resolved, consistent with
  [`LADDER.md`](../../../examples/LADDER.md)'s "Journal honesty"):
  `DeleteBookmark` is a hard delete with no compensating action.
- **Time**: model code never calls `morph::time::Timestamp::now()`/
  `DateTime::now()` directly — always `morph::ladder::now()`
  (`examples/common/clock.hpp`, already shipped by rung 1 — no new task
  needed for it).
- **No `sleep_for` outside `pump.hpp`** — a review-rejectable defect
  ([`TESTING.md`](../../../examples/TESTING.md) "Pumping discipline").
- **Presenters/GUI code take `(Bridge&, IExecutor*)`, never construct
  backends or executors themselves** ([`TESTING.md`](../../../examples/TESTING.md)
  presenter rule 2).
- **Schema-driven GUI, always** ([`IMPLEMENTATION.md`](../../../examples/IMPLEMENTATION.md)
  rule 2). No hand-built input widgets without a written justification.
- Every ladder CMake target wraps its definition in
  `if(AF_COVERAGE) apply_coverage(<target>) endif()`.
- Model coverage target: the measured ceiling, not a blind 100%
  ([`IMPLEMENTATION.md`](../../../examples/IMPLEMENTATION.md) rule 5),
  store-error branches provoked through the real schema
  (`db_busy_fixture.hpp` for `SQLITE_BUSY`, a dropped table or a conflicting
  row for the rest — never a mock driver, per finding 018's now-closed
  resolution).
- License hygiene: nothing ported from linkding/Shaarli beyond
  requirements/data-shape/behavior; all implementation original.

---

## Task 1: The rung's authorizer and principal charset

**Files:**
- Create: `examples/bookmarks/include/bookmarks/auth/bookmarks_authorizer.hpp`
- Test: `examples/bookmarks/tests/test_bookmarks_authorizer.cpp`

**Interfaces:**
- Produces: `bookmarks::auth::isValidPrincipal(std::string_view) -> bool`;
  `bookmarks::auth::kMetadataFetcherPrincipal` (a `std::string_view`
  constant, `"system:metadata-fetcher"` — the service-principal convention
  the README names, consumed by Task 12's background worker);
  `bookmarks::auth::BookmarksAuthorizer`, a concrete class derived from
  `::morph::session::SigningAuthorizer`, inheriting its constructors,
  overriding `authorizeRegister`/`authorizeInstance` (the former exempts
  `"AuthModel"` from the authentication gate — Task 12's `AuthModel` is how
  a caller obtains a token in the first place). Every later task that
  builds a `RemoteServer` (Task 12, Task 14+'s test fixtures) constructs one
  of these and passes it as the server's authorizer.
  `bookmarks::auth::setTokenIssuer`/`bookmarks::auth::tokenIssuer` — a
  process-global holder for the shared `TokenIssuer`, mirroring
  `morph::journal::setActionLog`'s identical shape (the same answer to the
  same "registry-constructed models are always default-constructed"
  problem, docs/findings/003/020): `AuthModel` (Task 12) has no
  constructor-injection seam for the secret it needs to mint tokens, so
  `App` installs one process-wide at startup instead.

This is the one piece every other model-bearing task depends on, and it is
small and fully testable in isolation — mirroring rung 1 Task 1's clock.

- [ ] **Step 1: Write the failing test**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/auth/bookmarks_authorizer.hpp"

#include <catch2/catch_test_macros.hpp>

using bookmarks::auth::BookmarksAuthorizer;
using bookmarks::auth::isValidPrincipal;
using bookmarks::auth::kMetadataFetcherPrincipal;
using morph::session::Context;
using morph::session::SessionToken;
using morph::session::TokenIssuer;

namespace {
constexpr std::string_view kSecret = "test-only-shared-secret";
}

TEST_CASE("isValidPrincipal accepts ordinary usernames and the service principal",
          "[bookmarks][auth]") {
    CHECK(isValidPrincipal("alice"));
    CHECK(isValidPrincipal("alice_2"));
    CHECK(isValidPrincipal("alice.smith-99"));
    CHECK(isValidPrincipal(kMetadataFetcherPrincipal));
}

TEST_CASE("isValidPrincipal rejects the empty string, control bytes, and overlong input",
          "[bookmarks][auth]") {
    // Empty: never a valid identity to register as.
    CHECK_FALSE(isValidPrincipal(""));
    // A raw control byte -- exactly the class of input finding 026 says
    // TokenIssuer::issue()'s unescaped glz::write_json can corrupt. Rejected
    // here, at this rung's own boundary, regardless of whether core is ever
    // fixed.
    CHECK_FALSE(isValidPrincipal(std::string_view{"ali\x01ce", 6}));
    CHECK_FALSE(isValidPrincipal(std::string_view{"ali\nce", 6}));
    // 65 bytes -- one past the 64-byte bound.
    const std::string tooLong(65, 'a');
    CHECK_FALSE(isValidPrincipal(tooLong));
    // 64 bytes -- the boundary itself is accepted.
    const std::string atLimit(64, 'a');
    CHECK(isValidPrincipal(atLimit));
}

TEST_CASE("BookmarksAuthorizer authenticates and authorizes a validly signed token",
          "[bookmarks][auth]") {
    const BookmarksAuthorizer authz{std::string{kSecret}};
    const TokenIssuer issuer{std::string{kSecret}};

    const std::string token = issuer.issue(SessionToken{
        .principal = "alice",
        .issuedAtMs = 0,
        .expiresAtMs = 4102444800000,  // year 2100, far future
        .roles = {},
    });

    Context ctx;
    ctx.token = token;

    CHECK(authz.authorize(ctx, "BookmarkModel", "CreateBookmark"));
    const auto principal = authz.authenticate(ctx);
    REQUIRE(principal.has_value());
    CHECK(*principal == "alice");
}

TEST_CASE("BookmarksAuthorizer rejects a tampered or expired token", "[bookmarks][auth]") {
    const BookmarksAuthorizer authz{std::string{kSecret}};
    const TokenIssuer issuer{std::string{kSecret}};

    const std::string expired = issuer.issue(SessionToken{
        .principal = "alice",
        .expiresAtMs = 1,  // 1970-01-01T00:00:00.001Z -- long expired
    });
    Context expiredCtx;
    expiredCtx.token = expired;
    CHECK_FALSE(authz.authorize(expiredCtx, "BookmarkModel", "CreateBookmark"));

    const std::string valid = issuer.issue(SessionToken{
        .principal = "alice",
        .expiresAtMs = 4102444800000,
    });
    Context tamperedCtx;
    tamperedCtx.token = valid + "x";  // corrupt the signature
    CHECK_FALSE(authz.authorize(tamperedCtx, "BookmarkModel", "CreateBookmark"));

    Context noTokenCtx;  // empty token: malformed
    CHECK_FALSE(authz.authorize(noTokenCtx, "BookmarkModel", "CreateBookmark"));
}

TEST_CASE("BookmarksAuthorizer::authorizeRegister requires an authenticated principal",
          "[bookmarks][auth]") {
    const BookmarksAuthorizer authz{std::string{kSecret}};

    Context anonymous;  // principal never stamped -- the "not authenticated" state
    CHECK_FALSE(authz.authorizeRegister(anonymous, "BookmarkModel"));

    Context authenticated;
    authenticated.principal = "alice";  // as RemoteServer would stamp it post-authenticate()
    CHECK(authz.authorizeRegister(authenticated, "BookmarkModel"));

    // AuthModel is exempt -- its whole job is minting the token a caller
    // does not have yet (Task 12), so it cannot itself require one.
    CHECK(authz.authorizeRegister(anonymous, "AuthModel"));
}

TEST_CASE("BookmarksAuthorizer::authorizeInstance enforces real ownership for a "
          "plain-registered instance, and passes through an ownerless (shared) one",
          "[bookmarks][auth]") {
    const BookmarksAuthorizer authz{std::string{kSecret}};

    Context asAlice;
    asAlice.principal = "alice";
    Context asMallory;
    asMallory.principal = "mallory";

    // A plain-registered instance genuinely recorded "alice" as its owner
    // (RemoteServer's real register path, verified in this plan's own
    // research -- see remote.hpp:1011): the owner may act on it...
    CHECK(authz.authorizeInstance(asAlice, "BookmarkModel", "EditBookmark", 42, "alice"));
    // ...a different, real, authenticated principal may not.
    CHECK_FALSE(authz.authorizeInstance(asMallory, "BookmarkModel", "EditBookmark", 42, "alice"));

    // An empty recorded owner -- what a *shared* instance always gets
    // (remote.hpp:800, "shared instances are ownerless, by design") -- must
    // pass through for anyone, matching the framework's own documented
    // rationale for why authorizeInstance cannot reject shared access.
    CHECK(authz.authorizeInstance(asMallory, "SharedFeedModel", "ListSharedFeed", 7, ""));
}

TEST_CASE("setTokenIssuer/tokenIssuer share one process-global slot", "[bookmarks][auth]") {
    CHECK(bookmarks::auth::tokenIssuer() == nullptr);
    auto issuer = std::make_shared<morph::session::TokenIssuer>(std::string{kSecret});
    bookmarks::auth::setTokenIssuer(issuer);
    CHECK(bookmarks::auth::tokenIssuer() == issuer);
    bookmarks::auth::setTokenIssuer(nullptr);
    CHECK(bookmarks::auth::tokenIssuer() == nullptr);
}
```

- [ ] **Step 2: Run to verify it fails to compile** (the header does not exist yet)

Run: `cmake --build build/clang-coverage --target ladder_bookmarks_tests` (target
does not exist until Task 13 wires the CMakeLists.txt — for this task alone,
compile the test file directly against `morph`/Catch2's include paths, or
defer running it until Task 13's CMake task exists and come back; either is
acceptable, but the header must not exist yet at this point).
Expected: FAIL — `bookmarks/auth/bookmarks_authorizer.hpp` file not found.

- [ ] **Step 3: Write the implementation**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/session/session_auth.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

/// @file
/// The one `IAuthorizer` every model-bearing `RemoteServer` in this rung
/// installs. Real signed-token authentication (README "Sessions &
/// authorization" -- bookmarks is the first rung to wire this end-to-end,
/// not merely touch `IAuthorizer`), plus the two hooks
/// `SigningAuthorizer` leaves at their allow-all defaults:
/// `authorizeRegister` (must be authenticated) and `authorizeInstance` (real
/// per-instance ownership for a plain-registered instance; a pass-through
/// for an ownerless/shared one -- see this plan's own "Corrections to the
/// README" for why both `BookmarkModel`/`TagModel` and `SharedFeedModel` are
/// registered plain, making this one check correct for all three without
/// branching on model type).

namespace bookmarks::auth {

/// @brief Service principal the internal metadata-fetch worker (Task 12)
///        authenticates as. Reserved by convention, not by any framework
///        mechanism -- nothing stops a real user from registering under this
///        name too, since usernames are not a secret; the worker is
///        distinguished by holding a token only the server process itself
///        can mint (it shares the server's `TokenIssuer` secret), not by the
///        string alone.
inline constexpr std::string_view kMetadataFetcherPrincipal = "system:metadata-fetcher";

/// @brief Longest principal this rung accepts, in bytes.
inline constexpr std::size_t kMaxPrincipalBytes = 64;

/// @brief Whether @p principal is acceptable as a login/registration
///        identity for this rung.
///
/// Defense-in-depth against finding 026
/// (`docs/findings/026-control-byte-escaping-missing-in-three-sibling-writers.md`):
/// `morph::session::TokenIssuer::issue()` writes `SessionToken::principal`
/// through a plain `glz::write_json` with no control-byte escaping
/// (`session_auth.hpp:346`). A principal containing a raw control byte would
/// corrupt the token's JSON payload on the way in. This rung does not fix
/// that shared code -- the finding is `disposition: open`, not this rung's
/// to close -- but nothing requires accepting hostile input at its own
/// boundary while waiting for it. The bound is deliberately ASCII-only and
/// short: this is a *username*, not free text, so `[A-Za-z0-9._-]` covers
/// every reasonable login identity without needing Unicode normalization
/// decisions (contrast tag names, Task 6, which are free text and do need
/// one).
/// @param principal Candidate principal string.
/// @return `true` if @p principal is non-empty, at most `kMaxPrincipalBytes`
///         long, and every byte is an ASCII letter, digit, `.`, `_`, or `-`.
[[nodiscard]] inline bool isValidPrincipal(std::string_view principal) noexcept {
    if (principal.empty() || principal.size() > kMaxPrincipalBytes) {
        return false;
    }
    for (const char ch : principal) {
        const auto byte = static_cast<unsigned char>(ch);
        const bool ok = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                       (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// @brief This rung's `IAuthorizer`: real signed-token auth
///        (`SigningAuthorizer`'s inherited `authorize`/`authenticate`), plus
///        "must be authenticated to register" and real per-instance
///        ownership.
class BookmarksAuthorizer : public ::morph::session::SigningAuthorizer {
  public:
    using SigningAuthorizer::SigningAuthorizer;

    /// @brief Only an authenticated caller may create an instance of any
    ///        model this rung serves — **except** `AuthModel` (Task 12),
    ///        whose whole job is minting the token a caller has not
    ///        obtained yet. Every other model gates on it identically.
    /// @param ctx       Per-call session; `principal` is already the
    ///                  verified identity by the time `RemoteServer` calls
    ///                  this (or empty, if authentication failed/was absent
    ///                  — which is the normal, expected state for a caller
    ///                  about to register `AuthModel` for its first login).
    /// @param modelType `"AuthModel"` is exempt; every other model requires
    ///                  a non-empty `ctx.principal`.
    /// @return `true` iff @p modelType is `"AuthModel"` or `ctx.principal`
    ///         is non-empty.
    [[nodiscard]] bool authorizeRegister(const ::morph::session::Context& ctx,
                                        std::string_view modelType) const override {
        return modelType == "AuthModel" || !ctx.principal.empty();
    }

    /// @brief Real ownership for a plain-registered instance; a pass-through
    ///        for an ownerless (shared) one.
    ///
    /// `ownerPrincipal` is the value `RemoteServer` recorded at `register`
    /// time. For `BookmarkModel`/`TagModel` (registered plain, Task 6/9)
    /// that is the real authenticated principal who registered the
    /// instance, so this genuinely denies every other principal. For
    /// `SharedFeedModel` (also registered plain in this rung -- see the
    /// plan's "Corrections" section for why `AllowShared` was not used --
    /// `ownerPrincipal` is likewise a real, single registering principal;
    /// the empty-owner branch below exists for correctness against any
    /// future `AllowShared` model this authorizer is reused for, not
    /// because this rung currently produces an empty owner anywhere. See
    /// `tests/test_policy_hardening.cpp`'s `OwnershipAuthorizer` for the
    /// identical one-line shape this mirrors.
    /// @param ctx            Per-call session; `principal` is the verified identity.
    /// @param modelType      Ignored: the same rule applies to every model.
    /// @param actionType     Ignored.
    /// @param modelId        Ignored: the decision only needs the owner.
    /// @param ownerPrincipal Principal recorded as the instance's owner, or
    ///                       empty if none was recorded (a shared instance).
    /// @return `true` if @p ownerPrincipal is empty or matches `ctx.principal`.
    [[nodiscard]] bool authorizeInstance(const ::morph::session::Context& ctx,
                                        [[maybe_unused]] std::string_view modelType,
                                        [[maybe_unused]] std::string_view actionType,
                                        [[maybe_unused]] std::uint64_t modelId,
                                        std::string_view ownerPrincipal) const override {
        return ownerPrincipal.empty() || ownerPrincipal == ctx.principal;
    }
};

/// @brief Process-global holder for the shared `TokenIssuer`, mirroring
///        `morph::journal::setActionLog`'s identical shape
///        (`include/morph/journal/action_log.hpp`) — the same answer to the
///        same problem: registry-constructed models are always
///        default-constructed (docs/findings/003, docs/findings/020), so
///        `AuthModel` (Task 12) has no constructor-injection seam for the
///        secret it needs to mint tokens. `App` calls `setTokenIssuer` once
///        at startup, with the *same* secret it hands to
///        `BookmarksAuthorizer`, so a token `AuthModel::execute(const
///        Login&)` mints verifies against the very authorizer that will
///        check every subsequent call.
/// @param issuer The issuer every `AuthModel` instance will read, or
///        `nullptr` to clear it (tests do this via `DbFixture`-adjacent
///        RAII if a test needs isolation — see `test_app.cpp`'s login case,
///        Task 12).
namespace detail {

/// @brief Backing storage for `setTokenIssuer`/`tokenIssuer` — a single
///        shared slot, guarded by a single mutex. Not exposed directly;
///        both public functions below go through this pair, so they
///        genuinely observe each other's writes (unlike two independent
///        function-local statics, which would each own an unrelated slot).
[[nodiscard]] inline std::mutex& tokenIssuerMutex() {
    static std::mutex mtx;
    return mtx;
}

[[nodiscard]] inline std::shared_ptr<::morph::session::TokenIssuer>& tokenIssuerSlot() {
    static std::shared_ptr<::morph::session::TokenIssuer> slot;
    return slot;
}

}  // namespace detail

inline void setTokenIssuer(std::shared_ptr<::morph::session::TokenIssuer> issuer) {
    const std::scoped_lock lock{detail::tokenIssuerMutex()};
    detail::tokenIssuerSlot() = std::move(issuer);
}

/// @brief Returns the process-global `TokenIssuer` installed by
///        `setTokenIssuer`, or `nullptr` if none is installed yet.
[[nodiscard]] inline std::shared_ptr<::morph::session::TokenIssuer> tokenIssuer() {
    const std::scoped_lock lock{detail::tokenIssuerMutex()};
    return detail::tokenIssuerSlot();
}

}  // namespace bookmarks::auth
```

- [ ] **Step 4: Run to verify it passes**

Run (once Task 13's CMake exists; otherwise defer to that task and return
here): `ctest --test-dir build/clang-coverage -R '\[bookmarks\]\[auth\]' --output-on-failure`
Expected: all cases pass.

- [ ] **Step 5: Commit**

```bash
git add examples/bookmarks/include/bookmarks/auth/bookmarks_authorizer.hpp \
        examples/bookmarks/tests/test_bookmarks_authorizer.cpp
git commit -m "bookmarks: add the rung's signed-token authorizer and principal charset"
```

---

## Task 2: Core types, units, and errors

**Files:**
- Create: `examples/bookmarks/include/bookmarks/core/types.hpp`
- Create: `examples/bookmarks/include/bookmarks/units.hpp`
- Create: `examples/bookmarks/include/bookmarks/core/errors.hpp`
- Test: `examples/bookmarks/tests/test_bookmarks_types.cpp`

**Interfaces:**
- Produces: `bookmarks::BookmarkId`, `bookmarks::TagId` (both
  `hasValue()`-capable strong ids wrapping `std::optional<std::int64_t>`,
  with `glz::meta` specialisations so they serialise as a nullable integer —
  the numeric-surrogate-key sibling of pastebin's `PasteId`, which wraps a
  string); `bookmarks::Cursor` (opaque pagination cursor, `hasValue()`-capable,
  wraps `std::optional<std::int64_t>` — shared by every list action in this
  rung, since every one of them keyset-paginates on a numeric surrogate PK);
  `bookmarks::ImportOpId` (idempotency key, `hasValue()`-capable, wraps
  `std::optional<std::string>` — a client-chosen opaque token, same shape as
  `PasteId`); `bookmarks::Ack` (trivial fieldless result, mirrors
  `pastebin::Ack`); `bookmarks::Unit::count`,
  `morph::units::UnitTraits<bookmarks::Unit>`, `bookmarks::Count` (a
  dimensionless `Quantity<Unit::count, 1>`, the sibling of
  `pastebin::Reads`); `bookmarks::BookmarksError`,
  `bookmarks::NotFound`, `bookmarks::ValidationError`, `bookmarks::Conflict`,
  `bookmarks::Forbidden`, `bookmarks::TooLarge` (all `BookmarksError`
  subclasses).
- Consumes: nothing beyond `<glaze/glaze.hpp>` and `<morph/util/quantity.hpp>`.

`BookmarkId`/`TagId`/`Cursor`/`ImportOpId` mirror `pastebin::PasteId`'s exact
shape and rationale (`examples/pastebin/include/pastebin/core/types.hpp`) —
deliberately near-duplicated per-type rather than factored into a shared
template: that file's own doc comment explains why ("do not promote this
into a generic helper here — the promotion rule triggers on a third
consumer, not the first," `IMPLEMENTATION.md`'s rule-of-three). Four
concrete structs across two rungs is still within that budget.

- [ ] **Step 1: Write the failing test**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/core/errors.hpp"
#include "bookmarks/core/types.hpp"
#include "bookmarks/units.hpp"

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>

TEST_CASE("BookmarkId/TagId round-trip through JSON as a nullable integer", "[bookmarks][types]") {
    bookmarks::BookmarkId empty;
    CHECK_FALSE(empty.hasValue());
    std::string json;
    REQUIRE_FALSE(glz::write_json(empty, json));
    CHECK(json == "null");

    const bookmarks::BookmarkId id{42};
    REQUIRE(id.hasValue());
    CHECK(*id == 42);
    json.clear();
    REQUIRE_FALSE(glz::write_json(id, json));
    CHECK(json == "42");

    bookmarks::TagId decoded;
    REQUIRE_FALSE(glz::read_json(decoded, json));
    REQUIRE(decoded.hasValue());
    CHECK(*decoded == 42);
}

TEST_CASE("BookmarkId equality and ordering follow the payload", "[bookmarks][types]") {
    CHECK(bookmarks::BookmarkId{} == bookmarks::BookmarkId{});
    CHECK(bookmarks::BookmarkId{1} != bookmarks::BookmarkId{2});
    CHECK(bookmarks::BookmarkId{1} < bookmarks::BookmarkId{2});
}

TEST_CASE("Cursor and ImportOpId are independently hasValue()-capable", "[bookmarks][types]") {
    CHECK_FALSE(bookmarks::Cursor{}.hasValue());
    CHECK(bookmarks::Cursor{7}.hasValue());
    CHECK_FALSE(bookmarks::ImportOpId{}.hasValue());
    CHECK(bookmarks::ImportOpId{"chunk-1"}.hasValue());
    CHECK(*bookmarks::ImportOpId{"chunk-1"} == "chunk-1");
}

TEST_CASE("Count is a whole-number dimensionless quantity", "[bookmarks][types]") {
    const auto five = bookmarks::Count::fromDouble(5.0);
    REQUIRE(five.hasValue());
    CHECK(morph::math::floor(*five) == 5);
}

TEST_CASE("Every bookmarks error derives from BookmarksError and carries its message",
          "[bookmarks][types]") {
    try {
        throw bookmarks::NotFound{"no such bookmark"};
    } catch (const bookmarks::BookmarksError& err) {
        CHECK(std::string{err.what()} == "no such bookmark");
    }
    // Compile-time check that every leaf really is-a BookmarksError.
    static_assert(std::is_base_of_v<bookmarks::BookmarksError, bookmarks::NotFound>);
    static_assert(std::is_base_of_v<bookmarks::BookmarksError, bookmarks::ValidationError>);
    static_assert(std::is_base_of_v<bookmarks::BookmarksError, bookmarks::Conflict>);
    static_assert(std::is_base_of_v<bookmarks::BookmarksError, bookmarks::Forbidden>);
    static_assert(std::is_base_of_v<bookmarks::BookmarksError, bookmarks::TooLarge>);
}
```

- [ ] **Step 2: Run to verify it fails** — the three headers do not exist yet.
Expected: FAIL, file not found.

- [ ] **Step 3: Write `examples/bookmarks/include/bookmarks/core/types.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <utility>

/// @file
/// Bookmarks' strong id/protocol-scalar types. `BookmarkId`/`TagId` are the
/// numeric-surrogate-key sibling of `pastebin::PasteId` (which wraps a
/// string, since a paste's id *is* its animal-name primary key) —
/// bookmarks' primary keys are ordinary auto-incrementing integers (bank's
/// convention, `Light::PrimaryKey::ServerSideAutoIncrement`), so the
/// wrapped payload is `std::int64_t`, not `std::string`. Same
/// `hasValue()`-capable shape and the same `fromOptional` factory
/// (`examples/pastebin/include/pastebin/core/types.hpp`'s own doc comment
/// explains why it exists as a named factory rather than a second
/// same-arity constructor).

namespace bookmarks {

/// @brief Strong id for a bookmark (a `bookmarks` table surrogate key).
///
/// Wire form: a plain nullable JSON integer (via the `glz::meta`
/// specialisation below) — exactly like an unwrapped `std::optional<std::int64_t>`.
struct BookmarkId {
    /// @brief The payload; `std::nullopt` means "not entered".
    std::optional<std::int64_t> value;

    /// @brief Constructs the empty state.
    constexpr BookmarkId() noexcept = default;

    /// @brief Engages with @p id.
    explicit BookmarkId(std::int64_t id) noexcept : value{id} {}

    /// @brief Adopts an optional payload as-is.
    /// @param payload The optional payload to adopt as-is.
    /// @return A `BookmarkId` wrapping @p payload directly.
    [[nodiscard]] static BookmarkId fromOptional(std::optional<std::int64_t> payload) noexcept {
        BookmarkId result;
        result.value = payload;
        return result;
    }

    /// @brief Whether a value has been entered.
    /// @return `true` if the payload is engaged.
    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }

    /// @brief Unchecked access to the engaged value (UB when empty, exactly
    ///        like `std::optional::operator*`).
    /// @return The engaged value.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] std::int64_t operator*() const noexcept { return *value; }

    /// @brief Equality/ordering on the payload; empty compares only equal to empty.
    [[nodiscard]] auto operator<=>(const BookmarkId&) const noexcept = default;
};

/// @brief Strong id for a tag (a `tags` table surrogate key). Same shape as
///        `BookmarkId` — see that type's doc comment.
struct TagId {
    std::optional<std::int64_t> value;

    constexpr TagId() noexcept = default;
    explicit TagId(std::int64_t id) noexcept : value{id} {}

    [[nodiscard]] static TagId fromOptional(std::optional<std::int64_t> payload) noexcept {
        TagId result;
        result.value = payload;
        return result;
    }

    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] std::int64_t operator*() const noexcept { return *value; }
    [[nodiscard]] auto operator<=>(const TagId&) const noexcept = default;
};

/// @brief Opaque pagination cursor, shared by every list action in this
///        rung (`ListBookmarks`, `ListSharedFeed`) — each keyset-paginates
///        on a numeric surrogate primary key, so one cursor shape serves
///        all of them (`IMPLEMENTATION.md` rule 3's protocol-scalars row:
///        a named opaque newtype per *role*, and "pagination cursor" is one
///        role here, not one per entity).
struct Cursor {
    std::optional<std::int64_t> value;

    constexpr Cursor() noexcept = default;
    explicit Cursor(std::int64_t token) noexcept : value{token} {}

    [[nodiscard]] static Cursor fromOptional(std::optional<std::int64_t> payload) noexcept {
        Cursor result;
        result.value = payload;
        return result;
    }

    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] std::int64_t operator*() const noexcept { return *value; }
    [[nodiscard]] auto operator<=>(const Cursor&) const noexcept = default;
};

/// @brief Idempotency key for one chunk of an `ImportBookmarks` call
///        (`IMPLEMENTATION.md` rule 3's protocol-scalars row: op-ids /
///        idempotency keys get a named opaque newtype). String-payload,
///        client-chosen, opaque — same shape as `pastebin::PasteId`.
struct ImportOpId {
    std::optional<std::string> value;

    constexpr ImportOpId() noexcept = default;
    explicit ImportOpId(std::string token) noexcept : value{std::move(token)} {}

    [[nodiscard]] static ImportOpId fromOptional(std::optional<std::string> payload) noexcept {
        ImportOpId result;
        result.value = std::move(payload);
        return result;
    }

    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] const std::string& operator*() const noexcept { return *value; }
    [[nodiscard]] auto operator<=>(const ImportOpId&) const noexcept = default;
};

/// @brief Trivial, fieldless acknowledgement result for actions with
///        nothing else to return. Mirrors `pastebin::Ack`.
struct Ack {};

}  // namespace bookmarks

/// @brief On the wire a `BookmarkId` is its nullable underlying integer.
template <>
struct glz::meta<bookmarks::BookmarkId> {
    static constexpr auto value = &bookmarks::BookmarkId::value;
    static constexpr std::string_view name = "BookmarkId";
};

/// @brief On the wire a `TagId` is its nullable underlying integer.
template <>
struct glz::meta<bookmarks::TagId> {
    static constexpr auto value = &bookmarks::TagId::value;
    static constexpr std::string_view name = "TagId";
};

/// @brief On the wire a `Cursor` is its nullable underlying integer.
template <>
struct glz::meta<bookmarks::Cursor> {
    static constexpr auto value = &bookmarks::Cursor::value;
    static constexpr std::string_view name = "Cursor";
};

/// @brief On the wire an `ImportOpId` is its nullable underlying string.
template <>
struct glz::meta<bookmarks::ImportOpId> {
    static constexpr auto value = &bookmarks::ImportOpId::value;
    static constexpr std::string_view name = "ImportOpId";
};
```

- [ ] **Step 4: Write `examples/bookmarks/include/bookmarks/units.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/util/quantity.hpp>

/// @file
/// Bookmarks' one-unit system: a dimensionless count, reused for every
/// whole-number quantity this rung's DTOs carry (a tag's bookmark count, a
/// bulk edit's affected-row count, an import's imported/skipped counts).
/// Modeled on `pastebin/units.hpp` — see that file for the full
/// UnitTraits/consteval-algebra contract this mirrors; this rung needs no
/// unit algebra either, for the same reason.

namespace bookmarks {

/// @brief Units bookmarks works in.
enum class Unit {
    count,  ///< dimensionless whole-number count
};

}  // namespace bookmarks

/// @brief Static unit metadata: schema id, display text, default decimals.
template <>
struct morph::units::UnitTraits<bookmarks::Unit> {
    static constexpr morph::units::UnitMeta meta(bookmarks::Unit unit) noexcept {
        switch (unit) {
            case bookmarks::Unit::count:
                return {"count", "", 1};
            default:
                return {"?", "?", 1};
        }
    }
};

namespace bookmarks {

/// @brief A whole-number count (bookmark counts, affected-row counts,
///        import result counts).
///
/// `morph::units::Quantity<U, DeclaredDecimals>` requires `DeclaredDecimals
/// >= 1` (zero is not legal); every value that ever appears is a whole
/// number by construction. See `pastebin::Reads`'s identical doc comment.
using Count = ::morph::units::Quantity<Unit::count, 1>;

}  // namespace bookmarks
```

- [ ] **Step 5: Write `examples/bookmarks/include/bookmarks/core/errors.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdexcept>
#include <string>

/// @file
/// Domain exceptions. A model's `execute(...)` throws one of these; morph
/// captures it as a `std::exception_ptr` and delivers it to the caller's
/// `.onError(...)` callback. See `pastebin/core/errors.hpp` for the
/// identical shape and rationale this mirrors.

namespace bookmarks {

/// @brief Base of every bookmarks-specific error a model throws.
struct BookmarksError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// @brief No bookmark/tag exists at the given id (never existed, deleted,
///        or not owned by the caller — see `Forbidden` for the
///        distinguished case where it exists but belongs to someone else).
struct NotFound : BookmarksError {
    using BookmarksError::BookmarksError;
};

/// @brief An action's `validate()` rejected its input.
struct ValidationError : BookmarksError {
    using BookmarksError::BookmarksError;
};

/// @brief A write lost a race: the target row changed between this
///        client's read and its write (the compare-and-swap conflict shape
///        `pastebin::Conflict` established this session for `EditPaste`),
///        or a `MergeTags`/rename would collide with an existing tag name.
struct Conflict : BookmarksError {
    using BookmarksError::BookmarksError;
};

/// @brief The caller is authenticated, but the target row exists and is
///        owned by a different principal. Distinguished from `NotFound`
///        deliberately: `docs/spec/security.md`'s registration/instance
///        hooks already keep a foreign id from being *reached* in most
///        cases (Task 14), but a model's own re-check (rule 1 — the local
///        backend enforces nothing) needs its own typed signal, and the
///        expected-strain-points test for "local mode has no authorization
///        at all" (Task 15) specifically wants to see this thrown, not a
///        NotFound that would quietly look like the row never existed.
struct Forbidden : BookmarksError {
    using BookmarksError::BookmarksError;
};

/// @brief An import chunk (or other bounded payload) exceeded this rung's
///        own size bound, distinct from the transport's own message-size
///        limit (`docs/spec/security.md`) which rejects the call before a
///        model ever sees it.
struct TooLarge : BookmarksError {
    using BookmarksError::BookmarksError;
};

}  // namespace bookmarks
```

- [ ] **Step 6: Run to verify it passes**

Run: `ctest --test-dir build/clang-coverage -R '\[bookmarks\]\[types\]' --output-on-failure`
Expected: all cases pass.

- [ ] **Step 7: Commit**

```bash
git add examples/bookmarks/include/bookmarks/core/types.hpp \
        examples/bookmarks/include/bookmarks/units.hpp \
        examples/bookmarks/include/bookmarks/core/errors.hpp \
        examples/bookmarks/tests/test_bookmarks_types.cpp
git commit -m "bookmarks: add core strong types, unit system, and error hierarchy"
```

---

## Task 3: Bookmark DTOs

**Files:**
- Create: `examples/bookmarks/include/bookmarks/dto/bookmark_dto.hpp`
- Test: `examples/bookmarks/tests/test_bookmark_dto.cpp`

**Interfaces:**
- Consumes: `bookmarks::BookmarkId`, `bookmarks::Cursor`, `bookmarks::Ack`
  (Task 2); `morph::time::Timestamp` (`<morph/util/datetime.hpp>`).
- Produces: `bookmarks::Visibility` (`Private`/`Shared`), `bookmarks::ReadState`
  (`Unread`/`Read`), `bookmarks::ArchiveState` (`Active`/`Archived`),
  `bookmarks::ReadFilter` (`Any`/`UnreadOnly`/`ReadOnly`),
  `bookmarks::ArchiveFilter` (`Any`/`ActiveOnly`/`ArchivedOnly`);
  `bookmarks::CreateBookmark`/`CreateBookmarkResult`,
  `bookmarks::EditBookmark`, `bookmarks::ArchiveBookmark`,
  `bookmarks::UnarchiveBookmark`, `bookmarks::DeleteBookmark`,
  `bookmarks::GetBookmark`, `bookmarks::BookmarkView`,
  `bookmarks::BookmarkSummary`, `bookmarks::ListBookmarks`/
  `bookmarks::ListBookmarksResult`, `bookmarks::GetChangesSince`/
  `bookmarks::GetChangesSinceResult`, `bookmarks::RecordMetadata` (the
  background worker's write-back action, Task 12) — all consumed by
  `BookmarkModel` (Task 6/7/8) and every presenter/GUI task downstream.

`kMaxUrlBytes`/`kMaxTitleBytes` bounds mirror `pastebin::kMaxSyntaxBytes`'s
own reasoning (a real storage-column width, checked by a `static_assert`
against the entity in Task 5, not a number pulled from the air) —
`SqlAnsiString`-style fixed columns are not used here (url/title are
variable-length `TEXT`, per rule 4's "content needs no equivalent bound"
clause for `pastebin::CreatePaste::content`), so these bounds exist purely
as this rung's own sanity limits, not a truncation-avoidance requirement;
still enforced in `validate()` so an absurdly long value is rejected with a
typed error rather than silently accepted into an unbounded `TEXT` column.

- [ ] **Step 1: Write the failing test**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/dto/bookmark_dto.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("CreateBookmark validate() requires a non-empty url within the length bound",
          "[bookmarks][dto]") {
    bookmarks::CreateBookmark action;
    CHECK_FALSE(action.validate());  // empty url

    action.url = "https://example.com";
    CHECK(action.validate());

    action.url = std::string(bookmarks::kMaxUrlBytes + 1, 'a');
    CHECK_FALSE(action.validate());

    action.url = std::string(bookmarks::kMaxUrlBytes, 'a');
    CHECK(action.validate());
}

TEST_CASE("CreateBookmark's optionalFields excludes everything but url", "[bookmarks][dto]") {
    // Mirrors CreatePaste::optionalFields's own test intent: a create with
    // only a url must be schema-submittable without hand-typing every
    // enum's default.
    using bookmarks::CreateBookmark;
    STATIC_REQUIRE(CreateBookmark::optionalFields.size() == 4);
}

TEST_CASE("EditBookmark validate() requires an id and a non-empty url", "[bookmarks][dto]") {
    bookmarks::EditBookmark action;
    CHECK_FALSE(action.validate());
    action.id = bookmarks::BookmarkId{1};
    CHECK_FALSE(action.validate());  // still no url
    action.url = "https://example.com";
    CHECK(action.validate());
}

TEST_CASE("GetBookmark/ArchiveBookmark/UnarchiveBookmark/DeleteBookmark all require an id",
          "[bookmarks][dto]") {
    CHECK_FALSE(bookmarks::GetBookmark{}.validate());
    CHECK(bookmarks::GetBookmark{.id = bookmarks::BookmarkId{1}}.validate());
    CHECK_FALSE(bookmarks::ArchiveBookmark{}.validate());
    CHECK_FALSE(bookmarks::UnarchiveBookmark{}.validate());
    CHECK_FALSE(bookmarks::DeleteBookmark{}.validate());
}

TEST_CASE("RecordMetadata requires an id; title/faviconPath may be empty (a failed fetch)",
          "[bookmarks][dto]") {
    CHECK_FALSE(bookmarks::RecordMetadata{}.validate());
    bookmarks::RecordMetadata action{.id = bookmarks::BookmarkId{1}};
    CHECK(action.validate());  // empty title/faviconPath is a legitimate "fetch found nothing"
}

TEST_CASE("Visibility/ReadState/ArchiveState/ReadFilter/ArchiveFilter reflect as readable strings",
          "[bookmarks][dto]") {
    std::string json;
    REQUIRE_FALSE(glz::write_json(bookmarks::Visibility::Shared, json));
    CHECK(json == "\"Shared\"");
    json.clear();
    REQUIRE_FALSE(glz::write_json(bookmarks::ReadFilter::UnreadOnly, json));
    CHECK(json == "\"UnreadOnly\"");
}
```

- [ ] **Step 2: Run to verify it fails** — the header does not exist yet.

- [ ] **Step 3: Write the implementation**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bookmarks/core/types.hpp"

#include <morph/util/datetime.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// Bookmark wire DTOs. `RecordMetadata` is the one action a GUI client never
/// sends — it is dispatched exclusively by the app-layer metadata-fetch
/// worker's internal client (Task 12), the same "internal-only" shape
/// `pastebin::ExpirePaste` established.

namespace bookmarks {

/// @brief Whether a bookmark is visible only to its owner or to the shared feed.
enum class Visibility { Private, Shared };

/// @brief Whether a bookmark has been read.
enum class ReadState { Unread, Read };

/// @brief Whether a bookmark is archived (hidden from the default list, not deleted).
enum class ArchiveState { Active, Archived };

/// @brief `ListBookmarks`' read-state filter.
enum class ReadFilter { Any, UnreadOnly, ReadOnly };

/// @brief `ListBookmarks`' archive-state filter.
enum class ArchiveFilter { Any, ActiveOnly, ArchivedOnly };

/// @brief Longest `url`, in bytes, this rung accepts (a sanity bound, not a
///        storage-column width — url/title are variable-length `TEXT`
///        columns with no fixed capacity to overflow, per
///        `IMPLEMENTATION.md` rule 4's "content needs no equivalent bound"
///        clause).
inline constexpr std::size_t kMaxUrlBytes = 2048;
/// @brief Longest `title`, in bytes, this rung accepts.
inline constexpr std::size_t kMaxTitleBytes = 512;

struct CreateBookmark {
    std::string url;
    std::string title;        // empty = not yet known; the metadata worker fills it in
    std::string description;
    std::string notes;
    std::vector<std::string> tags;  // tag names; auto-created on first use (Task 6)
    Visibility visibility = Visibility::Private;

    /// @brief Every member but `url` may be omitted from a schema-driven
    ///        submission — see `pastebin::CreatePaste::optionalFields`'s
    ///        doc comment for why this list exists at all.
    static constexpr std::array<std::string_view, 4> optionalFields{"description", "notes", "tags", "visibility"};

    [[nodiscard]] bool validate() const noexcept {
        return !url.empty() && url.size() <= kMaxUrlBytes && title.size() <= kMaxTitleBytes;
    }
};

struct CreateBookmarkResult {
    BookmarkId id;
};

/// @brief Full replace-set edit: `tags` is the *desired final* tag set, not
///        a delta — `BookmarkModel::execute(const EditBookmark&)` (Task 6)
///        diffs it against the current junction rows.
struct EditBookmark {
    BookmarkId id;
    std::string url;
    std::string title;
    std::string description;
    std::string notes;
    std::vector<std::string> tags;
    Visibility visibility = Visibility::Private;

    static constexpr std::array<std::string_view, 4> optionalFields{"description", "notes", "tags", "visibility"};

    [[nodiscard]] bool validate() const noexcept {
        return id.hasValue() && !url.empty() && url.size() <= kMaxUrlBytes && title.size() <= kMaxTitleBytes;
    }
};

struct ArchiveBookmark {
    BookmarkId id;
    [[nodiscard]] bool validate() const noexcept { return id.hasValue(); }
};

struct UnarchiveBookmark {
    BookmarkId id;
    [[nodiscard]] bool validate() const noexcept { return id.hasValue(); }
};

struct DeleteBookmark {
    BookmarkId id;
    [[nodiscard]] bool validate() const noexcept { return id.hasValue(); }
};

struct GetBookmark {
    BookmarkId id;
    [[nodiscard]] bool validate() const noexcept { return id.hasValue(); }
};

/// @brief The full, owner-only view of one bookmark.
struct BookmarkView {
    BookmarkId id;
    std::string url;
    std::string title;
    std::string description;
    std::string notes;
    std::vector<std::string> tags;
    ::morph::time::Timestamp createdAt;
    ::morph::time::Timestamp updatedAt;
    ReadState readState = ReadState::Unread;
    ArchiveState archiveState = ArchiveState::Active;
    Visibility visibility = Visibility::Private;
};

/// @brief One row of `ListBookmarks`'/`GetChangesSince`'s result —
///        deliberately narrower than `BookmarkView`: a listing must not
///        leak `notes` (mirrors `pastebin::PasteSummary`'s non-leak rule).
struct BookmarkSummary {
    BookmarkId id;
    std::string url;
    std::string title;
    std::vector<std::string> tags;
    ::morph::time::Timestamp createdAt;
    ::morph::time::Timestamp updatedAt;
    ReadState readState = ReadState::Unread;
    ArchiveState archiveState = ArchiveState::Active;
    Visibility visibility = Visibility::Private;
};

struct ListBookmarks {
    Cursor cursor;             // empty = first page
    ReadFilter readFilter = ReadFilter::Any;
    ArchiveFilter archiveFilter = ArchiveFilter::ActiveOnly;  // archived hidden by default, linkding's own convention
    std::string tag;           // empty = no tag filter
    std::string searchText;    // empty = no text filter

    static constexpr std::array<std::string_view, 5> optionalFields{"cursor", "readFilter", "archiveFilter", "tag",
                                                                     "searchText"};

    [[nodiscard]] bool validate() const noexcept { return true; }  // every field is optional
};

struct ListBookmarksResult {
    std::vector<BookmarkSummary> bookmarks;
    Cursor nextCursor;  // empty = no further page
};

/// @brief Minimal changes-since poll (README's rung-3 event-pattern
///        preview): every bookmark this owner touched (created, edited,
///        archived/unarchived, or metadata-recorded) since @p since.
struct GetChangesSince {
    ::morph::time::Timestamp since;  // empty = every bookmark ever (first poll)

    static constexpr std::array<std::string_view, 1> optionalFields{"since"};

    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct GetChangesSinceResult {
    std::vector<BookmarkSummary> changed;
    /// @brief The instant this query ran, captured *before* the query
    ///        itself (`BookmarkModel::execute`'s own doc comment, Task 7,
    ///        has the full argument for why) — the next poll's `since`.
    ::morph::time::Timestamp asOf;
};

/// @brief Internal-only: the metadata-fetch worker's write-back
///        (`app::MetadataFetchWorker`, Task 12). Never dispatched by a GUI
///        client — mirrors `pastebin::ExpirePaste`'s "internal-only"
///        convention exactly.
struct RecordMetadata {
    BookmarkId id;
    std::string title;        // empty = the fetch found no <title>
    std::string faviconPath;  // empty = no favicon fetched

    static constexpr std::array<std::string_view, 2> optionalFields{"title", "faviconPath"};

    [[nodiscard]] bool validate() const noexcept { return id.hasValue(); }
};

}  // namespace bookmarks

/// @brief Reflects `Visibility` as readable strings — same rationale and
///        `glz::enumerate` shape as `pastebin`'s enum reflections
///        (`glz::meta<pastebin::Visibility>`'s doc comment has the full
///        argument: a bare ordinal degrades the schema writer's `$defs`
///        entry to an any-type union).
template <>
struct glz::meta<bookmarks::Visibility> {
    using enum bookmarks::Visibility;
    static constexpr auto value = glz::enumerate(Private, Shared);
};

template <>
struct glz::meta<bookmarks::ReadState> {
    using enum bookmarks::ReadState;
    static constexpr auto value = glz::enumerate(Unread, Read);
};

template <>
struct glz::meta<bookmarks::ArchiveState> {
    using enum bookmarks::ArchiveState;
    static constexpr auto value = glz::enumerate(Active, Archived);
};

template <>
struct glz::meta<bookmarks::ReadFilter> {
    using enum bookmarks::ReadFilter;
    static constexpr auto value = glz::enumerate(Any, UnreadOnly, ReadOnly);
};

template <>
struct glz::meta<bookmarks::ArchiveFilter> {
    using enum bookmarks::ArchiveFilter;
    static constexpr auto value = glz::enumerate(Any, ActiveOnly, ArchivedOnly);
};
```

- [ ] **Step 4: Run to verify it passes.**

- [ ] **Step 5: Commit**

```bash
git add examples/bookmarks/include/bookmarks/dto/bookmark_dto.hpp \
        examples/bookmarks/tests/test_bookmark_dto.cpp
git commit -m "bookmarks: add Bookmark DTOs"
```

---

## Task 4: Tag, Bulk, SharedFeed, and Import/Export DTOs

**Files:**
- Create: `examples/bookmarks/include/bookmarks/dto/tag_dto.hpp`
- Create: `examples/bookmarks/include/bookmarks/dto/bulk_dto.hpp`
- Create: `examples/bookmarks/include/bookmarks/dto/shared_feed_dto.hpp`
- Create: `examples/bookmarks/include/bookmarks/dto/import_export_dto.hpp`
- Test: `examples/bookmarks/tests/test_tag_bulk_dto.cpp`

**Interfaces:**
- Consumes: `bookmarks::TagId`, `bookmarks::BookmarkId`, `bookmarks::Cursor`,
  `bookmarks::Count`, `bookmarks::BookmarkSummary`, `bookmarks::ImportOpId`
  (Task 2/3).
- Produces: `bookmarks::RenameTag`, `bookmarks::MergeTags`,
  `bookmarks::ListTags`/`bookmarks::ListTagsResult`,
  `bookmarks::TagSummary`; `bookmarks::BulkArchiveOp`
  (`None`/`Archive`/`Unarchive`), `bookmarks::BulkEdit`/
  `bookmarks::BulkEditResult`; `bookmarks::ListSharedFeed`/
  `bookmarks::ListSharedFeedResult`; `bookmarks::ImportBookmarks`/
  `bookmarks::ImportBookmarksResult`, `bookmarks::ExportBookmarks`/
  `bookmarks::ExportBookmarksResult`, `bookmarks::kMaxTagNameBytes`,
  `bookmarks::kMaxImportChunkBytes` — consumed by `TagModel` (Task 9),
  `BookmarkModel::execute(const BulkEdit&)` (Task 8), `SharedFeedModel`
  (Task 10), the import/export pipeline (Task 11).

Tag names are **not** bounded to a `SqlAnsiString`-style fixed column —
`TagRecord::name` (Task 5) is a plain variable-length `TEXT` column, exactly
like `url`/`title`, specifically to avoid re-opening the silent-truncation
bug class `pastebin::kMaxSyntaxBytes` (and this session's earlier
`EditPaste`/`syntax` fix) exists to close: a tag name is free-form Unicode
text a user types, not a label drawn from a bounded set, and truncating a
multi-byte codepoint mid-sequence is exactly the harm that fix eliminated
for pastebin. `kMaxTagNameBytes` is therefore a `validate()`-only sanity
bound (like `kMaxUrlBytes`), not a storage-capacity `static_assert`.

- [ ] **Step 1: Write the failing test**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/dto/bulk_dto.hpp"
#include "bookmarks/dto/import_export_dto.hpp"
#include "bookmarks/dto/shared_feed_dto.hpp"
#include "bookmarks/dto/tag_dto.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("RenameTag requires an id and a non-empty, bounded name", "[bookmarks][dto]") {
    bookmarks::RenameTag action;
    CHECK_FALSE(action.validate());
    action.id = bookmarks::TagId{1};
    CHECK_FALSE(action.validate());  // still no name
    action.name = "programming";
    CHECK(action.validate());
    action.name = std::string(bookmarks::kMaxTagNameBytes + 1, 'x');
    CHECK_FALSE(action.validate());
}

TEST_CASE("MergeTags requires two distinct ids", "[bookmarks][dto]") {
    bookmarks::MergeTags action;
    CHECK_FALSE(action.validate());
    action.sourceId = bookmarks::TagId{1};
    action.targetId = bookmarks::TagId{1};
    CHECK_FALSE(action.validate());  // merging a tag into itself
    action.targetId = bookmarks::TagId{2};
    CHECK(action.validate());
}

TEST_CASE("BulkEdit requires at least one id", "[bookmarks][dto]") {
    bookmarks::BulkEdit action;
    CHECK_FALSE(action.validate());
    action.ids = {bookmarks::BookmarkId{1}};
    CHECK(action.validate());
}

TEST_CASE("BulkArchiveOp reflects as a readable string", "[bookmarks][dto]") {
    std::string json;
    REQUIRE_FALSE(glz::write_json(bookmarks::BulkArchiveOp::Archive, json));
    CHECK(json == "\"Archive\"");
}

TEST_CASE("ImportBookmarks requires a non-empty, bounded chunk and an opId", "[bookmarks][dto]") {
    bookmarks::ImportBookmarks action;
    CHECK_FALSE(action.validate());
    action.chunk = "<A HREF=\"https://example.com\">Example</A>";
    CHECK_FALSE(action.validate());  // still no opId
    action.opId = bookmarks::ImportOpId{"chunk-1"};
    CHECK(action.validate());
    action.chunk = std::string(bookmarks::kMaxImportChunkBytes + 1, 'x');
    CHECK_FALSE(action.validate());
}

TEST_CASE("ListSharedFeed/ListTags/ExportBookmarks validate() with no required fields",
          "[bookmarks][dto]") {
    CHECK(bookmarks::ListSharedFeed{}.validate());
    CHECK(bookmarks::ListTags{}.validate());
    CHECK(bookmarks::ExportBookmarks{}.validate());
}
```

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Write `examples/bookmarks/include/bookmarks/dto/tag_dto.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bookmarks/core/types.hpp"
#include "bookmarks/units.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace bookmarks {

/// @brief Longest tag name, in bytes, this rung accepts — a `validate()`
///        sanity bound only, not a storage-column width. See this task's
///        own header comment for why `TagRecord::name` carries no
///        `SqlAnsiString` capacity to check against.
inline constexpr std::size_t kMaxTagNameBytes = 128;

struct RenameTag {
    TagId id;
    std::string name;

    [[nodiscard]] bool validate() const noexcept {
        return id.hasValue() && !name.empty() && name.size() <= kMaxTagNameBytes;
    }
};

/// @brief Reassigns every bookmark tagged `sourceId` to `targetId`
///        (deduplicating), then deletes `sourceId` — `TagModel::execute`
///        (Task 9) does the cascade; this DTO only carries the two ids.
struct MergeTags {
    TagId sourceId;
    TagId targetId;

    [[nodiscard]] bool validate() const noexcept {
        return sourceId.hasValue() && targetId.hasValue() && *sourceId != *targetId;
    }
};

struct ListTags {
    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct TagSummary {
    TagId id;
    std::string name;
    Count bookmarkCount;
};

struct ListTagsResult {
    std::vector<TagSummary> tags;
};

}  // namespace bookmarks
```

- [ ] **Step 4: Write `examples/bookmarks/include/bookmarks/dto/bulk_dto.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bookmarks/core/types.hpp"
#include "bookmarks/units.hpp"

#include <array>
#include <glaze/glaze.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace bookmarks {

/// @brief `BulkEdit`'s archive-state instruction — a three-state enum
///        (`IMPLEMENTATION.md` rule 3: never a `bool` two-state flag, and
///        this action genuinely has a third "don't touch archive state at
///        all" option a bool cannot express).
enum class BulkArchiveOp { None, Archive, Unarchive };

/// @brief The rung's first multi-entity atomic action — all-or-nothing
///        against SQLite (README). `addTags`/`removeTags` are name-based
///        (auto-create-on-first-use for `addTags`, same as
///        `EditBookmark::tags`'s handling — Task 8's own doc comment has
///        the exact SQL). Every id must be owned by the caller or the
///        *whole* batch is rejected (Task 8's resolved "reject the whole
///        batch on one violation" design decision).
struct BulkEdit {
    std::vector<BookmarkId> ids;
    std::vector<std::string> addTags;
    std::vector<std::string> removeTags;
    BulkArchiveOp archive = BulkArchiveOp::None;

    static constexpr std::array<std::string_view, 3> optionalFields{"addTags", "removeTags", "archive"};

    [[nodiscard]] bool validate() const noexcept { return !ids.empty(); }
};

struct BulkEditResult {
    Count affected;
};

}  // namespace bookmarks

/// @brief Reflects `BulkArchiveOp` as readable strings — same rationale as
///        every other enum reflection in this rung.
template <>
struct glz::meta<bookmarks::BulkArchiveOp> {
    using enum bookmarks::BulkArchiveOp;
    static constexpr auto value = glz::enumerate(None, Archive, Unarchive);
};
```

- [ ] **Step 5: Write `examples/bookmarks/include/bookmarks/dto/shared_feed_dto.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bookmarks/core/types.hpp"
#include "bookmarks/dto/bookmark_dto.hpp"

#include <array>
#include <string_view>
#include <vector>

namespace bookmarks {

struct ListSharedFeed {
    Cursor cursor;  // empty = first page

    static constexpr std::array<std::string_view, 1> optionalFields{"cursor"};

    [[nodiscard]] bool validate() const noexcept { return true; }
};

/// @brief `BookmarkSummary` doubles as the shared feed's row shape — same
///        non-leak rule applies (no `notes`), and a shared bookmark's
///        `visibility` is always `Shared` by construction (the query that
///        builds this only ever selects `WHERE visibility = Shared`, Task
///        10), so there is nothing this result type needs beyond what
///        `BookmarkSummary` already carries.
struct ListSharedFeedResult {
    std::vector<BookmarkSummary> bookmarks;
    Cursor nextCursor;
};

}  // namespace bookmarks
```

- [ ] **Step 6: Write `examples/bookmarks/include/bookmarks/dto/import_export_dto.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bookmarks/core/types.hpp"
#include "bookmarks/units.hpp"

#include <cstddef>
#include <string>

namespace bookmarks {

/// @brief Longest one `ImportBookmarks` chunk this rung accepts, in bytes —
///        well under the transport's own message-size bound
///        (`docs/spec/security.md`), so a client that respects this limit
///        never has to distinguish "this rung refused it" from "the
///        transport refused it" (Task 11 measures the transport's own
///        bound directly, the same way `pastebin`'s "An oversized
///        CreatePaste is refused by the transport" test does).
inline constexpr std::size_t kMaxImportChunkBytes = 65536;

/// @brief One chunk of a Netscape Bookmark HTML import. Idempotent per
///        `opId` (Task 5's `ImportedOpRecord`/Task 11's dedup check): a
///        retried chunk after a dropped connection is a safe no-op, never
///        a duplicate import.
struct ImportBookmarks {
    std::string chunk;
    ImportOpId opId;

    [[nodiscard]] bool validate() const noexcept {
        return !chunk.empty() && chunk.size() <= kMaxImportChunkBytes && opId.hasValue();
    }
};

struct ImportBookmarksResult {
    Count imported;
    Count skipped;  // e.g. a malformed <A> entry within an otherwise valid chunk
};

struct ExportBookmarks {
    [[nodiscard]] bool validate() const noexcept { return true; }
};

struct ExportBookmarksResult {
    std::string html;  // a complete Netscape Bookmark File
};

}  // namespace bookmarks
```

- [ ] **Step 7: Run to verify it passes.**

- [ ] **Step 8: Commit**

```bash
git add examples/bookmarks/include/bookmarks/dto/tag_dto.hpp \
        examples/bookmarks/include/bookmarks/dto/bulk_dto.hpp \
        examples/bookmarks/include/bookmarks/dto/shared_feed_dto.hpp \
        examples/bookmarks/include/bookmarks/dto/import_export_dto.hpp \
        examples/bookmarks/tests/test_tag_bulk_dto.cpp
git commit -m "bookmarks: add Tag, BulkEdit, SharedFeed, and import/export DTOs"
```

---

## Task 5: Entities, schema, and `db_model.hpp`

**Files:**
- Create: `examples/bookmarks/include/bookmarks/db/bookmark_entity.hpp`
- Create: `examples/bookmarks/include/bookmarks/db/tag_entity.hpp`
- Create: `examples/bookmarks/include/bookmarks/db/bookmark_tag_entity.hpp`
- Create: `examples/bookmarks/include/bookmarks/db/imported_op_entity.hpp`
- Create: `examples/bookmarks/include/bookmarks/db/database.hpp`
- Create: `examples/bookmarks/include/bookmarks/db/db_model.hpp`
- Create: `examples/bookmarks/src/db/schema.cpp`
- Test: `examples/bookmarks/tests/test_bookmarks_schema.cpp`

**Interfaces:**
- Produces: `bookmarks::db::BookmarkRecord`, `bookmarks::db::TagRecord`,
  `bookmarks::db::BookmarkTagRecord`, `bookmarks::db::ImportedOpRecord`
  (all plain `Light::Field<>`/`Light::BelongsTo<>` entities — **no**
  relation-typed member on `BookmarkRecord`/`TagRecord`, per the Global
  Constraints' `HasMany`/`HasManyThrough`-vs-`Update()` rule);
  `bookmarks::db::setup(const std::string&)`; `bookmarks::db::WithMapper`
  (the exact two-branch `#ifdef __EMSCRIPTEN__` mixin
  `pastebin::db::WithMapper` established for finding 025, reused verbatim
  with only the namespace changed). Consumed by every model task (6-10).

- [ ] **Step 1: Write the failing test**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/db/bookmark_entity.hpp"
#include "bookmarks/db/bookmark_tag_entity.hpp"
#include "bookmarks/db/imported_op_entity.hpp"
#include "bookmarks/db/tag_entity.hpp"
#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>

using morph::ladder::testkit::DbFixture;

TEST_CASE("The bookmarks schema creates all four tables and a bookmark round-trips",
          "[bookmarks][schema]") {
    DbFixture fixture;
    Lightweight::DataMapper mapper;

    bookmarks::db::BookmarkRecord rec;
    rec.ownerPrincipal = "alice";
    rec.url = "https://example.com";
    rec.title = "Example";
    rec.createdAtMs = 1000;
    rec.updatedAtMs = 1000;
    mapper.Create(rec);
    REQUIRE(rec.id.Value() > 0);

    bookmarks::db::TagRecord tag;
    tag.ownerPrincipal = "alice";
    tag.name = "example";
    mapper.Create(tag);
    REQUIRE(tag.id.Value() > 0);

    bookmarks::db::BookmarkTagRecord junction;
    junction.bookmark = rec.id.Value();
    junction.tag = tag.id.Value();
    mapper.Create(junction);
    REQUIRE(junction.id.Value() > 0);

    bookmarks::db::ImportedOpRecord op;
    op.ownerPrincipal = "alice";
    op.opId = "chunk-1";
    op.appliedAtMs = 1000;
    mapper.Create(op);
    REQUIRE(op.id.Value() > 0);

    // Tag reads go through a plain query, never an embedded relation field
    // (Global Constraints) -- proving that path works end-to-end here.
    auto rows = mapper.Query<bookmarks::db::BookmarkTagRecord>()
                    .Where(Lightweight::FieldNameOf<&bookmarks::db::BookmarkTagRecord::bookmark>, "=", rec.id.Value())
                    .All();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().tag.Value() == tag.id.Value());
}

TEST_CASE("Duplicate (ownerPrincipal, name) tags are rejected by the unique index",
          "[bookmarks][schema]") {
    DbFixture fixture;
    Lightweight::DataMapper mapper;
    bookmarks::db::TagRecord first;
    first.ownerPrincipal = "alice";
    first.name = "dup";
    mapper.Create(first);

    bookmarks::db::TagRecord second;
    second.ownerPrincipal = "alice";
    second.name = "dup";
    CHECK_THROWS_AS(mapper.Create(second), Lightweight::SqlException);

    // A different owner may reuse the same name -- the index is scoped per owner.
    bookmarks::db::TagRecord thirdOwner;
    thirdOwner.ownerPrincipal = "bob";
    thirdOwner.name = "dup";
    CHECK_NOTHROW(mapper.Create(thirdOwner));
}

TEST_CASE("BookmarkRecord has no relation-typed member -- Update() must compile",
          "[bookmarks][schema]") {
    // A compile-time proof, not a runtime assertion: if BookmarkRecord ever
    // grows an embedded HasMany/HasManyThrough field, this line stops
    // compiling with the exact "no member IsModified" error the Global
    // Constraints section documents -- catching the regression at build
    // time, in the one file whose entire job is proving this works.
    DbFixture fixture;
    Lightweight::DataMapper mapper;
    bookmarks::db::BookmarkRecord rec;
    rec.ownerPrincipal = "alice";
    rec.url = "https://example.com";
    rec.createdAtMs = 1;
    rec.updatedAtMs = 1;
    mapper.Create(rec);
    rec.title = "Changed";
    CHECK_NOTHROW(mapper.Update(rec));
}
```

- [ ] **Step 2: Run to verify it fails** — the headers do not exist yet.

- [ ] **Step 3: Write `examples/bookmarks/include/bookmarks/db/bookmark_entity.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>

#include <cstdint>
#include <string_view>

/// @file
/// `BookmarkRecord` deliberately carries **zero** relation-typed members
/// (no `HasMany`, no `HasManyThrough`) — see this plan's Global Constraints
/// section for the verified reason: `DataMapper::Update()`'s
/// non-reflection path calls `field.IsModified()` on every member via
/// `EnumerateRecordMembers` (which does not filter by field kind), and
/// neither relation type declares that method, so a record embedding one
/// fails to compile the instant `Update()` is instantiated for it — exactly
/// what `examples/bank/include/bank/db/account_entity.hpp`'s own comment
/// independently documents for `HasMany`. Tag associations are read via a
/// plain `Query<BookmarkTagRecord>()` call in the model (`bookmark_model.cpp`,
/// Task 6), never through a relation field on this record.

namespace bookmarks::db {

/// @brief One row of the `bookmarks` table.
struct BookmarkRecord {
    static constexpr std::string_view TableName = "bookmarks";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    /// Authenticated owner (`session::Context::principal`) — every query the
    /// model issues filters on this column; see Task 6's `execute()` bodies.
    Light::Field<std::string, Light::SqlRealName{"owner_principal"}> ownerPrincipal;  // 1
    Light::Field<std::string, Light::SqlRealName{"url"}> url;  // 2
    Light::Field<std::string, Light::SqlRealName{"title"}> title;  // 3
    Light::Field<std::string, Light::SqlRealName{"description"}> description;  // 4
    Light::Field<std::string, Light::SqlRealName{"notes"}> notes;  // 5
    Light::Field<bool, Light::SqlRealName{"is_unread"}> isUnread{true};  // 6
    Light::Field<bool, Light::SqlRealName{"is_archived"}> isArchived{false};  // 7
    Light::Field<bool, Light::SqlRealName{"is_shared"}> isShared{false};  // 8
    Light::Field<std::int64_t, Light::SqlRealName{"created_at_ms"}> createdAtMs{0};  // 9
    Light::Field<std::int64_t, Light::SqlRealName{"updated_at_ms"}> updatedAtMs{0};  // 10
    /// Empty = no favicon fetched yet. Path, not bytes — the metadata
    /// worker's own doc comment (Task 12) explains why blobs never travel
    /// the action protocol.
    Light::Field<std::string, Light::SqlRealName{"favicon_path"}> faviconPath;  // 11
};

}  // namespace bookmarks::db
```

- [ ] **Step 4: Write `examples/bookmarks/include/bookmarks/db/tag_entity.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>

#include <cstdint>
#include <string_view>

namespace bookmarks::db {

/// @brief One row of the `tags` table. `name` is a plain variable-length
///        `TEXT` column, not a fixed `SqlAnsiString` — see
///        `bookmarks/dto/tag_dto.hpp`'s file comment for why (tag names are
///        free-form Unicode text; truncating one is exactly the harm this
///        session's `pastebin::EditPaste`/`syntax` fix eliminated
///        elsewhere). No relation-typed member — see `bookmark_entity.hpp`'s
///        file comment.
struct TagRecord {
    static constexpr std::string_view TableName = "tags";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::string, Light::SqlRealName{"owner_principal"}> ownerPrincipal;  // 1
    Light::Field<std::string, Light::SqlRealName{"name"}> name;  // 2
};

}  // namespace bookmarks::db
```

- [ ] **Step 5: Write `examples/bookmarks/include/bookmarks/db/bookmark_tag_entity.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bookmarks/db/bookmark_entity.hpp"
#include "bookmarks/db/tag_entity.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>

#include <cstdint>
#include <string_view>

namespace bookmarks::db {

/// @brief The bookmark<->tag many-to-many junction (`IMPLEMENTATION.md`
///        rule 4's "real Lightweight idiom" clause — this is an ordinary
///        `BelongsTo`-pair entity, not the sanctioned raw-SQL escape tier).
///        `BelongsTo<>` supports `Update()` (unlike `HasMany`/
///        `HasManyThrough` — see `bookmark_entity.hpp`'s file comment), but
///        this record never needs it: tag assignment/removal is always a
///        `Create`/delete of a whole row (`BookmarkModel::execute`, Task 6).
struct BookmarkTagRecord {
    static constexpr std::string_view TableName = "bookmark_tags";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::BelongsTo<&BookmarkRecord::id, Light::SqlRealName{"bookmark_id"}> bookmark;  // 1
    Light::BelongsTo<&TagRecord::id, Light::SqlRealName{"tag_id"}> tag;  // 2
};

}  // namespace bookmarks::db
```

- [ ] **Step 6: Write `examples/bookmarks/include/bookmarks/db/imported_op_entity.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>

#include <cstdint>
#include <string_view>

namespace bookmarks::db {

/// @brief One applied `ImportBookmarks` chunk, keyed by `(owner_principal,
///        op_id)` — Task 11's idempotency check: a repeated chunk with the
///        same `opId` after a dropped connection finds its row already
///        present and is a safe no-op.
struct ImportedOpRecord {
    static constexpr std::string_view TableName = "imported_ops";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::string, Light::SqlRealName{"owner_principal"}> ownerPrincipal;  // 1
    Light::Field<std::string, Light::SqlRealName{"op_id"}> opId;  // 2
    Light::Field<std::int64_t, Light::SqlRealName{"applied_at_ms"}> appliedAtMs{0};  // 3
};

}  // namespace bookmarks::db
```

- [ ] **Step 7: Write `examples/bookmarks/include/bookmarks/db/database.hpp`** (mirrors `pastebin::db::setup` exactly)

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace bookmarks::db {

/// @brief Points Lightweight's default connection at @p connectionString and
///        applies every pending migration. Production-bootstrap-only, called
///        once by Task 12's server app — see `pastebin::db::setup`'s
///        identical doc comment for why tests never call this.
/// @param connectionString ODBC connection string.
void setup(const std::string& connectionString);

}  // namespace bookmarks::db
```

- [ ] **Step 8: Write `examples/bookmarks/include/bookmarks/db/db_model.hpp`** (byte-for-byte the same mixin as `pastebin::db::WithMapper`, namespace changed)

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifndef __EMSCRIPTEN__
#include <Lightweight/DataMapper/DataMapper.hpp>

#include <optional>
#endif

/// @file
/// See `pastebin::db::WithMapper`'s file comment
/// (`examples/pastebin/include/pastebin/db/db_model.hpp`) for the full
/// rationale this mixin reuses verbatim — the WASM header-vs-link
/// dependency finding (025) applies identically to this rung's three models.

namespace bookmarks::db {

#ifndef __EMSCRIPTEN__

/// @brief Base providing `mapper()` — one lazily-constructed DataMapper per model.
class WithMapper {
protected:
    WithMapper() = default;

    /// @brief Returns this model's DataMapper, opening it on first use.
    [[nodiscard]] Lightweight::DataMapper& mapper() {
        if (!_mapper.has_value()) {
            _mapper.emplace();
        }
        return *_mapper;
    }

private:
    std::optional<Lightweight::DataMapper> _mapper;
};

#else

/// @brief Persistence-free base for the browser build. No `mapper()`.
class WithMapper {
protected:
    WithMapper() = default;
};

#endif

}  // namespace bookmarks::db
```

- [ ] **Step 9: Write `examples/bookmarks/src/db/schema.cpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/db/database.hpp"

#include <Lightweight/SqlConnection.hpp>
#include <Lightweight/SqlMigration.hpp>
#include <Lightweight/SqlQuery/Migrate.hpp>

namespace bookmarks::db {

void setup(const std::string& connectionString) {
    Lightweight::SqlConnection::SetDefaultConnectionString(Lightweight::SqlConnectionString{connectionString});
    Lightweight::SqlMigration::MigrationManager::GetInstance().CreateMigrationHistory();
    Lightweight::SqlMigration::MigrationManager::GetInstance().ApplyPendingMigrations();
}

}  // namespace bookmarks::db

using namespace Lightweight::SqlColumnTypeDefinitions;

LIGHTWEIGHT_SQL_MIGRATION(20260807000001, "Create bookmarks tables") {
    plan.CreateTableIfNotExists("bookmarks")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("owner_principal", Varchar(64))
        .RequiredColumn("url", Text())
        .RequiredColumn("title", Text())
        .RequiredColumn("description", Text())
        .RequiredColumn("notes", Text())
        .RequiredColumn("is_unread", Bool())
        .RequiredColumn("is_archived", Bool())
        .RequiredColumn("is_shared", Bool())
        .RequiredColumn("created_at_ms", Bigint())
        .RequiredColumn("updated_at_ms", Bigint())
        .RequiredColumn("favicon_path", Text());
    // Every list/get/edit/archive query filters on owner_principal first;
    // the changes-since poll (Task 7) additionally filters on
    // updated_at_ms, and the shared feed (Task 10) on is_shared alone.
    plan.CreateIndex("idx_bookmarks_owner", "bookmarks", {"owner_principal"});
    plan.CreateIndex("idx_bookmarks_owner_updated", "bookmarks", {"owner_principal", "updated_at_ms"});
    plan.CreateIndex("idx_bookmarks_shared", "bookmarks", {"is_shared"});

    plan.CreateTableIfNotExists("tags")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("owner_principal", Varchar(64))
        .RequiredColumn("name", Text());
    // Tag names are unique per owner, not globally -- two different users
    // may both have a tag named "work".
    plan.CreateUniqueIndex("idx_tags_owner_name", "tags", {"owner_principal", "name"});

    const auto bookmarksRef = Lightweight::SqlForeignKeyReferenceDefinition{.tableName = "bookmarks", .columnName = "id"};
    const auto tagsRef = Lightweight::SqlForeignKeyReferenceDefinition{.tableName = "tags", .columnName = "id"};
    plan.CreateTableIfNotExists("bookmark_tags")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredForeignKey("bookmark_id", Bigint(), bookmarksRef)
        .RequiredForeignKey("tag_id", Bigint(), tagsRef);
    // A bookmark may never carry the same tag twice -- this is what makes
    // TagModel::execute(const MergeTags&)'s "INSERT OR IGNORE"-shaped
    // dedup (Task 9) meaningful rather than a defensive no-op.
    plan.CreateUniqueIndex("idx_bookmark_tags_pair", "bookmark_tags", {"bookmark_id", "tag_id"});

    plan.CreateTableIfNotExists("imported_ops")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("owner_principal", Varchar(64))
        .RequiredColumn("op_id", Varchar(128))
        .RequiredColumn("applied_at_ms", Bigint());
    plan.CreateUniqueIndex("idx_imported_ops_owner_op", "imported_ops", {"owner_principal", "op_id"});
}
```

- [ ] **Step 10: Run to verify it passes.**

- [ ] **Step 11: Commit**

```bash
git add examples/bookmarks/include/bookmarks/db/ examples/bookmarks/src/db/schema.cpp \
        examples/bookmarks/tests/test_bookmarks_schema.cpp
git commit -m "bookmarks: add entities, schema migration, and the WithMapper mixin"
```

---

## Task 6: `BookmarkModel` — CRUD, archive/unarchive, tag replace-set

**Files:**
- Create: `examples/bookmarks/include/bookmarks/models/bookmark_model.hpp`
- Create: `examples/bookmarks/src/models/bookmark_model.cpp`
- Test: `examples/bookmarks/tests/test_bookmark_model.cpp`

**Interfaces:**
- Consumes: everything from Tasks 2-5.
- Produces: `bookmarks::BookmarkModel` (declares **every** `execute()`
  overload this rung's `BookmarkModel` ever has, including
  `ListBookmarks`/`GetChangesSince` (Task 7) and `BulkEdit`/`RecordMetadata`
  (Task 8) — the header is written once, complete, here; those two later
  tasks only add bodies to `bookmark_model.cpp`, never touch the header
  again). `BRIDGE_REGISTER_MODEL`/`BRIDGE_REGISTER_ACTION` wiring for every
  action this task itself implements (`CreateBookmark`, `EditBookmark`,
  `ArchiveBookmark`, `UnarchiveBookmark`, `DeleteBookmark`, `GetBookmark`) —
  Tasks 7/8 add their own `BRIDGE_REGISTER_ACTION` lines for the actions
  they implement, in the same header.

**A test-only session helper this and every later model-test task needs:**
`BookmarkModel::execute()` reads `session::current()->principal` as the
owner filter (this plan's "Corrections" section — no per-instance state, a
fresh read every call). Model unit tests call `model.execute(action)`
directly, C++-to-C++, exactly as `pastebin`'s tests do — which means no
`RemoteServer`/`Bridge` ever runs to install a `Context` via
`session::detail::ScopedContext`, so `session::current()` would return
`nullptr` in every test unless the test installs one itself.
`session::detail::ScopedContext` is a `detail::` symbol, and testkit
reaching into `morph::*::detail` namespaces is an already-accepted,
already-tracked pattern in this codebase (`docs/findings/019-testkit-reaches-into-four-detail-namespaces.md`)
— not a new departure. `ScopedPrincipal`, defined once in
`test_bookmark_model.cpp` (not promoted to shared `examples/common/testkit`
yet — one consumer so far; the promotion rule triggers at a third), wraps
it:

```cpp
class ScopedPrincipal {
  public:
    explicit ScopedPrincipal(std::string principal) : _ctx{.principal = std::move(principal)}, _scope{_ctx} {}

  private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};
```

- [ ] **Step 1: Write the failing test**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/models/bookmark_model.hpp"
#include "testkit/db_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <morph/session/session.hpp>

using morph::ladder::testkit::DbFixture;

namespace {
class ScopedPrincipal {
  public:
    explicit ScopedPrincipal(std::string principal) : _ctx{.principal = std::move(principal)}, _scope{_ctx} {}

  private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};
}  // namespace

TEST_CASE("CreateBookmark stores a bookmark owned by the authenticated principal",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal principal{"alice"};

    bookmarks::CreateBookmark action;
    action.url = "https://example.com";
    action.title = "Example";
    action.tags = {"work", "reading"};
    const auto id = model.execute(action).id;
    REQUIRE(id.hasValue());

    const auto view = model.execute(bookmarks::GetBookmark{.id = id});
    CHECK(view.url == "https://example.com");
    CHECK(view.title == "Example");
    CHECK(view.readState == bookmarks::ReadState::Unread);
    CHECK(view.archiveState == bookmarks::ArchiveState::Active);
    CHECK(view.tags.size() == 2);
}

TEST_CASE("CreateBookmark without a principal is Forbidden", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    // No ScopedPrincipal installed -- session::current() is nullptr.
    bookmarks::CreateBookmark action;
    action.url = "https://example.com";
    REQUIRE_THROWS_AS(model.execute(action), bookmarks::Forbidden);
}

TEST_CASE("GetBookmark refuses a different principal's bookmark with Forbidden, not NotFound",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    bookmarks::BookmarkId id;
    {
        const ScopedPrincipal alice{"alice"};
        id = model.execute(bookmarks::CreateBookmark{.url = "https://example.com"}).id;
    }
    const ScopedPrincipal mallory{"mallory"};
    REQUIRE_THROWS_AS(model.execute(bookmarks::GetBookmark{.id = id}), bookmarks::Forbidden);
}

TEST_CASE("EditBookmark replaces the tag set: adds new tags, drops removed ones, keeps shared ones",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};

    auto create = bookmarks::CreateBookmark{.url = "https://example.com", .tags = {"a", "b"}};
    const auto id = model.execute(create).id;

    bookmarks::EditBookmark edit{.id = id, .url = "https://example.com", .tags = {"b", "c"}};
    const auto edited = model.execute(edit);
    std::vector<std::string> tags = edited.tags;
    std::ranges::sort(tags);
    CHECK(tags == std::vector<std::string>{"b", "c"});  // "a" dropped, "b" kept, "c" auto-created
}

TEST_CASE("ArchiveBookmark/UnarchiveBookmark flip archiveState and nothing else",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};
    const auto id = model.execute(bookmarks::CreateBookmark{.url = "https://example.com"}).id;

    model.execute(bookmarks::ArchiveBookmark{.id = id});
    CHECK(model.execute(bookmarks::GetBookmark{.id = id}).archiveState == bookmarks::ArchiveState::Archived);
    model.execute(bookmarks::UnarchiveBookmark{.id = id});
    CHECK(model.execute(bookmarks::GetBookmark{.id = id}).archiveState == bookmarks::ArchiveState::Active);
}

TEST_CASE("DeleteBookmark removes the bookmark and its tag associations", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};
    const auto id = model.execute(bookmarks::CreateBookmark{.url = "https://example.com", .tags = {"a"}}).id;

    model.execute(bookmarks::DeleteBookmark{.id = id});
    REQUIRE_THROWS_AS(model.execute(bookmarks::GetBookmark{.id = id}), bookmarks::NotFound);
}

TEST_CASE("GetBookmark against an unknown id throws NotFound, and an empty id is a ValidationError",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};
    REQUIRE_THROWS_AS(model.execute(bookmarks::GetBookmark{.id = bookmarks::BookmarkId{99999}}),
                      bookmarks::NotFound);
    REQUIRE_THROWS_AS(model.execute(bookmarks::GetBookmark{}), bookmarks::ValidationError);
}
```

- [ ] **Step 2: Run to verify it fails to compile** — the header/model do not exist yet.

- [ ] **Step 3: Write `examples/bookmarks/include/bookmarks/models/bookmark_model.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

#include "bookmarks/core/errors.hpp"
#include "bookmarks/db/db_model.hpp"
#include "bookmarks/dto/bookmark_dto.hpp"
#include "bookmarks/dto/bulk_dto.hpp"
#include "bookmarks/dto/import_export_dto.hpp"

/// @file
/// `BookmarkModel` — every action this rung's one entity-owning model
/// serves. Declared once, complete, here; Tasks 7/8 add bodies to
/// `bookmark_model.cpp` for `ListBookmarks`/`GetChangesSince`/`BulkEdit`/
/// `RecordMetadata` without touching this header again.

namespace bookmarks {

/// @brief Create/read/edit/archive/delete/list/bulk-edit over the
///        `bookmarks`/`bookmark_tags` tables, scoped to the authenticated
///        caller's own collection.
///
/// Registered **plain** — no `BRIDGE_MODEL_KEY`, no `AllowShared` (this
/// plan's "Corrections to the README" — a *shared* instance is recorded
/// with an empty owner, defeating `authorizeInstance`'s real per-instance
/// ownership check). Every `execute()` reads `session::current()->principal`
/// fresh and uses it both as the query filter and as the authorization
/// re-check `IMPLEMENTATION.md` rule 1 requires (the local backend enforces
/// nothing at all).
class BookmarkModel : private db::WithMapper {
public:
    CreateBookmarkResult execute(const CreateBookmark& action);
    BookmarkView execute(const EditBookmark& action);
    Ack execute(const ArchiveBookmark& action);
    Ack execute(const UnarchiveBookmark& action);
    Ack execute(const DeleteBookmark& action);
    BookmarkView execute(const GetBookmark& action);
    ListBookmarksResult execute(const ListBookmarks& action);            // Task 7
    GetChangesSinceResult execute(const GetChangesSince& action);        // Task 7
    BulkEditResult execute(const BulkEdit& action);                      // Task 8
    Ack execute(const RecordMetadata& action);                          // Task 8, internal-only
    ImportBookmarksResult execute(const ImportBookmarks& action);        // Task 11
    ExportBookmarksResult execute(const ExportBookmarks& action);        // Task 11
};

}  // namespace bookmarks

BRIDGE_REGISTER_MODEL(bookmarks::BookmarkModel, "BookmarkModel")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::CreateBookmark, "CreateBookmark")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::EditBookmark, "EditBookmark")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::ArchiveBookmark, "ArchiveBookmark")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::UnarchiveBookmark, "UnarchiveBookmark")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::DeleteBookmark, "DeleteBookmark")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::GetBookmark, "GetBookmark")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::ListBookmarks, "ListBookmarks",
                       ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::GetChangesSince, "GetChangesSince",
                       ::morph::model::Loggable::No)
// BulkEdit is outbox-managed (Task 8) -- Loggable::No here too, so the
// framework's own auto-append never double-logs alongside the model's own
// outbox write.
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::BulkEdit, "BulkEdit", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::RecordMetadata, "RecordMetadata")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::ImportBookmarks, "ImportBookmarks")
BRIDGE_REGISTER_ACTION(bookmarks::BookmarkModel, bookmarks::ExportBookmarks, "ExportBookmarks",
                       ::morph::model::Loggable::No)
```

- [ ] **Step 4: Write `examples/bookmarks/src/models/bookmark_model.cpp`** (this task's six actions only —
      `ListBookmarks`/`GetChangesSince`/`BulkEdit`/`RecordMetadata` bodies land in Tasks 7/8, appended to this same file)

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/models/bookmark_model.hpp"

#include "bookmarks/db/bookmark_entity.hpp"
#include "bookmarks/db/bookmark_tag_entity.hpp"
#include "bookmarks/db/tag_entity.hpp"

#include "clock.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlErrorDetection.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <Lightweight/SqlTransaction.hpp>

#include <morph/session/session.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bookmarks {

namespace {

[[nodiscard]] std::int64_t nowMs() noexcept {
    return (*::morph::ladder::now().value).value.time_since_epoch().count();
}

[[nodiscard]] ::morph::time::Timestamp fromEpochMs(std::int64_t epochMs) noexcept {
    return ::morph::time::Timestamp{::morph::time::DateTime{
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{epochMs}}}};
}

/// @brief The authenticated caller's principal, or throws `Forbidden`.
///
/// `session::current()` is populated fresh on every dispatched action
/// (`session::detail::ScopedContext`, installed by `RemoteServer`/
/// `LocalBackend` around each `execute()`); reading it here rather than
/// once at construction is what lets a single plain-registered
/// `BookmarkModel` instance serve whichever principal's call actually
/// reaches it -- there is exactly one instance per registration, so in
/// practice this is stable across a registration's whole lifetime, but the
/// model never assumes that, matching rule 1's "models re-check their own
/// authorization" requirement. `nullptr`/empty is treated identically to an
/// unauthenticated caller: `Forbidden`, not a crash -- reachable from a
/// test that calls `execute()` directly with no session installed, and
/// (defensively) from a local backend, which installs a `Context` but
/// never verifies it.
[[nodiscard]] const std::string& requireOwner() {
    const auto* ctx = ::morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw Forbidden{"no authenticated principal"};
    }
    return ctx->principal;
}

}  // namespace

/// @brief Reads every tag name currently associated with @p bookmarkId, for
///        @p owner's own tags only (a tag row is always owned by the same
///        principal as every bookmark it's attached to, by construction --
///        `applyTagSet` below never creates a cross-owner association).
[[nodiscard]] static std::vector<std::string> readTagNames(::Lightweight::DataMapper& mapper, std::uint64_t bookmarkId) {
    auto junctionRows = mapper.Query<db::BookmarkTagRecord>()
                             .Where(::Lightweight::FieldNameOf<&db::BookmarkTagRecord::bookmark>, "=", bookmarkId)
                             .All();
    std::vector<std::string> names;
    names.reserve(junctionRows.size());
    for (const auto& row : junctionRows) {
        auto tagRows = mapper.Query<db::TagRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::TagRecord::id>, "=", row.tag.Value())
                            .All();
        if (!tagRows.empty()) {
            names.push_back(tagRows.front().name.Value());
        }
    }
    return names;
}

/// @brief Replaces @p bookmarkId's tag set with exactly @p desiredNames,
///        auto-creating any tag @p owner has never used before. Must run
///        inside the caller's own `SqlTransaction` -- this function opens
///        none of its own, so every write it makes commits or rolls back
///        with the surrounding action.
static void applyTagSet(::Lightweight::DataMapper& mapper, std::uint64_t bookmarkId, const std::string& owner,
                        const std::vector<std::string>& desiredNames) {
    const auto current = readTagNames(mapper, bookmarkId);
    std::vector<std::string> toAdd;
    for (const auto& name : desiredNames) {
        if (std::ranges::find(current, name) == current.end()) {
            toAdd.push_back(name);
        }
    }
    std::vector<std::string> toRemove;
    for (const auto& name : current) {
        if (std::ranges::find(desiredNames, name) == desiredNames.end()) {
            toRemove.push_back(name);
        }
    }

    for (const auto& name : toAdd) {
        auto existing =
            mapper.Query<db::TagRecord>()
                .Where(::Lightweight::FieldNameOf<&db::TagRecord::ownerPrincipal>, "=", owner)
                .Where(::Lightweight::FieldNameOf<&db::TagRecord::name>, "=", name)
                .All();
        std::uint64_t tagId = 0;
        if (existing.empty()) {
            db::TagRecord tag;
            tag.ownerPrincipal = owner;
            tag.name = name;
            mapper.Create(tag);
            tagId = tag.id.Value();
        } else {
            tagId = existing.front().id.Value();
        }
        db::BookmarkTagRecord junction;
        junction.bookmark = bookmarkId;
        junction.tag = tagId;
        mapper.Create(junction);
    }

    for (const auto& name : toRemove) {
        auto tagRows = mapper.Query<db::TagRecord>()
                            .Where(::Lightweight::FieldNameOf<&db::TagRecord::ownerPrincipal>, "=", owner)
                            .Where(::Lightweight::FieldNameOf<&db::TagRecord::name>, "=", name)
                            .All();
        if (tagRows.empty()) {
            continue;
        }
        ::Lightweight::SqlStatement stmt{mapper.Connection()};
        stmt.Prepare("DELETE FROM bookmark_tags WHERE bookmark_id = ? AND tag_id = ?");
        (void) stmt.Execute(bookmarkId, tagRows.front().id.Value());
    }
}

[[nodiscard]] static BookmarkView toView(const db::BookmarkRecord& rec, std::vector<std::string> tags) {
    BookmarkView view;
    view.id = BookmarkId{static_cast<std::int64_t>(rec.id.Value())};
    view.url = rec.url.Value();
    view.title = rec.title.Value();
    view.description = rec.description.Value();
    view.notes = rec.notes.Value();
    view.tags = std::move(tags);
    view.createdAt = fromEpochMs(rec.createdAtMs.Value());
    view.updatedAt = fromEpochMs(rec.updatedAtMs.Value());
    view.readState = rec.isUnread.Value() ? ReadState::Unread : ReadState::Read;
    view.archiveState = rec.isArchived.Value() ? ArchiveState::Archived : ArchiveState::Active;
    view.visibility = rec.isShared.Value() ? Visibility::Shared : Visibility::Private;
    return view;
}

/// @brief Loads @p id, requiring it to exist and be owned by @p owner.
/// @throws NotFound if no such row exists at all.
/// @throws Forbidden if it exists but belongs to a different principal --
///         distinguished on purpose (`bookmarks::Forbidden`'s own doc
///         comment) so the "local mode has no authorization at all" test
///         (Task 15) has something specific to assert against.
[[nodiscard]] static db::BookmarkRecord loadOwned(::Lightweight::DataMapper& mapper, std::uint64_t id,
                                                  const std::string& owner) {
    auto rows =
        mapper.Query<db::BookmarkRecord>().Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::id>, "=", id).All();
    if (rows.empty()) {
        throw NotFound{"no such bookmark"};
    }
    if (rows.front().ownerPrincipal.Value() != owner) {
        throw Forbidden{"bookmark belongs to a different principal"};
    }
    return rows.front();
}

CreateBookmarkResult BookmarkModel::execute(const CreateBookmark& action) {
    if (!action.validate()) {
        throw ValidationError{"CreateBookmark: a non-empty url within the length bound is required"};
    }
    const auto& owner = requireOwner();

    db::BookmarkRecord rec;
    rec.ownerPrincipal = owner;
    rec.url = action.url;
    rec.title = action.title;
    rec.description = action.description;
    rec.notes = action.notes;
    rec.isShared = action.visibility == Visibility::Shared;
    const auto now = nowMs();
    rec.createdAtMs = now;
    rec.updatedAtMs = now;

    ::Lightweight::SqlTransaction transaction{mapper().Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
    mapper().Create(rec);
    applyTagSet(mapper(), rec.id.Value(), owner, action.tags);
    transaction.Commit();

    return CreateBookmarkResult{.id = BookmarkId{static_cast<std::int64_t>(rec.id.Value())}};
}

BookmarkView BookmarkModel::execute(const EditBookmark& action) {
    if (!action.validate()) {
        throw ValidationError{"EditBookmark: id and a non-empty url within the length bound are required"};
    }
    const auto& owner = requireOwner();
    auto rec = loadOwned(mapper(), static_cast<std::uint64_t>(*action.id), owner);

    rec.url = action.url;
    rec.title = action.title;
    rec.description = action.description;
    rec.notes = action.notes;
    rec.isShared = action.visibility == Visibility::Shared;
    rec.updatedAtMs = nowMs();

    ::Lightweight::SqlTransaction transaction{mapper().Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
    mapper().Update(rec);
    applyTagSet(mapper(), rec.id.Value(), owner, action.tags);
    transaction.Commit();

    return toView(rec, readTagNames(mapper(), rec.id.Value()));
}

Ack BookmarkModel::execute(const ArchiveBookmark& action) {
    if (!action.validate()) {
        throw ValidationError{"ArchiveBookmark: id is required"};
    }
    const auto& owner = requireOwner();
    auto rec = loadOwned(mapper(), static_cast<std::uint64_t>(*action.id), owner);
    rec.isArchived = true;
    rec.updatedAtMs = nowMs();
    mapper().Update(rec);
    return Ack{};
}

Ack BookmarkModel::execute(const UnarchiveBookmark& action) {
    if (!action.validate()) {
        throw ValidationError{"UnarchiveBookmark: id is required"};
    }
    const auto& owner = requireOwner();
    auto rec = loadOwned(mapper(), static_cast<std::uint64_t>(*action.id), owner);
    rec.isArchived = false;
    rec.updatedAtMs = nowMs();
    mapper().Update(rec);
    return Ack{};
}

Ack BookmarkModel::execute(const DeleteBookmark& action) {
    if (!action.validate()) {
        throw ValidationError{"DeleteBookmark: id is required"};
    }
    const auto& owner = requireOwner();
    const auto id = static_cast<std::uint64_t>(*action.id);
    (void) loadOwned(mapper(), id, owner);  // NotFound/Forbidden, same as every other action

    ::Lightweight::SqlTransaction transaction{mapper().Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
    {
        ::Lightweight::SqlStatement stmt{mapper().Connection()};
        stmt.Prepare("DELETE FROM bookmark_tags WHERE bookmark_id = ?");
        (void) stmt.Execute(id);
    }
    {
        ::Lightweight::SqlStatement stmt{mapper().Connection()};
        stmt.Prepare("DELETE FROM bookmarks WHERE id = ?");
        (void) stmt.Execute(id);
    }
    transaction.Commit();
    return Ack{};
}

BookmarkView BookmarkModel::execute(const GetBookmark& action) {
    if (!action.validate()) {
        throw ValidationError{"GetBookmark: id is required"};
    }
    const auto& owner = requireOwner();
    const auto rec = loadOwned(mapper(), static_cast<std::uint64_t>(*action.id), owner);
    return toView(rec, readTagNames(mapper(), rec.id.Value()));
}

}  // namespace bookmarks
```

- [ ] **Step 5: Run to verify it passes**

Run (once Task 13's CMake exists): `ctest --test-dir build/clang-coverage -R '\[bookmarks\]\[model\]' --output-on-failure`

- [ ] **Step 6: Commit**

```bash
git add examples/bookmarks/include/bookmarks/models/bookmark_model.hpp \
        examples/bookmarks/src/models/bookmark_model.cpp \
        examples/bookmarks/tests/test_bookmark_model.cpp
git commit -m "bookmarks: add BookmarkModel CRUD, archive/unarchive, and tag replace-set"
```

---

## Task 7: `BookmarkModel` — `ListBookmarks` and `GetChangesSince`

**Files:**
- Modify: `examples/bookmarks/src/models/bookmark_model.cpp` (append two
  `execute()` bodies; header already declares both, Task 6)
- Modify: `examples/bookmarks/tests/test_bookmark_model.cpp` (append cases)

**Interfaces:** No new types. Consumes `ListBookmarks`/`ListBookmarksResult`,
`GetChangesSince`/`GetChangesSinceResult`, `BookmarkSummary` (Task 3).

**The `asOf` ordering argument** (README's own rigor standard, matching
finding 018/022's treatment): `GetChangesSinceResult::asOf` must be captured
**before** the query runs, not after. If it were captured after, a write
that lands *during* the query window (between the query starting and the
result being read) could be invisible to *this* poll (its `updated_at_ms`
might not yet be committed when the `SELECT` ran) and then get skipped by
the *next* poll too, because the next poll's `since` would already be past
that write's timestamp — a silently lost update. Capturing `asOf` first
means the next poll's `since` is always a instant *no later than* the
query that just ran, so any write racing the query is, at worst, seen
*again* on the next poll (a harmless duplicate in `changed`) rather than
never.

- [ ] **Step 1: Append the failing tests**

```cpp
TEST_CASE("ListBookmarks filters by archive state and hides archived bookmarks by default",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};
    const auto activeId = model.execute(bookmarks::CreateBookmark{.url = "https://active.example"}).id;
    const auto archivedId = model.execute(bookmarks::CreateBookmark{.url = "https://archived.example"}).id;
    model.execute(bookmarks::ArchiveBookmark{.id = archivedId});

    const auto defaultPage = model.execute(bookmarks::ListBookmarks{});
    REQUIRE(defaultPage.bookmarks.size() == 1);
    CHECK(*defaultPage.bookmarks.front().id == *activeId);

    bookmarks::ListBookmarks archivedOnly;
    archivedOnly.archiveFilter = bookmarks::ArchiveFilter::ArchivedOnly;
    const auto archivedPage = model.execute(archivedOnly);
    REQUIRE(archivedPage.bookmarks.size() == 1);
    CHECK(*archivedPage.bookmarks.front().id == *archivedId);
}

TEST_CASE("ListBookmarks only ever returns the calling principal's own bookmarks",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    {
        const ScopedPrincipal alice{"alice"};
        model.execute(bookmarks::CreateBookmark{.url = "https://alice.example"});
    }
    const ScopedPrincipal mallory{"mallory"};
    model.execute(bookmarks::CreateBookmark{.url = "https://mallory.example"});
    const auto page = model.execute(bookmarks::ListBookmarks{});
    REQUIRE(page.bookmarks.size() == 1);
    CHECK(page.bookmarks.front().url == "https://mallory.example");
}

TEST_CASE("GetChangesSince returns only bookmarks touched after the given instant",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};

    const auto before = *morph::ladder::now();
    const morph::ladder::ScopedClockOverride clock1{before + std::chrono::milliseconds{10}};
    const auto id1 = model.execute(bookmarks::CreateBookmark{.url = "https://one.example"}).id;

    const auto cursor = model.execute(bookmarks::GetChangesSince{}).asOf;

    const morph::ladder::ScopedClockOverride clock2{before + std::chrono::milliseconds{20}};
    const auto id2 = model.execute(bookmarks::CreateBookmark{.url = "https://two.example"}).id;

    const auto changes = model.execute(bookmarks::GetChangesSince{.since = cursor});
    REQUIRE(changes.changed.size() == 1);
    CHECK(*changes.changed.front().id == *id2);
    (void) id1;
}
```

- [ ] **Step 2: Run to verify the new cases fail** (methods not yet implemented — link error / pure-virtual-like gap
  is not applicable here since the header already declares them; instead this fails at **Step 1's own compile** with
  "undefined reference" at link time, since the `.cpp` bodies do not exist yet).

- [ ] **Step 3: Append to `examples/bookmarks/src/models/bookmark_model.cpp`**

```cpp
ListBookmarksResult BookmarkModel::execute(const ListBookmarks& action) {
    const auto& owner = requireOwner();
    auto query = mapper().Query<db::BookmarkRecord>();
    (void) query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::ownerPrincipal>, "=", owner);
    if (action.archiveFilter == ArchiveFilter::ActiveOnly) {
        (void) query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::isArchived>, "=", false);
    } else if (action.archiveFilter == ArchiveFilter::ArchivedOnly) {
        (void) query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::isArchived>, "=", true);
    }
    if (action.readFilter == ReadFilter::UnreadOnly) {
        (void) query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::isUnread>, "=", true);
    } else if (action.readFilter == ReadFilter::ReadOnly) {
        (void) query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::isUnread>, "=", false);
    }
    if (action.cursor.hasValue()) {
        (void) query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::id>, "<",
                           static_cast<std::uint64_t>(*action.cursor));
    }
    // Text/tag filters run in C++ after the SQL page is fetched, not as a
    // LIKE/JOIN in the query above: this rung's scale (a demo bookmark
    // collection, not a production search index) does not warrant it, and
    // combining a tag filter with keyset pagination correctly needs the
    // junction table anyway, which the per-row loop below already touches.
    constexpr std::size_t kPageSize = 20;
    auto rows = query.OrderBy(::Lightweight::FieldNameOf<&db::BookmarkRecord::id>, ::Lightweight::SqlResultOrdering::DESCENDING)
                    .First(kPageSize + 1);
    const bool hasMore = rows.size() > kPageSize;
    if (hasMore) {
        rows.resize(kPageSize);
    }

    ListBookmarksResult result;
    for (const auto& rec : rows) {
        auto tags = readTagNames(mapper(), rec.id.Value());
        if (!action.tag.empty() && std::ranges::find(tags, action.tag) == tags.end()) {
            continue;
        }
        if (!action.searchText.empty() && rec.title.Value().find(action.searchText) == std::string::npos &&
            rec.url.Value().find(action.searchText) == std::string::npos) {
            continue;
        }
        BookmarkSummary summary;
        summary.id = BookmarkId{static_cast<std::int64_t>(rec.id.Value())};
        summary.url = rec.url.Value();
        summary.title = rec.title.Value();
        summary.tags = std::move(tags);
        summary.createdAt = fromEpochMs(rec.createdAtMs.Value());
        summary.updatedAt = fromEpochMs(rec.updatedAtMs.Value());
        summary.readState = rec.isUnread.Value() ? ReadState::Unread : ReadState::Read;
        summary.archiveState = rec.isArchived.Value() ? ArchiveState::Archived : ArchiveState::Active;
        summary.visibility = rec.isShared.Value() ? Visibility::Shared : Visibility::Private;
        result.bookmarks.push_back(std::move(summary));
    }
    if (hasMore && !result.bookmarks.empty()) {
        result.nextCursor = Cursor{static_cast<std::int64_t>(rows.back().id.Value())};
    }
    return result;
}

GetChangesSinceResult BookmarkModel::execute(const GetChangesSince& action) {
    const auto& owner = requireOwner();
    // Captured *before* the query -- see this task's own doc comment for
    // why a later capture would let a racing write be lost across two
    // consecutive polls instead of merely duplicated across them.
    const auto asOf = nowMs();
    const std::int64_t since = action.since.hasValue() ? (*action.since).value.time_since_epoch().count() : 0;

    auto rows = mapper()
                    .Query<db::BookmarkRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::ownerPrincipal>, "=", owner)
                    .Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::updatedAtMs>, ">", since)
                    .All();

    GetChangesSinceResult result;
    result.asOf = fromEpochMs(asOf);
    for (const auto& rec : rows) {
        BookmarkSummary summary;
        summary.id = BookmarkId{static_cast<std::int64_t>(rec.id.Value())};
        summary.url = rec.url.Value();
        summary.title = rec.title.Value();
        summary.tags = readTagNames(mapper(), rec.id.Value());
        summary.createdAt = fromEpochMs(rec.createdAtMs.Value());
        summary.updatedAt = fromEpochMs(rec.updatedAtMs.Value());
        summary.readState = rec.isUnread.Value() ? ReadState::Unread : ReadState::Read;
        summary.archiveState = rec.isArchived.Value() ? ArchiveState::Archived : ArchiveState::Active;
        summary.visibility = rec.isShared.Value() ? Visibility::Shared : Visibility::Private;
        result.changed.push_back(std::move(summary));
    }
    return result;
}
```

- [ ] **Step 4: Run to verify it passes.**

- [ ] **Step 5: Commit**

```bash
git add examples/bookmarks/src/models/bookmark_model.cpp examples/bookmarks/tests/test_bookmark_model.cpp
git commit -m "bookmarks: add BookmarkModel ListBookmarks and GetChangesSince"
```

---

## Task 8: `BookmarkModel` — `BulkEdit` (outbox-managed) and `RecordMetadata`

**Files:**
- Create: `examples/bookmarks/include/bookmarks/db/outbox_entity.hpp`
- Modify: `examples/bookmarks/src/db/schema.cpp` (append a second migration)
- Modify: `examples/bookmarks/src/models/bookmark_model.cpp` (append two
  `execute()` bodies, an outbox-write helper, and a `findOrCreateTagId`
  helper shared with `applyTagSet`)
- Modify: `examples/bookmarks/tests/test_bookmark_model.cpp`

**Interfaces:** Produces `bookmarks::db::BookmarkOutboxRecord` (the model's
own outbox table). Consumes `journal::LogEntry`, `IModelHolder::setOutboxManaged`.

**Outbox mechanics** (README's resolved "split by blast radius" decision):
`BulkEdit` writes its own `journal::LogEntry`-shaped row into
`bookmark_outbox`, inside the *same* `SqlTransaction` as the mutation, so a
crash mid-batch can never leave a committed partial edit with no
corresponding journal row (or vice versa) — the row and the mutation commit
or roll back together, atomically, by SQLite's own guarantee. A relay pass
(`journal::OutboxRelay`, wired in Task 12's `App`) drains `bookmark_outbox`
into the durable `FileActionLog` on its own schedule, exactly like
`examples/concepts/journal_and_outbox.cpp`'s worked demo — the only
difference is that this rung's outbox is a real SQL table, not a
stand-in `std::vector`. `IModelHolder::setOutboxManaged(true)` must be
called wherever a `BookmarkModel` instance is registered (Task 12's server
`App`) so the framework's default auto-append does not *also* log
`BulkEdit` — `BRIDGE_REGISTER_ACTION`'s `Loggable::No` for `BulkEdit`
(Task 6) already suppresses that half.

- [ ] **Step 1: Write `examples/bookmarks/include/bookmarks/db/outbox_entity.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>

#include <cstdint>
#include <string_view>

namespace bookmarks::db {

/// @brief `BookmarkModel`'s own transactional outbox — a row written inside
///        the same `SqlTransaction` as a multi-row mutation
///        (`BulkEdit`; `TagModel`'s `RenameTag`/`MergeTags`, Task 9, uses
///        the identical table), drained by `journal::OutboxRelay` (Task 12)
///        into the durable `FileActionLog`. Shaped after
///        `journal::LogEntry` (`include/morph/journal/action_log.hpp`) —
///        only the fields a relay actually needs, not a 1:1 mirror. A row
///        is deleted once relayed rather than flagged, so the table only
///        ever holds genuinely-unrelayed work.
struct BookmarkOutboxRecord {
    static constexpr std::string_view TableName = "bookmark_outbox";

    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;  // 0
    Light::Field<std::string, Light::SqlRealName{"model_type"}> modelType;  // 1
    Light::Field<std::string, Light::SqlRealName{"entity_key"}> entityKey;  // 2
    Light::Field<std::string, Light::SqlRealName{"action_type"}> actionType;  // 3
    Light::Field<std::string, Light::SqlRealName{"payload"}> payload;  // 4
    Light::Field<std::string, Light::SqlRealName{"result"}> result;  // 5
    Light::Field<std::string, Light::SqlRealName{"principal"}> principal;  // 6
    Light::Field<std::int64_t, Light::SqlRealName{"timestamp_ms"}> timestampMs{0};  // 7
    Light::Field<std::string, Light::SqlRealName{"idempotency_key"}> idempotencyKey;  // 8
};

}  // namespace bookmarks::db
```

- [ ] **Step 2: Append to `examples/bookmarks/src/db/schema.cpp`**

```cpp
LIGHTWEIGHT_SQL_MIGRATION(20260807000002, "Create bookmarks outbox table") {
    plan.CreateTableIfNotExists("bookmark_outbox")
        .PrimaryKeyWithAutoIncrement("id", Bigint())
        .RequiredColumn("model_type", Varchar(64))
        .RequiredColumn("entity_key", Varchar(64))
        .RequiredColumn("action_type", Varchar(64))
        .RequiredColumn("payload", Text())
        .RequiredColumn("result", Text())
        .RequiredColumn("principal", Varchar(64))
        .RequiredColumn("timestamp_ms", Bigint())
        .RequiredColumn("idempotency_key", Varchar(128));
    plan.CreateUniqueIndex("idx_bookmark_outbox_idempotency", "bookmark_outbox", {"idempotency_key"});
}
```

- [ ] **Step 3: Write the failing tests (appended)**

```cpp
TEST_CASE("BulkEdit archives every listed bookmark and adds/removes tags atomically",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};
    const auto id1 = model.execute(bookmarks::CreateBookmark{.url = "https://one.example", .tags = {"old"}}).id;
    const auto id2 = model.execute(bookmarks::CreateBookmark{.url = "https://two.example"}).id;

    bookmarks::BulkEdit edit;
    edit.ids = {id1, id2};
    edit.addTags = {"new"};
    edit.removeTags = {"old"};
    edit.archive = bookmarks::BulkArchiveOp::Archive;
    const auto result = model.execute(edit);
    CHECK(morph::math::floor(*result.affected) == 2);

    for (const auto id : {id1, id2}) {
        const auto view = model.execute(bookmarks::GetBookmark{.id = id});
        CHECK(view.archiveState == bookmarks::ArchiveState::Archived);
        CHECK(std::ranges::find(view.tags, "new") != view.tags.end());
        CHECK(std::ranges::find(view.tags, "old") == view.tags.end());
    }
}

TEST_CASE("BulkEdit rejects the whole batch if any id is not owned by the caller",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    bookmarks::BookmarkId aliceId;
    {
        const ScopedPrincipal alice{"alice"};
        aliceId = model.execute(bookmarks::CreateBookmark{.url = "https://alice.example"}).id;
    }
    const ScopedPrincipal mallory{"mallory"};
    const auto malloryId = model.execute(bookmarks::CreateBookmark{.url = "https://mallory.example"}).id;

    bookmarks::BulkEdit edit;
    edit.ids = {malloryId, aliceId};  // one owned, one not
    edit.archive = bookmarks::BulkArchiveOp::Archive;
    REQUIRE_THROWS_AS(model.execute(edit), bookmarks::Forbidden);

    // All-or-nothing: mallory's own bookmark was NOT archived either.
    CHECK(model.execute(bookmarks::GetBookmark{.id = malloryId}).archiveState == bookmarks::ArchiveState::Active);
}

TEST_CASE("BulkEdit writes exactly one outbox row per call, consumed by an OutboxRelay",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};
    const auto id = model.execute(bookmarks::CreateBookmark{.url = "https://one.example"}).id;

    bookmarks::BulkEdit edit;
    edit.ids = {id};
    edit.archive = bookmarks::BulkArchiveOp::Archive;
    model.execute(edit);

    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<bookmarks::db::BookmarkOutboxRecord>().All();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().actionType.Value() == "BulkEdit");
    CHECK(rows.front().principal.Value() == "alice");
}

TEST_CASE("RecordMetadata updates title/faviconPath regardless of the dispatching principal",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    bookmarks::BookmarkId id;
    {
        const ScopedPrincipal alice{"alice"};
        id = model.execute(bookmarks::CreateBookmark{.url = "https://one.example"}).id;
    }
    // Dispatched as the service principal, not "alice" -- must not throw Forbidden.
    const ScopedPrincipal worker{std::string{bookmarks::auth::kMetadataFetcherPrincipal}};
    model.execute(bookmarks::RecordMetadata{.id = id, .title = "Fetched Title"});

    const ScopedPrincipal alice{"alice"};
    CHECK(model.execute(bookmarks::GetBookmark{.id = id}).title == "Fetched Title");
}

TEST_CASE("RecordMetadata against an already-deleted bookmark is a benign no-op",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};
    const auto id = model.execute(bookmarks::CreateBookmark{.url = "https://one.example"}).id;
    model.execute(bookmarks::DeleteBookmark{.id = id});
    const ScopedPrincipal worker{std::string{bookmarks::auth::kMetadataFetcherPrincipal}};
    REQUIRE_NOTHROW(model.execute(bookmarks::RecordMetadata{.id = id, .title = "Too Late"}));
}
```

(Add `#include "bookmarks/auth/bookmarks_authorizer.hpp"` and
`#include "bookmarks/db/outbox_entity.hpp"` to the test file's includes.)

- [ ] **Step 4: Run to verify the new cases fail to link.**

- [ ] **Step 5: Append to `examples/bookmarks/src/models/bookmark_model.cpp`**

```cpp
// (near the top, alongside the other includes)
#include "bookmarks/db/outbox_entity.hpp"
#include <morph/core/registry.hpp>
```

```cpp
namespace {
// ... (existing helpers) ...

/// @brief Finds @p owner's tag named @p name, creating it if it does not
///        exist yet. Shared by `applyTagSet` (Task 6) and `BulkEdit`
///        (this task) — both run inside the caller's own transaction.
[[nodiscard]] std::uint64_t findOrCreateTagId(::Lightweight::DataMapper& mapper, const std::string& owner,
                                              const std::string& name) {
    auto existing = mapper.Query<db::TagRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::TagRecord::ownerPrincipal>, "=", owner)
                        .Where(::Lightweight::FieldNameOf<&db::TagRecord::name>, "=", name)
                        .All();
    if (!existing.empty()) {
        return existing.front().id.Value();
    }
    db::TagRecord tag;
    tag.ownerPrincipal = owner;
    tag.name = name;
    mapper.Create(tag);
    return tag.id.Value();
}

/// @brief Adds a bookmark<->tag association if it does not already exist —
///        the junction table's unique index (`idx_bookmark_tags_pair`)
///        makes a duplicate a no-op to *detect*, but this checks first
///        rather than relying on catching the constraint violation, so a
///        `BulkEdit`'s per-item loop never has to distinguish "this item's
///        add was a genuine no-op" from "this item hit an unrelated store
///        error" via exception type alone.
void addTagAssociationIfAbsent(::Lightweight::DataMapper& mapper, std::uint64_t bookmarkId, std::uint64_t tagId) {
    auto existing = mapper.Query<db::BookmarkTagRecord>()
                        .Where(::Lightweight::FieldNameOf<&db::BookmarkTagRecord::bookmark>, "=", bookmarkId)
                        .Where(::Lightweight::FieldNameOf<&db::BookmarkTagRecord::tag>, "=", tagId)
                        .All();
    if (!existing.empty()) {
        return;
    }
    db::BookmarkTagRecord junction;
    junction.bookmark = bookmarkId;
    junction.tag = tagId;
    mapper.Create(junction);
}

/// @brief Writes one row into `bookmark_outbox`. Must run inside the
///        caller's own `SqlTransaction` — see this task's own doc comment.
template <typename Action, typename Result>
void writeOutboxEntry(::Lightweight::DataMapper& mapper, const std::string& owner, const Action& action,
                      const Result& result, std::string_view actionType, std::string_view idempotencyKey) {
    db::BookmarkOutboxRecord entry;
    entry.modelType = "BookmarkModel";
    entry.entityKey = owner;
    entry.actionType = std::string{actionType};
    entry.payload = ::morph::model::ActionTraits<Action>::toJson(action);
    entry.result = ::morph::model::ActionTraits<Action>::resultToJson(result);
    entry.principal = owner;
    entry.timestampMs = nowMs();
    entry.idempotencyKey = std::string{idempotencyKey};
    mapper.Create(entry);
}

}  // namespace
```

`applyTagSet`'s own `toAdd` loop (Task 6) is revised in this task to call
`findOrCreateTagId` + `addTagAssociationIfAbsent` instead of its original
inline body, so the two call sites (`applyTagSet`, `BulkEdit` below) share
one implementation rather than duplicating it — a same-file refactor, no
interface change.

```cpp
BulkEditResult BookmarkModel::execute(const BulkEdit& action) {
    if (!action.validate()) {
        throw ValidationError{"BulkEdit: at least one id is required"};
    }
    const auto& owner = requireOwner();

    ::Lightweight::SqlTransaction transaction{mapper().Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};

    // Ownership check first, for *every* id, before any write: one
    // violation rejects the whole batch (README's "all-or-nothing"
    // framing, this task's resolved design decision) rather than applying
    // a partial edit and reporting which ids failed.
    std::vector<std::uint64_t> ids;
    ids.reserve(action.ids.size());
    for (const auto& bookmarkId : action.ids) {
        if (!bookmarkId.hasValue()) {
            throw ValidationError{"BulkEdit: every id must be engaged"};
        }
        const auto id = static_cast<std::uint64_t>(*bookmarkId);
        (void) loadOwned(mapper(), id, owner);  // throws Forbidden/NotFound -> whole transaction rolls back
        ids.push_back(id);
    }

    for (const auto id : ids) {
        if (action.archive == BulkArchiveOp::Archive) {
            ::Lightweight::SqlStatement stmt{mapper().Connection()};
            stmt.Prepare("UPDATE bookmarks SET is_archived = 1, updated_at_ms = ? WHERE id = ?");
            (void) stmt.Execute(nowMs(), id);
        } else if (action.archive == BulkArchiveOp::Unarchive) {
            ::Lightweight::SqlStatement stmt{mapper().Connection()};
            stmt.Prepare("UPDATE bookmarks SET is_archived = 0, updated_at_ms = ? WHERE id = ?");
            (void) stmt.Execute(nowMs(), id);
        }
        for (const auto& name : action.addTags) {
            const auto tagId = findOrCreateTagId(mapper(), owner, name);
            addTagAssociationIfAbsent(mapper(), id, tagId);
        }
        for (const auto& name : action.removeTags) {
            auto tagRows = mapper()
                               .Query<db::TagRecord>()
                               .Where(::Lightweight::FieldNameOf<&db::TagRecord::ownerPrincipal>, "=", owner)
                               .Where(::Lightweight::FieldNameOf<&db::TagRecord::name>, "=", name)
                               .All();
            if (tagRows.empty()) {
                continue;
            }
            ::Lightweight::SqlStatement stmt{mapper().Connection()};
            stmt.Prepare("DELETE FROM bookmark_tags WHERE bookmark_id = ? AND tag_id = ?");
            (void) stmt.Execute(id, tagRows.front().id.Value());
        }
    }

    BulkEditResult result{.affected = Count::fromDouble(static_cast<double>(ids.size()))};
    // idempotencyKey: not a client-supplied op-id (BulkEdit carries none —
    // unlike ImportBookmarks, retried bulk edits are not expected to be
    // idempotent at this layer), so a fresh key per call is enough to keep
    // this row distinguishable from any other outbox row; the relay's
    // dedup only matters across relay *retries* of the same row, not
    // across separate BulkEdit calls.
    writeOutboxEntry(mapper(), owner, action, result, "BulkEdit",
                     owner + "-bulkedit-" + std::to_string(nowMs()));
    transaction.Commit();
    return result;
}

Ack BookmarkModel::execute(const RecordMetadata& action) {
    if (!action.validate()) {
        throw ValidationError{"RecordMetadata: id is required"};
    }
    // Dispatched only by the internal metadata-fetch worker's
    // "system:metadata-fetcher" service principal (Task 12) -- deliberately
    // skips the ownership check every GUI-reachable action performs: the
    // worker acts *on behalf of* whichever principal owns the row, not on
    // behalf of itself. The trust boundary is the signed service-principal
    // token verified at authorize()/authenticate() time, not a row-level
    // owner match here -- mirrors pastebin::ExpirePaste's identical
    // internal-only shape (including the deleted-before-processed no-op
    // below, which mirrors ExpirePaste's "already gone" tolerance).
    const auto id = static_cast<std::uint64_t>(*action.id);
    auto rows =
        mapper().Query<db::BookmarkRecord>().Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::id>, "=", id).All();
    if (rows.empty()) {
        return Ack{};
    }
    auto rec = rows.front();
    if (!action.title.empty()) {
        rec.title = action.title;
    }
    if (!action.faviconPath.empty()) {
        rec.faviconPath = action.faviconPath;
    }
    rec.updatedAtMs = nowMs();
    mapper().Update(rec);
    return Ack{};
}
```

- [ ] **Step 8: Run to verify it passes.**

- [ ] **Step 9: Commit**

```bash
git add examples/bookmarks/include/bookmarks/db/outbox_entity.hpp \
        examples/bookmarks/src/db/schema.cpp \
        examples/bookmarks/src/models/bookmark_model.cpp \
        examples/bookmarks/tests/test_bookmark_model.cpp
git commit -m "bookmarks: add BulkEdit (outbox-managed) and RecordMetadata"
```

---

## Task 9: `TagModel` — `RenameTag`, `MergeTags` (outbox-managed), `ListTags`

**Files:**
- Create: `examples/bookmarks/include/bookmarks/models/tag_model.hpp`
- Create: `examples/bookmarks/src/models/tag_model.cpp`
- Test: `examples/bookmarks/tests/test_tag_model.cpp`

**Interfaces:**
- Consumes: Tasks 2, 4, 5, 8 (`BookmarkOutboxRecord`, `findOrCreateTagId`-
  style patterns — `TagModel` re-implements its own small ownership/outbox
  helpers rather than sharing translation units with `BookmarkModel`, the
  same "duplicated rather than shared across models" choice
  `paste_model.cpp`'s own animal-name keyspace arrays already establish as
  this codebase's convention for small internal details).
- Produces: `bookmarks::TagModel`, registered plain, same authorizer.

`MergeTags`' cascade is this rung's other multi-row, outbox-managed action
(README's split-by-blast-radius rule — `RenameTag` is single-row and stays
on the framework default).

- [ ] **Step 1: Write the failing test**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/models/bookmark_model.hpp"
#include "bookmarks/models/tag_model.hpp"
#include "testkit/db_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <morph/session/session.hpp>

using morph::ladder::testkit::DbFixture;

namespace {
class ScopedPrincipal {
  public:
    explicit ScopedPrincipal(std::string principal) : _ctx{.principal = std::move(principal)}, _scope{_ctx} {}

  private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};
}  // namespace

TEST_CASE("RenameTag renames a tag owned by the caller", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel bookmarkModel;
    bookmarks::TagModel tagModel;
    const ScopedPrincipal alice{"alice"};

    const auto bookmarkId = bookmarkModel.execute(bookmarks::CreateBookmark{.url = "https://one.example", .tags = {"old"}}).id;
    const auto tags = tagModel.execute(bookmarks::ListTags{}).tags;
    REQUIRE(tags.size() == 1);
    const auto tagId = tags.front().id;

    tagModel.execute(bookmarks::RenameTag{.id = tagId, .name = "new"});
    const auto renamed = tagModel.execute(bookmarks::ListTags{}).tags;
    REQUIRE(renamed.size() == 1);
    CHECK(renamed.front().name == "new");
    CHECK(bookmarkModel.execute(bookmarks::GetBookmark{.id = bookmarkId}).tags == std::vector<std::string>{"new"});
}

TEST_CASE("RenameTag against another principal's tag is Forbidden", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel bookmarkModel;
    bookmarks::TagModel tagModel;
    bookmarks::TagId aliceTagId;
    {
        const ScopedPrincipal alice{"alice"};
        bookmarkModel.execute(bookmarks::CreateBookmark{.url = "https://one.example", .tags = {"mine"}});
        aliceTagId = tagModel.execute(bookmarks::ListTags{}).tags.front().id;
    }
    const ScopedPrincipal mallory{"mallory"};
    REQUIRE_THROWS_AS(tagModel.execute(bookmarks::RenameTag{.id = aliceTagId, .name = "stolen"}),
                      bookmarks::Forbidden);
}

TEST_CASE("RenameTag colliding with an existing tag name is a Conflict", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel bookmarkModel;
    bookmarks::TagModel tagModel;
    const ScopedPrincipal alice{"alice"};
    bookmarkModel.execute(bookmarks::CreateBookmark{.url = "https://one.example", .tags = {"a", "b"}});
    const auto tags = tagModel.execute(bookmarks::ListTags{}).tags;
    const auto tagA = std::ranges::find_if(tags, [](auto& t) { return t.name == "a"; })->id;
    REQUIRE_THROWS_AS(tagModel.execute(bookmarks::RenameTag{.id = tagA, .name = "b"}), bookmarks::Conflict);
}

TEST_CASE("MergeTags reassigns every bookmark from source to target, dedups, and deletes source",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel bookmarkModel;
    bookmarks::TagModel tagModel;
    const ScopedPrincipal alice{"alice"};

    const auto id1 = bookmarkModel.execute(bookmarks::CreateBookmark{.url = "https://one.example", .tags = {"cpp"}}).id;
    const auto id2 =
        bookmarkModel.execute(bookmarks::CreateBookmark{.url = "https://two.example", .tags = {"cpp", "c++"}}).id;
    const auto tags = tagModel.execute(bookmarks::ListTags{}).tags;
    const auto cppId = std::ranges::find_if(tags, [](auto& t) { return t.name == "cpp"; })->id;
    const auto cxxId = std::ranges::find_if(tags, [](auto& t) { return t.name == "c++"; })->id;

    tagModel.execute(bookmarks::MergeTags{.sourceId = cppId, .targetId = cxxId});

    CHECK(bookmarkModel.execute(bookmarks::GetBookmark{.id = id1}).tags == std::vector<std::string>{"c++"});
    auto tagsOfId2 = bookmarkModel.execute(bookmarks::GetBookmark{.id = id2}).tags;
    CHECK(tagsOfId2.size() == 1);  // "cpp" and "c++" merged into one, not duplicated
    CHECK(tagsOfId2.front() == "c++");
    const auto remaining = tagModel.execute(bookmarks::ListTags{}).tags;
    CHECK(remaining.size() == 1);  // "cpp" is gone
}

TEST_CASE("MergeTags writes exactly one outbox row", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel bookmarkModel;
    bookmarks::TagModel tagModel;
    const ScopedPrincipal alice{"alice"};
    bookmarkModel.execute(bookmarks::CreateBookmark{.url = "https://one.example", .tags = {"a", "b"}});
    const auto tags = tagModel.execute(bookmarks::ListTags{}).tags;
    tagModel.execute(bookmarks::MergeTags{.sourceId = tags[0].id, .targetId = tags[1].id});

    Lightweight::DataMapper mapper;
    auto rows = mapper.Query<bookmarks::db::BookmarkOutboxRecord>().All();
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().actionType.Value() == "MergeTags");
}
```

- [ ] **Step 2: Run to verify it fails to compile.**

- [ ] **Step 3: Write `examples/bookmarks/include/bookmarks/models/tag_model.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

#include "bookmarks/core/errors.hpp"
#include "bookmarks/db/db_model.hpp"
#include "bookmarks/dto/tag_dto.hpp"

namespace bookmarks {

/// @brief Rename/merge/list over the `tags` table, scoped to the caller.
///        Registered plain — same rationale as `BookmarkModel`.
class TagModel : private db::WithMapper {
public:
    Ack execute(const RenameTag& action);
    Ack execute(const MergeTags& action);
    ListTagsResult execute(const ListTags& action);
};

}  // namespace bookmarks

BRIDGE_REGISTER_MODEL(bookmarks::TagModel, "TagModel")
BRIDGE_REGISTER_ACTION(bookmarks::TagModel, bookmarks::RenameTag, "RenameTag")
// MergeTags is outbox-managed (this task) -- Loggable::No so the framework
// auto-append never double-logs alongside the model's own outbox write.
BRIDGE_REGISTER_ACTION(bookmarks::TagModel, bookmarks::MergeTags, "MergeTags", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(bookmarks::TagModel, bookmarks::ListTags, "ListTags", ::morph::model::Loggable::No)
```

- [ ] **Step 4: Write `examples/bookmarks/src/models/tag_model.cpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/models/tag_model.hpp"

#include "bookmarks/db/bookmark_tag_entity.hpp"
#include "bookmarks/db/outbox_entity.hpp"
#include "bookmarks/db/tag_entity.hpp"

#include "clock.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlErrorDetection.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <Lightweight/SqlTransaction.hpp>

#include <morph/core/registry.hpp>
#include <morph/session/session.hpp>

#include <cstdint>
#include <string>

namespace bookmarks {

namespace {

[[nodiscard]] std::int64_t nowMs() noexcept {
    return (*::morph::ladder::now().value).value.time_since_epoch().count();
}

[[nodiscard]] const std::string& requireOwner() {
    const auto* ctx = ::morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw Forbidden{"no authenticated principal"};
    }
    return ctx->principal;
}

[[nodiscard]] db::TagRecord loadOwnedTag(::Lightweight::DataMapper& mapper, std::uint64_t id, const std::string& owner) {
    auto rows = mapper.Query<db::TagRecord>().Where(::Lightweight::FieldNameOf<&db::TagRecord::id>, "=", id).All();
    if (rows.empty()) {
        throw NotFound{"no such tag"};
    }
    if (rows.front().ownerPrincipal.Value() != owner) {
        throw Forbidden{"tag belongs to a different principal"};
    }
    return rows.front();
}

}  // namespace

Ack TagModel::execute(const RenameTag& action) {
    if (!action.validate()) {
        throw ValidationError{"RenameTag: id and a non-empty, bounded name are required"};
    }
    const auto& owner = requireOwner();
    auto rec = loadOwnedTag(mapper(), static_cast<std::uint64_t>(*action.id), owner);
    rec.name = action.name;
    try {
        mapper().Update(rec);
    } catch (const ::Lightweight::SqlException& error) {
        if (::Lightweight::IsUniqueConstraintViolation(error.info(), mapper().Connection().ServerType())) {
            throw Conflict{"RenameTag: a tag named '" + action.name + "' already exists"};
        }
        throw;
    }
    return Ack{};
}

Ack TagModel::execute(const MergeTags& action) {
    if (!action.validate()) {
        throw ValidationError{"MergeTags: sourceId and a distinct targetId are required"};
    }
    const auto& owner = requireOwner();
    const auto sourceId = static_cast<std::uint64_t>(*action.sourceId);
    const auto targetId = static_cast<std::uint64_t>(*action.targetId);
    (void) loadOwnedTag(mapper(), sourceId, owner);
    (void) loadOwnedTag(mapper(), targetId, owner);

    ::Lightweight::SqlTransaction transaction{mapper().Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};

    auto sourceRows = mapper()
                          .Query<db::BookmarkTagRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::BookmarkTagRecord::tag>, "=", sourceId)
                          .All();
    for (const auto& row : sourceRows) {
        const auto bookmarkId = row.bookmark.Value();
        auto clash = mapper()
                         .Query<db::BookmarkTagRecord>()
                         .Where(::Lightweight::FieldNameOf<&db::BookmarkTagRecord::bookmark>, "=", bookmarkId)
                         .Where(::Lightweight::FieldNameOf<&db::BookmarkTagRecord::tag>, "=", targetId)
                         .All();
        if (!clash.empty()) {
            // This bookmark already carries the target tag -- reassigning
            // would violate the (bookmark_id, tag_id) unique index. Drop
            // the source association instead; the target one already
            // covers it, so nothing is lost.
            ::Lightweight::SqlStatement stmt{mapper().Connection()};
            stmt.Prepare("DELETE FROM bookmark_tags WHERE bookmark_id = ? AND tag_id = ?");
            (void) stmt.Execute(bookmarkId, sourceId);
        } else {
            auto rec = row;
            rec.tag = targetId;
            mapper().Update(rec);
        }
    }
    {
        ::Lightweight::SqlStatement stmt{mapper().Connection()};
        stmt.Prepare("DELETE FROM tags WHERE id = ?");
        (void) stmt.Execute(sourceId);
    }

    Ack result{};
    db::BookmarkOutboxRecord entry;
    entry.modelType = "TagModel";
    entry.entityKey = owner;
    entry.actionType = "MergeTags";
    entry.payload = ::morph::model::ActionTraits<MergeTags>::toJson(action);
    entry.result = ::morph::model::ActionTraits<MergeTags>::resultToJson(result);
    entry.principal = owner;
    entry.timestampMs = nowMs();
    entry.idempotencyKey = owner + "-mergetags-" + std::to_string(nowMs());
    mapper().Create(entry);

    transaction.Commit();
    return result;
}

ListTagsResult TagModel::execute(const ListTags&) {
    const auto& owner = requireOwner();
    auto rows =
        mapper().Query<db::TagRecord>().Where(::Lightweight::FieldNameOf<&db::TagRecord::ownerPrincipal>, "=", owner).All();

    ListTagsResult result;
    for (const auto& rec : rows) {
        const auto count = mapper()
                                .Query<db::BookmarkTagRecord>()
                                .Where(::Lightweight::FieldNameOf<&db::BookmarkTagRecord::tag>, "=", rec.id.Value())
                                .All()
                                .size();
        TagSummary summary;
        summary.id = TagId{static_cast<std::int64_t>(rec.id.Value())};
        summary.name = rec.name.Value();
        summary.bookmarkCount = Count::fromDouble(static_cast<double>(count));
        result.tags.push_back(std::move(summary));
    }
    return result;
}

}  // namespace bookmarks
```

- [ ] **Step 5: Run to verify it passes.**

- [ ] **Step 6: Commit**

```bash
git add examples/bookmarks/include/bookmarks/models/tag_model.hpp \
        examples/bookmarks/src/models/tag_model.cpp \
        examples/bookmarks/tests/test_tag_model.cpp
git commit -m "bookmarks: add TagModel (RenameTag, outbox-managed MergeTags, ListTags)"
```

---

## Task 10: `SharedFeedModel`

**Files:**
- Create: `examples/bookmarks/include/bookmarks/models/shared_feed_model.hpp`
- Create: `examples/bookmarks/src/models/shared_feed_model.cpp`
- Test: `examples/bookmarks/tests/test_shared_feed_model.cpp`

**Interfaces:** Consumes Tasks 2, 3, 4, 5. Produces `bookmarks::SharedFeedModel`.

Registered plain, same authorizer, same `BookmarksAuthorizer` — **not**
`AllowShared` (this plan's "Corrections to the README" explains why: no
per-user state to converge on, and `AllowShared`'s `BRIDGE_MODEL_KEY`
machinery buys nothing here). `execute()` still requires *some*
authenticated principal (`requireOwner()`, reused only for its
authentication check — its value is never used to filter the query, since
the whole point of this model is a cross-principal read), so a completely
anonymous local-mode caller is refused exactly as consistently as every
other model in this rung, even though the row-level query itself carries
no ownership filter.

- [ ] **Step 1: Write the failing test**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/models/bookmark_model.hpp"
#include "bookmarks/models/shared_feed_model.hpp"
#include "testkit/db_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <morph/session/session.hpp>

using morph::ladder::testkit::DbFixture;

namespace {
class ScopedPrincipal {
  public:
    explicit ScopedPrincipal(std::string principal) : _ctx{.principal = std::move(principal)}, _scope{_ctx} {}

  private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};
}  // namespace

TEST_CASE("ListSharedFeed returns every user's shared bookmarks, never a private one",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel bookmarkModel;
    bookmarks::SharedFeedModel feedModel;
    {
        const ScopedPrincipal alice{"alice"};
        bookmarkModel.execute(bookmarks::CreateBookmark{.url = "https://alice-private.example"});
        bookmarkModel.execute(
            bookmarks::CreateBookmark{.url = "https://alice-shared.example", .visibility = bookmarks::Visibility::Shared});
    }
    const ScopedPrincipal bob{"bob"};
    bookmarkModel.execute(
        bookmarks::CreateBookmark{.url = "https://bob-shared.example", .visibility = bookmarks::Visibility::Shared});

    const auto feed = feedModel.execute(bookmarks::ListSharedFeed{});
    REQUIRE(feed.bookmarks.size() == 2);
    for (const auto& row : feed.bookmarks) {
        CHECK((row.url == "https://alice-shared.example" || row.url == "https://bob-shared.example"));
    }
}

TEST_CASE("ListSharedFeed excludes an archived-but-shared bookmark", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel bookmarkModel;
    bookmarks::SharedFeedModel feedModel;
    const ScopedPrincipal alice{"alice"};
    const auto id =
        bookmarkModel.execute(bookmarks::CreateBookmark{.url = "https://one.example", .visibility = bookmarks::Visibility::Shared}).id;
    bookmarkModel.execute(bookmarks::ArchiveBookmark{.id = id});
    CHECK(feedModel.execute(bookmarks::ListSharedFeed{}).bookmarks.empty());
}

TEST_CASE("ListSharedFeed with no session at all is Forbidden", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::SharedFeedModel feedModel;
    REQUIRE_THROWS_AS(feedModel.execute(bookmarks::ListSharedFeed{}), bookmarks::Forbidden);
}
```

- [ ] **Step 2: Run to verify it fails to compile.**

- [ ] **Step 3: Write `examples/bookmarks/include/bookmarks/models/shared_feed_model.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

#include "bookmarks/core/errors.hpp"
#include "bookmarks/db/db_model.hpp"
#include "bookmarks/dto/shared_feed_dto.hpp"

namespace bookmarks {

/// @brief The one cross-principal read in this rung: every `Shared`,
///        non-archived bookmark, from every owner. Registered plain — see
///        this task's own header comment for why `AllowShared` is not used.
class SharedFeedModel : private db::WithMapper {
public:
    ListSharedFeedResult execute(const ListSharedFeed& action);
};

}  // namespace bookmarks

BRIDGE_REGISTER_MODEL(bookmarks::SharedFeedModel, "SharedFeedModel")
BRIDGE_REGISTER_ACTION(bookmarks::SharedFeedModel, bookmarks::ListSharedFeed, "ListSharedFeed",
                       ::morph::model::Loggable::No)
```

- [ ] **Step 4: Write `examples/bookmarks/src/models/shared_feed_model.cpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/models/shared_feed_model.hpp"

#include "bookmarks/db/bookmark_entity.hpp"
#include "bookmarks/db/bookmark_tag_entity.hpp"
#include "bookmarks/db/tag_entity.hpp"

#include "clock.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>

#include <morph/session/session.hpp>

#include <cstdint>
#include <string>

namespace bookmarks {

namespace {

[[nodiscard]] ::morph::time::Timestamp fromEpochMs(std::int64_t epochMs) noexcept {
    return ::morph::time::Timestamp{::morph::time::DateTime{
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{epochMs}}}};
}

/// @brief Requires *some* authenticated principal, but never filters on it
///        — this model's whole point is a cross-principal read. See this
///        task's own doc comment for why the check still exists.
void requireAnyPrincipal() {
    const auto* ctx = ::morph::session::current();
    if (ctx == nullptr || ctx->principal.empty()) {
        throw Forbidden{"no authenticated principal"};
    }
}

}  // namespace

ListSharedFeedResult SharedFeedModel::execute(const ListSharedFeed& action) {
    requireAnyPrincipal();
    auto query = mapper().Query<db::BookmarkRecord>();
    (void) query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::isShared>, "=", true);
    (void) query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::isArchived>, "=", false);
    if (action.cursor.hasValue()) {
        (void) query.Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::id>, "<",
                           static_cast<std::uint64_t>(*action.cursor));
    }
    constexpr std::size_t kPageSize = 20;
    auto rows = query.OrderBy(::Lightweight::FieldNameOf<&db::BookmarkRecord::id>, ::Lightweight::SqlResultOrdering::DESCENDING)
                    .First(kPageSize + 1);
    const bool hasMore = rows.size() > kPageSize;
    if (hasMore) {
        rows.resize(kPageSize);
    }

    ListSharedFeedResult result;
    for (const auto& rec : rows) {
        auto junctionRows = mapper()
                                .Query<db::BookmarkTagRecord>()
                                .Where(::Lightweight::FieldNameOf<&db::BookmarkTagRecord::bookmark>, "=", rec.id.Value())
                                .All();
        std::vector<std::string> tags;
        for (const auto& jrow : junctionRows) {
            auto tagRows =
                mapper().Query<db::TagRecord>().Where(::Lightweight::FieldNameOf<&db::TagRecord::id>, "=", jrow.tag.Value()).All();
            if (!tagRows.empty()) {
                tags.push_back(tagRows.front().name.Value());
            }
        }
        BookmarkSummary summary;
        summary.id = BookmarkId{static_cast<std::int64_t>(rec.id.Value())};
        summary.url = rec.url.Value();
        summary.title = rec.title.Value();
        summary.tags = std::move(tags);
        summary.createdAt = fromEpochMs(rec.createdAtMs.Value());
        summary.updatedAt = fromEpochMs(rec.updatedAtMs.Value());
        summary.readState = rec.isUnread.Value() ? ReadState::Unread : ReadState::Read;
        summary.archiveState = ArchiveState::Active;  // the query already excludes archived rows
        summary.visibility = Visibility::Shared;      // the query already excludes non-shared rows
        result.bookmarks.push_back(std::move(summary));
    }
    if (hasMore && !result.bookmarks.empty()) {
        result.nextCursor = Cursor{static_cast<std::int64_t>(rows.back().id.Value())};
    }
    return result;
}

}  // namespace bookmarks
```

- [ ] **Step 5: Run to verify it passes.**

- [ ] **Step 6: Commit**

```bash
git add examples/bookmarks/include/bookmarks/models/shared_feed_model.hpp \
        examples/bookmarks/src/models/shared_feed_model.cpp \
        examples/bookmarks/tests/test_shared_feed_model.cpp
git commit -m "bookmarks: add SharedFeedModel"
```

---

## Task 11: `BookmarkModel` — `ImportBookmarks`/`ExportBookmarks`

**Files:**
- Create: `examples/bookmarks/include/bookmarks/import/netscape_bookmarks.hpp`
- Create: `examples/bookmarks/src/import/netscape_bookmarks.cpp`
- Modify: `examples/bookmarks/src/models/bookmark_model.cpp` (append two
  `execute()` bodies; header already declares both, per this plan's edit to
  Task 6)
- Modify: `examples/bookmarks/tests/test_bookmark_model.cpp`

**Interfaces:** Produces `bookmarks::import::parseNetscapeChunk(std::string_view)
-> std::vector<bookmarks::import::ParsedEntry>` (`ParsedEntry{url, title}`,
plain internal structs — not wire DTOs, so ordinary `std::string` fields are
fine here, rule 3 governs only action/result fields) and
`bookmarks::import::escapeHtml(std::string_view) -> std::string`. A
**hand-rolled parser, deliberately minimal** — this rung's own written
justification (`IMPLEMENTATION.md` rule 2's custom-element bar applies by
analogy: morph ships no HTML-parsing facility and none is warranted for one
demo import feature; a hand-rolled Netscape-format scanner is squarely
app-layer, not a framework gap to file).

- [ ] **Step 1: Write the failing test**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/import/netscape_bookmarks.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("parseNetscapeChunk extracts url and title from <A HREF> entries",
          "[bookmarks][import]") {
    const std::string chunk = R"(<DL><p>
    <DT><A HREF="https://example.com">Example</A>
    <DT><A HREF="https://second.example">Second &amp; Site</A>
</DL><p>)";
    const auto entries = bookmarks::import::parseNetscapeChunk(chunk);
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].url == "https://example.com");
    CHECK(entries[0].title == "Example");
    CHECK(entries[1].url == "https://second.example");
    CHECK(entries[1].title == "Second & Site");  // entity-decoded
}

TEST_CASE("parseNetscapeChunk skips a malformed <A> with no href", "[bookmarks][import]") {
    const std::string chunk = R"(<DT><A>No href here</A>
<DT><A HREF="https://good.example">Good</A>)";
    const auto entries = bookmarks::import::parseNetscapeChunk(chunk);
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].url.empty());  // caller counts this as skipped
    CHECK(entries[1].url == "https://good.example");
}

TEST_CASE("escapeHtml escapes the five predefined XML entities", "[bookmarks][import]") {
    CHECK(bookmarks::import::escapeHtml("a & b < c > d \"e\" 'f'") ==
          "a &amp; b &lt; c &gt; d &quot;e&quot; &#39;f&#39;");
}
```

- [ ] **Step 2: Run to verify it fails.**

- [ ] **Step 3: Write `examples/bookmarks/include/bookmarks/import/netscape_bookmarks.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace bookmarks::import {

/// @brief One parsed `<A HREF="...">title</A>` entry. `url` empty means
///        "malformed, skip" — the caller (`BookmarkModel::execute(const
///        ImportBookmarks&)`) counts these toward `skipped`, not `imported`.
struct ParsedEntry {
    std::string url;
    std::string title;
};

/// @brief Extracts every `<A HREF="...">...</A>` entry from one Netscape
///        Bookmark File chunk. Deliberately minimal: recognizes `HREF`
///        case-insensitively, decodes the five predefined XML entities in
///        the title text, and tolerates (by skipping) an `<A>` with no
///        `HREF` attribute or an unterminated tag. Anything this rung's own
///        `ExportBookmarks` never produces (nested tags inside the title,
///        `HREF` values containing an escaped quote) is out of scope by
///        design, not an oversight — see this task's own header comment.
/// @param chunk Raw HTML/text to scan.
/// @return Every entry found, in document order.
[[nodiscard]] std::vector<ParsedEntry> parseNetscapeChunk(std::string_view chunk);

/// @brief Escapes `&`, `<`, `>`, `"`, and `'` for safe inclusion in
///        generated Netscape Bookmark File output.
/// @param text Raw text to escape.
/// @return The escaped text.
[[nodiscard]] std::string escapeHtml(std::string_view text);

}  // namespace bookmarks::import
```

- [ ] **Step 4: Write `examples/bookmarks/src/import/netscape_bookmarks.cpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/import/netscape_bookmarks.hpp"

#include <cctype>

namespace bookmarks::import {

namespace {

[[nodiscard]] std::string decodeEntities(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '&') {
            if (text.substr(i, 5) == "&amp;") {
                out += '&';
                i += 5;
                continue;
            }
            if (text.substr(i, 4) == "&lt;") {
                out += '<';
                i += 4;
                continue;
            }
            if (text.substr(i, 4) == "&gt;") {
                out += '>';
                i += 4;
                continue;
            }
            if (text.substr(i, 6) == "&quot;") {
                out += '"';
                i += 6;
                continue;
            }
            if (text.substr(i, 6) == "&#39;;" || text.substr(i, 5) == "&#39;") {
                out += '\'';
                i += 5;
                continue;
            }
        }
        out += text[i];
        ++i;
    }
    return out;
}

/// @brief Case-insensitive substring search for @p needle in @p haystack,
///        starting at @p from.
[[nodiscard]] std::size_t findCaseInsensitive(std::string_view haystack, std::string_view needle, std::size_t from) {
    if (needle.empty() || needle.size() > haystack.size()) {
        return std::string_view::npos;
    }
    for (std::size_t i = from; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                std::tolower(static_cast<unsigned char>(needle[j]))) {
                match = false;
                break;
            }
        }
        if (match) {
            return i;
        }
    }
    return std::string_view::npos;
}

}  // namespace

std::vector<ParsedEntry> parseNetscapeChunk(std::string_view chunk) {
    std::vector<ParsedEntry> entries;
    std::size_t pos = 0;
    while (true) {
        const auto tagStart = findCaseInsensitive(chunk, "<a", pos);
        if (tagStart == std::string_view::npos) {
            break;
        }
        const auto tagEnd = chunk.find('>', tagStart);
        if (tagEnd == std::string_view::npos) {
            break;  // unterminated tag -- nothing more to parse in this chunk
        }
        const auto closeStart = findCaseInsensitive(chunk, "</a>", tagEnd);
        if (closeStart == std::string_view::npos) {
            break;  // unterminated element
        }

        const std::string_view attrs = chunk.substr(tagStart, tagEnd - tagStart);
        ParsedEntry entry;
        const auto hrefPos = findCaseInsensitive(attrs, "href=", 0);
        if (hrefPos != std::string_view::npos) {
            auto valueStart = hrefPos + 5;
            if (valueStart < attrs.size() && attrs[valueStart] == '"') {
                const auto valueEnd = attrs.find('"', valueStart + 1);
                if (valueEnd != std::string_view::npos) {
                    entry.url = std::string{attrs.substr(valueStart + 1, valueEnd - valueStart - 1)};
                }
            }
        }
        entry.title = decodeEntities(chunk.substr(tagEnd + 1, closeStart - tagEnd - 1));
        entries.push_back(std::move(entry));

        pos = closeStart + 4;
    }
    return entries;
}

std::string escapeHtml(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += ch;
        }
    }
    return out;
}

}  // namespace bookmarks::import
```

- [ ] **Step 5: Run to verify the parser tests pass, then write the failing model-level tests (appended to `test_bookmark_model.cpp`)**

```cpp
TEST_CASE("ImportBookmarks stores every well-formed entry in one chunk", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};

    bookmarks::ImportBookmarks action;
    action.chunk = R"(<DT><A HREF="https://one.example">One</A>
<DT><A HREF="https://two.example">Two</A>
<DT><A>No href</A>)";
    action.opId = bookmarks::ImportOpId{"chunk-1"};
    const auto result = model.execute(action);
    CHECK(morph::math::floor(*result.imported) == 2);
    CHECK(morph::math::floor(*result.skipped) == 1);

    const auto page = model.execute(bookmarks::ListBookmarks{});
    CHECK(page.bookmarks.size() == 2);
}

TEST_CASE("ImportBookmarks is idempotent on a retried opId", "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};

    bookmarks::ImportBookmarks action;
    action.chunk = R"(<DT><A HREF="https://one.example">One</A>)";
    action.opId = bookmarks::ImportOpId{"chunk-retry"};
    model.execute(action);
    model.execute(action);  // simulates a retry after a dropped connection

    const auto page = model.execute(bookmarks::ListBookmarks{});
    CHECK(page.bookmarks.size() == 1);  // not duplicated
}

TEST_CASE("ExportBookmarks emits every owned bookmark as a Netscape file, and it re-imports",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    {
        const ScopedPrincipal alice{"alice"};
        model.execute(bookmarks::CreateBookmark{.url = "https://one.example", .title = "One"});
        model.execute(bookmarks::CreateBookmark{.url = "https://two.example", .title = "Two"});
    }
    std::string exported;
    {
        const ScopedPrincipal alice{"alice"};
        exported = model.execute(bookmarks::ExportBookmarks{}).html;
    }
    CHECK(exported.find("https://one.example") != std::string::npos);
    CHECK(exported.find("https://two.example") != std::string::npos);

    const ScopedPrincipal bob{"bob"};
    bookmarks::ImportBookmarks reimport;
    reimport.chunk = exported;
    reimport.opId = bookmarks::ImportOpId{"reimport-1"};
    const auto result = model.execute(reimport);
    CHECK(morph::math::floor(*result.imported) == 2);
}
```

- [ ] **Step 6: Append to `examples/bookmarks/src/models/bookmark_model.cpp`**

```cpp
// (near the top)
#include "bookmarks/db/imported_op_entity.hpp"
#include "bookmarks/import/netscape_bookmarks.hpp"
```

```cpp
ImportBookmarksResult BookmarkModel::execute(const ImportBookmarks& action) {
    if (!action.validate()) {
        throw ValidationError{"ImportBookmarks: a non-empty, bounded chunk and opId are required"};
    }
    const auto& owner = requireOwner();
    const auto& opIdStr = *action.opId;

    auto existingOp = mapper()
                          .Query<db::ImportedOpRecord>()
                          .Where(::Lightweight::FieldNameOf<&db::ImportedOpRecord::ownerPrincipal>, "=", owner)
                          .Where(::Lightweight::FieldNameOf<&db::ImportedOpRecord::opId>, "=", opIdStr)
                          .All();
    if (!existingOp.empty()) {
        // Already applied -- a retried chunk after a dropped connection is
        // a safe no-op, per this task's idempotency requirement. Reports
        // zero: the caller's own first, successful attempt already learned
        // the real counts, and a retry's purpose is confirming "did this
        // land," not re-reporting them.
        return ImportBookmarksResult{.imported = Count::fromDouble(0.0), .skipped = Count::fromDouble(0.0)};
    }

    const auto entries = ::bookmarks::import::parseNetscapeChunk(action.chunk);
    std::size_t imported = 0;
    std::size_t skipped = 0;

    ::Lightweight::SqlTransaction transaction{mapper().Connection(), ::Lightweight::SqlTransactionMode::ROLLBACK};
    for (const auto& entry : entries) {
        if (entry.url.empty()) {
            ++skipped;
            continue;
        }
        db::BookmarkRecord rec;
        rec.ownerPrincipal = owner;
        rec.url = entry.url;
        rec.title = entry.title;
        const auto now = nowMs();
        rec.createdAtMs = now;
        rec.updatedAtMs = now;
        mapper().Create(rec);
        ++imported;
    }
    db::ImportedOpRecord op;
    op.ownerPrincipal = owner;
    op.opId = opIdStr;
    op.appliedAtMs = nowMs();
    mapper().Create(op);
    transaction.Commit();

    return ImportBookmarksResult{.imported = Count::fromDouble(static_cast<double>(imported)),
                                 .skipped = Count::fromDouble(static_cast<double>(skipped))};
}

ExportBookmarksResult BookmarkModel::execute(const ExportBookmarks&) {
    const auto& owner = requireOwner();
    auto rows = mapper()
                    .Query<db::BookmarkRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::ownerPrincipal>, "=", owner)
                    .All();
    std::string html = "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n<TITLE>Bookmarks</TITLE>\n<H1>Bookmarks</H1>\n<DL><p>\n";
    for (const auto& rec : rows) {
        html += "    <DT><A HREF=\"" + ::bookmarks::import::escapeHtml(rec.url.Value()) + "\">" +
               ::bookmarks::import::escapeHtml(rec.title.Value()) + "</A>\n";
    }
    html += "</DL><p>\n";
    return ExportBookmarksResult{.html = std::move(html)};
}
```

- [ ] **Step 7: Run to verify it passes.**

- [ ] **Step 8: Commit**

```bash
git add examples/bookmarks/include/bookmarks/import/ examples/bookmarks/src/import/ \
        examples/bookmarks/src/models/bookmark_model.cpp examples/bookmarks/tests/test_bookmark_model.cpp
git commit -m "bookmarks: add Netscape import/export"
```

---

## Task 12: `App` — server bootstrap, metadata-fetch worker, outbox relay

**Files:**
- Create: `examples/bookmarks/include/bookmarks/app/metadata_fetcher.hpp`
- Create: `examples/bookmarks/include/bookmarks/dto/auth_dto.hpp`
- Create: `examples/bookmarks/src/dto/auth_dto.cpp`
- Create: `examples/bookmarks/include/bookmarks/models/auth_model.hpp`
- Create: `examples/bookmarks/src/models/auth_model.cpp`
- Create: `examples/bookmarks/include/bookmarks/app/app.hpp`
- Create: `examples/bookmarks/src/app/app.cpp`
- Test: `examples/bookmarks/tests/test_app.cpp`

**Interfaces:**
- Produces: `bookmarks::app::IBookmarkMetadataFetcher` (injectable, one
  `fetch(url) -> FetchedMetadata{title, faviconPath}` method),
  `bookmarks::app::NullMetadataFetcher` (deterministic, no real network —
  see below), `bookmarks::AuthToken`, `bookmarks::Login`/
  `bookmarks::LoginResult`, `bookmarks::AuthModel` (mints a signed token —
  the *only* action `authorizeRegister` lets an unauthenticated caller
  reach, Task 1's exemption), `bookmarks::app::App` (owns the
  `RemoteServer` + `BookmarksAuthorizer`, installs the process-global
  `TokenIssuer` (`auth::setTokenIssuer`) `AuthModel` reads, and owns the
  metadata-fetch worker and the outbox relay). Consumed by Task 13's server
  binary and Task 15/16's tests.

**Why no real HTTP client**: morph ships no HTTP client, and building one
is squarely out of this rung's scope — the framework subsystem under
stress here is the **background-job dispatch pattern** (an internal client
routing through the full server pipeline, README's resolved design), not
network I/O. `IBookmarkMetadataFetcher` is the pluggable extension point a
real deployment would implement; this rung ships only
`NullMetadataFetcher`, which performs no I/O and returns an empty
`FetchedMetadata` — deterministic and instant, so tests never depend on
timing or a real network.

- [ ] **Step 1: Write `examples/bookmarks/include/bookmarks/app/metadata_fetcher.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace bookmarks::app {

/// @brief What a metadata fetch produces. Both fields empty is a legitimate
///        "found nothing" result, not a distinguished failure — mirrors
///        `RecordMetadata`'s own "empty = not found" DTO convention.
struct FetchedMetadata {
    std::string title;
    std::string faviconPath;
};

/// @brief Pluggable page-metadata fetcher. See this task's own header
///        comment for why morph/this rung ships no real HTTP implementation.
class IBookmarkMetadataFetcher {
public:
    virtual ~IBookmarkMetadataFetcher() = default;

    /// @brief Fetches title/favicon metadata for @p url.
    /// @param url The bookmark's url.
    /// @return The fetched metadata, or an empty one if nothing was found.
    [[nodiscard]] virtual FetchedMetadata fetch(const std::string& url) = 0;
};

/// @brief The shipped default: performs no I/O, always returns an empty
///        result. Deterministic and instant, for tests and for a
///        deployment that has not yet plugged in a real fetcher.
class NullMetadataFetcher : public IBookmarkMetadataFetcher {
public:
    [[nodiscard]] FetchedMetadata fetch(const std::string&) override { return {}; }
};

}  // namespace bookmarks::app
```

- [ ] **Step 2: Write `examples/bookmarks/include/bookmarks/dto/auth_dto.hpp`**

Every model-bearing action in this rung needs a signed token before it can
do anything (`BookmarksAuthorizer::authorizeRegister`, Task 1) — `Login` is
how a caller gets one in the first place, so it is deliberately the *one*
action in this rung `authorizeRegister` lets an unauthenticated caller
reach (Task 1's `modelType == "AuthModel"` exemption).

**Dev-mode login, stated plainly, not smoothed over**: `Login` takes a bare
`username` with no password or other credential — this rung ships no user
registry, no password hashing, no account-recovery flow, none of which
`examples/bookmarks/README.md` asks for (its DoD is "two users... with
isolated collections," not a production auth system). What *is* real and
load-bearing: the **token** `Login` mints is a genuine, server-signed
`SigningAuthorizer`-verified credential — nothing about `EditBookmark`,
`GetBookmark`, or any other action trusts a client's claimed identity
un-verified. The trust boundary this rung actually stress-tests
(`authenticate` → `authorize`/`authorizeInstance`/`authorizeRegister` →
`session::current()->principal` inside a model) is exactly as real after
login as a production deployment's would be; only the *login step itself*
is a stand-in for a real credential check, which a real deployment would
replace with one (password verification, OAuth, etc.) without touching
anything downstream of `Login` at all — the seam is exactly at
`AuthModel::execute(const Login&)`'s body, and nowhere else.

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <compare>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <utility>

namespace bookmarks {

/// @brief Opaque bearer-token newtype (`IMPLEMENTATION.md` rule 3's
///        protocol-scalars row: capability/confirmation tokens get a named
///        opaque wrapper, never a loose `std::string`). Same
///        `hasValue()`-capable shape as `PasteId`/`BookmarkId` — see
///        either's doc comment for the `fromOptional` factory rationale.
///        Named `AuthToken`, not `SessionToken`, to avoid colliding with
///        `morph::session::SessionToken` (an unrelated type this DTO's own
///        model wraps, not reuses).
struct AuthToken {
    std::optional<std::string> value;

    constexpr AuthToken() noexcept = default;
    explicit AuthToken(std::string token) noexcept : value{std::move(token)} {}

    [[nodiscard]] static AuthToken fromOptional(std::optional<std::string> payload) noexcept {
        AuthToken result;
        result.value = std::move(payload);
        return result;
    }

    [[nodiscard]] bool hasValue() const noexcept { return value.has_value(); }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    [[nodiscard]] const std::string& operator*() const noexcept { return *value; }
    [[nodiscard]] auto operator<=>(const AuthToken&) const noexcept = default;
};

/// @brief Dev-mode login: no password. See this task's own step comment
///        for exactly what that does and does not mean for this rung's
///        security posture.
struct Login {
    std::string username;

    /// @brief Reuses `auth::isValidPrincipal` — a username this rejects
    ///        could never be used as an `ownerPrincipal` anywhere else in
    ///        this rung anyway (Task 1's own charset rationale, including
    ///        finding 026's defense-in-depth argument).
    [[nodiscard]] bool validate() const noexcept;
};

struct LoginResult {
    AuthToken token;
    std::string principal;  // echoes the verified username back for display
};

}  // namespace bookmarks

template <>
struct glz::meta<bookmarks::AuthToken> {
    static constexpr auto value = &bookmarks::AuthToken::value;
    static constexpr std::string_view name = "AuthToken";
};
```

`Login::validate()` is declared, not defined inline, because it needs
`auth::isValidPrincipal` (`bookmarks/auth/bookmarks_authorizer.hpp`) —
including that header here would pull `morph/session/session_auth.hpp`
(and, transitively, its whole HMAC/base64 implementation) into every
translation unit that only wants the DTO shape. Define it in a small
`.cpp` instead:

```cpp
// examples/bookmarks/src/dto/auth_dto.cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/dto/auth_dto.hpp"

#include "bookmarks/auth/bookmarks_authorizer.hpp"

namespace bookmarks {

bool Login::validate() const noexcept { return auth::isValidPrincipal(username); }

}  // namespace bookmarks
```

- [ ] **Step 3: Write `examples/bookmarks/include/bookmarks/models/auth_model.hpp`/`.cpp`**

```cpp
// examples/bookmarks/include/bookmarks/models/auth_model.hpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

#include "bookmarks/core/errors.hpp"
#include "bookmarks/dto/auth_dto.hpp"

namespace bookmarks {

/// @brief Mints a signed token for whichever `username` the caller claims —
///        see `auth_dto.hpp`'s own doc comment for exactly what "dev-mode
///        login" does and does not mean here. Stateless: no database, no
///        `WithMapper` base, since there is nothing to persist.
class AuthModel {
public:
    LoginResult execute(const Login& action);
};

}  // namespace bookmarks

BRIDGE_REGISTER_MODEL(bookmarks::AuthModel, "AuthModel")
BRIDGE_REGISTER_ACTION(bookmarks::AuthModel, bookmarks::Login, "Login", ::morph::model::Loggable::No)
```

```cpp
// examples/bookmarks/src/models/auth_model.cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/models/auth_model.hpp"

#include "bookmarks/auth/bookmarks_authorizer.hpp"

#include <morph/session/session_auth.hpp>

namespace bookmarks {

LoginResult AuthModel::execute(const Login& action) {
    if (!action.validate()) {
        throw ValidationError{"Login: username must be a valid principal"};
    }
    auto issuer = auth::tokenIssuer();
    if (!issuer) {
        // No App has installed one yet -- e.g. a test that constructs
        // AuthModel without going through App's constructor. A clear,
        // typed failure, not a null-dereference.
        throw ValidationError{"Login: no token issuer installed"};
    }
    const auto token = issuer->issue(::morph::session::SessionToken{
        .principal = action.username,
        .issuedAtMs = 0,
        .expiresAtMs = 4102444800000,  // year 2100 -- this rung sets no shorter session lifetime
        .roles = {},
    });
    return LoginResult{.token = AuthToken{token}, .principal = action.username};
}

}  // namespace bookmarks
```

- [ ] **Step 4: Write the failing test**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/app/app.hpp"
#include "bookmarks/models/bookmark_model.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

#include <catch2/catch_test_macros.hpp>
#include <morph/session/session.hpp>

using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::pumpUntil;

namespace {
class ScopedPrincipal {
  public:
    explicit ScopedPrincipal(std::string principal) : _ctx{.principal = std::move(principal)}, _scope{_ctx} {}

  private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

class StubFetcher : public bookmarks::app::IBookmarkMetadataFetcher {
  public:
    bookmarks::app::FetchedMetadata fetch(const std::string& url) override {
        return {.title = "Fetched: " + url, .faviconPath = ""};
    }
};
}  // namespace

TEST_CASE("App::fetchMetadataOnce records fetched titles for every empty-title bookmark",
          "[bookmarks][app]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    bookmarks::BookmarkId id;
    {
        const ScopedPrincipal alice{"alice"};
        id = model.execute(bookmarks::CreateBookmark{.url = "https://one.example"}).id;  // no title
    }

    bookmarks::app::App app{fixture.actionLogPath(), "test-secret", std::make_shared<StubFetcher>(),
                            std::chrono::hours{1}, std::chrono::hours{1}};
    app.fetchMetadataOnce();
    REQUIRE(pumpUntil([&app] { return !app.fetchInFlight(); }));

    const ScopedPrincipal alice{"alice"};
    CHECK(model.execute(bookmarks::GetBookmark{.id = id}).title == "Fetched: https://one.example");
}

TEST_CASE("App::fetchMetadataOnce leaves an already-titled bookmark untouched", "[bookmarks][app]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    {
        const ScopedPrincipal alice{"alice"};
        model.execute(bookmarks::CreateBookmark{.url = "https://one.example", .title = "Already Set"});
    }
    bookmarks::app::App app{fixture.actionLogPath(), "test-secret", std::make_shared<StubFetcher>(),
                            std::chrono::hours{1}, std::chrono::hours{1}};
    app.fetchMetadataOnce();
    REQUIRE(pumpUntil([&app] { return !app.fetchInFlight(); }));
    const ScopedPrincipal alice{"alice"};
    CHECK(model.execute(bookmarks::GetBookmark{.id = model.execute(bookmarks::ListBookmarks{}).bookmarks.front().id})
              .title == "Already Set");
}

TEST_CASE("App::relayOutboxOnce drains a BulkEdit outbox row into the durable action log",
          "[bookmarks][app]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    const ScopedPrincipal alice{"alice"};
    const auto id = model.execute(bookmarks::CreateBookmark{.url = "https://one.example"}).id;
    bookmarks::BulkEdit edit;
    edit.ids = {id};
    edit.archive = bookmarks::BulkArchiveOp::Archive;
    model.execute(edit);

    Lightweight::DataMapper mapper;
    REQUIRE(mapper.Query<bookmarks::db::BookmarkOutboxRecord>().All().size() == 1);

    bookmarks::app::App app{fixture.actionLogPath(), "test-secret", std::make_shared<bookmarks::app::NullMetadataFetcher>(),
                            std::chrono::hours{1}, std::chrono::hours{1}};
    app.relayOutboxOnce();
    CHECK(mapper.Query<bookmarks::db::BookmarkOutboxRecord>().All().empty());
}

TEST_CASE("AuthModel::execute(Login) mints a token that verifies against the same App's authorizer",
          "[bookmarks][app]") {
    DbFixture fixture;
    bookmarks::app::App app{fixture.actionLogPath(), "login-test-secret"};
    bookmarks::AuthModel authModel;
    const auto result = authModel.execute(bookmarks::Login{.username = "alice"});
    REQUIRE(result.token.hasValue());
    CHECK(result.principal == "alice");

    const bookmarks::auth::BookmarksAuthorizer authz{"login-test-secret"};
    morph::session::Context ctx;
    ctx.token = *result.token;
    const auto principal = authz.authenticate(ctx);
    REQUIRE(principal.has_value());
    CHECK(*principal == "alice");
}

TEST_CASE("AuthModel::execute(Login) throws before any App has installed a TokenIssuer",
          "[bookmarks][app]") {
    bookmarks::AuthModel authModel;
    REQUIRE_THROWS_AS(authModel.execute(bookmarks::Login{.username = "alice"}), bookmarks::ValidationError);
}

TEST_CASE("Login rejects an invalid username via the shared principal charset", "[bookmarks][app]") {
    bookmarks::AuthModel authModel;
    REQUIRE_THROWS_AS(authModel.execute(bookmarks::Login{.username = ""}), bookmarks::ValidationError);
}
```

(Add `#include "bookmarks/models/auth_model.hpp"` to this test file's
includes. The "throws before any App has installed a TokenIssuer" case must
run in a process where no earlier test in the same binary has left an `App`
alive — Catch2 runs `TEST_CASE`s in one process, and `~App()` clears the
global issuer per this task's own `App::~App()`, so as long as every other
`[bookmarks][app]` case constructs its own `App` as a local (destroyed at
scope exit, which every case above already does), this one sees a clean
`nullptr` regardless of run order.)

(`DbFixture::actionLogPath()` — confirm this accessor exists on the shared
testkit fixture during implementation; if it does not, add a one-line
accessor to `examples/common/testkit/db_fixture.hpp` returning a
`std::filesystem::path` next to its existing database-path member, matching
whatever naming convention that file already uses for the database path.)

- [ ] **Step 5: Run to verify it fails to compile.**

- [ ] **Step 6: Write `examples/bookmarks/include/bookmarks/app/app.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "bookmarks/app/metadata_fetcher.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/journal/file_action_log.hpp>
#include <morph/qt/qt_executor.hpp>
#include <morph/session/session_auth.hpp>

#include <QObject>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

namespace bookmarks::app {

/// @brief Owns the server-side pieces every bookmarks deployment shares:
///        the worker pool, the `RemoteServer` with a real
///        `auth::BookmarksAuthorizer` installed, the durable
///        `FileActionLog`, the periodic metadata-fetch worker, and the
///        periodic outbox relay. Mirrors `pastebin::app::App`'s shape —
///        same declaration-order-for-teardown-safety rationale (see that
///        header's own comment), same internal-client pattern for
///        dispatching background work.
class App : public QObject {
    Q_OBJECT
public:
    /// @param actionLogPath   Where `FileActionLog` persists entries.
    /// @param tokenSecret     Shared secret for `BookmarksAuthorizer` and
    ///                        the metadata-fetch worker's own `TokenIssuer`
    ///                        — both must use the same secret so the
    ///                        worker's self-minted token verifies.
    /// @param fetcher         Metadata fetch implementation; defaults to
    ///                        `NullMetadataFetcher` (no real network).
    /// @param fetchInterval   How often the metadata-fetch worker runs.
    ///                        Tests pass a long interval and call
    ///                        `fetchMetadataOnce()` directly instead.
    /// @param relayInterval   How often the outbox relay runs. Same testing
    ///                        convention as `fetchInterval`.
    /// @param workers         Size of the model worker pool.
    /// @param parent          Optional `QObject` parent.
    explicit App(std::filesystem::path actionLogPath, std::string tokenSecret,
                std::shared_ptr<IBookmarkMetadataFetcher> fetcher = std::make_shared<NullMetadataFetcher>(),
                std::chrono::milliseconds fetchInterval = std::chrono::seconds{5},
                std::chrono::milliseconds relayInterval = std::chrono::seconds{2}, std::size_t workers = 4,
                QObject* parent = nullptr);

    ~App() override;

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    /// @brief The server every transport wraps or dispatches against.
    [[nodiscard]] std::shared_ptr<::morph::backend::RemoteServer> server() const noexcept { return _server; }

    /// @brief Finds every bookmark (across every owner) with an empty
    ///        title, calls the injected fetcher, and dispatches
    ///        `RecordMetadata` through the internal client for each. Does
    ///        not block on the dispatched calls settling.
    void fetchMetadataOnce();

    /// @brief Whether any `RecordMetadata` dispatched by a previous
    ///        `fetchMetadataOnce()` has not settled yet. Same settle-seam
    ///        contract as `pastebin::app::App::sweepInFlight()` — pump on
    ///        this until it is `false`, then destroy.
    [[nodiscard]] bool fetchInFlight() const noexcept { return _fetchInFlight->load() != 0; }

    /// @brief Drains `bookmark_outbox` into the durable action log via
    ///        `journal::OutboxRelay`. Synchronous — no in-flight seam
    ///        needed, unlike the fetch worker's async dispatch.
    void relayOutboxOnce();

private:
    // See pastebin::app::App's identical comment: the executor must be
    // declared (and therefore destroyed) after the pool, so every
    // in-flight dispatch has resolved (the pool's destructor joins its
    // threads) before the executor those completions post through goes away.
    ::morph::qt::QtExecutor _fetchExecutor;
    std::shared_ptr<std::atomic<int>> _fetchInFlight{std::make_shared<std::atomic<int>>(0)};
    std::shared_ptr<::morph::journal::FileActionLog> _actionLog;
    ::morph::exec::ThreadPoolExecutor _pool;
    std::shared_ptr<::morph::backend::RemoteServer> _server;
    ::morph::bridge::Bridge _fetchBridge;
    std::shared_ptr<IBookmarkMetadataFetcher> _fetcher;
    QTimer _fetchTimer;
    QTimer _relayTimer;
};

}  // namespace bookmarks::app
```

- [ ] **Step 7: Write `examples/bookmarks/src/app/app.cpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/app/app.hpp"

#include "bookmarks/auth/bookmarks_authorizer.hpp"
#include "bookmarks/db/bookmark_entity.hpp"
#include "bookmarks/db/outbox_entity.hpp"
#include "bookmarks/dto/bookmark_dto.hpp"
#include "bookmarks/models/bookmark_model.hpp"

#include <morph/core/logger.hpp>
#include <morph/journal/outbox.hpp>

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlStatement.hpp>

#include <chrono>
#include <span>
#include <vector>

namespace bookmarks::app {

App::App(std::filesystem::path actionLogPath, std::string tokenSecret,
         std::shared_ptr<IBookmarkMetadataFetcher> fetcher, std::chrono::milliseconds fetchInterval,
         std::chrono::milliseconds relayInterval, std::size_t workers, QObject* parent)
    : QObject{parent},
      _actionLog{std::make_shared<::morph::journal::FileActionLog>(std::move(actionLogPath))},
      _pool{workers},
      _server{std::make_shared<::morph::backend::RemoteServer>(
          _pool, std::make_shared<auth::BookmarksAuthorizer>(tokenSecret))},
      _fetchBridge{std::make_unique<::morph::backend::SimulatedRemoteBackend>(*_server)},
      _fetcher{std::move(fetcher)} {
    ::morph::journal::setActionLog(_actionLog);

    // Installed process-wide so AuthModel::execute(const Login&) (Task 12's
    // own earlier step) can mint tokens that verify against this exact
    // secret -- the same "registry-constructed models have no DI seam"
    // answer morph::journal::setActionLog already uses just above.
    auth::setTokenIssuer(std::make_shared<::morph::session::TokenIssuer>(tokenSecret));

    // The worker's self-minted service-principal token -- README's
    // resolved service-principal convention. Shares tokenSecret with the
    // authorizer above, so it verifies exactly like a real user's.
    const ::morph::session::TokenIssuer issuer{tokenSecret};
    ::morph::session::Context session;
    session.principal = std::string{auth::kMetadataFetcherPrincipal};
    session.token = issuer.issue(::morph::session::SessionToken{
        .principal = std::string{auth::kMetadataFetcherPrincipal},
        .issuedAtMs = 0,
        .expiresAtMs = 4102444800000,  // year 2100 -- the process's own lifetime is the real bound
        .roles = {},
    });
    _fetchBridge.setDefaultSession(session);

    connect(&_fetchTimer, &QTimer::timeout, this, &App::fetchMetadataOnce);
    _fetchTimer.start(fetchInterval);
    connect(&_relayTimer, &QTimer::timeout, this, &App::relayOutboxOnce);
    _relayTimer.start(relayInterval);
}

App::~App() {
    _fetchTimer.stop();
    _relayTimer.stop();
    ::morph::journal::setActionLog(nullptr);
    // Matches setActionLog's own clear-on-destruction discipline just
    // above: a later test that never constructs an App must see
    // auth::tokenIssuer() == nullptr, not a previous test's still-live
    // issuer (holding a *different* secret than whatever that later test
    // expects to be the "wrong" or "absent" one).
    auth::setTokenIssuer(nullptr);
}

void App::fetchMetadataOnce() {
    std::vector<std::pair<std::uint64_t, std::string>> needsFetch;
    {
        ::Lightweight::SqlStatement stmt;
        stmt.Prepare("SELECT id, url FROM bookmarks WHERE title = ''");
        auto cursor = stmt.Execute();
        while (cursor.FetchRow()) {
            needsFetch.emplace_back(cursor.GetColumn<std::uint64_t>(1), cursor.GetColumn<std::string>(2));
        }
    }
    if (needsFetch.empty()) {
        return;
    }

    // Same shared_ptr-captured-handler pattern as
    // pastebin::app::App::sweepExpiredOnce() -- see that function's own
    // extensive doc comment for the exact race this closes (a plain local
    // handler destroyed before RemoteServer has looked up the target
    // instance would silently drop the reclaim/record).
    auto handler = std::make_shared<::morph::bridge::BridgeHandler<BookmarkModel>>(_fetchBridge, &_fetchExecutor);
    auto inFlight = _fetchInFlight;
    for (const auto& [id, url] : needsFetch) {
        const auto metadata = _fetcher->fetch(url);  // synchronous by design -- see metadata_fetcher.hpp
        inFlight->fetch_add(1);
        handler
            ->execute(RecordMetadata{.id = BookmarkId{static_cast<std::int64_t>(id)}, .title = metadata.title,
                                     .faviconPath = metadata.faviconPath})
            .then([handler, inFlight](Ack) { inFlight->fetch_sub(1); })
            .onError([handler, inFlight, id](const std::exception_ptr&) {
                inFlight->fetch_sub(1);
                ::morph::log::logError("[bookmarks::App] metadata fetch: RecordMetadata failed for bookmark " +
                                       std::to_string(id));
            });
    }
}

void App::relayOutboxOnce() {
    ::Lightweight::DataMapper mapper;
    ::morph::journal::OutboxRelay relay;
    relay.drainOutbox = [&mapper] {
        auto rows = mapper.Query<db::BookmarkOutboxRecord>().All();
        std::vector<::morph::journal::LogEntry> entries;
        entries.reserve(rows.size());
        for (const auto& row : rows) {
            ::morph::journal::LogEntry entry;
            entry.modelType = row.modelType.Value();
            entry.entityKey = row.entityKey.Value();
            entry.actionType = row.actionType.Value();
            entry.payload = row.payload.Value();
            entry.result = row.result.Value();
            entry.principal = row.principal.Value();
            entry.timestampMs = row.timestampMs.Value();
            entry.idempotencyKey = row.idempotencyKey.Value();
            entries.push_back(std::move(entry));
        }
        return entries;
    };
    relay.markRelayed = [&mapper](std::span<const ::morph::journal::LogEntry> rows) {
        for (const auto& row : rows) {
            ::Lightweight::SqlStatement stmt{mapper.Connection()};
            stmt.Prepare("DELETE FROM bookmark_outbox WHERE idempotency_key = ?");
            (void) stmt.Execute(row.idempotencyKey);
        }
    };
    relay.sink = _actionLog;
    (void) relay.relay();
}

}  // namespace bookmarks::app
```

- [ ] **Step 6: Run to verify it passes.**

- [ ] **Step 9: Commit**

```bash
git add examples/bookmarks/include/bookmarks/app/ examples/bookmarks/include/bookmarks/dto/auth_dto.hpp \
        examples/bookmarks/include/bookmarks/models/auth_model.hpp examples/bookmarks/src/models/auth_model.cpp \
        examples/bookmarks/src/app/app.cpp examples/bookmarks/tests/test_app.cpp
git commit -m "bookmarks: add App (server bootstrap, AuthModel/Login, metadata worker, outbox relay)"
```

---

## Task 13: `CMakeLists.txt` for the bookmarks rung

**Files:**
- Create: `examples/bookmarks/CMakeLists.txt`

**Interfaces:** None — `morph_add_rung()` (confirmed fully generalized by
reading `cmake/morph_add_rung.cmake`: it globs `src/models/*.cpp` with no
per-model logic, so three models' `.cpp` files fold into one
`ladder_bookmarks_lib` the same way one folds into `ladder_pastebin_lib`)
does everything else, and `bookmarks` is already listed in
`examples/CMakeLists.txt`'s `_morph_known_rungs` — **no change needed
there**.

- [ ] **Step 1: Write `examples/bookmarks/CMakeLists.txt`**

```cmake
# SPDX-License-Identifier: Apache-2.0
#
# bookmarks — rung 2 of the application ladder (examples/bookmarks/README.md).
# All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake);
# this file only pulls in bookmarks-specific dependencies it doesn't know
# about, then calls it.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME bookmarks)

# ── The WASM client's server url ────────────────────────────────────────────
# Same mechanism as pastebin's own CMakeLists.txt — see that file's comment.
if(TARGET ladder_bookmarks_gui_wasm)
    if(NOT DEFINED MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL)
        set(MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL "ws://127.0.0.1:8766" CACHE STRING
            "URL bookmarks' WASM client connects to; must be a reachable ladder_bookmarks_server.")
    endif()
    target_compile_definitions(ladder_bookmarks_gui_wasm PRIVATE
        MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL="${MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL}"
    )
endif()
```

(Port `8766`, not pastebin's `8765` — the two rungs' standalone servers must
never collide if both are run locally at once.)

- [ ] **Step 2: Configure and build**

Run: `cmake --build build/clang-coverage --target ladder_bookmarks_tests`
Expected: every task's test file compiles and links into one binary; this
is the point at which every task's own "Step 2/4: run to verify it
fails/passes" that was deferred pending this task's existence can finally
be run for real, in order, task by task, to confirm the whole rung actually
builds and passes end to end. **Do this now, as part of this task, before
committing** — treat any task whose tests do not pass at this point as
unfinished, not as this task's own defect.

- [ ] **Step 3: Commit**

```bash
git add examples/bookmarks/CMakeLists.txt
git commit -m "bookmarks: add CMakeLists.txt, completing the buildable rung skeleton"
```

---

## Task 14: Model tests — backend-mode matrix for CRUD/list/changes-since

**Files:**
- Modify: `examples/bookmarks/tests/test_bookmark_model.cpp` (append)

**Interfaces:** None new. Consumes `testkit::BackendRig`, `Mode`,
`morph::session::TokenIssuer`.

Every model test through Task 11 calls `model.execute(action)` directly,
C++-to-C++, with `ScopedPrincipal` standing in for a real dispatch's
`Context` — the fast, direct-call style `pastebin`'s own model tests use.
`TESTING.md`'s backend-mode-matrix rule additionally requires the **real**
dispatch path — `Local`/`LocalSingleThread`/`Socket` via `BackendRig` — for
at least the actions whose correctness depends on the dispatch machinery
itself, not just the model's own logic: authentication (`Socket` mode's
real `RemoteServer` + `BookmarksAuthorizer`) is exactly that case. This
task adds the matrix for the create → list → get round trip, driven by real
signed tokens.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("BookmarkModel over the full backend-mode matrix: create, list, get round-trip",
          "[bookmarks][model]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;

    constexpr std::string_view kSecret = "matrix-test-secret";
    const auto authorizer = std::make_shared<bookmarks::auth::BookmarksAuthorizer>(std::string{kSecret});
    BackendRig rig{mode, 1, authorizer};

    const morph::session::TokenIssuer issuer{std::string{kSecret}};
    morph::session::Context ctx;
    ctx.principal = "alice";
    ctx.token = issuer.issue(morph::session::SessionToken{
        .principal = "alice", .expiresAtMs = 4102444800000});
    rig.bridge(0).setDefaultSession(ctx);

    auto handler = rig.client<bookmarks::BookmarkModel>(0);
    bookmarks::CreateBookmark create;
    create.url = "https://matrix.example";
    create.title = "Matrix";
    const auto createResult = awaitQt(handler.execute(create));
    REQUIRE(createResult.id.hasValue());

    const auto listResult = awaitQt(handler.execute(bookmarks::ListBookmarks{}));
    REQUIRE(listResult.bookmarks.size() == 1);

    const auto view = awaitQt(handler.execute(bookmarks::GetBookmark{.id = createResult.id}));
    CHECK(view.url == "https://matrix.example");
    CHECK(view.title == "Matrix");
}
```

- [ ] **Step 2: Run to verify it fails** (before the matrix loop existed, only the direct-call tests covered this
  path — Local/LocalSingleThread should already pass once written, since the model logic itself is already correct;
  the point of this case is Socket mode specifically, where a bug in the auth wiring would newly surface).

- [ ] **Step 3: Run to verify it passes** across all three modes.

- [ ] **Step 4: Commit**

```bash
git add examples/bookmarks/tests/test_bookmark_model.cpp
git commit -m "bookmarks: add the backend-mode matrix for BookmarkModel's create/list/get round trip"
```

---

## Task 15: `BulkEdit` atomicity under injected failure, cross-user `Socket`-mode auth enforcement, and the local-mode-has-no-authorization strain point

**Files:**
- Modify: `examples/bookmarks/tests/test_bookmark_model.cpp` (append)

**Interfaces:** Consumes `testkit::db_busy_fixture.hpp`'s `DbBusyFixture`
(finding 018's resolved mechanism, rung 1) and `BackendRig::Socket`.

Three genuinely new pieces of coverage, each answering a specific
requirement `examples/bookmarks/README.md`'s DoD/Expected-strain-points
sections name:

1. **`BulkEdit` is atomic under injected mid-batch failure** (DoD). Forcing
   a real mid-transaction failure (not a mock) the same way rung 1's
   `SQLITE_BUSY` tests do: hold a genuine write lock open on a second
   connection (`DbBusyFixture`) so the transaction's own write blocks and
   then fails once the connection-under-test's `PRAGMA busy_timeout` is
   shortened (`ScopedShortBusyTimeout`, mirroring
   `test_paste_model.cpp`'s exact pattern for the identical purpose —
   define a local copy of that helper in this file too, same rationale:
   test-only, one file's own concern, not yet promoted).
2. **`authorizeInstance`/`authorizeRegister` genuinely deny cross-user
   access over a real `Socket` transport** (DoD: "authorization enforced
   server-side, not by the client"). Two real sockets, two real signed
   tokens, one tries to `GetBookmark` an id it does not own.
3. **"Local mode has no authorization at all" is demonstrated, not just
   asserted in prose** (Expected strain points). `Mode::Local`'s
   `LocalBackend` never consults an `IAuthorizer` at all (verified against
   `backend.hpp` while researching Task 1) — so two different
   `ScopedPrincipal`s sharing one `BackendRig{Mode::Local}` and one
   `BookmarkModel` instance rely **entirely** on the model's own
   `requireOwner()`/`loadOwned()` re-check for isolation. This test proves
   that re-check is what's actually doing the work, by constructing the
   exact scenario where it is the *only* thing standing between mallory and
   alice's bookmark.

- [ ] **Step 1: Write the failing tests**

```cpp
namespace {
class ScopedShortBusyTimeout {
  public:
    explicit ScopedShortBusyTimeout(int milliseconds) {
        ::Lightweight::SqlConnection::SetPostConnectedHook([milliseconds](::Lightweight::SqlConnection& connection) {
            ::Lightweight::SqlStatement stmt{connection};
            (void) stmt.ExecuteDirect("PRAGMA busy_timeout = " + std::to_string(milliseconds));
        });
    }
    ~ScopedShortBusyTimeout() { ::Lightweight::SqlConnection::ResetPostConnectedHook(); }
    ScopedShortBusyTimeout(const ScopedShortBusyTimeout&) = delete;
    ScopedShortBusyTimeout& operator=(const ScopedShortBusyTimeout&) = delete;
    ScopedShortBusyTimeout(ScopedShortBusyTimeout&&) = delete;
    ScopedShortBusyTimeout& operator=(ScopedShortBusyTimeout&&) = delete;
};
}  // namespace

TEST_CASE("BulkEdit rolls back entirely when a genuine SQLITE_BUSY interrupts the batch",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel seedModel;
    bookmarks::BookmarkId id1;
    bookmarks::BookmarkId id2;
    {
        const ScopedPrincipal alice{"alice"};
        id1 = seedModel.execute(bookmarks::CreateBookmark{.url = "https://one.example"}).id;
        id2 = seedModel.execute(bookmarks::CreateBookmark{.url = "https://two.example"}).id;
    }

    const ScopedShortBusyTimeout shortTimeout{200};
    bookmarks::BookmarkModel contendedModel;
    const ScopedPrincipal alice{"alice"};

    const morph::ladder::testkit::DbBusyFixture busy{"bookmarks"};
    bookmarks::BulkEdit edit;
    edit.ids = {id1, id2};
    edit.archive = bookmarks::BulkArchiveOp::Archive;
    REQUIRE_THROWS(contendedModel.execute(edit));

    // Neither bookmark was archived, and no outbox row survived -- the
    // whole transaction (mutation + outbox write) rolled back together.
    CHECK(seedModel.execute(bookmarks::GetBookmark{.id = id1}).archiveState == bookmarks::ArchiveState::Active);
    CHECK(seedModel.execute(bookmarks::GetBookmark{.id = id2}).archiveState == bookmarks::ArchiveState::Active);
    Lightweight::DataMapper mapper;
    CHECK(mapper.Query<bookmarks::db::BookmarkOutboxRecord>().All().empty());
}

TEST_CASE("BackendRig::Socket: authorizeInstance denies a second principal's GetBookmark",
          "[bookmarks][model][socket-only]") {
    DbFixture fixture;
    constexpr std::string_view kSecret = "cross-user-secret";
    const auto authorizer = std::make_shared<bookmarks::auth::BookmarksAuthorizer>(std::string{kSecret});
    BackendRig rig{Mode::Socket, 2, authorizer};
    const morph::session::TokenIssuer issuer{std::string{kSecret}};

    auto tokenFor = [&issuer](std::string principal) {
        morph::session::Context ctx;
        ctx.principal = principal;
        ctx.token = issuer.issue(morph::session::SessionToken{.principal = std::move(principal), .expiresAtMs = 4102444800000});
        return ctx;
    };
    rig.bridge(0).setDefaultSession(tokenFor("alice"));
    rig.bridge(1).setDefaultSession(tokenFor("mallory"));

    auto aliceHandler = rig.client<bookmarks::BookmarkModel>(0);
    auto malloryHandler = rig.client<bookmarks::BookmarkModel>(1);

    const auto created = awaitQt(aliceHandler.execute(bookmarks::CreateBookmark{.url = "https://alice.example"}));

    bool malloryFailed = false;
    malloryHandler.execute(bookmarks::GetBookmark{.id = created.id})
        .then([](bookmarks::BookmarkView) {})
        .onError([&malloryFailed](const std::exception_ptr&) { malloryFailed = true; });
    REQUIRE(pumpUntil([&malloryFailed] { return malloryFailed; }));
}

TEST_CASE("Mode::Local has no authorization at all: isolation depends entirely on the model's own re-check",
          "[bookmarks][model]") {
    DbFixture fixture;
    // No authorizer passed -- Mode::Local's LocalBackend never consults one
    // regardless (verified against backend.hpp), so this is the same as
    // passing one: the point this test makes.
    BackendRig rig{Mode::Local, 1};
    auto handler = rig.client<bookmarks::BookmarkModel>(0);

    bookmarks::BookmarkId aliceId;
    {
        const ScopedPrincipal alice{"alice"};
        // Constructed directly, not through the rig's handler -- this
        // establishes the row to attack; the attack itself goes through
        // the rig, matching a real client's only path.
        bookmarks::BookmarkModel seedModel;
        aliceId = seedModel.execute(bookmarks::CreateBookmark{.url = "https://alice.example"}).id;
    }

    // No token/session set on rig.bridge(0) at all -- Local mode's own
    // Context::principal, whatever the caller sets client-side, would
    // normally be untrustworthy on a Socket transport; here there is no
    // authorizer to strip it, so it passes straight through. This test
    // simulates the honest worst case: an attacker who sets principal
    // directly, which Local mode lets through unchecked.
    morph::session::Context ctx;
    ctx.principal = "mallory";
    rig.bridge(0).setDefaultSession(ctx);

    bool malloryFailed = false;
    handler.execute(bookmarks::GetBookmark{.id = aliceId})
        .then([](bookmarks::BookmarkView) {})
        .onError([&malloryFailed](const std::exception_ptr&) { malloryFailed = true; });
    REQUIRE(pumpUntil([&malloryFailed] { return malloryFailed; }));
    // malloryFailed is true only because BookmarkModel::execute(GetBookmark)
    // itself re-checked ownership (loadOwned/requireOwner) -- Local mode
    // contributed nothing to this result. Documented, not smoothed over,
    // per the README's own "Expected strain points" framing.
}
```

- [ ] **Step 2: Run to verify all three fail without the corresponding production behavior** (the first two should
  already pass, since Tasks 6/8/1 implemented the behavior they check — this step is a sanity confirmation, not a
  true red-first cycle, since the feature predates this task by design; **the third case is the one to actually
  watch**, since it exists to document existing behavior rather than drive new code).

- [ ] **Step 3: Run to verify it passes.**

- [ ] **Step 4: Commit**

```bash
git add examples/bookmarks/tests/test_bookmark_model.cpp
git commit -m "bookmarks: add BulkEdit atomicity, cross-user Socket auth, and local-mode-no-auth tests"
```

---

## Task 16: The cross-model rename race, and background-worker/import dispatch-pattern proof

**Files:**
- Modify: `examples/bookmarks/tests/test_tag_model.cpp` (append)
- Modify: `examples/bookmarks/tests/test_app.cpp` (append)

**Interfaces:** None new.

Two remaining README commitments: the "cross-model rename race" expected
strain point (`TagModel` renames a tag while a concurrent `BookmarkModel`
`BulkEdit` adds the old name), and confirming the metadata-fetch worker's
dispatch genuinely goes through `SimulatedRemoteBackend`/`RemoteServer`
(not a shortcut), the same proof pastebin's own sweep tests established for
`ExpirePaste`.

- [ ] **Step 1: Write the failing tests**

```cpp
// test_tag_model.cpp:
TEST_CASE("Cross-model race: TagModel renames a tag while BookmarkModel's BulkEdit adds the old "
          "name -- documents where consistency becomes app responsibility, per the README",
          "[bookmarks][model]") {
    DbFixture fixture;
    bookmarks::BookmarkModel bookmarkModel;
    bookmarks::TagModel tagModel;
    const ScopedPrincipal alice{"alice"};

    const auto id = bookmarkModel.execute(bookmarks::CreateBookmark{.url = "https://one.example", .tags = {"old"}}).id;
    const auto tagId = tagModel.execute(bookmarks::ListTags{}).tags.front().id;

    // Sequential, not genuinely racing (this test suite calls execute()
    // directly, C++-to-C++, with no thread-level concurrency -- the README's
    // own framing already concedes "the strand cannot fix it," i.e. this is
    // a documentation test, not a fix-verification test): rename first,
    // then a second bookmark's BulkEdit tries to add the *old* name back.
    tagModel.execute(bookmarks::RenameTag{.id = tagId, .name = "new"});
    const auto id2 = bookmarkModel.execute(bookmarks::CreateBookmark{.url = "https://two.example"}).id;

    bookmarks::BulkEdit edit;
    edit.ids = {id2};
    edit.addTags = {"old"};  // the pre-rename name -- TagModel already renamed it away
    bookmarkModel.execute(edit);

    // BulkEdit's own findOrCreateTagId has no way to know "old" was renamed
    // to "new" -- it faithfully creates a *new* tag literally named "old".
    // This is the documented, accepted outcome: two strands, no
    // cross-instance transaction, and the model layer cannot see the other
    // model's in-flight rename. Consistency here is app/UI responsibility
    // (e.g. a client re-fetching the tag list before offering it), not a
    // framework or model guarantee.
    const auto tags = tagModel.execute(bookmarks::ListTags{}).tags;
    CHECK(std::ranges::any_of(tags, [](auto& t) { return t.name == "new"; }));
    CHECK(std::ranges::any_of(tags, [](auto& t) { return t.name == "old"; }));  // recreated, not merged
}

// test_app.cpp:
TEST_CASE("App's metadata-fetch worker dispatches through the real RemoteServer, not a shortcut",
          "[bookmarks][app]") {
    DbFixture fixture;
    bookmarks::BookmarkModel model;
    bookmarks::BookmarkId id;
    {
        const ScopedPrincipal alice{"alice"};
        id = model.execute(bookmarks::CreateBookmark{.url = "https://one.example"}).id;
    }

    class RecordingFetcher : public bookmarks::app::IBookmarkMetadataFetcher {
      public:
        bookmarks::app::FetchedMetadata fetch(const std::string& url) override {
            calls.push_back(url);
            return {.title = "Recorded"};
        }
        std::vector<std::string> calls;
    };
    auto fetcher = std::make_shared<RecordingFetcher>();

    bookmarks::app::App app{fixture.actionLogPath(), "test-secret", fetcher, std::chrono::hours{1},
                            std::chrono::hours{1}};
    // Proves the dispatch went through the server's own registration path
    // (which requires authorizeRegister to pass -- an unauthenticated
    // internal client would fail here exactly like a real socket client
    // would): if the worker's own token/session wiring were broken, this
    // whole call would silently no-op (the completion's onError path,
    // logged but not surfaced to this test directly) and fetchInFlight()
    // would still settle to false, but the title would never update --
    // which the assertion below would catch.
    app.fetchMetadataOnce();
    REQUIRE(pumpUntil([&app] { return !app.fetchInFlight(); }));
    REQUIRE(fetcher->calls.size() == 1);
    CHECK(fetcher->calls.front() == "https://one.example");

    const ScopedPrincipal alice{"alice"};
    CHECK(model.execute(bookmarks::GetBookmark{.id = id}).title == "Recorded");
}
```

- [ ] **Step 2: Run to verify it fails/document as expected.**

- [ ] **Step 3: Run to verify it passes.**

- [ ] **Step 4: Commit**

```bash
git add examples/bookmarks/tests/test_tag_model.cpp examples/bookmarks/tests/test_app.cpp
git commit -m "bookmarks: document the cross-model rename race and prove the worker's real dispatch path"
```

---

## Task 17: Presenters and presenter tests

**Files:**
- Create: `examples/bookmarks/gui_lib/bookmark_presenter.hpp`
- Create: `examples/bookmarks/gui_lib/bookmark_presenter.cpp`
- Create: `examples/bookmarks/gui_lib/tag_presenter.hpp`
- Create: `examples/bookmarks/gui_lib/tag_presenter.cpp`
- Create: `examples/bookmarks/gui_lib/shared_feed_presenter.hpp`
- Create: `examples/bookmarks/gui_lib/shared_feed_presenter.cpp`
- Test: `examples/bookmarks/tests/test_bookmark_presenter.cpp`
- Test: `examples/bookmarks/tests/test_tag_presenter.cpp`
- Test: `examples/bookmarks/tests/test_shared_feed_presenter.cpp`

**Interfaces:** Produces `bookmarks::gui::BookmarkPresenter`,
`bookmarks::gui::TagPresenter`, `bookmarks::gui::SharedFeedPresenter` — each
a thin `::morph::ladder::gui::Presenter` subclass over a
`BridgeHandler<Model>`, following `pastebin::gui::PastePresenter`'s exact
shape (`examples/pastebin/gui_lib/paste_presenter.hpp`): the `Q_MOC_RUN`
include guard around the model header (moc must never see
`Lightweight`-touching headers — that file's own doc comment has the full
mis-parse story), the `track<T>()`-with-third-`onErr`-argument pattern
(finding 023's shipped workaround), one signal per success case plus one
shared `failed(QString)`.

- [ ] **Step 1: Write the failing test** (`BookmarkPresenter` only shown; `TagPresenter`/`SharedFeedPresenter` follow
  the identical shape — write their own test cases the same way, one per action, plus one shared failure case each)

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmark_presenter.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/pump.hpp"
#include "testkit/db_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <morph/session/session_auth.hpp>

using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;
using morph::ladder::testkit::pumpUntil;

TEST_CASE("BookmarkPresenter::create emits created() on success, failed() on validation error",
          "[bookmarks][presenter]") {
    const auto mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    constexpr std::string_view kSecret = "presenter-test-secret";
    const auto authorizer = std::make_shared<bookmarks::auth::BookmarksAuthorizer>(std::string{kSecret});
    BackendRig rig{mode, 1, authorizer};
    const morph::session::TokenIssuer issuer{std::string{kSecret}};
    morph::session::Context ctx;
    ctx.principal = "alice";
    ctx.token = issuer.issue(morph::session::SessionToken{.principal = "alice", .expiresAtMs = 4102444800000});
    rig.bridge(0).setDefaultSession(ctx);

    bookmarks::gui::BookmarkPresenter presenter{rig.bridge(0), rig.clientExecutor()};

    bool created = false;
    bool failed = false;
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::created, [&](bookmarks::CreateBookmarkResult) {
        created = true;
    });
    QObject::connect(&presenter, &bookmarks::gui::BookmarkPresenter::failed, [&](QString) { failed = true; });

    presenter.create(bookmarks::CreateBookmark{.url = "https://one.example"});
    REQUIRE(pumpUntil([&] { return created; }));
    CHECK_FALSE(presenter.busy());

    presenter.create(bookmarks::CreateBookmark{});  // empty url -- ValidationError
    REQUIRE(pumpUntil([&] { return failed; }));
}
```

(`rig.clientExecutor()` — confirm the exact accessor name on `BackendRig`
during implementation against `backend_rig.hpp`'s real public surface;
`pastebin`'s own presenter tests already call it under some name — reuse
that spelling verbatim rather than guessing a new one.)

- [ ] **Step 2: Run to verify it fails to compile.**

- [ ] **Step 3: Write `examples/bookmarks/gui_lib/bookmark_presenter.hpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gui/presenter.hpp"
#include "bookmarks/dto/bookmark_dto.hpp"
#include "bookmarks/dto/bulk_dto.hpp"
#include "bookmarks/dto/import_export_dto.hpp"

#include <exception>

// See pastebin::gui::PastePresenter's identical guard and doc comment
// (examples/pastebin/gui_lib/paste_presenter.hpp) for why moc must never
// see morph/core/bridge.hpp or bookmark_model.hpp.
#ifndef Q_MOC_RUN
#include "bookmarks/models/bookmark_model.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#endif

namespace bookmarks::gui {

/// @brief Routes every `BookmarkModel` action through a
///        `BridgeHandler<BookmarkModel>`. Translates and routes only — no
///        domain logic (`IMPLEMENTATION.md` rule 2).
class BookmarkPresenter : public ::morph::ladder::gui::Presenter {
    Q_OBJECT
  public:
    BookmarkPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, QObject* parent = nullptr);

    void create(CreateBookmark action);
    void edit(EditBookmark action);
    void archive(ArchiveBookmark action);
    void unarchive(UnarchiveBookmark action);
    void remove(DeleteBookmark action);
    void get(GetBookmark action);
    void list(ListBookmarks action);
    void bulkEdit(BulkEdit action);
    void importChunk(ImportBookmarks action);
    void exportAll(ExportBookmarks action);

  signals:
    void created(CreateBookmarkResult result);
    void edited(BookmarkView view);
    void archived();
    void unarchived();
    void removed();
    void loaded(BookmarkView view);
    void listed(ListBookmarksResult result);
    void bulkEdited(BulkEditResult result);
    void imported(ImportBookmarksResult result);
    void exported(ExportBookmarksResult result);
    void failed(QString message);

  private:
    void reportError(const std::exception_ptr& err);

    ::morph::bridge::BridgeHandler<BookmarkModel> _handler;
};

}  // namespace bookmarks::gui
```

- [ ] **Step 4: Write `examples/bookmarks/gui_lib/bookmark_presenter.cpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmark_presenter.hpp"

namespace bookmarks::gui {

BookmarkPresenter::BookmarkPresenter(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor,
                                     QObject* parent)
    : Presenter{parent}, _handler{bridge, executor} {}

void BookmarkPresenter::reportError(const std::exception_ptr& err) {
    try {
        std::rethrow_exception(err);
    } catch (const std::exception& ex) {
        emit failed(QString::fromStdString(ex.what()));
    }
}

void BookmarkPresenter::create(CreateBookmark action) {
    track<CreateBookmarkResult>(
        _handler.execute(std::move(action)), [this](CreateBookmarkResult result) { emit created(result); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::edit(EditBookmark action) {
    track<BookmarkView>(
        _handler.execute(std::move(action)), [this](BookmarkView view) { emit edited(view); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::archive(ArchiveBookmark action) {
    track<Ack>(
        _handler.execute(std::move(action)), [this](Ack) { emit archived(); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::unarchive(UnarchiveBookmark action) {
    track<Ack>(
        _handler.execute(std::move(action)), [this](Ack) { emit unarchived(); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::remove(DeleteBookmark action) {
    track<Ack>(
        _handler.execute(std::move(action)), [this](Ack) { emit removed(); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::get(GetBookmark action) {
    track<BookmarkView>(
        _handler.execute(std::move(action)), [this](BookmarkView view) { emit loaded(view); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::list(ListBookmarks action) {
    track<ListBookmarksResult>(
        _handler.execute(std::move(action)), [this](ListBookmarksResult result) { emit listed(result); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::bulkEdit(BulkEdit action) {
    track<BulkEditResult>(
        _handler.execute(std::move(action)), [this](BulkEditResult result) { emit bulkEdited(result); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::importChunk(ImportBookmarks action) {
    track<ImportBookmarksResult>(
        _handler.execute(std::move(action)), [this](ImportBookmarksResult result) { emit imported(result); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

void BookmarkPresenter::exportAll(ExportBookmarks action) {
    track<ExportBookmarksResult>(
        _handler.execute(std::move(action)), [this](ExportBookmarksResult result) { emit exported(result); },
        [this](const std::exception_ptr& err) { reportError(err); });
}

}  // namespace bookmarks::gui
```

- [ ] **Step 5: Write `TagPresenter`/`SharedFeedPresenter`, header + cpp, the identical shape**

`TagPresenter` wraps `BridgeHandler<TagModel>` with `rename(RenameTag)` →
`renamed()`, `merge(MergeTags)` → `merged()`, `list(ListTags)` →
`listed(ListTagsResult)`, plus `failed(QString)`. `SharedFeedPresenter`
wraps `BridgeHandler<SharedFeedModel>` with `list(ListSharedFeed)` →
`listed(ListSharedFeedResult)`, plus `failed(QString)`. Both follow
`BookmarkPresenter`'s exact structure above — write them the same way, one
`track<T>()` call per action, no domain logic.

- [ ] **Step 6: Write the remaining presenter tests** — one success + one
  failure case per action, across the full `Local`/`LocalSingleThread`/
  `Socket` matrix, for `BookmarkPresenter` (every action listed in Step 3),
  `TagPresenter`, and `SharedFeedPresenter`. Follow
  `pastebin`'s `test_paste_presenter.cpp` for the exact matrix/assertion
  shape this rung's own Step 1 case above already demonstrates for one
  action.

- [ ] **Step 7: Run to verify it passes.**

- [ ] **Step 8: Commit**

```bash
git add examples/bookmarks/gui_lib/ examples/bookmarks/tests/test_bookmark_presenter.cpp \
        examples/bookmarks/tests/test_tag_presenter.cpp examples/bookmarks/tests/test_shared_feed_presenter.cpp
git commit -m "bookmarks: add BookmarkPresenter, TagPresenter, SharedFeedPresenter"
```

---

## Task 18: GUI shell, server binary, and offscreen smoke test

**Files:**
- Create: `examples/bookmarks/gui_lib/bookmark_forms_controller.hpp`
- Create: `examples/bookmarks/gui_lib/bookmark_forms_controller.cpp`
- Create: `examples/bookmarks/gui_lib/bookmark_qml_bridges.hpp`
- Create: `examples/bookmarks/gui_lib/bookmark_qml_bridges.cpp`
- Create: `examples/bookmarks/gui/main.cpp`
- Create: `examples/bookmarks/gui/qml/Main.qml`
- Create: `examples/bookmarks/gui/qml/LoginView.qml`
- Create: `examples/bookmarks/gui/qml/BookmarkListView.qml`
- Create: `examples/bookmarks/src/server/main.cpp`
- Test: `examples/bookmarks/tests/test_gui_qml_smoke.cpp`

**Interfaces:** Consumes every model/presenter task. Produces the desktop
client and standalone server binaries plus their QML/bridge glue. Schema-driven
throughout (`IMPLEMENTATION.md` rule 2) — `Login`, `CreateBookmark`,
`EditBookmark`, `RenameTag`, `MergeTags` all render from
`morph::forms::schemaJson<A>()` through the shipped `MorphForms` module,
exactly as `pastebin::gui::PasteFormsController`
(`examples/pastebin/gui_lib/paste_forms_controller.hpp/.cpp`) already
proves out — mirror that file's shape (and its finding-021 written
justification for owning a `FormsControllerCore` directly rather than
composing over `AppContext`, since the same constraint applies here
unchanged) for `BookmarkFormsController`.

**One genuinely new piece of glue, with its own written justification**
(rule 2's "(b) pure glue with no domain logic" clause): after a successful
`Login`, the GUI must attach the returned `AuthToken` to the `Bridge` so
every subsequent action carries it. This is infrastructure wiring, not
business logic — the equivalent of `pastebin`'s own `AppContext`-composition
pattern, one layer up. `BookmarkQmlBridges`' `onLoginSucceeded` handler
(mirroring `pastebin::gui::PasteBridge`/`FormsBridge`'s shape,
`paste_qml_bridges.hpp/.cpp`) does exactly this and nothing else:

```cpp
// excerpt of BookmarkQmlBridges::onLoginSucceeded, gui_lib/bookmark_qml_bridges.cpp
void BookmarkQmlBridges::onLoginSucceeded(const LoginResult& result) {
    ::morph::session::Context session;
    session.principal = result.principal;
    session.token = result.token.hasValue() ? *result.token : std::string{};
    _bridge.setDefaultSession(session);
    emit loggedIn(QString::fromStdString(result.principal));
}
```

- [ ] **Step 1: Write `examples/bookmarks/gui_lib/bookmark_forms_controller.hpp`/`.cpp`**

Mirror `paste_forms_controller.hpp`/`.cpp` exactly: a `FormsControllerCore`
wrapping `submitIfValid(actionType, jsonPayload)` for `Login`,
`CreateBookmark`, `EditBookmark`, `RenameTag`, `MergeTags`, and
`ImportBookmarks`, each dispatched to the correct model
(`AuthModel`/`BookmarkModel`/`TagModel`) by `actionType` string. Cite
finding 021 in the class doc comment, unchanged from `pastebin`'s own.

- [ ] **Step 2: Write `examples/bookmarks/gui_lib/bookmark_qml_bridges.hpp`/`.cpp`**

Mirror `paste_qml_bridges.hpp`/`.cpp`: `AuthBridge` (login submit +
`loggedIn(QString)`/`failed(QString)` signals, the `onLoginSucceeded`
handler above), `BookmarkBridge` (list/get/create/edit/archive/delete,
`QVariantMap`/`QVariantList` bags — same "exactly N keys, no leaked field"
discipline `PasteBridge` established, reviewed in rung 1's own final
review), `TagBridge`, `SharedFeedBridge`. Each takes `(Bridge&, IExecutor*)`
only (presenter rule 2).

- [ ] **Step 3: Write `examples/bookmarks/gui/qml/LoginView.qml`, `BookmarkListView.qml`, `Main.qml`**

`Main.qml` composes a `StackView`: `LoginView` first (a single schema-driven
`DynamicForm` bound to `AuthBridge`'s `Login` schema plus a submit button —
no hand-built username field, the generated form already renders
`Login::username`'s single `std::string` member), pushing to
`BookmarkListView` on `loggedIn`. `BookmarkListView` is
`morph::forms`' list/table view bound to `BookmarkBridge::listed`, with a
schema-driven `DynamicForm` for `CreateBookmark` above it — the same
composition `pastebin`'s `PasteView.qml` already establishes. No hand-built
widgets beyond the `StackView`/layout scaffolding itself (rule 2's
"(b) pure glue" exemption — navigation chrome, not domain logic).

- [ ] **Step 4: Write `examples/bookmarks/gui/main.cpp`**

Mirror `pastebin/gui/main.cpp`: constructs `AppContext` (Local or Remote per
CLI flag, `examples/common/gui::AppContext`, unchanged from rung 1),
constructs every bridge/presenter, exposes them to QML as context
properties, loads `Main.qml` from the `Bookmarks` QML module (URI
capitalization matches `morph_add_rung()`'s convention —
`cmake/morph_add_rung.cmake`'s own `_uri_head`/`_uri_tail` logic, already
generalized).

- [ ] **Step 5: Write `examples/bookmarks/src/server/main.cpp`**

```cpp
// SPDX-License-Identifier: Apache-2.0
#include "bookmarks/app/app.hpp"
#include "bookmarks/db/database.hpp"

#include <morph/qt/qt_websocket_server.hpp>

#include <QCoreApplication>
#include <QTimer>

#include <csignal>
#include <cstdlib>
#include <iostream>

namespace {
volatile std::sig_atomic_t g_shutdownRequested = 0;
void handleSigterm(int) { g_shutdownRequested = 1; }
}  // namespace

int main(int argc, char** argv) {
    QCoreApplication qtApp{argc, argv};
    std::signal(SIGTERM, handleSigterm);
    std::signal(SIGINT, handleSigterm);

    const char* secretEnv = std::getenv("BOOKMARKS_TOKEN_SECRET");
    if (secretEnv == nullptr) {
        std::cerr << "BOOKMARKS_TOKEN_SECRET must be set\n";
        return 2;
    }
    bookmarks::db::setup("DRIVER=SQLite3;Database=bookmarks.db");

    bookmarks::app::App app{"bookmarks-journal.jsonl", secretEnv};
    ::morph::qt::QtWebSocketServer wsServer{app.server()};
    const std::uint16_t port = 8766;
    if (!wsServer.listen(port)) {
        std::cerr << "failed to listen on port " << port << "\n";
        return 1;
    }
    std::cout << "bookmarks server listening on ws://127.0.0.1:" << port << "\n";

    QTimer shutdownPoll;
    QObject::connect(&shutdownPoll, &QTimer::timeout, [&] {
        if (g_shutdownRequested != 0) {
            qtApp.quit();
        }
    });
    shutdownPoll.start(std::chrono::milliseconds{200});

    const int rc = QCoreApplication::exec();
    wsServer.closeGracefully(std::chrono::seconds{2});
    return rc;
}
```

(Mirrors `pastebin::src::server::main.cpp`'s exact SIGTERM-poll shutdown
shape — see that file for the full `App::sweepInFlight()`-style
pump-then-destroy contract; this rung's own `App` has no equivalent drain
step to call before destruction since its worker's own `fetchInFlight()`
observability is a test-only concern, not a server-shutdown one — document
this asymmetry rather than silently copying an unnecessary drain call.)

- [ ] **Step 6: Write the offscreen QML smoke test**

Mirror `pastebin`'s `test_gui_qml_smoke.cpp`: load `Main.qml` under
`QT_QPA_PLATFORM=offscreen`, assert zero QML warnings with every bridge
context property present but unconnected to a live backend (the same
known, documented limitation `pastebin`'s own smoke test carries — Task 12
of rung 1's ledger — restated here rather than silently inherited).

- [ ] **Step 7: Manually verify end to end** (real server + real client, real
  WebSocket, exactly as rung 1's Task 12 did): start `ladder_bookmarks_server`
  with a real `BOOKMARKS_TOKEN_SECRET`, launch `ladder_bookmarks_gui` in
  Remote mode, log in as two different usernames from two client instances,
  confirm isolated collections, confirm the shared feed shows a bookmark
  marked shared by either user, confirm `BulkEdit`/`RenameTag`/`MergeTags`
  work end to end, confirm clean `SIGTERM` shutdown. Remove any temporary
  autopilot/scripting used to drive this before committing (verify with a
  diff review, the same discipline rung 1's Task 12 self-review applied).

- [ ] **Step 8: Run to verify the automated tests pass.**

- [ ] **Step 9: Commit**

```bash
git add examples/bookmarks/gui_lib/bookmark_forms_controller.* examples/bookmarks/gui_lib/bookmark_qml_bridges.* \
        examples/bookmarks/gui/ examples/bookmarks/src/server/main.cpp examples/bookmarks/tests/test_gui_qml_smoke.cpp
git commit -m "bookmarks: add the schema-driven GUI shell, server binary, and QML smoke test"
```

---

## Task 19: WASM client wiring

**Files:**
- Create: `examples/bookmarks/gui_wasm/main_wasm.cpp`
- Modify: `.github/workflows/wasm-ladder.yml`

**Interfaces:** None new — this task is entirely about making the already-generic
machinery cover a second rung.

Rung 1's Task 13 built two things this task reuses **unchanged**: the
`db_model.hpp` `#ifdef __EMSCRIPTEN__` two-branch `WithMapper` pattern
(finding 025) and `morph_add_rung()`'s `MORPH_CLIENT_ONLY` `FATAL_ERROR`
guard (`cmake/morph_add_rung.cmake`, already applied to every rung
generically). This rung's own `db_model.hpp` (Task 5) already has the
two-branch shape, so **no CMake or db_model change is needed here at all**
— confirmed by reading `morph_add_rung.cmake`'s `ladder_${_rung}_gui_wasm`
block during this task's own research, which is rung-name-generic
throughout.

- [ ] **Step 1: Write `examples/bookmarks/gui_wasm/main_wasm.cpp`**

Mirror `examples/pastebin/gui_wasm/main_wasm.cpp` exactly (or, if rung 1's
file itself references `examples/common/wasm_spike/main_wasm.cpp`'s
registration-retry-timer pattern for finding 024's transient
"handler not bound" gap, carry that same retry timer here too — this
rung's own `Main.qml`/`AppContext` wiring hits the identical
register-before-settled window pastebin's did): reads
`MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL` (Task 13's compile definition),
constructs `AppContext` in Remote mode against it, loads the same
`Bookmarks` QML module the desktop client does.

- [ ] **Step 2: Configure with Emscripten and verify the target exists**

Run (requires an Emscripten toolchain — CI-only in this environment, per
rung 1's own finding that no local Emscripten was available when its
WASM work was authored): confirm `ladder_bookmarks_gui_wasm` is generated
by `morph_add_rung()` once `gui_wasm/main_wasm.cpp` exists, the same way
`ladder_pastebin_gui_wasm` was. If it is not generated, read
`morph_add_rung.cmake`'s own skip-reason `message(STATUS ...)` output
first — it names every prerequisite by design (rung 1's Task 12 fix round
established this) rather than silently vanishing.

- [ ] **Step 3: Extend `.github/workflows/wasm-ladder.yml`**

Add `ladder_bookmarks_gui_wasm` to the "Build the WASM-remote spike and
every rung's WASM client" step, by name, next to
`ladder_pastebin_gui_wasm` — matching that workflow's own documented
design ("fails loud if a target silently stops being generated"). Rung 1's
own final review flagged that step's title as overclaiming ("every rung's
WASM client" when it names exactly two targets); **fix that overclaim now,
in this task**, rather than repeating it a third time — either add a plain
`cmake --build build-wasm-ladder` pass after the two named-target builds
(covering any future rung automatically, closing the gap rung 1's review
flagged) or rename the step to name exactly what it builds. Pick the
former: it is the one rung 1's own review suggested, and it means Task 19
of rung 3 will not need to touch this file at all.

```yaml
      - name: Build the WASM-remote spike and every rung's WASM client
        run: |
          export EM_CACHE="$PWD/.emcache"
          cmake --build build-wasm-ladder --target morph_ladder_wasm_spike
          cmake --build build-wasm-ladder --target ladder_pastebin_gui_wasm
          cmake --build build-wasm-ladder --target ladder_bookmarks_gui_wasm
          # Catches any further rung's WASM client too, without editing this
          # file again -- closing the gap rung 1's own final review flagged.
          cmake --build build-wasm-ladder
```

- [ ] **Step 4: Commit**

```bash
git add examples/bookmarks/gui_wasm/main_wasm.cpp .github/workflows/wasm-ladder.yml
git commit -m "bookmarks: add the WASM client and extend the WASM CI gate to cover it"
```

---

## Self-Review

**Spec coverage against `examples/bookmarks/README.md`:**

| README section | Covered by |
|---|---|
| "What to implement" 1 (CRUD + archive/unarchive + tag assignment) | Task 6 |
| "What to implement" 2 (search/list + pagination) | Task 7 |
| "What to implement" 3 (BulkEdit, atomic) | Task 8, atomicity proven in Task 15 |
| "What to implement" 4 (tag rename/merge, cascades) | Task 9 |
| "What to implement" 5 (Netscape import/export, message-size bound) | Task 11 |
| "What to implement" 6 (sharing, merged shared feed) | Task 10 |
| Sessions & authorization (real signed tokens, `authorizeRegister`/`authorizeInstance`) | Task 1, exercised end-to-end in Task 14/15/18 |
| Background-job pattern, service principal | Task 12 |
| Journal split-by-blast-radius (outbox for multi-row, default for single-row) | Task 8 (`BulkEdit`), Task 9 (`MergeTags`) |
| No generic undo | Design decision only, README + Global Constraints — no task implements `undoLast()`, by design |
| Model topology / shared feed (this plan's corrected design) | Task 6/9/10 (plain registration), Task 1 (one authorizer) |
| Bookmark<->tag many-to-many | Task 5 (junction entity, no embedded relation field) |
| Bulk-write mechanics (`SqlTransaction`, not `ExecuteBatch`) | Task 8 |
| Expected strain point: background fetch racing user edits | Not a dedicated task — `RecordMetadata`'s `Update()` on the same row a user's `EditBookmark` might concurrently touch relies on SQLite's own write serialization, the same argument rung 1's burn-race test documents; **gap**: no dedicated concurrency test proves this for bookmarks specifically. Flagged here rather than silently assumed — a fix-round or a rung-2-specific follow-up task should add a `BackendRig::Socket` race test mirroring pastebin's own, or explicitly accept the same "SQLite serializes writers so this doesn't discriminate the guard" caveat that test's own comment states. |
| Expected strain point: cross-model rename race | Task 16 |
| Expected strain point: local mode has no authorization | Task 15 |
| Expected strain point: Unicode tags (NFC/NFD, case) | **Gap, stated plainly**: this plan's Task 5/9 store tag names as plain `TEXT` with no normalization step and no dedicated Unicode test. The README asks this be picked and tested, not merely left to SQLite's default (byte-exact, case-sensitive) comparison. Not fixed in this plan — flagged for a follow-up task (a `RenameTag`/tag-creation normalization pass, e.g. NFC via a small dependency-free normalizer or documenting byte-exact comparison as the deliberate choice) rather than silently omitted. |
| Expected strain point: favicon/preview blobs (paths in SQLite, bytes on disk) | Task 5 (`favicon_path` column) — **gap**: no task actually writes bytes to disk (`NullMetadataFetcher` never produces a `faviconPath`); a real `IBookmarkMetadataFetcher` implementation is explicitly out of scope (Task 12's own justification), so this is inherently untestable beyond the column existing. Consistent with, not contradicting, that scope decision. |
| Expected strain point: import of thousands of bookmarks, chunked, idempotent | Task 11 (idempotency proven); **gap**: no test imports at real scale (thousands of entries) or proves a mid-import connection drop resumes correctly beyond the single-chunk-retry case Task 11 covers — the DoD's own bar is "chunked actions... must resume without duplicating," which the single-chunk idempotency test satisfies at the unit level but not at the "thousands of bookmarks across many chunks" scale the strain point names. Flagged, not smoothed over. |
| DoD: two users, isolated collections, working shared feed, `authorizeRegister`/`authorizeInstance` enforced | Task 14/15/18 |
| DoD: metadata auto-fetch as background job, `GetChangesSince` poll | Task 12/16, `GetChangesSince` in Task 7 |
| DoD: `BulkEdit` atomic under injected mid-batch failure | Task 15 |
| DoD: background-job design record written in the README | Already done, this session, before this plan was written |

**Placeholder scan**: none remaining — the two instances caught during this
plan's own writing (Task 6's copy-paste residue, Task 1's two-independent-statics
bug) were fixed in place, not left as notes, consistent with this document's
own "No Placeholders" standard.

**Type/signature consistency check**: `BookmarkId`/`TagId`/`Cursor` (Task 2)
are used identically in every DTO (Tasks 3/4) and every model (Tasks 6-10) —
`static_cast<std::uint64_t>(*id)` at every entity-boundary crossing,
`BookmarkId{static_cast<std::int64_t>(rec.id.Value())}` at every
entity-to-DTO crossing, consistently. `Count` (Task 2) is used identically
in `TagSummary::bookmarkCount`, `BulkEditResult::affected`,
`ImportBookmarksResult::imported`/`skipped` (Tasks 3/4). `AuthToken`/`Login`/
`LoginResult` (Task 12) are self-contained and touch no other DTO.
`BookmarksAuthorizer`'s exact `authorizeInstance`/`authorizeRegister`
signatures (Task 1) match `IAuthorizer`'s real declared signatures
verified against `include/morph/session/session.hpp` directly — not
guessed. `journal::LogEntry`'s field names (`idempotencyKey`, `principal`,
`timestampMs`, etc.) are used identically in Task 8's `writeOutboxEntry`,
Task 9's `MergeTags`, and Task 12's `relayOutboxOnce`, all verified against
`include/morph/journal/action_log.hpp` directly.

**Judgment calls this plan made that the original task breakdown did not
fully specify** (each with its reasoning, so a reviewer can assess them
rather than discover them mid-implementation):

1. **`BookmarkModel`/`TagModel`/`SharedFeedModel` are all registered
   plain, not `AllowShared`** — a correction to the README's own "shared
   instances keyed by principal" framing, forced by `remote.hpp:800`'s
   "shared instances are ownerless, by design," which would have made
   `authorizeInstance` a no-op for exactly the models that most need it.
   Documented at length in this plan's own "Corrections to the README"
   section. This is the single largest deviation from the brief's original
   framing, and it is a correctness fix, not a style preference — the
   README's original design would have shipped with **zero** real
   per-instance ownership enforcement.
2. **`RecordMetadata` bypasses the ownership check** other actions
   perform, since it is dispatched by the trusted service principal on
   behalf of an arbitrary owner. Mirrors `pastebin::ExpirePaste`'s
   identical internal-only shape.
3. **`AuthModel`/`Login` were added**, not named in the original task
   breakdown at all — a genuine gap the breakdown didn't anticipate: every
   other action requires a token, but nothing minted the *first* one. Dev-mode,
   no password, stated plainly as a scope decision in Task 12's own step
   comment, not smoothed over.
4. **`BookmarksAuthorizer::authorizeRegister` exempts `"AuthModel"`** —
   the necessary consequence of (3): the blanket "must be authenticated"
   gate cannot apply to the one action that exists to *become*
   authenticated.
5. **The process-global `TokenIssuer` holder** (`auth::setTokenIssuer`/
   `tokenIssuer`) — the same "registry-constructed models have no DI seam"
   answer `morph::journal::setActionLog` already established; not a new
   pattern invented for this rung.
6. **Tag associations are read via plain `Query<BookmarkTagRecord>()`
   calls, never `HasManyThrough`** — forced by the verified
   `DataMapper::Update()`/`HasMany`/`HasManyThrough` incompatibility (this
   plan's Global Constraints section), which the original task breakdown's
   framing ("both `BookmarkRecord`/`TagRecord` expose the inverse
   `HasManyThrough` for reads") did not anticipate.
7. **`kMaxTagNameBytes`/`kMaxUrlBytes`/`kMaxTitleBytes` are `validate()`-only
   sanity bounds, not `SqlAnsiString` storage-capacity checks** — a
   deliberate departure from `pastebin::kMaxSyntaxBytes`'s pattern, because
   these columns are plain `TEXT` (unbounded), and the whole point of
   `kMaxSyntaxBytes`'s `static_assert` was tying a bound to a *fixed*
   column's real capacity, which does not apply here.
8. **`BulkEdit` rejects the whole batch on the first unowned id**, not a
   skip-and-report partial result — the README's own "all-or-nothing"
   framing settles this, but the original task breakdown left both options
   open; this plan picks and documents the choice rather than leaving it
   for the implementer to guess mid-task.
9. **Two genuine coverage gaps are left open, not silently dropped**: the
   Unicode-tag-normalization strain point and the at-scale chunked-import
   strain point (see the spec-coverage table above). Both are named
   explicitly rather than claimed as done.

**Framework gaps discovered during this plan's own research that the
original task breakdown did not anticipate:**

- The `HasMany`/`HasManyThrough`-vs-`Update()` incompatibility (item 6
  above) — verified against Lightweight's own vendored source
  (`DataMapper.hpp`, `Description.hpp`), independently confirming
  `examples/bank/include/bank/db/account_entity.hpp`'s own comment for
  `HasMany` and extending the same proof to `HasManyThrough`. Not a new
  finding this plan files (Lightweight's own `AccountRecord` comment
  already documents the `HasMany` half; this plan's own Global Constraints
  section is where the `HasManyThrough` extension is recorded) — but worth
  a finding if a future rung hits it again without this plan's research to
  reference, per the promotion rule's spirit (a third independent
  rediscovery of the same gap is the signal to actually file one).
- The shared-instance-ownerless-by-design vs. plain-registration-real-owner
  distinction (`remote.hpp:800` vs. `remote.hpp:1011`) is not itself a
  framework *defect* — the doc comment at `remote.hpp:714-722` states the
  design intentionally and correctly — but it is a **documentation gap in
  this rung's own README**, which this plan's research corrected in the
  plan itself but has not yet corrected in `examples/bookmarks/README.md`
  proper. **A fix-round task, executed before or alongside Task 1, should
  update the README's "Model topology and the shared feed" bullet to match
  this plan's corrected design** — left as an explicit follow-up rather
  than silently diverging from the design-authority document this plan
  claims to follow.

## Execution Handoff

**Plan complete and saved to `docs/superpowers/plans/2026-08-07-ladder-rung2-bookmarks.md`.
Two execution options:**

**1. Subagent-Driven (recommended)** — I dispatch a fresh subagent per task,
review between tasks, fast iteration.

**2. Inline Execution** — Execute tasks in this session using
`executing-plans`, batch execution with checkpoints.

**Which approach?**

**If Subagent-Driven chosen:**
- **REQUIRED SUB-SKILL:** Use `superpowers:subagent-driven-development`
- Fresh subagent per task + two-stage review

**If Inline Execution chosen:**
- **REQUIRED SUB-SKILL:** Use `superpowers:executing-plans`
- Batch execution with checkpoints for review
