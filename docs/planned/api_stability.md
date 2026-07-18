# API stability / 1.0 commitment policy (planned)

> **Status: planned — not yet implemented.** This spec defines the versioning,
> deprecation, and compatibility policy `morph` commits to at 1.0. It is a
> project-governance document, not a library feature; it builds on the mechanical
> guards in [drift_guard.md](drift_guard.md) and the wire policy in
> [protocol_versioning.md](protocol_versioning.md). See [todo.md](../todo.md).

## Background

The API is still being corrected. This work sits on `fix/spec-audit-remediation`,
and [todo.md](../todo.md) is explicit: "The API is still being corrected ...
Before production adoption at scale, declare a supported version, a deprecation
policy, and ABI/source-compat expectations (header-only eases ABI but not
source)." Every planned item is deliberately "opt-in or backward compatible by default"
so the items "can largely land independently" ([todo.md](../todo.md)), which is the
right posture *pre*-1.0 — but there is no *declared commitment* a consumer can
depend on. A production adopter today has no statement of what may change under
them, or on what notice.

`morph` is **header-only** ([ARCHITECTURE.md](../ARCHITECTURE.md)), which changes
the compatibility calculus: there is no shared-object ABI to preserve (every
consumer recompiles against the headers), but **source compatibility** — the
public API surface a consumer's code names — is exactly what a header-only library
must govern, because a source break surfaces at *their* next compile.

## Goal

Declare, at 1.0, a concrete and enforceable compatibility policy: what the stable
public surface is, what semantic-versioning guarantees apply to it, a deprecation
window, and how source and wire compatibility are treated for a header-only
library. The policy is precise enough that [drift_guard.md](drift_guard.md)-style
CI can enforce the mechanical parts.

## Design

### 1. The stable surface is the per-topic public namespaces

[ARCHITECTURE.md](../ARCHITECTURE.md) already draws the line the policy adopts:
"The public surface is split per topic so callers always know whether a name is
part of the stable API or an implementation detail," and "every nested `detail`
namespace ... holds implementation symbols" that "callers never type directly."

The 1.0 commitment formalises this:

