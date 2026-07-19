# Register authorization & opaque model ids (planned)

> **Status: planned — not yet implemented.** This spec extends the trust model
> in [security.md](../spec/security.md). It closes the two residual multi-tenant gaps
> that `authorizeInstance` (already shipped) does **not** cover: unauthorized
> `register`, and guessable sequential model ids. See [todo.md](../todo.md).

## Background: what already exists

`security.md` documents the shipped state:

- Every `execute` is gated by `IAuthorizer::authorize` (type-level).
- `IAuthorizer::authorizeInstance` (optional, defaults to allow-all) gates
  `execute` and `deregister` per **instance**, comparing the caller against the
  owner principal recorded at `register` time.
- The owner is the **verified** principal from `authenticate(env.session)` at
  register time (empty if the authorizer does not authenticate).

Two gaps remain, both called out in `security.md`'s threat model:

1. **`register` is type-unauthorized.** Any client that can reach the transport
   can create model instances. `authorizeInstance` cannot help — there is no
   instance yet at register time.
2. **Model ids are guessable.** `RemoteServer` assigns ids from a sequential
   counter (`_nextId.fetch_add(1) + 1`, `remote.hpp`). Even with
   `authorizeInstance` enforcing ownership, an attacker can enumerate live ids;
   the ownership check is the only thing stopping cross-tenant access, so a bug
   or misconfiguration there is immediately exploitable across a dense id space.

## Goal

Two independent, opt-in hardening steps a deployer can enable for a multi-tenant
`RemoteServer`:

1. **`authorizeRegister`** — an `IAuthorizer` hook consulted on every `register`,
   so a deployer can bound *who may create* instances (and of which types).
2. **Opaque model ids** — make server-assigned ids unguessable so id enumeration
   is not a usable primitive, turning ownership enforcement into defence-in-depth
   rather than the sole barrier.

Both default to today's behavior so existing single-trust-domain deployments are
unaffected.

## Part 1 — `authorizeRegister`

### API

A fourth optional method on `IAuthorizer` (`session.hpp`), mirroring
`authorizeInstance`'s opt-in shape:

```cpp
/// @brief Optional gate on model creation. Consulted on every `register`.
///
/// `authorize` and `authorizeInstance` both act on an existing instance; neither
/// can bound *who may create* one. This hook does. The DEFAULT allows all, so an
/// authorizer that does not override it keeps today's behavior.
[[nodiscard]] virtual bool authorizeRegister(
    const Context& ctx,
    std::string_view modelType) const { return true; }   // DEFAULT: allow
```

### Enforcement

In `RemoteServer`'s `register` handling (`remote.hpp`), before constructing the
instance:

1. Decode the `register` envelope (`typeId`, optional `contextKey`, `session`).
2. **Authenticate** the caller (`authenticate(env.session)`) — the same call that
   already records the owner principal — and stamp the verified principal onto
   `env.session.principal` (clearing it when `authenticate` returns `nullopt`),
   exactly as `dispatchExecute` already does, so the register decision keys on
   the *verified* identity, not the client's claim.
