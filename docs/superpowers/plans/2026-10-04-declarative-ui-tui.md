# Declarative UI Program — Master Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement the part plans task-by-task. Read this file first, then
> `2026-10-04-declarative-ui-tui-interfaces.md`: together they fix the branch, the commit layout, the conventions
> every part follows, and the names parts share.

**Goal:** Every example application under `examples/` describes its UI once — declarative controllers plus a
toolkit-free view tree — and one binary per app picks a terminal UI or Qt Quick at runtime by dependency injection;
the shipped QML forms renderer is replaced by a C++ forms engine. Delivered on **one branch, as one pull request,
with one commit per semantic unit**.

**Specs** (`docs/superpowers/specs/`): `2026-10-04-core-cpp-mouse-design.md` (spec 0),
`2026-10-04-declarative-ui-tui-design.md` (spec 1), `2026-10-04-forms-engine-design.md` (spec 2),
`2026-10-04-qtquick-frontend-design.md` (spec 3), `2026-10-04-examples-migration-design.md` (spec 4).

## The parts

Execute in this order; each part assumes every earlier part has landed on the branch.

| Part | Plan (`docs/superpowers/plans/2026-10-04-declarative-ui-tui-…`) | Spec | `wip` key(s) | Ends as commit(s) |
|---|---|---|---|---|
| 0 | `0-core-cpp.md` | 0 | (core-cpp repo), `corecpp` | `core: build against core-cpp 0.7` |
| 1 | `1-reactive.md` | 1 §3–4b | `reactive` | `reactive: signal graph, view state and declarative control` |
| 2 | `2-ui.md` | 1 §5–5b | `ui` | `ui: toolkit-agnostic view tree, mount, frontend seam and RecordingBackend` |
| 3 | `3-tui.md` | 1 §2, §6 | `ioloop`, then `tui` | `core: IoLoopDriver::Caller, the I/O loop on the calling thread`, then `tui: morph::tui terminal frontend` |
| 4 | `4-qt-quick.md` | 3 | `qtquick` | `qt_quick: morph::qt_quick frontend` |
| 5 | `5-forms-engine.md` | 2 | `forms` | `forms: the C++ forms engine, rendered through the view tree` |
| 6 | `6-examples-foundation.md` | 4 §2–4, 1 §8 | `examples-common` | `examples/common: Qt-free app environment, transport, poller and test waits; gallery and workout` |
| 7 | `7-examples-bank.md` | 4 §5 | `bank` | `examples/bank: one app, any frontend` |
| 8 | `8-examples-ladder-a.md` | 4 §5 | `pastebin`, `bookmarks`, `polls` | one commit per app |
| 9 | `9-examples-ladder-b.md` | 4 §5 | `kanban`, `ledger`, `lims` | one commit per app |
| 10 | `10-examples-forms-and-retire.md` | 4 §5, §8; 2 §11 | `forms-demo`, `retire` | `examples/forms: …`, then `forms, examples: retire the QML renderer and the Qt client stack` |

## The pull request's history

Branch `feature/declarative-ui`, cut from `master`. When every part has landed, `git log --oneline master..HEAD`
shows exactly, in order:

1. `docs: declarative UI program — design and implementation plans`
2. `core: build against core-cpp 0.7`
3. `reactive: …`
4. `ui: …`
5. `core: IoLoopDriver::Caller …`
6. `tui: …`
7. `qt_quick: …`
8. `forms: …`
9. `examples/common: …`
10. `examples/bank: …`
11. `examples/pastebin: …`
12. `examples/bookmarks: …`
13. `examples/polls: …`
14. `examples/kanban: …`
15. `examples/ledger: …`
16. `examples/lims: …`
17. `examples/forms: …`
18. `forms, examples: retire the QML renderer and the Qt client stack`

Each commit builds, passes its tests and carries its own `docs/spec/` files, map entries, `CHANGELOG.md` lines and
pinned facts. Example-only commits get no CHANGELOG entry (CONTRIBUTING), except where they add a framework seam;
commit 18 gets a "Removed" entry.

## Conventions every part follows

### `wip` commits, one per task

Every task ends with a commit whose subject starts `wip(<key>): `, `<key>` from the table. They exist for review (a
reviewer reads `git show` of one task) and for bisecting inside a part; they never survive the part.

### Squashing a part

The last task of every part — and of each group in parts 3, 8 and 9 — runs:

```bash
key=reactive   # the group's key
pre=$(git rev-parse HEAD)
first=$(git log --reverse --format=%H --grep="^wip($key):" master..HEAD | head -1)
test -n "$first" || { echo "no wip($key) commits -- wrong key?"; exit 1; }
git log --format=%s "$first^..HEAD" | grep -v "^wip($key): " && { echo "foreign commit inside the part"; exit 1; }
git reset --soft "$first^"
git commit -F /path/to/message.txt   # the message the part plan gives
git diff --exit-code "$pre" HEAD && echo "squash preserved the tree"
git log --oneline master..HEAD
```

