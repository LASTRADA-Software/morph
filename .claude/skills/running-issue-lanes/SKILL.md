---
name: running-issue-lanes
description: Use when asked to work this repository's issue backlog rather than one named issue - "work through the issues", "keep triaging and dispatching", "run the backlog", "batch these up", "what should we work on next" - or when resuming a backlog run already in progress. Also use when a dispatched lane has reported back and its branch needs handling.
---

# Running issue lanes

You are the **runner**. You drive the backlog loop: triage, batch, compress,
dispatch, verify, land, refill. Lanes write the feature code; you do not. Your
output is a backlog that keeps moving and branches that are safe to land.

**The load-bearing idea: a lane's report is a hypothesis about its own work.**
It is written by the thing being assessed, from the context that produced the
work, and it is the one artefact nobody else has checked. Treat it the way
`triage-issue` treats an issue body — read it, then verify the claim it turns on.

## Step 0 — take stock of the open issues

**The open issues are the only state there is.** There is no separate tracker or
plan to consult: an issue's `triage:` label says what may be done with it, its
`area:` label says which tree it touches, and the open PRs say what is already in
flight. Keep that true — if a decision is not written onto an issue, it did not
happen.

```bash
git fetch -q origin && git log --oneline -1 origin/master
gh issue list --state open --limit 200 --json number,title,labels
gh pr list --state open --json number,title,headRefName,isDraft
git worktree list | grep '\.claude/worktrees'     # lanes still holding files
```

Bucket the issues by `triage:` label. **A fast-forwarded main checkout matters** —
triaging or dispatching against a stale tree produces stale verdicts.

This runs as a pipeline, so on every pass do all of these rather than choosing
one: **open PRs get landed or fixed** (Step 7), **untriaged issues get triaged**
(Step 1, capped), and **every free lane slot gets refilled with a batch**
(Steps 2 and 4). A pass that drains the queue without refilling it, or refills
without draining, stalls the half it skipped.

## Step 1 — sweep anything untriaged

Run the `triage-issue` skill on each issue with no `triage:` label. Do it
yourself; do not dispatch it.

For a standing cadence use a **cron schedule, not the `Monitor` tool** — `Monitor`
streams events from a script and is capped at 30 minutes, so it cannot hold a
multi-hour cadence. Bound the scheduled prompt exactly like a lane (below), cap
issues per sweep, and give it an explicit "reply one line and stop" for the empty
case. Say out loud that the cadence is session-only and expires.

## Step 2 — batch `triage: valid` into lanes by file tree

Group on **disjoint files**. Never on issue count, never on subject matter.

> A chain like `#567 → #568 → #569/#570 → #571` is five issues but **one lane** —
> each step is blocked on the last. Counting issues suggests five lanes; counting
> file trees gives one, plus whatever is genuinely disjoint.

**A one-ticket PR is the exception, not the default.** Always try to put more
than one issue in a PR: several tickets go on **one branch, one PR, one CI
cycle**. Aim for **3–5 tickets per lane** where the files allow it.

This is arithmetic, not tidiness. A CI run on this repository takes **26–74
minutes**, and every merge invalidates every other open PR's base, forcing a
rebase and a full re-run. So a single-ticket PR costs its own cycle *plus* a
cycle on every PR it lands ahead of. Six single-ticket merges in one session
cost well over a dozen cycles.

**One commit per ticket, always.** This is what makes batching safe rather than
risky: when one ticket turns out to be wrong — and it will — the lane drops that
commit and force-pushes instead of redoing the batch. Put it in the lane prompt.

Three things must *not* go in a batch, because each turns one bad ticket into a
stalled PR:

- **A ticket that needs a decision nobody in the lane can make.** One of those
  blocks every other ticket in the PR on a question that may take days.
- **A ticket touching a file another open PR holds.** The batch inherits that
  contention for all of its tickets, not just the one.
