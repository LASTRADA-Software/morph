# Protocol / action-schema versioning & negotiation (planned)

> **Status: planned — not yet implemented.** This spec extends
> [wire.md](../spec/wire.md) (the `Envelope` and its forward-compatible decode) and
> [security.md](../spec/security.md). It adds a negotiated protocol version on
> connect and a documented action-evolution policy, on top of the passive
> forward-compatibility the wire layer already gives. See [todo.md](../todo.md).

## The gap

The wire has *passive* forward compatibility but no *negotiated* version:

- **Unknown keys are ignored.** `wire::decode` reads with
  `glz::read<{.error_on_unknown_keys = false}>`, so "a newer peer may add a field
  an older peer does not know (and vice versa) without breaking the parse — this
  is the wire's forward-compatibility contract" ([wire.md](../spec/wire.md)). A
  new `Envelope` field is absorbed silently by an old peer.
- **But there is no version field and no handshake.** Neither the `Envelope`
  ([wire.md](../spec/wire.md) API reference lists `kind`, `callId`, `typeId`,
  `contextKey`, `modelId`, `modelType`, `actionType`, `body`, `message`,
  `session` — no version) nor the connect flow carries a protocol version. A
  peer cannot discover *whether* the other side speaks a compatible protocol; it
  can only find out by a request failing in some field-specific way.
