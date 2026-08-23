---
name: triage-issue
description: Analyse a morph issue against the framework's architecture before anyone implements it - decide whether it is well-framed, mis-framed, or false, verify its premise empirically, and label it. Use when asked to triage, analyse, review, or act on a GitHub issue in this repository.
---

# Triage a morph issue

An issue describes a symptom and proposes a fix. Both can be wrong, and in this
repository both regularly have been — not because the reporter was careless,
but because a symptom observed from inside one layer often reads as a gap in
the framework when it is really a consequence of the architecture working.

**The job is not to implement the issue. It is to decide what the issue
actually is**, then label it so the next reader inherits that judgement.

Roughly a third of the issues triaged so far turned out to need something other
than what they asked for. Budget for that outcome; it is the normal case, not a
failure.

## Workflow

### 1. Read the issue, then read what it is about

`gh issue view <n>`. Then read the **spec** for the subsystem it touches —
`docs/spec/` is the authoritative design reference (AGENTS.md), and
`docs/ARCHITECTURE.md` says explicitly: *"Where this document and a spec
disagree, the spec wins."*

Read the actual code too. An issue's quoted snippet is often abridged, and the
surrounding comment frequently answers the question — several issues here were
resolved by a comment already sitting three lines above the cited code.

### 2. Check the premise empirically, before evaluating the fix

**This is the highest-value step and the most often skipped.** An issue
reasoned out from reading source is a hypothesis. Test it.

- Can you reproduce the claimed behaviour in ten lines?
- Does the number it quotes still hold? Re-measure it.
- If it says "X is impossible", try X.

Cheap experiments that have changed verdicts here: a standalone `.cpp` compiled
with `-fsyntax-only`; a `LIGHTWEIGHT`-free `main()` calling the function
directly; `llvm-cov` on a five-line template; a throwaway CMake+moc project.

### 3. Test it against the architecture invariants