- **A ticket whose premise you have not re-measured this sweep** (Step 6). Mixing
  fresh and stale figures in one branch means the whole batch is dispatched
  against a number nobody checked.

The trade is real and worth stating when you report: a batch is atomic for CI,
so the slowest ticket sets the pace for the rest, and a batched PR is harder to
review than a focused one. One commit per ticket mitigates both. At the measured
cycle times the arithmetic still favours batching by a wide margin.

**Merge ordering matters more once PRs are bigger.** Land the PR that unblocks
the most first, and among equals prefer the one whose merge forces the fewest
rebases on the others.

## Step 3 — compress `triage: rescope` by fix site

Consolidation must produce tickets **a single lane can land**. That is the whole
constraint, and theme-based grouping violates it.

> #522 is the cautionary case, in its own words: *"The suggested fix spans `core`
> + `qt` + `net` + the ladder + 11 test doubles. No single lane could land it."*
> It had to be re-scoped into five sequential tickets before anyone could start.

Merge two issues only when they share a **fix site** — best of all when one of
them says so. Leave them separate when they merely share a subsystem. **Never
merge a live defect into a rewrite of the thing it lives in**; record the
dependency on both issues instead.

When consolidating:

- **Reproduce the original evidence verbatim** — measured output, and every
  *"not verified"* clause. A consolidated issue with cleaner evidence than its
  sources has destroyed what made it useful.
- **Close originals as superseded, not resolved**, each saying: re-open this if
  the successor closes without acting on its part.
- **File, don't fold.** An aside inside a closed issue lapses unless it gets its
  own issue.
- **Apply no `triage:` label** — AGENTS.md forbids self-assessing that verdict.
  The new issue is untriaged by design; Step 1 picks it up.

## Step 4 — dispatch batches, up to five lanes, and do not wait for CI

One `Agent` call per lane, `isolation: "worktree"`, **one batch each** (Step 2).

**A lane's job ends when it has pushed and opened a PR — not when CI is green.**
Waiting is what makes a lane expensive: a CI cycle here is 26–74 minutes, and a
lane that sits watching one is a worker doing nothing while tickets queue behind
it. Put this in every lane prompt:

> Push, open the PR, report back **immediately**. Do not wait for CI, do not
> poll it in a loop, do not re-run it. Report the check counts you happened to
> see at hand-off and say they are incomplete. The runner lands PRs; you do not.

The landing sweep (Step 7) is what closes the loop, and it runs on its own
cadence. This is a pipeline: lanes produce PRs continuously, the sweep drains
them. The two must not block each other.

**Five lanes is the ceiling, and disjointness is the real bound.** Five batches
only pay off if all five sit on genuinely disjoint file trees; concurrent lanes
on overlapping trees produce PRs that invalidate each other, and the rebases cost
more than the parallelism buys. If only three disjoint trees have `valid` work,
run three lanes — a fourth on a tree another lane already holds is worse than an
idle slot.

The hard bounds below are what make five lanes safe rather than reckless. They
are per-lane and do not relax as the count rises: six lanes dispatched *without*
them became 16 agent tasks and ~318 background bash tasks. The count was never
the problem.

**Bounding the agent count does not bound the task count.** Three multipliers
escape a "don't spawn agents" instruction. Paste this into every lane prompt:

```markdown
## Hard bounds — not negotiable
- **Do not dispatch subagents.** No Agent tool, no Workflow tool, no
  task-spawning tool, for any reason.
- **Do not invoke `/code-review` or `/simplify`.** They fork themselves into
  background agents that cannot be withdrawn. Do the review reasoning inline
  and write it into the PR body.
- **Do not background any build or test.** No `run_in_background`, no trailing
  `&`. Foreground only, one at a time.
- Work only in your own worktree and branch. Do not merge.
```

> Six lanes dispatched without these became **16 agent tasks and ~318 background
> bash tasks**, ~70 running at once. Four lanes dispatched with all three spelled
> out stayed at **four tasks**, each returning a green PR. The prompt, not the count.

