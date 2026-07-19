# Spec ↔ code drift guard in CI (planned)

> **Status: planned — not yet implemented.** This spec defines a CI check that
> pins mechanical facts asserted across the `docs/spec/` files against the actual
> code, so future drift fails the build. It is a process guard, not a library
> feature. See [todo.md](../todo.md).

## The gap

The recurring finding of the spec audit (the branch this work sits on,
`fix/spec-audit-remediation`) was **header docs and specs disagreeing with the
code**. [todo.md](../todo.md) enumerates the actual drift that shipped:

- the `authenticate` "principal-clearing" behavior a doc described wrongly,
- the false "unknown keys ignored" claim that predated the real
  `error_on_unknown_keys = false` behavior ([wire.md](../spec/core/wire.md)),
- a stale `runFor` comment,
- a stale `AuthError` cardinality (the enum grew, the doc did not).

Each is a *mechanical fact* — an enum's member count, a constant's value, a
canonical error string, a glaze parsing flag — that a spec states in prose and
the code states in a declaration. Nothing checks that the two agree, so they
drift silently until an audit catches them, and the next change re-introduces the
gap. `CLAUDE.md` makes specs "the authoritative design reference," which only
holds if the mechanical claims in them are enforced.

## Goal

A CI check that extracts a small set of **machine-checkable mechanical facts**
from the code and asserts they match the values the specs pin, failing the build
on any mismatch. It targets exactly the class of drift the audit found — not
prose or design intent, which cannot be mechanically checked — so it is a tight,
low-false-positive guard.

## Design

### What is pinned (the checkable facts)

A single source-of-truth manifest (a small data file, e.g.
`docs/spec/pinned_facts.toml`, NEW) lists the facts and their expected values.
The check reads the manifest, extracts the same facts from the code, and diffs.
The facts, drawn from the audit's failure classes:

| Fact class | Examples (verified real symbols) | Source of truth in code |
|---|---|---|
| **Enum cardinalities** | `AuthError` (`Malformed`/`BadSignature`/`Expired`/`NotYetValid`), `LogLevel` (5: `debug`/`info`/`warn`/`error`/`off`), `ReconnectOutcome` (3), `Metric` (once [observability.md](observability.md) lands) | the `enum class` declaration in the header |
| **Key constants** | `kMaxEnvelopeBytes` (`8 * 1024 * 1024`, [wire.md](../spec/core/wire.md)), `kMaxDecimalPlaces` ([ARCHITECTURE.md](../ARCHITECTURE.md), `morph::math`), `kClockSkewMs` (60s, [security.md](../spec/security.md)) | the `constexpr` definition |
| **Canonical error strings** | `BackendChangedError` = `"backend changed before completion resolved"`, `BridgeDestroyedError`, `DisconnectedError` ([backend.md](../spec/core/backend.md)); `err "unauthorized"`, `"model not found"`, `"register requires a typeId"` | the string literal in the throw/reply site |
| **Glaze behavior flags** | `error_on_unknown_keys = false` on `wire::decode` ([wire.md](../spec/core/wire.md)); duplicate-key last-wins (behavioral, asserted by a pinned test) | the `glz::read<{...}>` options at the call site |

The manifest is the one place a human states "the spec claims X"; the code is
scanned for the actual value; CI fails if they diverge. When a value legitimately
changes, the developer updates both the code and the manifest (and the prose spec)
in the same commit — which is exactly the discipline `CLAUDE.md` already requires
("If a change invalidates any part of a spec, update the spec").

### How the facts are extracted

Two complementary mechanisms, kept deliberately simple to avoid a brittle parser:

1. **A compiled assertion TU (NEW test).** For anything expressible as a
   compile-time or run-time equality — enum cardinality, constant values, error
   `what()` strings — a small test translation unit `#include`s the real headers
   and `static_assert`s / `EXPECT_EQ`s the value against the manifest constant
   (the manifest is rendered into a generated header of expected values at
   configure time, so the TU never hand-copies a value the manifest already
   states). This is the authoritative check because it uses the *actual*
   symbols, not a text scan — expected values shown inline for clarity: e.g.

   ```cpp
   // tests/test_pinned_facts.cpp — NEW.
   static_assert(morph::wire::kMaxEnvelopeBytes == 8 * 1024 * 1024);
   static_assert(static_cast<int>(morph::session::AuthError::NotYetValid) == 3);  // last member
   EXPECT_EQ(std::string{morph::backend::DisconnectedError{}.what()},
             "transport disconnected before completion resolved");
   ```

   A drift makes the test fail to compile or fail at run time — the strongest
   possible guard, since it binds to the symbol itself.

