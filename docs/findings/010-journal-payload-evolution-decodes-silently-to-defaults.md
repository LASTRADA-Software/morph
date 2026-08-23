---
id: 010
title: A journal entry written by an older build decodes leniently to defaults, with no signal that a field was lost -- so "reconstructible from the journal alone" is false across a rename or retype
subsystem: journal
severity: major
source: lims rung 6, build order §6 (the rung the README makes the owner of this answer)
disposition: open
test: examples/lims/tests/test_verification_audit.cpp (case "A renamed payload field decodes to a default, silently -- the payload-evolution gap")
---

`morph::journal` stores an action's request and result as opaque JSON
(`LogEntry::payload` / `LogEntry::result`), and replay decodes them with the
**current** action structs. `ActionTraits<A>::fromJson` /
`resultFromJson` read with `error_on_unknown_keys = false`
(`include/morph/core/registry.hpp`), so a line written by an older build:

- silently **drops** any key the current struct no longer has, and
- silently **defaults** any field the current struct has but the line does not.

Nothing reports either. Not the codec, not `LogEntry`, not the reader. The
decode succeeds and returns a plausible value that is not what was recorded.

`LogEntry::v` does not help: it versions the *line format*
(`kLogFormatVersion`, bumped only for a breaking change to `LogEntry`'s own
shape), not the application payload inside it. Two entries with different
application payload shapes are both `v == 1`.

## Measured

```cpp
// A journal line from an older build, where `reference` was called `ref`.
const auto older = R"({"clientId":1,"ref":"WW-1"})";
const auto decoded = morph::model::ActionTraits<lims::RegisterSample>::fromJson(older);
CHECK(decoded.reference.empty());       // passes: "WW-1" is gone
CHECK(decoded.clientId == lims::ClientId{1});

// The result half, which an audit trail actually reads, fails the same way.
const auto olderResult =
    R"({"id":1,"clientId":1,"reference":"WW-1","sampleState":"Published","version":9})";
const auto view = morph::model::ActionTraits<lims::ReceiveSample>::resultFromJson(olderResult);
CHECK(view.state == lims::SampleState::Registered);  // passes: NOT Published
CHECK(*view.version == 9);
```

Both assertions pass today. The second is the damaging one: an audit trail
reconstructed from that entry reports the sample as `Registered` when the
journal says it was `Published`, and reports it with full confidence.

## Why the rung's own mitigation does not cover it

`lims` reconstructs its audit trail from the journal alone and marks any
entry it cannot interpret `AuditStepKind::Unreadable` rather than skipping it
(the skip-silently variant is the tempting one and produces a shorter,
plausible, wrong history). That catches an unknown action id and a result that
does not parse. It cannot catch this: the payload *does* parse, it just parses
into something else. Only a per-entry declaration of which payload shape was
written could.

## What should happen

The rung README names this rung as the owner of the ladder's answer, and
rungs 5 and 7 are meant to reuse it. This finding deliberately stops at the
diagnosis rather than shipping a half-considered scheme, because whatever is
chosen becomes a cross-rung contract. The options, with the trade-off that
decides between them:

1. **A per-entry application payload version.** `LogEntry` grows a field (or
   an agreed `metadata` key) carrying an app-declared schema version per
   action type — e.g. a `static constexpr std::uint32_t payloadVersion` on the
   action struct, defaulting to 1, stamped at append time. Replay refuses (or
   routes to a migration) an entry whose version it does not know, instead of
   lenient-decoding it. Cost: a `LogEntry` field and a stamping path; benefit:
   the failure becomes loud and per-entry, and migrations become expressible.
2. **Strict decode on replay.** A replay-only codec with
   `error_on_unknown_keys = true` and no defaulting, so any shape mismatch
   throws. Cheapest to build, and catches a *removed* field immediately — but
   it cannot distinguish "field added since" from "field renamed", so every
   additive change also becomes a hard replay failure. Additive changes are the
   common case, which is why lenient decode was chosen in the first place.
3. **Accept and document.** State in `docs/spec/journal/journal.md` that
   journal payloads are only replayable by a build whose action structs are
   field-compatible with the writer's, and that "reconstructible from the
   journal alone" holds *within* a schema generation only. This is the
   status quo made honest, and it is a legitimate answer — but it should be a
   decision, not the current situation of a documented promise the code does
   not keep.

Option 1 is what the rung README's "per-entry schema/app-version pinning plus
a migration story" anticipates.
