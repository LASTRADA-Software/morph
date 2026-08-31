# Scenario corpus expansion — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fill `scripts/scenario/scenarios/` until `python3 scripts/scenario/scenario_coverage.py` exits `0` on both axes — 65 domain workflows across five rungs, every registered action dispatched, every envelope kind sent, every refusal asserted or exempt with a written reason.

**Architecture:** A new standard-library server driver (`run_scenarios.py`) owns the lifecycle `morph_scenario.py` deliberately does not, so a rung's whole directory can be run, re-run and mutation-tested against one live server. The corpus itself is authored rung by rung against those real servers — never from source-reading alone — smallest workflow floor first.

**Tech Stack:** Python 3.9+ standard library only. No build step, no `pip install`. The scenario DSL of `scripts/scenario/README.md`. Servers are the five `ladder_<rung>_server` binaries.

**Design spec:** [2026-08-31-scenario-corpus-expansion-design.md](../specs/2026-08-31-scenario-corpus-expansion-design.md)

## Global Constraints

- **Python 3.9+ standard library only.** No dependency may be added to `scripts/scenario/`.
- **Docstring on every public function**, house style: one-line summary, then detail, `@param`/`@return` where they earn their place. Match `scripts/scenario/morph_scenario.py`.
- **No second parser.** Scenario files are read only through `morph_scenario.parse_scenario`.
- **No `sleep`, no wall-clock waits** in any scenario. Inherited from the runner's own design (morph#147).
- **Every scenario re-runnable against a database it has already run on.** One server serves a whole rung directory and `--mutate` reruns each file many times. No assertion may depend on an empty database; listing assertions pin to captured ids, never to counts or array positions.
- **Every `do`, `send`, `deregister` needs at least one `expect`.** The parser rejects a file that omits one.
- **A workflow needs ≥4 distinct actions and ≥3 chained `do` steps** — a `do` whose arguments reference a name an earlier step captured. `session principal=$who token=$token` does not count as chaining.
- **The read is what proves the write.** A mutating action's own `ok` is not the assertion.
- **Every file carries a header comment** naming the journey, the README section it comes from, and any non-guessable wire shape.
- **No change to the measurement.** No threshold, floor, or qualification rule in `scenario_coverage.py` moves.
- **No change to the example applications.** Not to add limit knobs, not to add actions.
- Build directory for the servers: `build/ladder-srv/examples/<rung>/ladder_<rung>_server`, configured with `cmake --preset clang-release -B build/ladder-srv -DMORPH_BUILD_LADDER=ON -DMORPH_LADDER_RUNGS=all -DMORPH_BUILD_NET=ON -DMORPH_BUILD_QT=ON -DMORPH_BUILD_TESTS=ON`.

---

### Task 1: The server driver

**Files:**
- Create: `scripts/scenario/run_scenarios.py`
- Modify: `scripts/scenario/README.md` (a "Running a whole rung" section)

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `RUNGS: dict[str, RungSpec]`, `RungSpec(binary: str, env: Callable[[pathlib.Path, int], dict[str,str]])`, `PORT_LINE: re.Pattern`, `parse_port(line: str) -> int | None`, `find_server(build_dir, rung) -> pathlib.Path`, `ServerProcess` context manager exposing `.port`, `run_rung(...) -> int`, `main(argv) -> int`. Every later task invokes it only as `python3 scripts/scenario/run_scenarios.py --rung <name> [--twice] [--mutate]`.

- [ ] **Step 1: Write the failing test for the port-line reader**

Append to `scripts/scenario/test_morph_scenario.py`, extending the import block with `run_scenarios`:

```python
class PortLineTests(unittest.TestCase):
    def test_reads_the_ws_form(self):
        self.assertEqual(
            run_scenarios.parse_port("bookmarks-server: listening on ws://127.0.0.1:46757"), 46757)

    def test_reads_the_bare_port_form(self):
        self.assertEqual(
            run_scenarios.parse_port("pastebin-server: listening on port 41221"), 41221)

    def test_ignores_kanbans_attachment_side_channel(self):
        self.assertIsNone(run_scenarios.parse_port(
            "kanban-server: attachment side channel listening on http://127.0.0.1:5001"))

    def test_ignores_unrelated_output(self):
        self.assertIsNone(run_scenarios.parse_port("kanban-server: migrating schema"))
```

- [ ] **Step 2: Run it and watch it fail**