2. **A prose-vs-manifest lint (NEW CI script).** A lightweight script asserts the
   spec markdown actually *cites* the pinned value (e.g. that `wire.md` contains
   "8 MiB" / "`kMaxEnvelopeBytes`" and "error_on_unknown_keys = false"), so a spec
   cannot silently stop mentioning a fact the manifest still tracks. This catches
   the "doc quietly went stale" direction the audit found, where the code was
   right and the prose lagged. The same script carries a small
   **banned-terminology list** for phrasing that superseded designs left
   behind — the first entry is the pipe-delimited-era "*N*-part protocol"
   wording (the audit found three stale instances in `ARCHITECTURE.md` after
   the JSON `Envelope` superseded that protocol); a doc or comment
   reintroducing a banned term fails the lint the same way a missing citation
   does.

The compiled TU is the hard gate; the prose lint is the advisory nudge that keeps
the *documentation* honest, not just the manifest.

### Where it runs

A dedicated CI job (alongside the existing Docs/Doxygen job noted in `CLAUDE.md`),
fast enough to run per-commit. It has no runtime dependency and adds nothing to
the shipped library — it is purely a build-time verification, in the same spirit
as the Doxygen `FAIL_ON_WARNINGS` gate `CLAUDE.md` already documents.

## Non-goals

- **Not a prose/design checker.** It pins *mechanical* facts (numbers, enum
  sizes, literal strings, parser flags), not reasoning, invariants, or intent —
  those are what human review and the specs themselves are for. It cannot and does
  not try to verify that a design *makes sense*, only that a stated constant is
  the real constant.
- **Not a library feature.** Nothing ships in `include/morph/`; this is a test TU
  plus a CI script plus a manifest under `docs/`.
- **Not a replacement for updating specs.** The `CLAUDE.md` rule ("update the
  spec, not the other way around") still governs; the guard *enforces* a slice of
  it mechanically, it does not excuse skipping the prose update — the prose lint
  specifically pushes back on that.
- **Not an ABI/source-compat checker.** API stability is
  [api_stability.md](api_stability.md); this guard is about *documentation
  accuracy*, a different axis.

## Testing (planned)

- Introducing a real drift (change `kMaxEnvelopeBytes` in the header but not the
  manifest; add an `AuthError` enumerator without updating the pinned cardinality;
  alter a canonical error string) makes the pinned-facts TU fail to compile or
  fail at run time — proving the guard catches each audit-class regression.
- Removing a pinned value's mention from its spec markdown fails the prose lint.
- Reintroducing a banned term (e.g. "6-part protocol") anywhere in `docs/` or a
  source comment fails the prose lint.
- A legitimate coordinated change (code + manifest + prose in one commit) passes
  cleanly — no false positive.
- The job runs per-commit within the CI time budget and adds nothing to the
  library artifact.

## Cross-references

- [wire.md](../spec/core/wire.md) — `kMaxEnvelopeBytes`, the
  `error_on_unknown_keys = false` flag, and the duplicate-key behavior — three of
  the exact facts the audit found drifted.
- [security.md](../spec/security.md) — `AuthError` cardinality and `kClockSkewMs`,
  audit-class facts this pins.
- [backend.md](../spec/core/backend.md) — the canonical error-type `what()` strings and
  reply strings (`"unauthorized"`, `"model not found"`) pinned as literals.
- [logger.md](../spec/core/logger.md) — `LogLevel`'s 5-member cardinality.
- [api_stability.md](api_stability.md) — the complementary compatibility guard;
  drift-guard checks *doc accuracy*, api-stability checks *API compatibility*.
- `CLAUDE.md` — the "specs are authoritative; update the spec on any change" rule
  this mechanically enforces, and the existing Doxygen `FAIL_ON_WARNINGS` CI gate
  it sits beside.
