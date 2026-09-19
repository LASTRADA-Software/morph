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
triaging or dispatching against a stale tree produces stale verdicts. Then pick up
at whichever step below is unsatisfied: untriaged issues exist → Step 1; no lane
free → Step 5; a lane free and `valid` work available → Step 2.

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

Several small tickets in one tree go on **one branch, one PR, one CI cycle**.

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

## Step 4 — dispatch, at most two lanes

One `Agent` call per lane, `isolation: "worktree"`, one issue or one batch each.

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

## Step 7 — land, then refill

- **Land what unblocks the most first.** A chain head sitting green in review
  stalls every lane behind it.
- **Rebase a stale-base PR before merging if the gates have since tightened.** A
  PR whose CI ran two merges ago was judged by the old gates.
- **A merge is outward-facing — confirm it.** Approval to merge one PR is not
  approval for the next.
- **Prune the lane's worktree.** They accumulate; ~39 stale ones had built up
  under `.claude/worktrees/` before anyone looked.
- Refill the free slot from Step 2, and go again.

Watch pending CI with one `Monitor` over all open PRs, emitting **failures and
completion only** — re-arm on expiry rather than polling.

## Red flags

- Counting issues to decide how many lanes to run.
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