Also give every lane:

- **Which files the other lane holds**, by path, and that they are off limits. A
  lane needing a file another lane owns has made a finding about the shared
  design — tell it to comment that on its issue and report back, not to edit.
- **What landed since its base**, especially tightened CI gates, so a newly
  strict leg is expected rather than a surprise.
- **Permission to reject the ticket.** AGENTS.md: delivering exactly what a ticket
  says, when what it says is wrong, is the most expensive outcome available.
- **File-don't-fold**, with descriptive labels and **never** a `triage:` label.
- The commit trailer and PR footer this repo uses.

## Step 5 — verify the report before acting on it

Pick the one claim the branch's safety rests on and check *that*, against the
repository rather than the report.

| Claim shape | The check |
|---|---|
| "Purely additive" / "nothing else changes" | `gh pr view <n> --json files -q '.files[] \| "\(.additions)\t\(.deletions)\t\(.path)"'`, then grep the diff's deleted lines for the API it promised to leave alone. |
| "CI is green, N/N" | `gh pr checks <n> \| awk -F'\t' '{c[$2]++} END {for (k in c) print c[k], k}'` |
| "No conflict with the other lane" | `gh pr diff --name-only` on both, and intersect. |
| "Issue X's premise still holds" | Re-measure it (Step 6). |

**Ask GitHub for the counts; do not hand-roll them from the patch.** A plausible
`awk '/^\+[^+]/{a++}'` over `gh pr diff` undercounts by exactly the number of
added blank lines — the `[^+]` that excludes the `+++` header also excludes a bare
`+`. A verification step that silently under-reports is the failure this step exists
to prevent.

A lane that volunteers a correction to its own ticket, reports one of its own tests
as vacuous, or declines to claim something it could have claimed is *more*
trustworthy — weight it accordingly, and still check the one claim.

## Step 6 — re-measure before dispatching against an old number

An issue's figures were true at the revision in its verification status. Master
moves. Re-measure **before** a lane is dispatched against a figure, not after.

> #542 named `tests/offline_sqlite` as its sharpest case; #555 had already fixed
> it, after the sweep the issue was written against.
>
> #575 claimed a precondition was undocumented. `docs/spec/` documented it in two
> files, including the exact hazard it described. Triaged `invalid` and closed —
> by the same person who filed it.

If a PR in flight rewrites the code a measurement was taken on, that measurement
is already stale. Say so on the issue before someone builds against it.

## Step 7 — the landing sweep: drain the queue on a cadence

Lanes produce PRs and move on (Step 4). Nothing lands until a sweep lands it, so
**run this on a schedule — every two hours is the working cadence** — and walk
*every* open PR each time. Use a cron schedule, not `Monitor`; `Monitor` caps at
30 minutes and cannot hold the cadence.

For each open PR, count the checks and sort it into one of four buckets:

| Bucket | Action |
|---|---|
| **green** | Apply the staleness test below, then merge. |
| **red** | Diagnose it *now* — the lane that wrote it is gone. |
| **pending** | Leave it. Do not poll it; the next sweep will catch it. |
| **fork** | Never merge, never approve its workflows. Not yours. |

### Staleness: a *material* delta, not any delta

A PR whose base has moved is not automatically disqualified — reflexively
rebasing every stale PR costs one full CI cycle each, which in a queue model is
most of them, every sweep. Ask instead what actually moved:

```bash
mb=$(git merge-base origin/master <head>)
git diff --name-only $mb origin/master
```

- A **gate** changed (`.github/workflows/`, `scripts/`, `CMakePresets.json`) → **rebase**. Its CI was judged by rules that no longer exist.
- A **source file it touches, or a header those include**, changed → **rebase**. The combination was never tested.
- Only unrelated files, or reporting-only lines → **merge**. Re-running CI cannot change the verdict, and the cycle buys nothing.