- **Stable:** every non-`detail` symbol in the public namespaces
  ([ARCHITECTURE.md](../ARCHITECTURE.md)'s namespace map) — `morph::log`,
  `morph::exec`, `morph::async`, `morph::model` (traits + `Loggable`),
  `morph::backend` (`LocalBackend`/`RemoteServer`/`SimulatedRemoteBackend`),
  `morph::bridge`, `morph::offline`, `morph::session`, `morph::journal`,
  `morph::math`, `morph::units`, `morph::time`, `morph::forms`, `morph::qt` — plus
  the registration macros (`BRIDGE_REGISTER_MODEL`, `BRIDGE_REGISTER_ACTION`,
  `BRIDGE_REGISTER_VALIDATOR`) and the wire `Envelope`/`kind` contract.
- **Not stable:** everything in a `detail` namespace. These "do appear in some
  public signatures (e.g. `Bridge`'s constructor takes
  `unique_ptr<backend::detail::IBackend>`)" ([ARCHITECTURE.md](../ARCHITECTURE.md))
  but a consumer never *names* them, so their shape may change. The policy states
  this explicitly so a consumer who reaches into `detail` is warned they are off
  the stable surface.

### 2. Semantic versioning applied to source compatibility

Because there is no ABI, versioning governs **source** compatibility of the stable
surface:

- **Major (2.0):** a breaking source change to the stable surface — removing or
  renaming a public symbol, changing a public signature incompatibly, or a
  behavior change that breaks a documented contract in `docs/spec/`.
- **Minor (1.x):** additive, source-compatible — new public symbols, new optional
  parameters with defaults, new opt-in behavior. This is the shape *every* planned
  `todo.md` item already takes ("opt-in or backward compatible by default"), so
  the entire planned roadmap fits within 1.x.
- **Patch (1.x.y):** bug fixes and doc corrections that do not change the stable
  surface or its documented behavior — the `fix/spec-audit-remediation` class of
  change.

A change is classified against the stable surface *and its `docs/spec/` documented
behavior* — the spec is the contract, per `CLAUDE.md`, so a behavior change that
contradicts a spec is a major even if the signature is unchanged.

### 3. Deprecation window

Before a stable symbol is removed or its behavior changed in a major:

- It is marked `[[deprecated("...")]]` with a pointer to the replacement, kept
  working for **at least one full minor release**, and its impending removal is
  recorded in the spec's Status/Limitations section.
- Only at the next major is it removed. This mirrors the action-evolution
  deprecation window in [protocol_versioning.md](protocol_versioning.md) — the two
  policies use the same "deprecate for a window, remove at a version bump"
  discipline, one for the C++ API and one for the wire.

### 4. Wire compatibility is versioned separately

The C++ API version and the **wire** protocol version are independent axes. The
wire's compatibility is governed by [protocol_versioning.md](protocol_versioning.md)'s
`kProtocolVersion` and additive-only action-evolution policy: a 1.x C++ release
may speak protocol version 1, and a wire break bumps `kProtocolVersion`
independently of the library's semantic version. A consumer therefore reasons
about two compatibility promises — "will my code still compile?" (this spec) and
"will my client still talk to that server?" ([protocol_versioning.md](protocol_versioning.md))
— and the policy states both, and that they move independently.

### 5. What CI enforces

The mechanical parts are enforced, not just documented, reusing the
[drift_guard.md](drift_guard.md) machinery:

- A public-symbol inventory (the stable surface) is tracked; removing or renaming
  a stable symbol without a major bump fails CI — the same
  pin-a-fact-and-diff mechanism the drift guard uses for constants and enums,
  extended to the public API roster.
- `[[deprecated]]` markers are required to reference a replacement and a target
  removal version, checked by a lint.
- The existing Doxygen `FAIL_ON_WARNINGS` gate (`CLAUDE.md`) already forces
  complete public-API docs; the stability policy piggybacks on it as the
  "everything public is documented" precondition.

## Non-goals

- **No ABI stability promise.** `morph` is header-only; there is no shared-object
  ABI to preserve and none is guaranteed. Consumers recompile against the headers;
  the promise is *source* compatibility of the stable surface, not binary.
- **Not a promise about `detail`.** Symbols in `detail` namespaces are explicitly
  outside the commitment and may change in any release; a consumer who names one
  is unsupported.
- **Not a feature freeze.** The policy governs *how* the surface evolves (additive
  in minors, breaking only in majors with a deprecation window), not *whether* it
  evolves — the whole `todo.md` roadmap is 1.x-compatible.
- **Not a change to any code.** This is governance: a `VERSION`, a policy
  document, `[[deprecated]]` discipline, and CI enforcement — no library behavior
  changes.
- **Does not supersede the wire policy.** Wire compatibility lives in
  [protocol_versioning.md](protocol_versioning.md); this spec references it and
  states the two version axes are independent, it does not restate or override it.

## Testing (planned)

- Removing or renaming a stable public symbol without a major-version bump fails
  the public-surface CI check; adding a new public symbol in a minor passes.
- A `[[deprecated]]` marker lacking a replacement pointer or removal-version note
  fails the deprecation lint; a well-formed one passes and the symbol still
  compiles and works through its window.
- A behavior change that contradicts a `docs/spec/` contract is flagged as a major
  (caught in review, aided by the [drift_guard.md](drift_guard.md) pinned-facts
  check when the change also alters a pinned constant/string).
- The declared C++ version and `kProtocolVersion`
  ([protocol_versioning.md](protocol_versioning.md)) can advance independently in
  CI without either check falsely failing the other.

## Cross-references

- [ARCHITECTURE.md](../ARCHITECTURE.md) — the per-topic public-namespace /
  `detail` split that defines the stable surface, and the header-only design
  decision that makes this a source- (not ABI-) compatibility policy.
- [protocol_versioning.md](protocol_versioning.md) — the independent wire /
  action-schema versioning axis and the shared deprecation-window discipline.
- [drift_guard.md](drift_guard.md) — the pinned-facts CI mechanism this reuses to
  enforce the public-symbol inventory and deprecation markers.
- `CLAUDE.md` — the "specs are the authoritative contract" rule (so a spec-behavior
  break is a major) and the Doxygen `FAIL_ON_WARNINGS` gate the policy builds on.
- [todo.md](../todo.md) — the "every item is opt-in / backward compatible" posture
  that places the entire planned roadmap inside 1.x.