Run: `python3 scripts/scenario/test_morph_scenario.py 2>&1 | tail -5`
Expected: FAIL — `ImportError: cannot import name 'run_scenarios'` (or `ModuleNotFoundError`).

- [ ] **Step 3: Write `run_scenarios.py`**

The whole module. Key decisions already settled by the spec: one server per rung directory; the WebSocket line only; `broken-on-purpose.scenario` inverted; temp dir deleted on the way out.

```python
#!/usr/bin/env python3
"""Runs a whole rung's scenario directory against a server it starts itself.

`morph_scenario.py` owns no server lifecycle by design -- it drives a server
someone else started. That is the right boundary for one scenario and the
wrong one for a corpus: a directory of sixty-five files, each of which must
also survive `mutate_scenario.py` re-running it once per assertion, cannot be
driven by hand. This is the someone else.

One server per rung, not per file: every scenario is required to be
re-runnable against a database it has already run on (see the design spec's
authoring constraints), so a shared server is what that requirement buys, and
`--twice` is what proves it was paid.
"""
```

Followed by `RUNGS` (binary name and the env each server reads), `PORT_LINE = re.compile(r"listening on (?:ws://[^:]+:|port )(\d+)\s*$")`, `parse_port`, `find_server` (searching `build/*/examples/<rung>/` when `--build-dir` is absent), a `ServerProcess` context manager that spawns with `PASTEBIN_PORT=0`-style zero ports and blocks on `stdout` until `parse_port` answers, `run_rung`, and `main`.

- [ ] **Step 4: Run the test to verify it passes**

Run: `python3 scripts/scenario/test_morph_scenario.py 2>&1 | tail -3`
Expected: PASS, with the pre-existing case count plus four.

- [ ] **Step 5: Drive the existing corpus with it**

Run: `python3 scripts/scenario/run_scenarios.py --rung pastebin -v`
Expected: `paste-lifecycle.scenario` passes, `broken-on-purpose.scenario` is reported as *expected to fail* and does, exit `0`.

Run: `python3 scripts/scenario/run_scenarios.py --rung ledger --twice --mutate`
Expected: the ledger file passes twice against one database and no mutant survives.

- [ ] **Step 6: Commit**

```bash
git add scripts/scenario/run_scenarios.py scripts/scenario/test_morph_scenario.py scripts/scenario/README.md
git commit -m "scenario: drive a whole rung directory against a server it starts"
```

---

### Tasks 2-6: the corpus, one rung per task

Five tasks with an identical shape, ordered by ascending workflow floor so the pattern is established on the smallest rung first:

| Task | Rung | Floor | Actions to dispatch |
|---|---|---|---|
| 2 | pastebin | 8 | 6 |
| 3 | polls | 10 | 9 |
| 4 | bookmarks | 12 | 17 |
| 5 | ledger | 15 | 17 (14 new files; one exists) |
| 6 | kanban | 20 | 22 |

**Files, per task:**
- Create: `scripts/scenario/scenarios/<rung>/<journey>.scenario` — one per journey in that rung's inventory in the design spec.
- Modify: `scripts/scenario/README.md`'s rung table, on the last of the five.

**Interfaces:**
- Consumes: `run_scenarios.py` from Task 1, invoked as `--rung <name>`.
- Produces: scenario files only. No Python, no C++, no API.

Each task runs the same five steps.

- [ ] **Step 1: Probe the rung's real wire shapes**

Start the rung's server through the driver's own env table, or by hand, and use a throwaway Python probe against it (`sys.path.insert(0, 'scripts/scenario'); import morph_scenario`) to learn what the DTOs do not say: which fields each action actually requires, whether enums travel as integers or names, whether a result body is an object or a bare scalar, what each refusal string is *verbatim*.

This step is not optional and is not replaceable by reading headers. The ledger scenario's header comment exists precisely because five of its wire shapes are not guessable from the DTOs. Record what you find in each file's header comment.

- [ ] **Step 2: Author the rung's journeys**

One file per entry in that rung's inventory in the design spec's "The journey inventory". Each file must satisfy every Global Constraint above — in particular ≥4 distinct actions, ≥3 capture-chained `do` steps, re-runnability, and a read that proves each write.

- [ ] **Step 3: Run, re-run, and mutate**

```bash
python3 scripts/scenario/run_scenarios.py --rung <name>
python3 scripts/scenario/run_scenarios.py --rung <name> --twice
python3 scripts/scenario/run_scenarios.py --rung <name> --mutate
```

