# AGENTS.md

Working agreements for any agent or contributor making changes in this
repository. Applies to every tool — the filename is the cross-tool convention,
not a Claude-specific one.

## File what you find

**Anything you discover during analysis gets an issue, even when it is outside
the task you are on.** A defect noticed in passing and left unrecorded is a
defect nobody will find again on purpose — the conditions that surfaced it were
incidental, and they will not recur to order.

This applies to the awkward cases especially:

- **Findings that contradict the work you are doing.** If a fix reveals the
  issue it closes was mis-framed, say so in the issue rather than quietly
  shipping around it.
- **Findings with weak evidence.** File them with the evidence you actually
  have, and label it as weak. "Observed once in CI, not reproduced in 25 local
  runs on another platform" is a useful record; silence is not.
- **Findings you cannot size.** An unquantified report beats an unrecorded one.

Do **not** fold an unrelated finding into the current change. File it, link it,
and keep the change you are making about one thing. If it belongs to the change,
it is not a separate finding.

### What a filed issue must contain

- **A verification status, stated plainly.** Say what you measured, on what
  revision, and what you did *not* verify. Distinguish "reproduced" from
  "inferred from reading the code" — both are legitimate, conflating them is
  not.
- **Real output**, not a paraphrase of it, wherever a claim rests on a
  measurement.
- **What would change the verdict** — the condition under which the issue should
  be closed, or re-opened.

### Labels

Apply descriptive labels (`bug`, `enhancement`, `documentation`, `area: *`).

**Do not apply a `triage: *` label by hand.** That verdict is the output of an
assessment, not a self-assessment by whoever wrote the issue — it is assigned by
running the `triage-issue` skill (`.claude/skills/triage-issue/`). An author
labelling their own issue `triage: valid` records only that they thought it
worth filing, which every open issue already implies.

## Verify rather than assert

A control that reports success while measuring nothing is the failure mode this
repository has hit most often — an invalid coverage config, a sanitizer job with
no instrumentation, a filter over an invariant body, a conformance suite that
drove neither implementation, a compiler cache reporting 97% over half the
build.

So, before claiming something works:

- **Ask whether the check would still pass if the feature did nothing.** If it
  would, it is not evidence. Mutate the feature and confirm the check fails.
- **Compare against what the thing is supposed to cover**, not just against its
  own report. A cache reporting a hit rate says nothing until you know how many
  compilations it was offered.
- **Run the measurement on a configuration where the feature can appear.**
  Measuring a rung-4 feature on a rung-2 build proves nothing about the feature.

State results as measured or as inferred, and never round the second up to the
first.

## Design specs (`docs/spec/`)

One file per public type or subsystem. There is **no size limit** on spec
files — a spec should be as long as precision requires. These are the
**authoritative design reference** — before making any change to a public type
or subsystem, read its spec file first. The spec captures the reasoning,
invariants, API surface, and design decisions that the code alone doesn't
document. If a change invalidates any part of a spec, update the spec (not
the other way around).

## Feature documentation (`docs/superpowers/`)

One file per feature, compressed reference documentation:

- Keep each file **under 500 lines**; be concise, not exhaustive.
- Describe **only the existing behavior, in present tense**. Never document
  the previous state, migrations, diffs, or task checklists — these files
  are not changelogs or implementation plans (git history covers that).
- When a feature changes, rewrite the affected sections to state the new
  current behavior.

## CI notes

- The Docs workflow runs Doxygen with `WARN_AS_ERROR = FAIL_ON_WARNINGS`:
  every public symbol needs complete `@param`/`@tparam`/`@return` docs or
  the build fails. Reproduce locally with
  `cmake -S . -B build -G Ninja -DMORPH_BUILD_DOCUMENTATION=ON -DMORPH_BUILD_TESTS=OFF -DMORPH_BUILD_EXAMPLES=OFF`
  then `cmake --build build --target doc`.