- **No action-evolution policy.** An action struct (the JSON inside the opaque
  `body`) can change shape between a client build and a server build. Because
  `body` is re-parsed by the action codec ([wire.md](../spec/wire.md)'s "body
  double-parse"), a removed or retyped field is a per-action decode failure with
  no framework-level story for it. There is no written rule that keeps action
  evolution additive-only, and no deprecation window.

Passive forward-compat handles *additive* changes gracefully but gives no
diagnostic, no compatibility check, and no policy for *removals or retypes* — the
changes that actually break peers.

## Goal

Two additive pieces, both defaulting to today's behavior:

1. **A negotiated protocol version** — a version field exchanged on connect so a
   peer learns the other side's protocol range up front and can refuse or adapt,
   instead of discovering incompatibility mid-request.
2. **A documented action-evolution policy** — additive-only field changes, a
   deprecation window, and the versioning discipline that makes the passive
   forward-compat contract *safe to rely on* rather than accidental.

## Design

### 1. Protocol version constant and envelope field (NEW)

Add a wire-layer version constant and an optional `Envelope` field:

```cpp
// namespace morph::wire — NEW.
inline constexpr std::uint32_t kProtocolVersion = 1;  // bumped on a breaking wire change

struct Envelope {
    // ... existing fields (kind, callId, typeId, ...) unchanged ...
    std::uint32_t protocolVersion{0};   // NEW: 0 == "unspecified / legacy peer"
};
```

- `protocolVersion` defaults to `0`, which decodes for **every existing peer**
  (an old encoder never sets it; `decode` leaves absent keys at their default per
  [wire.md](../spec/wire.md)). `0` means "legacy / unspecified" and is treated
  exactly as today's unversioned behavior — full backward compatibility.
- Because unknown keys are ignored, a new client sending `protocolVersion` to an
  old server is harmless (the field is dropped), and an old client omitting it is
  read as `0`. The field piggybacks on the existing forward-compat contract; no
  wire-shape break.
- `kMaxEnvelopeBytes` and the `kind` discriminator are unchanged.

### 2. A negotiation on connect (NEW)

Introduce a `"hello"` control `kind` exchanged once per connection, before any
`register`/`execute`:

| `kind` | Direction | Fields | Purpose |
|---|---|---|---|
| `"hello"` | request | `protocolVersion` (client's max) | Client announces the protocol version it speaks. |
| `"ok"` | reply | `body` = server's `{min,max}` supported range | Server confirms and states its range. |
| `"err"` | reply | `message` = `"protocol version unsupported"` | Server cannot satisfy the client's version. |

- The **client** (`QtWebSocketBackend`, `SimulatedRemoteBackend`) sends `"hello"`
  with `kProtocolVersion` immediately after connect (over the existing synchronous
  control path — `handleInline` for the simulated backend, the nested-loop
  `sendSync` for Qt, per [backend.md](../spec/backend.md)).
- The **server** (`RemoteServer`) answers with its supported `{min, max}` range.
  If the client's version is outside the range, it replies `err` and the client
  refuses to proceed (surfaces a clear "protocol version unsupported" rather than
  a mysterious per-request failure later).
- **Backward compatibility:** a server that never receives a `"hello"` (a legacy
  client) behaves exactly as today — `"hello"` is opt-in and its absence is the
  `protocolVersion == 0` legacy path. A legacy server that does not understand
  `"hello"` replies `err "unknown envelope kind: hello"` (the existing
  unrecognised-kind path, [backend.md](../spec/backend.md)); the client treats
  that specific error as "peer is unversioned / legacy" and continues without
  negotiation.

Negotiation happens on the control path, so it composes with the existing
authentication flow: `"hello"` carries no `session` and is not authorized (it
predates any `execute`); the version check is orthogonal to
`IAuthorizer::authorize` ([security.md](../spec/security.md)).

### 3. Action-evolution policy (documented rules, NEW)

The version field is only meaningful with a discipline that keeps action structs
compatible within a protocol version. The policy:

- **Additive-only within a major version.** New fields on an action or result
  struct must be *optional* (a `std::optional<...>`, a `Quantity`/`Timestamp`
  empty-capable field, or a type with a safe default) so an older peer that omits
  them decodes cleanly and a newer peer that receives them ignores unknowns. This
  is exactly what the passive forward-compat contract already permits — the policy
  makes it a *rule*, not luck.
- **Never renumber or rename** protocol vocabulary. This mirrors the existing unit
  enum rule ("Unit ids are protocol vocabulary: append enumerators, never
  renumber or rename," [ARCHITECTURE.md](../ARCHITECTURE.md)). Renaming a field is
  a removal + an addition = a break.
- **Deprecation window.** A field being removed is first marked deprecated (kept
  on the wire, ignored by new code) for at least one library release, then
  removed only at a `kProtocolVersion` bump. `kProtocolVersion` (a single
  integer, not major.minor) bumps only on a *breaking* change; additive changes
  do not bump it.
- **Removals/retypes require a version bump.** Any non-additive change increments
  `kProtocolVersion`, and the server advertises a `{min, max}` range that drops
  support for the old version only after the deprecation window.

This policy lives in `wire.md` (rewritten on implementation) and is the contract
`api_stability.md` builds its wire-compat guarantees on.

## Non-goals

- **Not per-action schema negotiation.** The handshake negotiates the *protocol*
  version (envelope shape, control kinds), not each action's schema. Action
  compatibility is governed by the additive-only policy, not a runtime schema
  exchange. `forms::schemaJson<A>()` already lets a client discover an action's
  current shape; this spec does not add a second mechanism.
- **Not automatic migration of stored data.** The journal's `LogEntry` payloads
  ([journal.md](../spec/journal.md)) are historical facts; replaying an old
  action across a version boundary is the host's concern, bounded by the same
  additive-only policy. No transform layer ships.
- **Not a break in the forward-compat contract.** Unknown keys stay ignored;
  `protocolVersion == 0` stays fully supported. Versioning *adds* a diagnostic and
  a policy on top of passive compat, it does not replace it.
- **Does not change local mode.** `LocalBackend` has no wire and no peer; there is
  nothing to negotiate.

## Testing (planned)

- A `protocolVersion` field round-trips through `encode`/`decode`; an envelope
  encoded without it decodes as `0` (legacy), and a new field on a peer's envelope
  is still ignored by an old decoder (forward-compat regression guard).
- A client `"hello"` at `kProtocolVersion` gets an `ok` with the server's
  `{min,max}` range; a client version outside the range gets
  `err "protocol version unsupported"` and refuses to proceed.
- A legacy server (no `"hello"` support) replies `err "unknown envelope kind:
  hello"`; the client treats it as unversioned and continues (backward compat).
- Additive-only evolution: an action gains a new optional field; an old client
  and new server (and vice versa) interoperate with no error. A field removal
  without a version bump is caught by a compatibility test (policy guard).

## Cross-references

- [wire.md](../spec/wire.md) — the `Envelope`, the `kind` discriminator, the
  `error_on_unknown_keys = false` forward-compat contract this builds on, the
  `body` double-parse that makes action evolution a per-action decode concern, and
  `kMaxEnvelopeBytes`.
- [backend.md](../spec/backend.md) — `RemoteServer` control-message handling
  (`handleInline`, the unrecognised-`kind` error path the legacy-detection reuses)
  and the client control paths (`QtWebSocketBackend` `sendSync`,
  `SimulatedRemoteBackend`) the `"hello"` exchange rides on.
- [security.md](../spec/security.md) — why the version handshake is orthogonal to
  authorization (it predates any authorized `execute`).
- [api_stability.md](api_stability.md) — the 1.0 policy that treats
  `kProtocolVersion` and the additive-only action rule as part of the supported
  compatibility surface.
- [ARCHITECTURE.md](../ARCHITECTURE.md) — the "append, never renumber or rename"
  protocol-vocabulary rule this generalises from unit enums to action structs.
