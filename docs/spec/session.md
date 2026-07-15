# The `session` types — design

`morph::session` provides the per-call context that travels from the caller
through the bridge to the model, together with a pluggable authorizer that
remote servers use to gate action dispatch and, optionally, to authenticate the
caller.

The authentication *mechanism* — signed bearer tokens, `SigningAuthorizer`, and
the `RemoteServer` enforcement points — lives in
[security.md](security.md), the primary companion to this spec. This file
documents the `session` **types and their contracts**; it cross-references
security.md for the full trust model rather than duplicating it.

## Contents

- [Context — an open data bag](#context--an-open-data-bag)
- [IAuthorizer — gate for action dispatch](#iauthorizer--gate-for-action-dispatch)
- [The `authenticate` hook — the authoritative principal](#the-authenticate-hook--the-authoritative-principal)
- [AllowAllAuthorizer and `allowAllAuthorizer()`](#allowallauthorizer-and-allowallauthorizer)
- [Trust boundary](#trust-boundary)
- [Thread safety — `current()` and `ScopedContext`](#thread-safety--current-and-scopedcontext)
- [API reference](#api-reference)
- [Design decisions](#design-decisions)
- [Limitations](#limitations)
- [Cross-references](#cross-references)

## Context — an open data bag

`Context` is a plain struct with five fields that callers populate however they
see fit. The framework only inspects `principal`, `token`, and the action ids
when consulting the configured `IAuthorizer`; everything else is passed through
verbatim from caller to model.

| Field | Type | Purpose |
|---|---|---|
| `principal` | `std::string` | Auth principal — user/identity id. A client *claim* until a verifying authorizer overwrites it (see below); authoritative for model code only after that. |
| `token` | `std::string` | Bearer credential verified server-side. Typically a signed token minted by a login action via `session::TokenIssuer` and attached to every call. Empty when unauthenticated. |
| `requestId` | `std::string` | Stable id for distributed tracing / log correlation. Empty when unused. |
| `locale` | `std::string` | BCP-47 locale tag (`en-US`, `fr-FR`) for i18n. Empty for app default. |
| `metadata` | `std::unordered_map<std::string, std::string>` | Free-form bag for feature flags, A/B buckets, app-specific metadata. |

On remote backends the entire `Context` — including `token` — is serialised into
the wire envelope's `session` field (see [wire.md](wire.md)) so `RemoteServer`
sees the same values the GUI sent. On the local backend it travels in-memory via
`ActionCall`.

`token` is the credential the server *verifies*; `principal` is the identity the
server *derives* from that credential. See
[the `authenticate` hook](#the-authenticate-hook--the-authoritative-principal)
for how the derived principal replaces the client's claim, and
[security.md](security.md) for the token format and login flow.

## IAuthorizer — gate for action dispatch

`IAuthorizer` is an abstract interface called once per `execute` envelope,
before the action is dispatched. A `false` return causes the server to reply
with `err|unauthorized` (the client surfaces the error through `.onError(...)`).

```cpp
[[nodiscard]] virtual bool authorize(
    const Context& ctx,
    std::string_view modelType,
    std::string_view actionType) const = 0;
```

The three parameters represent the caller's session, the target model **type**
string id, and the action **type** being invoked. Note that `authorize` sees
only *type* ids, never the target model **instance** id — row/instance-level
authorization (e.g. "may this principal edit *this* account?") is not
expressible here and belongs inside the model's `execute()`.

Real deployments subclass this to check principal claims, action permissions,
rate limits, etc. `authorize` is `const` and stateless by contract; any stateful
policy (rate-limit counters, revocation lists) must live in the subclass's own
members, guarded for concurrent access — the framework calls it from the
dispatch thread and gives it no per-call mutable state.

## The `authenticate` hook — the authoritative principal

`IAuthorizer` has a second, **optional** virtual:

```cpp
[[nodiscard]] virtual std::optional<std::string>
authenticate(const Context& ctx) const { return std::nullopt; }
```

The default returns `nullopt`, meaning "I do not authenticate — leave the
client's principal untouched". `RemoteServer` calls `authenticate` **after
`authorize` has already succeeded**, and if it returns a value, the server
**overwrites `Context::principal`** with that value before building the
`ScopedContext` around dispatch. From then on, a model reading
`session::current()->principal` sees the *verified* identity the authorizer
vouched for, not the string the client sent.

This split keeps the two concerns separate: `authorize` answers "is this call
permitted?" and `authenticate` answers "who is actually making it?". An
authorizer may implement either, both, or (via the default) only the first.

- `AllowAllAuthorizer` uses the **default** `authenticate` — it performs no
  authentication, so the client's `principal` passes through unchanged.
- `SigningAuthorizer` (`session_auth.hpp`) **overrides** it: it verifies
  `Context::token` and returns the token's `principal`, so the authenticated
  identity becomes authoritative for model code.

The full mechanism — token format, verification order, `RemoteServer`'s
`dispatchExecute` call site, and the login flow — is documented in
[security.md](security.md). This spec only fixes the *contract*: `authenticate`
is consulted post-`authorize`, and its return, when present, replaces the
principal.

## AllowAllAuthorizer and `allowAllAuthorizer()`

The framework ships a default authorizer that permits every call and performs no
authentication (it inherits the default `authenticate`). It is used as the
default by `RemoteServer` and can also be wired explicitly.

`allowAllAuthorizer()` returns a `std::shared_ptr<IAuthorizer>` pointing at a
process-wide singleton. This avoids allocating a new shared_ptr per server
instance when the default is sufficient.

`AllowAllAuthorizer` is **fail-open**: it is convenient for local and simulated
development and wrong for production. See [Trust boundary](#trust-boundary) and
[security.md](security.md) ("The default is fail-open") for why an exposed
server must replace it.

## Trust boundary

The authorizer exists because a `Context` arriving over the wire is untrusted
input. The rules that govern where trust begins:

- **On the remote path, every `Context` field is unauthenticated wire input.**
  `principal`, `token`, `requestId`, `locale`, and `metadata` are all
  deserialised verbatim from the envelope the client sent. `principal` in
  particular is a *claim*, not an identity, until a verifying authorizer's
  `authenticate` overwrites it. Model code must not treat any field as trusted
  before that point.
- **The local backend does not authorize at all.** `authorize` is a
  **remote-only gate**: `LocalBackend::execute` installs the session context but
  never consults the authorizer (its sole call site is `RemoteServer`'s
  `dispatchExecute`, see [backend.md](backend.md)). Any security-critical check
  must therefore be enforced inside the model so it holds in both local and
  remote modes.
- **The `RemoteServer` default authorizer is fail-open.** An unconfigured server
  uses `allowAllAuthorizer()` and permits everything. Production deployments must
  install a verifying, deny-by-default authorizer.
- **`authorize` sees only type ids, not the instance id.** Instance-/row-level
  authorization is not expressible at this layer and belongs in the model.

The registry that maps those type ids to runners is described in
[registry.md](registry.md); the enforcement points and the full threat model are
in [security.md](security.md).

## Thread safety — `current()` and `ScopedContext`

Models that need session data can access it without changing their
`execute()` signature. The mechanism is a thread-local pointer (the
`detail::tlsCurrent()` `const Context*&`) installed by the backend for the
duration of the model invocation.

**`current()`** returns `const Context*` — the active context for the
in-progress action, or `nullptr` when called outside a dispatch. It is declared
`noexcept`. Models that don't need session data ignore it entirely.

**The context is thread-local and dispatch-scoped.** The backend installs the
pointer on the exact thread that runs the model call and clears it when that call
returns. It is therefore valid **only on that dispatch thread**:

- `current()` returns `nullptr` on any thread the model *spawns*, and on async
  work the model *schedules* to run later — the thread-local is not propagated
  across thread boundaries.
- A model that needs session data after crossing a thread boundary must
  **capture what it needs** (copy the `principal`, `locale`, or specific
  metadata values) *before* handing work to another thread, rather than calling
  `current()` from the other side.

**`ScopedContext`** (in `detail` namespace) is the RAII helper that installs a
`Context` pointer for its scope and restores the **previous** one on
destruction. Because it saves and restores the prior pointer rather than
clearing to `nullptr`, nested dispatch composes correctly — an inner
`ScopedContext` shadows the outer context and the outer one is restored when the
inner scope exits. Construction is `explicit`, and copy and move are all four
deleted. The stored pointer is the address of the referenced `Context`, which
must outlive the `ScopedContext`.

It is used by `LocalBackend::execute` to wrap `localOp(*holder)` and by
`RemoteServer::dispatchExecute` to wrap `ActionDispatcher::dispatch`, so the
context is live while the model's `execute()` runs. `ActionDispatcher::dispatch`
itself does not touch the thread-local pointer — it only looks up and invokes the
registered runner.

A usage example from the bank example (`bank/core/principal.hpp`):

```cpp
if (const auto* ctx = morph::session::current(); ctx != nullptr) {
    // use ctx->principal, ctx->locale, etc.
}
```

## API reference

### `Context`

| Member | Signature | Notes |
|---|---|---|
| `principal` | `std::string` | Auth principal; a client claim until a verifying authorizer's `authenticate` overwrites it. |
| `token` | `std::string` | Bearer credential verified server-side; travels in the wire envelope's `session`. Empty if unauthenticated. |
| `requestId` | `std::string` | Trace id; empty when unused. |
| `locale` | `std::string` | BCP-47 locale; empty for default. |
| `metadata` | `std::unordered_map<std::string, std::string>` | Free-form metadata bag. |

### `IAuthorizer`

| Member | Signature | Notes |
|---|---|---|
| `~IAuthorizer()` | `virtual ~IAuthorizer() = default` | Virtual destructor for polymorphic use. |
| `authorize` | `[[nodiscard]] virtual bool authorize(const Context&, std::string_view modelType, std::string_view actionType) const = 0` | Returns `true` to allow dispatch, `false` to reject. Called per `execute` envelope. Sees only type ids. |
| `authenticate` | `[[nodiscard]] virtual std::optional<std::string> authenticate(const Context&) const` | Optional. Default returns `nullopt`. Called after `authorize` succeeds; a returned value overwrites `Context::principal`, making it authoritative. |

### `AllowAllAuthorizer`

| Member | Signature | Notes |
|---|---|---|
| `authorize` | `[[nodiscard]] bool authorize(const Context&, std::string_view, std::string_view) const override` | Always returns `true`. All parameters ignored. |
| `authenticate` | (inherited default) | Not overridden — returns `nullopt`, so the client's principal is left untouched. |

### Free functions

| Symbol | Signature | Notes |
|---|---|---|
| `allowAllAuthorizer()` | `std::shared_ptr<IAuthorizer> allowAllAuthorizer()` | Returns a process-wide singleton `AllowAllAuthorizer`. |
| `current()` | `const Context* current() noexcept` | Returns the active context, or `nullptr` when none / on a non-dispatch thread. |

### `detail::ScopedContext`

| Member | Signature | Notes |
|---|---|---|
| ctor | `explicit ScopedContext(const Context& ctx)` | Saves the current thread-local context and installs `ctx`. `ctx` must outlive this object. |
| dtor | `~ScopedContext()` | Restores the **previously** active context (composes under nesting). |
| (copy/move) | deleted | Non-copyable, non-movable. |

## Design decisions

| Decision | Choice | Why |
|---|---|---|
| Context shape | **Plain struct, not polymorphic** | The context is a data bag, not a behaviour abstraction; a struct is simpler to construct, serialise, and consume. |
| Thread-local context | **`const Context*` TLS pointer, installed by the backend around the model call** | Models read session data without changing `execute()` signatures; the pointer is `const` (immutable during dispatch) and RAII-guarded by `ScopedContext`. |
| `ScopedContext` restores the previous pointer | **Save/restore rather than clear-to-null** | Nested dispatch (a model dispatching into another) composes: the inner context shadows the outer and the outer is restored on exit. |
| Authorizer granularity | **Per-envelope, before dispatch, type ids only** | One call per `execute`, not per sub-operation; instance-level authz is left to the model, which alone knows instance identity. |
| Authentication as a separate hook | **`authenticate` distinct from `authorize`, optional with a `nullopt` default** | Separates "is this permitted?" from "who is it?"; authorizers that don't authenticate cost nothing, and the verified principal — not the client's claim — becomes authoritative when one does. |
| Singleton authorizer | **Static local in `allowAllAuthorizer()`** | All trivial `RemoteServer` instances share one `AllowAllAuthorizer` allocation rather than each owning one. |
| Serialisation | **Entire `Context` travels on the wire** | The remote backend's `RemoteServer` sees the same `principal`, `token`, `requestId`, `locale`, and `metadata` the GUI sent; no information is stripped. |

## Limitations

- **`principal` carries no integrity on its own.** It is a plain string in a
  serialised struct; nothing in the type prevents a client from sending any
  value. It becomes trustworthy only after a verifying authorizer's
  `authenticate` overwrites it.
- **Authentication depends on the transport plus a verifying authorizer.** The
  `token` is a bearer credential with no envelope-level confidentiality or
  replay protection; its guarantees hold only under TLS (so the token cannot be
  captured and replayed) *and* with an authorizer such as `SigningAuthorizer`
  installed. A plain `RemoteServer` with the default authorizer authenticates
  nothing.
- **Stateful authorization policy needs the impl's own state.** `authorize` is
  `const` and receives no mutable per-call state, so rate limits, revocation
  lists, or lockout counters must be stored in (and synchronised by) the
  authorizer subclass itself.
- **`current()` is dispatch-thread-only.** It returns `nullptr` off the dispatch
  thread; session data needed across a thread boundary must be captured first.

See [security.md](security.md) for the complete threat model and hardening
checklist (TLS, message-size bounds, control-message authorization, secret
rotation).

## Cross-references

- [security.md](security.md) — **the authentication subsystem**: signed bearer
  tokens, `SigningAuthorizer`, the `RemoteServer` enforcement points, the threat
  model, and hardening guidance. The primary companion to this spec.
- [wire.md](wire.md) — the envelope the `Context` (including `token`) is
  serialised into on the remote path.
- [backend.md](backend.md) — `RemoteServer` / `LocalBackend` dispatch, and where
  the authorizer is (and is not) consulted.
- [registry.md](registry.md) — the model/action type registry behind the type
  ids `authorize` receives.
