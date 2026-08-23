# API stability & versioning policy

Cross-cutting governance spec: morph's semantic-versioning commitment, the
public/`detail` stable-surface split, the deprecation window, and how the C++
API version and the wire protocol version are independent axes. This is a
process document, not a description of a code type or subsystem — read it
before deciding whether a change is additive (minor), a fix (patch), or
breaking (major), and before removing or renaming anything in the stable
surface.

Related specs: [ARCHITECTURE.md](../ARCHITECTURE.md) (the per-topic
public/`detail` namespace split this formalises into a compatibility
promise), [wire.md](core/wire.md) (the `Envelope`/`kind` contract; its
"Protocol version negotiation" and "Action-evolution policy" sections are the
wire's own versioning and deprecation-window discipline — the independent
axis described below), `docs/spec/pinned_facts.toml` and
[CONTRIBUTING.md](../../CONTRIBUTING.md#quality-gates) (the pinned-facts
drift guard this policy's still-manual public-surface check could grow into
once it gains a symbol roster — see "Known limitation" below), `AGENTS.md`
(the "specs are the authoritative contract" rule this policy's major/minor/
patch classification relies on).

## Current version

morph is `0.1.0` (`CMakeLists.txt`'s `project(morph VERSION 0.1.0 ...)`),
mirrored in code by `include/morph/version.hpp`:

| Symbol | Meaning |
|---|---|
| `MORPH_VERSION_MAJOR` / `MORPH_VERSION_MINOR` / `MORPH_VERSION_PATCH` | Preprocessor version components. |
| `MORPH_MAKE_VERSION(major, minor, patch)` | Packs three components into one comparable integer, for `#if MORPH_VERSION >= MORPH_MAKE_VERSION(1, 2, 0)`-style feature checks. |
| `MORPH_VERSION` | `MORPH_MAKE_VERSION(MORPH_VERSION_MAJOR, MORPH_VERSION_MINOR, MORPH_VERSION_PATCH)` for the running release. |
| `morph::version::kMajor` / `kMinor` / `kPatch` | `constexpr int` mirrors of the macros, for code that prefers a typed constant. |
| `morph::version::kString` | `constexpr std::string_view` dotted version, e.g. `"0.1.0"`. |

`tests/test_version.cpp` cross-checks these constants against the
`PROJECT_VERSION_MAJOR`/`MINOR`/`PATCH` variables CMake derives from
`CMakeLists.txt`'s `project(VERSION ...)`, so the header and the build system
cannot silently disagree.

morph follows [Semantic Versioning 2.0.0](https://semver.org/). Per semver's
own rule for major version `0`, **morph has not yet made a 1.0 compatibility
promise**: any `0.x` release, including a patch release, may change anything
without a major bump — `0.1.0` is still initial development. The rest of this
document states the promise morph commits to **starting at 1.0.0**, published
now so it is public and reviewable before it takes effect, rather than
invented after the fact.

## The stable surface

The stable surface is every non-`detail` symbol in morph's per-topic public
namespaces, exactly as [ARCHITECTURE.md](../ARCHITECTURE.md)'s namespace map
defines them: `morph::log`, `morph::exec`, `morph::async`, `morph::model`
(traits and `Loggable`), `morph::backend` (`LocalBackend`/`RemoteServer`/
`SimulatedRemoteBackend`), `morph::bridge`, `morph::offline`, `morph::session`,
`morph::journal`, `morph::math`, `morph::units`, `morph::time`, `morph::forms`,
`morph::version`, `morph::qt` — plus the registration macros
(`BRIDGE_REGISTER_MODEL`, `BRIDGE_REGISTER_ACTION`, `BRIDGE_REGISTER_VALIDATOR`),
the `MORPH_VERSION*` macros above, and the wire `Envelope`/`kind` contract
([wire.md](core/wire.md)).

Every nested `detail` namespace is explicitly **not** part of the stable
surface. A `detail` symbol can still appear in a public signature — e.g.
`Bridge`'s constructor takes `unique_ptr<backend::detail::IBackend>`
([ARCHITECTURE.md](../ARCHITECTURE.md)) — but a caller never *names* a
`detail` type directly (construct the concrete class and let the conversion
happen implicitly). A caller who names a `detail` symbol anyway is outside
this policy's promise: that symbol's shape may change in any release,
including a patch.

## Semantic versioning applies to source compatibility, not ABI

morph is **header-only** ([ARCHITECTURE.md](../ARCHITECTURE.md)): there is no
shared-object ABI to preserve, and none is promised — every consumer
recompiles against the headers on every release. What semantic versioning
governs here is **source** compatibility of the stable surface — whether a
consumer's existing `#include`s and call sites still compile and behave the
same:

- **Major (`X`.0.0):** a breaking source change to the stable surface —
  removing or renaming a public symbol, an incompatible signature change, or a
  behavior change that breaks a contract documented in `docs/spec/`. A change
  is classified against the stable surface **and** its documented
  `docs/spec/` behavior together: per `AGENTS.md`, the spec is the contract,
  so a behavior change that contradicts a spec is a major even when the C++
  signature is unchanged.
- **Minor (`x`.Y.0):** additive and source-compatible — a new public symbol, a
  new optional parameter with a default, new opt-in behavior. Every item in
  [todo.md](../todo.md)'s roadmap is deliberately "opt-in or backward
  compatible by default," so the entire planned roadmap fits inside 1.x.
- **Patch (`x`.`y`.Z):** bug fixes and doc corrections that touch neither the
  stable surface nor its documented behavior.

## Deprecation window

Before a stable symbol is removed, or its documented behavior changed, in a
major release:

1. It is marked, in this exact shape, naming both the target removal version
   and the replacement:

   ```cpp
   [[deprecated("removed in <major>.<minor>.<patch>; use <replacement> instead")]]
   ```

   e.g. `[[deprecated("removed in 2.0.0; use morph::bridge::NewThing instead")]]`.
   `scripts/check_deprecated_markers.sh` enforces this exact shape in CI (the
   `deprecation-lint` job) — see "What CI enforces today" below.
2. It keeps working, unchanged, for **at least one full minor release**.
3. Its impending removal is recorded in the affected spec's
   Status/Limitations section.
4. It is removed only at the next major version.

This mirrors the deprecation-window discipline [wire.md](core/wire.md)'s
"Action-evolution policy" already applies to the wire's own action/result
evolution — mark a field deprecated, keep it working for at least one full
release, remove only at a `kProtocolVersion` bump: one discipline, applied to
the C++ source surface here and to the wire there.

## Two independent version axes: C++ source compatibility and the wire

The C++ API version (this document) and the **wire protocol version**
([wire.md](core/wire.md)'s `kProtocolVersion`) move independently:

- A 1.x release of the C++ library may speak the same wire protocol version
  throughout its whole 1.x line; a wire-breaking change bumps
  `kProtocolVersion` on its own schedule, unrelated to the library's
  major/minor/patch. [wire.md](core/wire.md)'s `"hello"` handshake
  (`RemoteServer::setSupportedVersionRange`) lets a server widen its accepted
  range to keep serving pre-bump clients through their own deprecation
  window, entirely independent of what the C++ library's own version is
  doing.
- A consumer therefore reasons about two separate promises: "will my code
  still compile against this morph release?" (this document) and "will my
  client still talk to that server?" ([wire.md](core/wire.md), "Protocol
  version negotiation"). Neither promise implies the other, and neither
  document restates the other.

## What CI enforces today

- **The declared version is internally consistent.** `tests/test_version.cpp`
  `static_assert`s `morph::version::kMajor`/`kMinor`/`kPatch`
  (`include/morph/version.hpp`) against the `PROJECT_VERSION_MAJOR`/`MINOR`/
  `PATCH` values `tests/CMakeLists.txt` forwards from `CMakeLists.txt`'s
  `project(morph VERSION ...)`, so the header and the build system cannot
  silently drift apart.
- **Deprecation markers are well-formed.** The `deprecation-lint` job
  (`.github/workflows/ci.yml`) runs `scripts/check_deprecated_markers.sh`
  against `include/morph`, failing the build if any `[[deprecated("...")]]`
  message does not name both a target removal version and a replacement in
  the shape given above.
- **Every public symbol is documented.** The existing Doxygen job (`AGENTS.md`,
  `.github/workflows/docs.yml`) already runs with `WARN_AS_ERROR =
  FAIL_ON_WARNINGS`, failing the build if any public symbol — including every
  symbol on the stable surface — lacks complete `@param`/`@tparam`/`@return`
  docs. This policy piggybacks on that gate rather than duplicating it: an
  "everything public is documented" precondition is a prerequisite for
  reasoning about the stable surface at all.
- **Individual mechanical facts cannot silently drift.**
  `docs/spec/pinned_facts.toml` pins specific mechanical facts that recur
  across specs — key constants, enum cardinalities, canonical error/reply
  strings — and two checks enforce them: `tests/test_pinned_facts.cpp`
  (real code vs. the manifest) and `scripts/check_spec_citations.sh` (the
  spec prose vs. the manifest). This is a narrower, already-shipped relative
  of the gap described next: it pins individual facts a human names in the
  manifest, not an enumerated roster of every symbol on the stable surface.

## Known limitation: no automated public-surface diff yet

There is currently **no CI check that fails a pull request for removing or
renaming a stable public symbol without a major-version bump** —
classifying a change as major/minor/patch is manual (author and reviewer
judgement) today. The pinned-facts drift guard described above
(`docs/spec/pinned_facts.toml`, `tests/test_pinned_facts.cpp`,
`scripts/check_spec_citations.sh`) has landed since this policy was first
drafted, and its manifest-plus-generated-header pattern is a plausible
foundation to build on: it already proves out "a human-pinned fact, checked
against the real code, at CI time." But its manifest format is flat
`KEY = value` scalar facts named one at a time, not an enumerated inventory
of every symbol on the stable surface — growing it into a public-symbol
diff (one entry per stable symbol, a CI job that fails on an unlisted
removal or rename) is new work that has not been done, not something that
falls out of the existing manifest for free. Until that work lands, this one
piece of the policy is enforced by review discipline alone.

## Non-goals

- **No ABI stability promise.** See "Semantic versioning applies to source
  compatibility, not ABI" above.
- **Not a promise about `detail`.** See "The stable surface" above.
- **Not a feature freeze.** The policy governs *how* the surface evolves
  (additive in minors, breaking only in majors with a deprecation window), not
  *whether* it evolves — the entire [todo.md](../todo.md) roadmap is
  1.x-compatible by its own "opt-in or backward compatible by default" rule.
- **No existing behavior changes.** This policy is governance plus tooling — a
  version header, a policy document, `[[deprecated]]` discipline, and CI
  enforcement — not a change to any existing public symbol's behavior.
- **Does not restate the wire policy.** Wire compatibility is
  [wire.md](core/wire.md)'s concern (its "Protocol version negotiation" and
  "Action-evolution policy" sections); this document states only that the two
  axes are independent, not the wire policy's own rules.