State which of the three you concluded, and why, in your report. Getting this
wrong in the safe direction is expensive but recoverable; getting it wrong in the
unsafe direction merges something nothing judged.

### Handling red

The lane is gone, so the sweep owns the failure. Read the failing job's log
before anything else — a leg that stopped within seconds of its siblings is a
cancellation, not a defect, and purged logs mean re-running is the only way to
learn anything. If the fix is small and obvious, make it. If it needs the
context the lane had, dispatch a lane whose whole batch is *that PR*, and give it
the failing log, not just the PR number.

### Ordering

**Land what unblocks the most first** — a chain head sitting green stalls
everything behind it. Among equals, prefer the PR whose merge forces the fewest
rebases on the rest. Merging several PRs on disjoint trees in one sweep is fine
and is the point; merging two that touch the same file is two rebases you chose.

### Always

- **Prune each merged lane's worktree.** They accumulate; ~39 stale ones had
  built up under `.claude/worktrees/` before anyone looked. A worktree still
  locked by a live agent is not yours to remove.
- **Refill every free lane slot** from Step 2 before ending the sweep. A sweep
  that lands three PRs and dispatches nothing has emptied the pipeline.

## Red flags

- Counting issues to decide how many lanes to run.
- Dispatching a lane with **one** ticket when two or three disjoint ones were
  sitting in `triage: valid`.
- A lane still running after it pushed, because it is watching its own CI.
- A sweep that merges PRs and dispatches nothing, leaving the pipeline empty.
- Rebasing every stale-base PR reflexively, instead of asking what moved.
- A batched branch with one commit for the whole batch, so a single bad ticket
  cannot be dropped without redoing the rest.
- A lane prompt that says "do not spawn agents" and stops there.
- Consolidating two issues because they share a subsystem.
- A consolidated issue with cleaner evidence than its sources.
- Relaying "CI is green" without having counted the checks.
- Dispatching against a number nobody re-measured.
- Merging a PR whose CI predates the gate that would now judge it.
- Writing feature code yourself instead of dispatching it.

## Rationalizations

| Excuse | Reality |
|---|---|
| "The lane said it was green." | The lane is reporting on itself. Counting costs one command. |
| "Two is a small number; the bounds are overkill." | Six became 318 because of the prompt, not the count. Two can too. |
| "Fewer issues is the goal." | *Landable* issues are the goal. #522 was one issue nobody could land. |
| "The evidence is in the originals, I'll link them." | Closed issues stop being read. Reproduce it or lose it. |
| "The measurement is only a few weeks old." | #542's sharpest case was fixed between filing and dispatch. |
| "It's green and clean, just merge it." | Green against which base? Check what landed since. |
| "This finding is small, I'll fold it into the PR." | AGENTS.md: file it, link it, keep the change about one thing. |
| "Faster if I just fix this one myself." | Then do — but a runner writing feature code has stopped running lanes. |
| "One ticket per PR keeps it focused and reviewable." | Focus is measured in hours here: 26–74 min a cycle, plus a forced rebase and re-run on every other open PR. One commit per ticket buys the reviewability without the cycles. |
| "These two tickets aren't really related." | Unrelated is fine. The batching constraint is *disjoint files a single lane can land*, not a shared theme — that is Step 3's rule for consolidating issues, not this one's for grouping work. |
| "I'll batch them next sweep, once the backlog settles." | The backlog does not settle; lanes file new issues while closing old ones. Batch what is `valid` and disjoint now. |
| "The lane should wait for CI so it can report a green PR." | That is 26–74 minutes of a worker doing nothing while tickets queue behind it. The sweep lands PRs; the lane's job ends at the push. |
| "Its base moved, so it has to be rebased." | Only if something *material* moved — a gate, or a file it touches. Reflexive rebasing costs one full cycle per PR per sweep and buys nothing when the delta is unrelated. |
| "I'll leave the red PR for the lane that wrote it." | That lane handed back and is gone. A red PR nobody owns sits red forever. |