`git diff --exit-code` proves the squash changed history and nothing else; the last line must show the expected
list so far.

### The docs commit

Before execution starts, the spec and plan commits already on the branch are squashed into commit 1 with the same
procedure over `master..HEAD` (all of them are `docs:` commits). Plans are not edited during execution: when a plan
turns out wrong, the code and its `docs/spec/` file are made right and the deviation is stated in the body of that
part's squashed commit — what the plan said, what was done instead, and why. AGENTS.md's filing rule applies to a
defect found in `include/morph` or `src` outside a part's scope.

### Before execution starts: file the morph finding — ask the user first

Planning found, by reading only, a defect in `include/morph` outside every part's scope; AGENTS.md files it. Confirm
with the user, then:

```bash
cd /Users/christianparpart/projects/morph
sed -n '/static ::core::async::Task<void> acceptFlow/,/^        }$/p' include/morph/net/socket_server.hpp
git -C ~/projects/core-cpp show v0.5.1:CHANGELOG.md | sed -n '/A POSIX accept that failed/,/accepted again at once/p'
```

and `gh issue create --repo LASTRADA-Software/morph --title "net: SocketServer's accept flow retries a dead listener
forever" --label bug --label "area: net"` with a body that states: **what** — `SocketServer::Core::acceptFlow`
returns only on `NetErrorCode::Cancelled` and retries every other failed `accept` after 50 ms; since core-cpp 0.5.1
(`EINVAL` from accept → `BadHandle`) a listener that is no longer listening reports `BadHandle` on every attempt,
so the flow wakes every 50 ms forever instead of ending; **evidence** — the two outputs above, pasted; **verification
status** — inferred from reading the code, not reproduced; observed in no test or deployment (weak evidence, labelled
as such); **what would close it** — the flow ends on `BadHandle` (and the server reports the listener as dead), with a
test that closes the listening socket underneath it and asserts the flow exits; reopen if a `BadHandle` is found that
a retry can recover from. Record the issue number for the PR body.

### Outward-facing steps are confirmed first

Pushing core-cpp's branch, merging it, tagging `v0.7.0`, filing the core-cpp Shift+Tab issue, pushing this branch
and opening the pull request are each confirmed with the user immediately before they are taken.

### Commit message trailer

Every commit ends with `Signed-off-by: Christian Parpart <christian@parpart.family>`.

## Build configurations used by the plans

| Directory | Configure |
|---|---|
| `build/reactive` | `cmake -S . -B build/reactive -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_EXAMPLES=OFF` |
| `build/tui` | `build/reactive`'s flags plus `-DMORPH_BUILD_TUI=ON` |
| `build/qt` | `build/reactive`'s flags plus `-DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON` |
| `build/all` | `-DCMAKE_BUILD_TYPE=Debug -DMORPH_BUILD_TUI=ON -DMORPH_BUILD_QT=ON -DMORPH_BUILD_QT_QUICK=ON -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=all -DMORPH_BUILD_BANK_EXAMPLE=ON -DMORPH_BUILD_NET=ON` (plus `-DMORPH_BUILD_FORMS_QML=ON` until part 10 removes it) |

Configuring prints `morph: warnings: ... strict=ON`; if it does not, stop and say so (CONTRIBUTING).

## Finishing the branch

After part 10's squash:

- [ ] **Whole-branch gates** on `build/all` and on Linux CI's configurations: build; `ctest`; the sanitizer
  presets with `scripts/check_sanitizer_instrumentation.sh`; the Docs build; clang-tidy-diff against
  `origin/master...HEAD` with the file count asserted; `scripts/check_install_export.sh`. Then a build with
  `MORPH_BUILD_TUI=OFF MORPH_BUILD_QT_QUICK=OFF`, proving both frontends are optional and the app libraries and
  their tests still build.
- [ ] **WebAssembly:** the `wasm-ladder.yml` and `wasm-demo.yml` configurations build.
- [ ] **Every commit builds:** `git rebase --exec "cmake --build build/all && ctest --test-dir build/all" master`
  (no `-i`). A failure names the commit that does not stand alone; fix it in that commit.
- [ ] **History check:** `git log --oneline master..HEAD` shows exactly the 18 commits above.
- [ ] **Push and open the PR — ask the user first.** Then `git push -u origin feature/declarative-ui` and
  `gh pr create`, the body listing the commits, linking the specs, and stating what was verified on which
  configurations.
- [ ] **Delete core-cpp's abandoned `feature/reactive-ui` — ask the user first.** Then
  `git -C ~/projects/core-cpp branch -D feature/reactive-ui` and
  `git -C ~/projects/core-cpp push origin --delete feature/reactive-ui`.