See [the invariants](#architecture-invariants) below. Most mis-framed issues
violate one, and naming which one is usually the whole analysis.

### 4. Decide a verdict, and label it

See [verdicts and labels](#verdicts-and-labels).

### 5. Write the verdict onto the issue

Comment with: what you verified and how, which invariant applies, and — if
rescoping — the corrected framing. Quote the evidence. Then apply labels.

If the verdict is `invalid` or `wrong-layer`, **say what would change your
mind**. A closed issue with a falsifiable re-open condition is useful; one
closed by assertion is not.

## Architecture invariants

Each of these has already caught a real mis-framed issue. Cited sources are
authoritative; check them rather than trusting this summary.

### 1. A model does not know the framework exists

A model implements actions. morph handles communication between the GUI and
the backend and executes those actions. A model that reaches into morph — to
schedule work, to fetch an executor, to inspect the bridge — has inverted the
layering.

*Corollary:* **a model is single-threaded per `ModelId`.**
`docs/spec/concurrency_and_lifetimes.md`: *"A model author writes
single-threaded code. For a given `ModelId`, the strand guarantees `execute()`
is never re-entered concurrently, so per-model state needs no locking."*

*Corollary:* **the unit of later work is another action.** A model's state
changes later by having an action dispatched at it, which re-enters on its
strand where mutation is safe. Anything that mutates model state off-strand is
a data race, however it is dressed up.

> Caught #129, which asked for a seam letting a model post background work and
> "later update its own state" — a thing that must not exist. The App layer
> owns background work; `bookmarks`' metadata fetcher re-entering as an
> ordinary dispatch is the architecture working, not a workaround.

### 2. A framework requirement on consumers must compose

Requiring every consumer to *derive from* a framework base is a large demand:
it constrains their hierarchy for what is usually an implementation detail, and
penalises types that already have a base, are `QObject`s, or are aggregates. A
data member composes; a base class does not.

> Caught #150, which introduced a `HasLifetime` base for callback liveness.
> Rescoped to a member-held token that also carries an explicit stop.

### 3. The value type reports the fact; the layer decides the policy

A type knows what it can represent. Only the surrounding layer knows what a
violation *means* — the same clamped value is a protocol violation off a socket
and a harmless normalisation from a local caller. Push policy outward, keep
facts in the type.

> Caught #131. The first fix put a process-wide policy and a throwing setter
> inside `Rational`. The decision belongs at the codec boundary
> (`ActionTraits<A>::fromJson`), because `morph::wire` carries an envelope's
> `body` as an opaque string and never parses it.

### 4. Templates parameterised on application types are the design

morph is header-only and its cost centre is instantiation of
`Completion<T>`/`BridgeHandler<Model>`/`ActionTraits<A>` against *the
application's* types. A library cannot pre-instantiate those. Issues proposing
to "just compile it" should be checked against what could actually move.

### 5. Spec and code disagreeing is itself the finding

Do not silently pick a side. Which one is wrong is a decision with consequences,
and it belongs to whoever owns the contract. Present both readings.

> #159: `forms.md` says `x-decimalPlaces` reconciliation is "an exact
> `Rational` re-rounding" after which "the value the handler stores is at the
> precision the schema advertised" (search `re-rounding` in that file); the
> code retags without changing the value. Filed with three resolutions rather
> than a unilateral fix.

### 6. A guarantee reimplemented per call site belongs in one place

Count the copies before proposing a primitive — and count them again before
rejecting one. Three hand-written workarounds carrying near-identical comments
is a rule-of-three trigger.

> #163: `ModelKey` admits only integrals and `std::string`, so a rung obeying
> `IMPLEMENTATION.md` rule 3 cannot use the keying macros at all. Three rungs
> had independently hand-written the same workaround.

### 7. Would this still pass if the feature did nothing?

Applies to any issue proposing a test, a gate, or a check — and to the tests you
write while resolving one. This repository has repeatedly produced controls that
appear to work while measuring nothing: a coverage script measuring a quarter of
the tree, a sanitizer job that would have been green with zero instrumentation,
an invalid `codecov.yml` behaving like a permissive one.

## Verdicts and labels

Apply exactly one `triage:` label, plus one `area:` label.

| Label | Meaning |
|---|---|
| `triage: valid` | Well-framed. The problem is real and the proposed direction fits the architecture. Implement as written. |
| `triage: rescope` | The problem is real; the framing or proposed fix is not. Rewrite the description before anyone builds it. |
| `triage: wrong-layer` | Asks for something the architecture forbids, or that belongs to a different layer. Usually closes. |
| `triage: unverified` | The premise is plausible but untested. Blocked on an experiment, not on a decision. |
| `triage: invalid` | The premise is false. Close with the evidence and a re-open condition. |
| `triage: parked` | Real, but deliberately deferred with an explicit re-entry condition. Do not implement; check the trigger. |

Area labels: `area: core`, `area: forms`, `area: journal`, `area: offline`,
`area: session`, `area: util`, `area: qt`, `area: ladder`, `area: ci`,
`area: docs`.

Add `bug` only for a defect in shipped behaviour — not for a missing feature,
and not for a spec/code disagreement (that is `triage: rescope` plus the area).

Create any missing labels first:

```bash
gh label create "triage: valid"       --color 0E8A16 --description "Well-framed; implement as written"
gh label create "triage: rescope"     --color FBCA04 --description "Real problem, wrong framing; rewrite before building"
gh label create "triage: wrong-layer" --color D93F0B --description "Belongs to a different layer, or the architecture forbids it"
gh label create "triage: unverified"  --color BFD4F2 --description "Premise plausible but untested; blocked on an experiment"
gh label create "triage: invalid"     --color B60205 --description "Premise is false"
gh label create "triage: parked"      --color C5DEF5 --description "Deferred with an explicit re-entry condition"
```

Then:

```bash
gh issue edit <n> --add-label "triage: rescope,area: core"
```

## Worked examples

Real triages, with what the evidence turned out to be.

**#129 → `wrong-layer`, closed.** Asked for a seam letting a model's
`execute()` post background work and later update its own state. Invariant 1:
background work is off-strand, so updating model state there is a data race —
and the motivating implementation already knew, capturing only plain values.
The narrower need (somewhere to get an executor, a test double) survived as
#160 and #161.

**#92 → `invalid`, closed.** Claimed `llvm-cov` emits per-instantiation `DA:`
records that nothing aggregates. Step 2 killed it: a five-line template with
two instantiations, exported in all three configurations including the
multi-`-object` form `coverage.sh` uses, produced **one** `DA:` record per
line with hits already summed. The proposed fix would have been a no-op.
Closed with the re-open condition: a real CI lcov showing two `DA:` records
for one source line.

**#138 → `rescope`.** The gap was real — 5 hand-rolled liveness tokens, 23
capture sites, 2 `QPointer` sites, and #137 was a real use-after-free. The
proposed fix (a base class) failed invariant 2, and it covered only half the
requirement: "the receiver was destroyed" and "the receiver stopped caring" are
different, and the second is the common case in a GUI. Rewritten around a
member-held token with a stop mechanism.

**#86 → `rescope`.** Title asserts boilerplate "unbounded with model count".
Re-measured at six rungs: 12.4 lines per QML *surface element*, with no
relationship to model count (`bank`: 11 models, 54 elements; `polls`: 1 model,
16). The growth law in the title was wrong, which changes which fix makes
sense.

**#159 → `rescope`, `area: forms`.** Spec and code disagree about whether
`x-decimalPlaces` enforcement re-rounds. Verified by running it: value
unchanged at `3858/3125`, only the tag moved. Filed with three resolutions,
because which side is wrong is a public-contract decision.

## Failure modes to avoid

- **Implementing a mis-framed issue faithfully.** The issue is a request, not a
  specification. Delivering exactly what it says, when what it says is wrong, is
  the most expensive outcome available.
- **Trusting a number in the issue.** Re-measure. Several have gone stale.
- **Undoing a deliberate fix.** Before changing something odd-looking, run
  `git log -S` on it. A per-commit cache key looked wasteful until its history
  showed it existed to stop branches deleting each other's caches.
- **Verdict by assertion.** "This is wrong because it violates the layering" is
  not an analysis until you name the invariant, quote the source, and say what
  would change your mind.
- **Silently widening scope.** If triage reveals two problems, file the second
  rather than folding it in.
