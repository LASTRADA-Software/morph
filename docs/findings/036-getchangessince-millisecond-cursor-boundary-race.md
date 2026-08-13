---
id: 036
title: "`BookmarkModel::execute(GetChangesSince)`'s strict `updatedAtMs > since` comparison can miss a change made in the same millisecond as the previous poll's `asOf` cursor"
subsystem: bookmarks
severity: minor
source: application-ladder CI hardening session (2026-08-11), found on the `Linux / all optional features (gcc)` CI leg while investigating an unrelated CI failure
disposition: open
test: `examples/bookmarks/tests/test_bookmark_presenter.cpp`, `"BookmarkPresenter::getChangesSince returns only bookmarks touched after the given instant, all three backend modes"` (`Mode::Local` generator case) — the test that caught it; fails intermittently, not deterministically
issue: https://github.com/LASTRADA-Software/morph/issues/43
---

## How this was found

Not from a design review — from CI, while investigating an unrelated
failure (finding 035). `Linux / all optional features (gcc)` failed:

```
BookmarkPresenter::getChangesSince returns only bookmarks touched after
the given instant, all three backend modes
  REQUIRE( secondPoll.changed.size() == 1 )
  with expansion:
  0 == 1
  with message:
  mode := 0
```

`mode := 0` is the first `GENERATE(Mode::Local, Mode::LocalSingleThread,
Mode::Socket)` value, i.e. `Mode::Local`. Like finding 035's
`FaultProxy::dropReply`, this did not reproduce locally in this session
(never observed failing on this machine) and only surfaced once the
ladder test suite actually started running under CI's load — consistent
with a genuine but narrow timing window, not a hard logic error.

## Root cause

`BookmarkModel::execute(const GetChangesSince&)`
(`examples/bookmarks/src/models/bookmark_model.cpp:409-424`):

```cpp
GetChangesSinceResult BookmarkModel::execute(const GetChangesSince& action) {
    const auto& owner = requireOwner();
    const auto asOf = nowMs();
    const std::int64_t since = action.since.hasValue() ? (*action.since).value.time_since_epoch().count() : 0;

    auto rows = mapper()
                    .Query<db::BookmarkRecord>()
                    .Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::ownerPrincipal>, "=", owner)
                    .Where(::Lightweight::FieldNameOf<&db::BookmarkRecord::updatedAtMs>, ">", since)
                    .All();

    GetChangesSinceResult result;
    result.asOf = fromEpochMs(asOf);
    ...
```

The failing test's sequence: poll once (empty inbox, cursor = poll 1's
`asOf`), create a bookmark, poll again with `since = cursor`, expect
exactly the new bookmark back. The query is a **strict** `updatedAtMs >
since`. If the created bookmark's own `updatedAtMs` (set from `nowMs()` at
creation time, millisecond resolution) lands in the **same millisecond**
as poll 1's `asOf` cursor — entirely possible on a fast machine or a
loaded CI runner where "poll, then create, then poll again" all executes
within one clock tick — the comparison excludes it: `updatedAtMs == since`
fails `updatedAtMs > since`, even though the creation genuinely happened
*after* the first poll captured its cursor in wall-clock terms (just not
in a *different* millisecond).

This is a boundary/granularity bug, not a logic error in the broader
design: the choice to capture `asOf` *before* running the query (per that
line's own comment, "so a racing write would be lost across two
consecutive polls instead of merely duplicated across them") is correct
and deliberately favors duplication over loss for a write racing the poll
itself. But it does not, and cannot by itself, fix the *narrower*
same-millisecond case where the racing write's timestamp collides exactly
with the cursor value — `>` treats "equal" as "not new," which is wrong
for a value that is genuinely a subsequent event sharing the same
millisecond tick as the cursor.

## Likely fix direction (not attempted this session)

`>=` instead of `>` would flip the bug into over-inclusion instead of
under-inclusion (a change made in the exact same millisecond as a poll's
own `asOf` capture, by some other concurrent actor, would show up on
*that same* poll and then again — spuriously — on the next one using it
as `since`). Neither operator is unconditionally correct at millisecond
granularity; the real fix likely needs either:

- Higher-resolution timestamps (microsecond or a monotonic per-write
  sequence number) so two writes in the same "millisecond" are still
  strictly orderable relative to a cursor, or
- An explicit tie-breaking convention (e.g. cursor = `(timestamp,
  sequence)` pair, `updatedAtMs > since.timestamp OR (updatedAtMs ==
  since.timestamp AND seq > since.seq)`).

**Confirmed**: rung 3/polls' own Zulip-pattern event log, `PollModel::
execute(GetEventsSince&)` (`examples/polls/src/models/poll_model.cpp:627-663`),
already avoids exactly this class of bug by cursoring on
`PollEventRecord::id` — a `ServerSideAutoIncrement` primary key — instead
of a timestamp: `Where(id, ">", *action.lastEventId)`, ascending. An
auto-increment id is inherently collision-free and strictly orderable
across writes regardless of clock resolution, which is precisely the
property `GetChangesSince`'s millisecond timestamp lacks.
`GetChangesSince` returning full row summaries (not an append-only event
log) makes porting the identical id-cursor scheme non-trivial — it would
need to cursor on something like `max(id) at the time of the previous
poll` per bookmark, or move to an outbox/event-log shape of its own — but
`GetEventsSince` is the concrete, working precedent for "how this
codebase already solves the identical ordering problem," not merely a
hypothetical direction.

Not investigated further or fixed in this session — this finding exists
to record the observation and root cause for whoever picks it up, per the
same reasoning as finding 035 (a subtle concurrency/timing fix attempted
under time pressure inside an already-large CI-hardening session is
higher-risk than filing it properly and picking it up with focus later).

## What's still open

- Design how `GetChangesSince`'s bulk-summary shape (not an append-only
  log) could adopt an id/sequence-based cursor instead of a timestamp —
  `GetEventsSince`'s scheme doesn't transfer as a direct copy-paste the
  way it would for another append-only log.
- No dedicated regression test forces the same-millisecond collision
  deterministically (e.g. by overriding the ladder's injectable clock,
  `examples/common/clock.hpp`'s `ScopedClockOverride`, to freeze `nowMs()`
  across the create-then-poll sequence) — the existing test relies on
  incidental timing and, like finding 035's test, can pass on a lucky run.