3. Call `authorizeRegister(session, typeId)`. A `false` return replies
   `err "unauthorized"` (with the request's `callId`) and **no instance is
   created**.
4. Only on `true` does the server run `ModelRegistryFactory::create(typeId)` and
   record the owner.

`handleInline` (the synchronous control path used for `register` from a worker
thread) runs the same check — the gate must hold on both entry points.

### Interaction with ownership

`authorizeRegister` and `authorizeInstance` compose: register decides *whether an
instance may be created and by whom*, then the recorded owner drives per-instance
`execute`/`deregister` decisions. A deployer typically installs both on one
authorizer (subclassing `SigningAuthorizer`), e.g. "only authenticated callers
may register; each instance is then private to its registrant."

### Backward compatibility

Default returns `true`. `AllowAllAuthorizer` and a plain `SigningAuthorizer`
impose no register restriction, so an unconfigured server behaves exactly as
today. `register` over the local path is unaffected (there is no authorizer on
`LocalBackend`; the factory closure constructs the instance directly).

## Part 2 — opaque model ids

### The change

Replace the sequential `_nextId` counter with a generator that produces
**unguessable** ids: an internal counter run through a **keyed** 64-bit
permutation whose key is drawn once at construction from a
`std::random_device`, so ids are non-sequential and not predictable from a
previously observed id. (The keying is essential: an *unkeyed* public mixing
function is invertible, so an attacker who observes one id could recover the
counter and predict the next.) The id stays a `std::uint64_t` on the wire
(`ModelId`), so the wire
format and `Envelope` are unchanged — only the *values* become opaque.

Requirements:

- **Uniqueness within a server.** The generator must not collide over a server's
  lifetime. A 64-bit space with a counter fed through a **bijective** keyed
  permutation (not raw `random()` draws) guarantees no collision until
  wraparound, which is unreachable in practice.
- **No cross-server meaning.** Ids remain backend-local, exactly as today
  (`HandlerBinding` re-registers on `switchBackend` and gets fresh ids); opaque
  ids do not change that contract.
- **Cheap and lock-free-ish.** Generation happens under the existing register
  path; an `std::atomic<uint64_t>` counter fed through the keyed permutation
  keeps it cheap.

### Why this is defence-in-depth, not the primary control

Opaque ids do **not** replace `authorizeInstance` — a caller who *observes* a
valid id (e.g. from its own prior register, or a leak) can still target it, so
ownership enforcement remains the actual authorization boundary. Opaque ids
remove *enumeration* as a cheap attack: without them, an attacker sweeps
`1, 2, 3, …` and only `authorizeInstance` stands in the way; with them, the
attacker must first learn a specific id. This narrows the blast radius of any
ownership-check bug.

### Backward compatibility

Ids remain `std::uint64_t` and opaque-by-value; no client parses or predicts
them today (they are handles echoed back verbatim), so making them random is
transparent to correct clients. Tests that assert specific id *values* (e.g.
"first register returns id 1") must switch to asserting *round-trip* identity
(the id returned by `register` is the id accepted by `execute`/`deregister`)
rather than a literal — which is the only contract the ids ever guaranteed.

## Non-goals

- **Not a replacement for the transport bound on who may connect.** Even with
  `authorizeRegister`, the transport should still restrict reachability
  (loopback bind, TLS + peer verification, network ACLs) per `security.md`.
- **Not rate limiting.** `authorizeRegister` gates *authorization*, not
  *frequency*; a client authorized to register can still register many instances.
  Per-connection register/rate caps remain the transport's job (`security.md`).
- **Not capability tokens.** Ids stay opaque handles, not signed capabilities;
  authorization is still decided server-side per call, not by possession of the
  id.

## Testing (planned)

- With an authorizer overriding `authorizeRegister` to deny a given `modelType`
  (or an unauthenticated caller), `register` replies `err "unauthorized"` and no
  instance is created; a subsequent `execute` against a guessed id fails
  "model not found".
- The default authorizer (and plain `SigningAuthorizer`) still register any type
  (backward compatibility).
- Opaque ids: two successive registers return non-adjacent, non-predictable ids;
  each returned id round-trips through `execute`/`deregister`; no collisions over
  a large register churn.

## Cross-references

- [security.md](../spec/security.md) — the trust model this extends; the shipped
  `authorize`/`authenticate`/`authorizeInstance` seam and the explicit "register
  is unauthorized / ids are guessable" limitations this spec closes.
- [backend.md](../spec/core/backend.md) — `RemoteServer` register/execute/deregister handling,
  `_nextId`, the `LogProvider`/owner recording, and the `make_shared` lifetime
  rule the new hooks slot into.
- [session.md](../spec/session/session.md) — `IAuthorizer`, `Context`, `authenticate`; the new
  `authorizeRegister` is declared alongside them.
- [wire.md](../spec/core/wire.md) — the `register` envelope (`typeId`, `contextKey`)
  and the envelope-level `session` the gate reads (wire.md documents `session`
  for `execute`; the register path already reads it for owner recording — see
  [security.md](../spec/security.md)); ids stay `uint64_t` so the envelope is
  unchanged.
