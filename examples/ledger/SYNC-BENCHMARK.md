# Sync philosophy: what morph does when two clients disagree

Rung 5's written deliverable (design spec §10). This is the ladder's answer to
"how does this framework resolve concurrent edits?", stated plainly, with the
two scenarios that make the answer concrete reproduced as tests.

## The answer, once, unhedged

**morph's ordering authority is server arrival order. Full stop.**

No hybrid-logical clock. No vector clock. No per-field merge. The unit of
conflict is **one action** — one user's one logical edit — not one field and
not one row.

That places morph deliberately between the two approaches it is most often
compared to:

| | unit of conflict | can keep both edits? | auditable as user intent? |
|---|---|---|---|
| Actual Budget (CRDT) | one field | yes, automatically | no — a field-diff reconstructed after the fact |
| last-write-wins row | one row | no | no — the losing edit vanishes silently |
| **morph** | **one action** | **no, not automatically** | **yes — the replayed action is the edit the user made** |

It is **coarser** than Actual's field-level CRDT: morph cannot automatically
keep two edits to different fields of the same transaction. It is **finer and
more auditable** than last-write-wins on a whole row: what replays is the whole
edit a user actually performed, not a diff synthesised later, and every action
is in the journal with its own identity.

This is the rung's answer, not an unresolved gap. The trade-off is chosen: a
ledger's correctness rests on *whose intent* produced a number, and an action
preserves that where a field-merge does not.

## Scenario A — two offline clients edit the same transaction

*Actual-style.* Two clients go offline. Client 1 edits a transaction's
description; client 2 edits the same transaction's category linkage. Both
queue through `SqliteOfflineQueue`. Both reconnect.

**What morph does:** both queued actions replay in server arrival order.
Whichever reaches the server first wins entirely for any field both actions
touched. The second either reapplies cleanly — when the fields do not overlap —
or fails validation against the now-changed state, surfaced through
`onBackendChanged` reconciliation.

**What a CRDT would do instead:** keep both edits unconditionally, one per
field, with no rejection and no notification.

**Why morph chooses otherwise:** the losing action is still a real thing a real
user did, recorded in the journal with its own identity, and its failure is
*visible*. A field-level merge produces a transaction that no user ever
authored — correct-looking, and traceable to nobody.

**Not yet reproducible.** This rung ships no transaction-edit action, no base
version on `transaction_journals`, and no offline-queue wiring, so there is
nothing to make two clients disagree *about* a transaction. Tracked in
morph#144. Stated here rather than demonstrated with a test that would pass
under the scenario's name without exercising it.

## Scenario B — a stale base version is rejected, never merged

*ODK-style.* Two clients read the same journal. Client 1 commits a change,
bumping the journal's implicit base version. Client 2's queued edit arrives
carrying the stale base.

**What morph does:** rejects it outright, with a typed error the presenter
surfaces. Not a silent overwrite. Not a merge.

**Why:** an edit computed against state that no longer exists is not a
conflict to resolve — it is a decision made on information now known to be
wrong. Applying it anyway would mean writing a number the user would not have
chosen had they seen the current state.

Reproduced in `tests/test_sync_benchmark.cpp`, `[ledger][sync-benchmark]` --
three cases: the rejection itself, that the winner's edit survives it intact
(a merge or a silent overwrite would both leave different state), and that a
client can re-read and reapply afterwards. That last one matters: rejecting a
stale edit is only defensible if the work is recoverable, otherwise "rejected
outright" would mean the user simply loses it, which is worse than the merge
this document argues against.

The mechanism is `UpdateRule::expectedVersion` -- optional, so an
unconditional update stays unconditional, and engaged when a client wants its
edit refused rather than applied blind. Before morph#144 the `version` column
existed and incremented on every write but nothing ever compared it: this
section described intended behaviour with no code behind it.

## Clock skew: client timestamps are claimed, never authoritative

Two clients whose clocks differ by ±5 minutes both write to one ledger. The
activity view orders strictly by journal — that is, server arrival — order.
Each entry's client-supplied timestamp is displayed and labelled as **claimed**.

Ordering never consults it. A client with a fast clock cannot reorder the
audit trail by lying about when it acted, whether by accident or design.

This is the practical consequence of the statement at the top: if server
arrival order is the authority, then a client's own clock is evidence about
that client, not about sequence.

## What this rung does not claim

- No automatic three-way merge, at any granularity.
- No "keep both" resolution without a user deciding.
- No ordering guarantee derived from client clocks.

Each of those is a real capability morph does not have. Naming them here is
the point of the document: the trade-off is only honest if the cost is stated
alongside the benefit.