Expected: exit `0` from all three. A surviving mutant means an assertion that is not load-bearing — fix the assertion, not the mutant.

- [ ] **Step 4: Check the rung against the ruler**

Run: `python3 scripts/scenario/scenario_coverage.py 2>&1 | grep '<name>'`
Expected: `<name>  actions N/N dispatched (0 exempt), workflows M/M` with `M` at or above the floor and no `WORKFLOW GAPS` line naming this rung.

An action no journey reaches gets a *journey* added that needs it, not a bare call appended to an existing file.

- [ ] **Step 5: Commit**

```bash
git add scripts/scenario/scenarios/<name>
git commit -m "scenario: <name> journeys to the workflow floor"
```

---

### Task 7: The protocol axis

**Files:**
- Create: `scripts/scenario/scenarios/pastebin/wire-kinds-and-typeid-refusals.scenario`
- Create: `scripts/scenario/scenarios/pastebin/malformed-envelope.scenario`
- Create: `scripts/scenario/scenarios/bookmarks/model-cap-bites.scenario`
- Modify: `scripts/scenario/coverage_allowlist.json`
- Modify: `scripts/scenario/README.md`

**Interfaces:**
- Consumes: `run_scenarios.py` from Task 1.
- Produces: nothing later tasks read.

- [ ] **Step 1: The four kinds and five typeId refusals**

`wire-kinds-and-typeid-refusals.scenario`, each verified against a live server before it is written:

```
send attach typeId=PasteModel
expect ok
send attach
expect err message == "attach requires a typeId"
```

…and the same pair for `assign`, `instances`, `schemas`, plus `send register` with no `typeId` for `register requires a typeId`.

- [ ] **Step 2: The malformed envelope**

`malformed-envelope.scenario`. A string where the envelope wants a number:

```
send hello modelId="abc"
expect err message ~ "^envelope decode failed: "
```

- [ ] **Step 3: The model cap**

`model-cap-bites.scenario`. 256 `send register typeId=BookmarkModel` / `expect ok` pairs, then a 257th asserting `too many models`. Generate the repetitive body with a throwaway script rather than by hand; commit the file, not the generator.

- [ ] **Step 4: The four allowlist entries**

Add to `coverage_allowlist.json`'s `messages`, each naming the exact setting no rung installs and the file it would be installed in: `server busy` (`LimitPolicy::maxInFlightExecutes`), `timeout` (`LimitPolicy::executeTimeout`), `payload missing required field(s): ` (`PayloadCompleteness::RequireDeclaredFields`), `server shutting down` (only while draining; unreachable without a wall-clock overlap).

- [ ] **Step 5: Correct the README**

`scripts/scenario/README.md`'s "What this deliberately does not do" claims *"morph does not serve schemas over the wire … no envelope `kind` exposes them to a remote client"*. `send schemas typeId=PasteModel` returns the full JSON Schema document. Rewrite the paragraph to say what is actually true: the `schemas` kind serves them, and what the runner still does not do is validate a scenario's inputs against them. Also update the rung table with every directory the corpus now has.

- [ ] **Step 6: Verify both axes are green**

```bash
python3 scripts/scenario/run_scenarios.py
python3 scripts/scenario/scenario_coverage.py; echo "exit=$?"
python3 scripts/scenario/test_morph_scenario.py 2>&1 | tail -3
```

Expected: every rung passes, `scenario_coverage.py` prints no `UNCOVERED` and no `WORKFLOW GAPS` and exits `0`, and the self-test passes.

- [ ] **Step 7: Commit**

```bash
git add scripts/scenario
git commit -m "scenario: close the protocol axis and correct the schemas claim"
```

## Self-review

- **Spec coverage.** Driver → Task 1. 65 journeys → Tasks 2-6. Protocol scenarios and allowlist → Task 7. README correction → Task 7 Step 5. Verification protocol → each task's Steps 3-4 and Task 7 Step 6. Out-of-scope items appear in no task.
- **Placeholders.** The per-rung tasks deliberately do not inline 65 scenario files. That is not a placeholder: each file's content is determined by a live server's actual wire shapes, which Step 1 of each task exists to discover, and writing guesses here would be worse than writing nothing. The journeys themselves are named and fixed in the design spec.
- **Consistency.** `run_scenarios.py` is invoked identically in every task: `--rung`, `--twice`, `--mutate`, `--build-dir`, `-v`. `parse_port` is the only name Task 1's test depends on.
