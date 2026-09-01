# Scenario corpus expansion — design

Fill `scripts/scenario/scenarios/` until `scenario_coverage.py` exits `0` on
both axes: 65 domain workflows across the five server rungs, every registered
action dispatched, every envelope kind sent and every refusal either asserted
or exempt with a written reason.

Companion to
[2026-08-30-scenario-coverage-design.md](2026-08-30-scenario-coverage-design.md)
(the protocol axis) and
[2026-08-30-scenario-workflows-design.md](2026-08-30-scenario-workflows-design.md)
(the workflow axis). Those two built the ruler. This one is the work the ruler
was built to measure: it adds no measurement code and changes no threshold.

## Contents

- [The measured baseline](#the-measured-baseline)
- [What is drivable, verified](#what-is-drivable-verified)
- [The driver](#the-driver)
- [Authoring constraints](#authoring-constraints)
- [The journey inventory](#the-journey-inventory)
- [Protocol-axis scenarios](#protocol-axis-scenarios)
- [Allowlist additions](#allowlist-additions)
- [Verification protocol](#verification-protocol)
- [Out of scope](#out-of-scope)

## The measured baseline

`python3 scripts/scenario/scenario_coverage.py`, on a clean tree:

```
  envelope kinds : 4/8 covered
  refusals       : 4/17 covered

  bookmarks  actions  3/17 dispatched, workflows  0/12
  kanban     actions  0/22 dispatched, workflows  0/20
  ledger     actions 13/17 dispatched, workflows  1/15
  pastebin   actions  3/6  dispatched, workflows  0/8
  polls      actions  0/9  dispatched, workflows  0/10

  total actions dispatched: 19/71
```

Four files exist. The gap is 64 workflow files, 52 undispatched actions, four
envelope kinds and eleven refusals.

## What is drivable, verified

Every protocol-axis claim below was confirmed against a live
`ladder_pastebin_server` before this spec was written, not inferred from
`remote.hpp`.

| Item | How | Result |
|---|---|---|
| `attach`, `assign`, `instances`, `schemas` | `send <kind> typeId=PasteModel` | `ok` |
| `attach requires a typeId` and its four siblings | `send <kind>` with no `typeId` | the exact refusal |
| `envelope decode failed: ` | `send hello modelId="abc"` — a string where the envelope wants a number | `envelope decode failed: 1:107: parse_number_failure` |

Four refusals are gated on server configuration no rung installs, and are
handled by [allowlist](#allowlist-additions) rather than by reshaping the
example apps into test fixtures:

| Refusal | Gate | Installed by |
|---|---|---|
| `too many models` | `LimitPolicy::maxLiveModels` | bookmarks, polls, kanban, ledger (256) |
| `server busy` | `LimitPolicy::maxInFlightExecutes` | no rung |
| `timeout` | `LimitPolicy::executeTimeout` | no rung |
| `payload missing required field(s): ` | `PayloadCompleteness::RequireDeclaredFields` | no rung |
| `server shutting down` | the server draining mid-request | not drivable at all |

`too many models` is drivable — 256 is a cap a scenario can reach — and is
asserted rather than exempted.

## The driver

`morph_scenario.py` deliberately owns no server lifecycle, and that stays true.
A corpus of 65 files cannot be authored or re-verified by hand, so the
lifecycle goes in a separate tool, exactly where the workflow-axis spec already
put it ("Per-rung directories are also what lets `run_all.sh` start one server,
run that rung's whole set against it, and tear it down").

`scripts/scenario/run_scenarios.py`, standard library only:

- For each requested rung (default: all of `SERVER_RUNGS`), create a temp
  directory and a fresh SQLite database, spawn the rung's server with
  `PORT=0`, and read the port off the line it prints.
- Run every `*.scenario` in `scenarios/<rung>/` against that one server, in
  sorted order. Report per-file pass/fail and a per-rung total.
- Tear the server down and delete the temp directory.
- Exit non-zero if any scenario failed.

Flags:

| Flag | Effect |
|---|---|
| `--rung NAME` | Restrict to one rung; repeatable. Default: every rung. |
| `--build-dir DIR` | Where the `ladder_*_server` binaries live. Auto-detected by searching `build/*/examples/<rung>/` when omitted. |
| `--mutate` | After a file passes, run `mutate_scenario.py` on it and fail if any mutant survives. |
| `--twice` | Run the whole directory a second time against the same database, proving re-runnability. |
| `-v` | Pass `--verbose` through to `morph_scenario.py`. |

Per-rung server environment, read from each `src/server/main.cpp`:

| Rung | Variables |
|---|---|
| pastebin | `PASTEBIN_DB`, `PASTEBIN_PORT` |
| bookmarks | `BOOKMARKS_DB`, `BOOKMARKS_PORT`, `BOOKMARKS_TOKEN_SECRET` |
| polls | `POLLS_DB`, `POLLS_PORT` |
| kanban | `KANBAN_DB`, `KANBAN_PORT`, `KANBAN_TOKEN_SECRET`, `KANBAN_ATTACHMENT_PORT` |
| ledger | `LEDGER_DB`, `LEDGER_PORT`, `LEDGER_TOKEN_SECRET` |

All five additionally get `QT_QPA_PLATFORM=offscreen`, and a database string of
the form `DRIVER=SQLite3;Database=<path>;Timeout=5000`.

The port line is not uniform. Four rungs print
`<rung>-server: listening on ws://127.0.0.1:<port>`; pastebin prints
`pastebin-server: listening on port <port>`; kanban prints a *second* line for
its attachment side channel (`listening on http://127.0.0.1:<port>`). The
driver matches the WebSocket line specifically — one regex accepting both
forms and requiring `ws://` or the literal `port`, so kanban's HTTP line can
never be mistaken for it.

`broken-on-purpose.scenario` is expected to *fail*: the driver knows it by name
and inverts its verdict, so a run where it accidentally passes is a failure.

## Authoring constraints

Five rules, each of which exists because breaking it produces a file that
passes while proving nothing.

1. **Re-runnable against a database it has already run on.** One server serves
   a whole rung directory, and `--mutate` reruns each file many times over. No
   assertion may depend on the database being empty. Listing assertions are
   pinned to captured ids (`field pastes ~ "\"id\":\"$id\""`), never to counts
   or to array positions.
2. **No wall-clock dependence.** Inherited from `morph_scenario.py`, which has
   no `sleep` by design. Nothing asserts on elapsed time, and nothing asserts
   on a background worker having ticked.
3. **Every workflow genuinely qualifies.** `scenario_facts` counts a file as a
   workflow only when it dispatches ≥4 distinct actions with ≥3 `do` steps
   whose arguments reference an earlier `capture`. The chain must be real: an
   id threaded into a *new* action, not the same id re-read.
4. **The read is what proves the write.** A mutating action's own `ok` is not
   the assertion; the subsequent read that observes its effect is. This is
   already the house style — see the `StoreTransaction` step in
   `open-account-transact-report-close.scenario`.
5. **Every file cites its source.** A header comment naming the journey, the
   README section it comes from, and any wire shape that is not guessable from
   the DTOs (enum integer values, strong-id wire forms, bare-scalar result
   bodies). Again the ledger file's existing style.

## The journey inventory

Sourced from each rung's README — "What to implement", "Definition of done"
and "Expected strain points" — not invented. 65 files, matching
`WORKFLOW_FLOORS` exactly.

### pastebin — 8

The README's own definition of done names the first one verbatim:
*create → list → open → burn → delete*.

1. `create-list-open-burn-delete` — the DoD journey, end to end.
2. `burn-after-reads-counts-down` — reads consume the allowance; the read past
   it finds nothing.
3. `read-is-a-write` — `readCount` advances across reads, the rung's headline
   semantic ("this is the interesting one").
4. `edit-then-reread` — the edit is proven by the re-read, not by its own `ok`.
5. `expire-then-read` — `ExpirePaste`, then the read that finds it gone.
6. `private-paste-stays-out-of-the-listing` — two clients; the owner reads it,
   the other's listing does not carry it.
7. `delete-is-final` — after delete, both `GetPaste` and `EditPaste` refuse.
8. `paging-the-listing` — cursor pagination, then a read of an id off a later
   page.

### polls — 10

1. `create-open-vote-finalize` — the organiser journey.
2. `two-participants-converge` — the shared-instance showcase.
3. `undo-is-principal-scoped` — A votes, B votes, A undoes, B's vote survives.
   The rung's headline design record.
4. `update-votes-then-state`.
5. `events-since-cursor` — mutate, poll, cursor advances.
6. `events-survive-instance-rebirth` — every client detaches, a new one
   reattaches and polls with the pre-death cursor.
7. `finalize-locks-the-poll` — votes, updates and comments all refused after.
8. `comments-on-a-poll`.
9. `admin-token-gates-finalize` — a participant cannot finalize.
10. `stale-cursor-resyncs`.

### bookmarks — 12

1. `sign-in-create-tag-list`.
2. `edit-and-reread`.
3. `archive-unarchive-round-trip`.
4. `delete-removes-from-the-listing`.
5. `bulk-edit-adds-and-removes-tags`.
6. `bulk-edit-is-all-or-nothing` — a bad id in the batch leaves every other
   bookmark untouched.
7. `rename-tag-cascades`.
8. `merge-tags-cascades`.
9. `list-filters-and-pagination`.
10. `metadata-arrives-later` — `RecordMetadata`, then `GetChangesSince` with
    the cursor from before it.
11. `import-then-export-round-trip`.
12. `shared-feed-across-two-users` — isolation and the merged feed, the DoD
    criterion.

### ledger — 15

`open-account-transact-report-close` already exists and counts as the first.

2. `zero-sum-holds-per-currency`.
3. `foreign-amount-pair-balances`.
4. `categorise-and-budget`.
5. `budget-limit-and-spend-report`.
6. `rule-sets-a-category-on-store`.
7. `update-rule-version-conflict` — `expectedVersion` / `VersionConflict`,
   the rung's Scenario B.
8. `import-chunk-dedups-on-rerun` — content-hash idempotency.
9. `import-then-report`.
10. `submit-report-then-poll-status`.
11. `only-the-runner-runs-a-report-job`.
12. `undo-is-a-compensating-entry`.
13. `second-undo-is-rejected` — `AlreadyReversed`, the bug morph#144 found.
14. `set-category-on-a-posted-transaction`.
15. `two-books-are-isolated`.

### kanban — 20

1. `sign-in-create-project-open-board`.
2. `columns-swimlanes-and-a-first-task`.
3. `move-a-task-across-columns` — the centerpiece action.
4. `wip-limit-rejects-a-move`.
5. `two-clients-converge-on-one-board`.
6. `events-since-cursor`.
7. `activity-stream-from-the-journal`.
8. `comment-on-a-task`.
9. `tag-mutation-applies`.
10. `attachments-add-list-remove`.
11. `rules-create-list-delete`.
12. `a-rule-cascades-on-a-move`.
13. `a-viewer-cannot-mutate`.
14. `a-member-can-move`.
15. `promoting-a-member-changes-what-they-may-do`.
16. `removing-a-member-revokes-access`.
17. `my-projects-after-joining-one`.
18. `project-roles-listing`.
19. `a-column-from-another-project-is-refused`.
20. `board-state-after-a-full-session`.

Action coverage is checked per rung once the inventory lands; any action no
journey reaches gets one added, rather than a bare call appended to an existing
file.

## Protocol-axis scenarios

Files must live under a rung directory to be attributed, so these go in
`scenarios/pastebin/` — whose README entry already claims "hostile envelopes" —
except the model cap, which needs a rung that installs one.

- `pastebin/wire-kinds-and-typeid-refusals.scenario` — `attach`, `assign`,
  `instances` and `schemas`, each once with a `typeId` and once without,
  asserting the four `ok` replies and the five `… requires a typeId` refusals
  (`register`'s included).
- `pastebin/malformed-envelope.scenario` — `envelope decode failed: `.
- `bookmarks/model-cap-bites.scenario` — registers past
  `kMaxLiveModels` (256) and asserts `too many models`. Confirmed against a
  live `ladder_bookmarks_server`: registration needs no session, and register
  #257 on a single connection is refused. The file is mechanically long (257
  `send register` steps, each with its `expect`) because the cap is 256 and
  the runner has no loop construct; that length is the honest cost of
  asserting a real limit rather than exempting it.

None of the three is a workflow, and none is counted as one.

## Allowlist additions

Four `messages` entries, each naming the exact setting no rung installs:

- `server busy` — `LimitPolicy::maxInFlightExecutes` is left at `0`
  (unbounded) by every rung's `App`; the shed path is unreachable against any
  server a scenario can drive.
- `timeout` — likewise `LimitPolicy::executeTimeout`.
- `payload missing required field(s): ` — requires
  `PayloadCompleteness::RequireDeclaredFields`, which no rung sets and which
  `remote.hpp` documents as deliberately not the default.
- `server shutting down` — emitted only while the server is draining, which a
  scenario cannot reach: the driver tears the server down after the last
  reply, and forcing the overlap would need a wall-clock wait.

Each entry names the file and constant, so the reverse audit retires it the
moment a rung installs the setting.

## Verification protocol

No file is committed unverified.

1. `run_scenarios.py --rung <name>` — every file in the rung passes against a
   real server.
2. `run_scenarios.py --rung <name> --twice` — the directory passes a second
   time against the same database, proving constraint 1.
3. `run_scenarios.py --rung <name> --mutate` — no surviving mutant in any file.
4. `scenario_coverage.py` — the rung's action set and workflow floor are met.
5. `test_morph_scenario.py` — the runner's own self-test still passes.

Work proceeds and commits rung by rung, smallest floor first: pastebin, polls,
bookmarks, ledger, kanban. The final commit adds the protocol scenarios, the
allowlist entries and the README update.

The README's "What this deliberately does not do" currently states that *"morph
does not serve schemas over the wire … no envelope `kind` exposes them to a
remote client"*. `send schemas typeId=PasteModel` returns the full JSON Schema
document. That paragraph is corrected in the same pass.

## What actually happened, against this plan

Two of the boundaries below were crossed, both deliberately and with the
author's agreement, and both are recorded here rather than left contradicting
the delivered work.

**kanban's authorizer was changed.** The rung could not be driven at all:
`KanbanAuthorizer` inherited `SigningAuthorizer` without the anonymous
`AuthModel`/`Login` carve-out `BookmarksAuthorizer` carries, so `authorize()`
demanded a token for the only action that mints one. Every action a fresh
remote client could send was answered `unauthorized`. That is a defect in the
example rather than an obstacle to testing it — the server ships a `Login` no
remote client can reach — and it was fixed by giving kanban the identical
carve-out, which bookmarks documents as having found the same way. All 137
kanban tests still pass. Without it, kanban's 22 actions and floor of 20 were
unreachable and this plan could not have been completed.

**`morph_scenario.py` learned one new rule.** A reply to an undecodable frame
carries `callId` 0, because the server has no envelope to read one from. The
runner correlated strictly by `callId` and so rejected it as a transport
violation, making the `envelope decode failed:` path unassertable. `Client.rpc`
now accepts a zero `callId` in exactly that shape — an `err` whose message
names a decode failure — and nowhere else.

Neither the coverage thresholds nor the workflow floors moved.

## Out of scope

- **No change to the measurement.** No threshold, floor, or qualification rule
  moves. A corpus that cannot meet a floor is a corpus to finish, not a floor
  to lower. *(Held.)*
- **No change to the example applications**, other than the kanban authorizer
  fix recorded above. In particular: no limit-policy knobs added to make a
  refusal assertable, and no actions added to make one reachable.
- **No CI wiring.** Running the corpus in CI needs a build of five servers in a
  job that has none today; that is its own change.
- **lims and crm.** Rungs 6 and 7 are in `rungs.txt` but not in `SERVER_RUNGS`,
  and ship no `src/server/main.cpp`. Nothing to drive.
