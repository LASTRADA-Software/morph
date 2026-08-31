# Scenario Workflow Axis Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `scripts/scenario/scenario_coverage.py` with a second coverage axis that measures *domain workflows* — which of each rung's registered actions the corpus exercises, and whether it exercises them in threaded sequences rather than isolated calls — and gates on both.

**Architecture:** The tool already extracts a protocol surface from headers and measures it against the corpus. This adds a parallel extraction (`BRIDGE_REGISTER_ACTION` per rung), a per-scenario workflow classifier (does a later step consume an earlier `capture`?), per-rung floors, and two new gate conditions. Scenario files move into per-rung directories so a rung can be attributed and so one server can serve one directory.

**Tech Stack:** Python 3.9+ standard library only. `unittest`, run via `python3 scripts/scenario/test_morph_scenario.py`. No build step, no `pip install`.

## Global Constraints

- **Standard library only.** No third-party imports, no `pip install`, no build step. Verified on CPython 3.9.6, 3.12, 3.14.
- **Python 3.9 floor.** `from __future__ import annotations` at the top of every module.
- **SPDX header** (`# SPDX-License-Identifier: Apache-2.0`) on every new file.
- **No second parser.** Scenario files are read only through `morph_scenario.parse_scenario`.
- **Docstring on every public function**, house style (one-line summary, then detail), matching `scripts/scenario/morph_scenario.py`.
- **Fail loudly, never open.** Every extractor carries a plausibility floor and raises a named error when a structural marker is missing. A tool that measures nothing must never report success — this is the whole reason the tool exists.
- **The gate never weakens to pass.** Exit `0` covered-or-exempt, `1` a real gap, `2` the tool is broken. Existing meanings are unchanged.
- Existing symbols to build on, all already in `scenario_coverage.py`: `Surface`, `Exercised`, `Allowlist`, `SurfaceError`, `AllowlistError`, `extract_shipped_surface`, `floor_violations`, `load_scenarios`, `exercised_by`, `covers`, `load_allowlist`, `allowlist_problems`, `_render`, `main`, `_repo_root`, `MIN_KINDS`, `MIN_MESSAGES`, `SERVER_HALF_MARKER`, `DECODE_FUNCTION_MARKER`.

---

### Task 1: Extract the per-rung action universe

**Files:**
- Modify: `scripts/scenario/scenario_coverage.py`
- Test: `scripts/scenario/test_morph_scenario.py`

**Interfaces:**
- Produces:
  - `MIN_ACTIONS: int = 65` — plausibility floor for the total action count.
  - `extract_actions(sources: dict[str, str]) -> dict[str, frozenset[str]]` — maps rung name to its registered action names. `sources` maps rung name to the concatenated text of that rung's C++ files.
  - `shipped_action_sources(root: pathlib.Path) -> dict[str, str]` — reads every `.cpp`/`.hpp` under `examples/<rung>/` for each rung that ships a server.
  - `SERVER_RUNGS: tuple[str, ...] = ("pastebin", "bookmarks", "polls", "kanban", "ledger")`
  - `action_floor_violations(actions: dict[str, frozenset[str]]) -> list[str]`

- [ ] **Step 1: Write the failing test**

Append to `scripts/scenario/test_morph_scenario.py`. Extend the existing `scenario_coverage` import block with `MIN_ACTIONS`, `SERVER_RUNGS`, `action_floor_violations`, `extract_actions`, `shipped_action_sources`.

```python
_FIXTURE_ACTIONS = '''
BRIDGE_REGISTER_ACTION(demo::PasteModel, demo::CreatePaste, "CreatePaste")
BRIDGE_REGISTER_ACTION(demo::PasteModel, demo::GetPaste, "GetPaste", ::morph::model::Loggable::No)
  BRIDGE_REGISTER_ACTION(
      demo::PasteModel, demo::ListPastes, "ListPastes")
// BRIDGE_REGISTER_ACTION(demo::PasteModel, demo::NotReal, "NotReal")
'''


class ActionExtractionTest(unittest.TestCase):
    def test_extracts_the_registered_action_names(self) -> None:
        actions = extract_actions({"demo": _FIXTURE_ACTIONS})
        self.assertEqual(actions["demo"], frozenset({"CreatePaste", "GetPaste", "ListPastes"}))

    def test_extraction_spans_a_registration_split_across_lines(self) -> None:
        # Same failure mode the message regexes had: a reflowed call site must
        # not silently vanish from the universe.
        actions = extract_actions({"demo": _FIXTURE_ACTIONS})
        self.assertIn("ListPastes", actions["demo"])

    def test_floor_rejects_an_implausibly_small_action_universe(self) -> None:
        problems = action_floor_violations({"demo": frozenset({"CreatePaste"})})
        self.assertTrue(problems)
        self.assertTrue(any("action" in p for p in problems))

    def test_floor_accepts_the_real_examples_tree(self) -> None:
        actions = extract_actions(shipped_action_sources(_repo_root()))
        self.assertEqual(action_floor_violations(actions), [])
        self.assertEqual(set(actions), set(SERVER_RUNGS))
        for rung in SERVER_RUNGS:
            self.assertTrue(actions[rung], f"{rung} registers no actions")

    def test_real_tree_pins_the_known_action_counts(self) -> None:
        # A set-pin, not a >= check: the floor only catches shrinkage, so an
        # ADDED action would otherwise be invisible. When this fails, a human
        # decides whether the new action needs a workflow or an allowlist entry.
        actions = extract_actions(shipped_action_sources(_repo_root()))
        self.assertEqual(len(actions["pastebin"]), 6)
        self.assertEqual(len(actions["polls"]), 9)
        self.assertEqual(len(actions["bookmarks"]), 17)
        self.assertEqual(len(actions["ledger"]), 17)
        self.assertEqual(len(actions["kanban"]), 22)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd scripts/scenario && python3 test_morph_scenario.py 2>&1 | tail -5
```

Expected: FAIL — `ImportError: cannot import name 'extract_actions' from 'scenario_coverage'`.

- [ ] **Step 3: Write minimal implementation**

Add to `scripts/scenario/scenario_coverage.py`, beside the existing extractors:

```python
# Rungs that ship a `ladder_<rung>_server`, and so can be driven by a scenario.
# `lims` and `crm` are deliberately absent: neither ships a server (crm ships no
# client either -- see its README's "What is not built"), so no scenario can
# reach them. lims's missing server is filed separately.
SERVER_RUNGS = ("pastebin", "bookmarks", "polls", "kanban", "ledger")

# Plausibility floor for the whole action universe, measured at 71. Same
# purpose as MIN_KINDS/MIN_MESSAGES: if the macro is renamed, this extractor
# silently finds nothing and would report full workflow coverage over an empty
# universe. Falling short is a statement about the tool, not the tree.
MIN_ACTIONS = 65

# `\s*` between the parts, so a registration reflowed across lines by
# clang-format is still found. The third argument is the wire action name --
# the string a scenario's `do` line uses -- which is why it, not the C++ type,
# is what gets extracted.
_ACTION = re.compile(
    r'BRIDGE_REGISTER_ACTION\(\s*[\w:]+\s*,\s*[\w:]+\s*,\s*"([^"]+)"'
)


def extract_actions(sources: dict[str, str]) -> dict[str, frozenset[str]]:
    """Extracts each rung's registered action names from its C++ text.

    @param sources Rung name to the concatenated text of that rung's sources.
    @return Rung name to the set of wire action names it registers.
    """
    return {rung: frozenset(_ACTION.findall(text)) for rung, text in sources.items()}


def shipped_action_sources(root: pathlib.Path) -> dict[str, str]:
    """Reads every C++ source under `examples/<rung>/` for each server rung.

    @param root Repository root.
    @return Rung name to concatenated source text, ready for `extract_actions`.
    @throws SurfaceError if a rung's directory is missing -- a renamed or moved
            rung must break this loudly rather than silently drop its actions.
    """
    sources: dict[str, str] = {}
    for rung in SERVER_RUNGS:
        directory = root / "examples" / rung
        if not directory.is_dir():
            raise SurfaceError(
                f"rung directory {directory} not found -- SERVER_RUNGS is stale, "
                "or a rung moved; refusing to report coverage over a partial tree"
            )
        chunks = [
            path.read_text(encoding="utf-8", errors="replace")
            for path in sorted(directory.rglob("*"))
            if path.is_file() and path.suffix in {".cpp", ".hpp"}
        ]
        sources[rung] = "\n".join(chunks)
    return sources


def action_floor_violations(actions: dict[str, frozenset[str]]) -> list[str]:
    """Reports every way the action universe is too small to be believable.

    @param actions Rung name to its registered action names.
    @return One message per problem; empty when the extraction is plausible.
    """
    problems: list[str] = []
    total = sum(len(names) for names in actions.values())
    if total < MIN_ACTIONS:
        problems.append(
            f"found {total} registered actions, expected at least {MIN_ACTIONS} "
            "-- the extractor is probably broken, not the examples tree"
        )
    for rung, names in sorted(actions.items()):
        if not names:
            problems.append(f"rung '{rung}' registers no actions -- extraction is broken for it")
    return problems
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cd scripts/scenario && python3 test_morph_scenario.py 2>&1 | tail -3
```

Expected: `OK`.

- [ ] **Step 5: Commit**

```bash
git add scripts/scenario/scenario_coverage.py scripts/scenario/test_morph_scenario.py
git commit -s -m "scenario: extract the per-rung action universe

The second coverage axis needs a universe too. Every
BRIDGE_REGISTER_ACTION(Model, Action, \"Name\") under examples/<rung>/ is a
domain action a scenario could dispatch; there are 71 across the five rungs
that ship a server.

Carries the same guards as the protocol extractor: a plausibility floor, a
loud failure when a rung directory is missing, and a set-pin test on the
per-rung counts. The floor only catches shrinkage, so the pin is what makes
an ADDED action visible -- it fails, and a human decides whether the action
needs a workflow or an exemption."
```

---

### Task 2: Classify scenarios by rung, and detect workflows

**Files:**
- Modify: `scripts/scenario/scenario_coverage.py`
- Test: `scripts/scenario/test_morph_scenario.py`

**Interfaces:**
- Consumes: `load_scenarios` (existing).
- Produces:
  - `WORKFLOW_MIN_ACTIONS: int = 4`, `WORKFLOW_MIN_CHAINED: int = 3`
  - `ScenarioFacts` — frozen dataclass: `rung: str`, `path: str`, `actions: frozenset[str]`, `chained_steps: int`, `is_workflow: bool`.
  - `scenario_facts(scenario) -> ScenarioFacts` — takes one `morph_scenario.Scenario`.
  - `load_scenarios(directory, recursive: bool = False)` — existing function gains recursion.

The rung is the scenario file's parent directory name, or `""` when the file sits directly in `scenarios/`.

A step is *chained* when any of its `args` contains a `$name` reference to a name some earlier step captured.

- [ ] **Step 1: Write the failing test**

Append to `scripts/scenario/test_morph_scenario.py`; extend the import block with `ScenarioFacts`, `WORKFLOW_MIN_ACTIONS`, `WORKFLOW_MIN_CHAINED`, `scenario_facts`.

```python
_FLAT_LIST = '''
model PasteModel
client alice
do CreatePaste content="a"
expect ok capture one=$.id
do ListPastes
expect ok field pastes ~ .
do GetPaste id=fixed-not-captured
expect err message == "GetPaste: no such paste"
'''

_REAL_WORKFLOW = '''
model PasteModel
client alice
do CreatePaste content="a"
expect ok capture id=$.id
do GetPaste id=$id
expect ok field content == "a"
do EditPaste id=$id content="b"
expect ok
do ListPastes
expect ok field pastes ~ $id
do DeletePaste id=$id
expect ok
'''


class WorkflowClassificationTest(unittest.TestCase):
    def test_a_flat_list_of_calls_is_not_a_workflow(self) -> None:
        # Three distinct actions, but nothing consumes the captured id, so this
        # is three one-shot calls sharing a socket.
        facts = scenario_facts(parse_scenario(_FLAT_LIST, "scenarios/pastebin/flat.scenario"))
        self.assertEqual(facts.chained_steps, 0)
        self.assertFalse(facts.is_workflow)

    def test_a_threaded_sequence_is_a_workflow(self) -> None:
        facts = scenario_facts(parse_scenario(_REAL_WORKFLOW, "scenarios/pastebin/w.scenario"))
        self.assertGreaterEqual(facts.chained_steps, WORKFLOW_MIN_CHAINED)
        self.assertTrue(facts.is_workflow)

    def test_records_the_actions_and_the_rung(self) -> None:
        facts = scenario_facts(parse_scenario(_REAL_WORKFLOW, "scenarios/pastebin/w.scenario"))
        self.assertEqual(
            facts.actions,
            frozenset({"CreatePaste", "GetPaste", "EditPaste", "ListPastes", "DeletePaste"}),
        )
        self.assertEqual(facts.rung, "pastebin")

    def test_a_file_directly_in_scenarios_has_no_rung(self) -> None:
        facts = scenario_facts(parse_scenario(_REAL_WORKFLOW, "scenarios/loose.scenario"))
        self.assertEqual(facts.rung, "")

    def test_enough_chaining_but_too_few_actions_is_not_a_workflow(self) -> None:
        # Guards WORKFLOW_MIN_ACTIONS independently of WORKFLOW_MIN_CHAINED:
        # create-then-read-repeatedly threads state but goes nowhere.
        text = (
            'model PasteModel\nclient alice\n'
            'do CreatePaste content="a"\nexpect ok capture id=$.id\n'
            'do GetPaste id=$id\nexpect ok field content == "a"\n'
            'do GetPaste id=$id\nexpect ok field readCount.num == 2\n'
            'do GetPaste id=$id\nexpect ok field readCount.num == 3\n'
        )
        facts = scenario_facts(parse_scenario(text, "scenarios/pastebin/x.scenario"))
        self.assertGreaterEqual(facts.chained_steps, WORKFLOW_MIN_CHAINED)
        self.assertEqual(len(facts.actions), 2)
        self.assertFalse(facts.is_workflow)

    def test_load_scenarios_recurses_when_asked(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "pastebin").mkdir()
            (root / "pastebin" / "a.scenario").write_text(_REAL_WORKFLOW, encoding="utf-8")
            (root / "b.scenario").write_text(_REAL_WORKFLOW, encoding="utf-8")
            self.assertEqual(len(load_scenarios(root)), 1)
            self.assertEqual(len(load_scenarios(root, recursive=True)), 2)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd scripts/scenario && python3 test_morph_scenario.py 2>&1 | tail -5
```

Expected: FAIL — `ImportError: cannot import name 'scenario_facts' from 'scenario_coverage'`.

- [ ] **Step 3: Write minimal implementation**

Add to `scripts/scenario/scenario_coverage.py`, and replace `load_scenarios` with the recursive version:

```python
# What separates a workflow from a list of calls. Set by measuring the corpus,
# not guessed: at three-and-two both shipped files qualify, and they are format
# demonstrations rather than journeys. Four-and-three puts the bar just above
# them -- the shortest qualifying shape is roughly
# `sign in -> create -> edit -> read back`. Both halves are load-bearing and
# neither implies the other: a file reading the same id four times threads
# state without going anywhere, and a file firing four unrelated actions goes
# nowhere while threading nothing.
WORKFLOW_MIN_ACTIONS = 4
WORKFLOW_MIN_CHAINED = 3


@dataclass(frozen=True)
class ScenarioFacts:
    """What one scenario file dispatches, and whether it is a workflow."""

    rung: str
    path: str
    actions: frozenset[str]
    chained_steps: int
    is_workflow: bool


def scenario_facts(scenario: Scenario) -> ScenarioFacts:
    """Summarises one parsed scenario for the workflow axis.

    A step counts as *chained* when one of its arguments references a name an
    earlier step captured. That is what distinguishes a journey from a list:
    threading state means each step depends on what the last one returned.

    @param scenario A scenario already parsed by `morph_scenario.parse_scenario`.
    @return Its rung, dispatched actions, chained-step count and workflow verdict.
    """
    path = pathlib.PurePath(scenario.path)
    rung = path.parent.name if path.parent.name != "scenarios" else ""

    actions: set[str] = set()
    captured: set[str] = set()
    chained = 0
    for step in scenario.steps:
        if step.verb == "do" and step.args:
            actions.add(step.args[0])
        # Read references before recording this step's own captures: a step
        # cannot chain off a value it produces itself.
        if any(
            name in captured
            for arg in step.args
            for name in _referenced_captures(arg)
        ):
            chained += 1
        for assertion in step.assertions:
            if assertion.kind == "capture" and assertion.capture_name:
                captured.add(assertion.capture_name)

    is_workflow = len(actions) >= WORKFLOW_MIN_ACTIONS and chained >= WORKFLOW_MIN_CHAINED
    return ScenarioFacts(
        rung=rung,
        path=scenario.path,
        actions=frozenset(actions),
        chained_steps=chained,
        is_workflow=is_workflow,
    )


def _referenced_captures(token: str) -> list[str]:
    """Returns every `$name` / `${name}` reference in @p token.

    Uses the runner's own `_VARIABLE` pattern so the two cannot disagree about
    what a capture reference looks like.
    """
    return [
        braced or bare
        for braced, bare in morph_scenario._VARIABLE.findall(token)  # noqa: SLF001
    ]


def load_scenarios(directory: pathlib.Path, recursive: bool = False) -> list[Scenario]:
    """Parses every `*.scenario` under @p directory through the runner's parser.

    Deliberately re-uses `morph_scenario.parse_scenario` rather than reading the
    files here: a second parser could drift from the real one, and then this
    report would be measuring a format nothing runs.

    @param directory Directory to read.
    @param recursive When true, descends into per-rung subdirectories.
    @return The parsed scenarios, ordered by path.
    """
    pattern = "**/*.scenario" if recursive else "*.scenario"
    out: list[Scenario] = []
    for path in sorted(directory.glob(pattern)):
        out.append(morph_scenario.parse_scenario(path.read_text(encoding="utf-8"), str(path)))
    return out
```

Note: `_referenced_captures` reaches into `morph_scenario._VARIABLE`, a private name. That is deliberate and is the lesser evil — re-declaring the pattern here would let the two definitions drift, and drift is the failure this whole tool exists to prevent. If `morph_scenario` ever exports it publicly, switch to that.

- [ ] **Step 4: Run test to verify it passes**

```bash
cd scripts/scenario && python3 test_morph_scenario.py 2>&1 | tail -3
```

Expected: `OK`.

- [ ] **Step 5: Commit**

```bash
git add scripts/scenario/scenario_coverage.py scripts/scenario/test_morph_scenario.py
git commit -s -m "scenario: tell a workflow from a list of calls

'More than one call' has to be mechanical or the gate is an opinion. A
scenario counts as a workflow when a later step consumes a value an earlier
step captured -- threading state is what makes a journey a journey.

Both thresholds are load-bearing and tested independently: three distinct
actions stops create-then-read qualifying, and two chained steps stops three
unrelated calls qualifying. A file that reads the same id twice threads state
without going anywhere; a file that fires three unrelated actions goes
nowhere while threading nothing.

_referenced_captures borrows morph_scenario's own _VARIABLE pattern rather
than re-declaring it. Reaching for a private name is the lesser evil: two
definitions of what a capture reference looks like could drift, and drift is
the failure this tool exists to catch."
```

---

### Task 3: Move the corpus into per-rung directories

**Files:**
- Move: `scripts/scenario/scenarios/pastebin.scenario` → `scripts/scenario/scenarios/pastebin/paste-lifecycle.scenario`
- Move: `scripts/scenario/scenarios/pastebin_broken.scenario` → `scripts/scenario/scenarios/pastebin/broken-on-purpose.scenario`
- Move: `scripts/scenario/scenarios/bookmarks_login.scenario` → `scripts/scenario/scenarios/bookmarks/login-retry-and-forged-tokens.scenario`
- Modify: `scripts/scenario/scenario_coverage.py` (pass `recursive=True` from `main`)
- Modify: `scripts/scenario/README.md`
- Test: `scripts/scenario/test_morph_scenario.py`

**Interfaces:**
- Consumes: `load_scenarios(..., recursive=True)` from Task 2.
- Produces: no new symbols. `main` now loads recursively.

`broken-on-purpose.scenario` is the deliberately-failing demonstration file. It must stay excluded from any pass/fail run, but it *is* still parsed by the coverage tool — which is correct, since it dispatches real actions.

- [ ] **Step 1: Move the files with git, preserving history**

```bash
cd scripts/scenario/scenarios
mkdir -p pastebin bookmarks
git mv pastebin.scenario        pastebin/paste-lifecycle.scenario
git mv pastebin_broken.scenario pastebin/broken-on-purpose.scenario
git mv bookmarks_login.scenario bookmarks/login-retry-and-forged-tokens.scenario
git status --short
```

Expected: three `R` (rename) entries, no `D`/`A` pairs.

- [ ] **Step 2: Make `main` load recursively**

In `scripts/scenario/scenario_coverage.py`, find the `load_scenarios(pathlib.Path(args.scenarios))` call inside `main` and change it to:

```python
        scenarios = load_scenarios(pathlib.Path(args.scenarios), recursive=True)
```

- [ ] **Step 3: Write a test pinning the layout**

Append to `scripts/scenario/test_morph_scenario.py`:

```python
class CorpusLayoutTest(unittest.TestCase):
    def test_every_shipped_scenario_lives_under_a_rung_directory(self) -> None:
        # The rung is the parent directory name, and that is how per-rung action
        # coverage is attributed. A file loose in scenarios/ would be counted
        # against no rung at all and silently excluded from every floor.
        root = _repo_root() / "scripts" / "scenario" / "scenarios"
        loose = sorted(p.name for p in root.glob("*.scenario"))
        self.assertEqual(loose, [], "scenario files must live in scenarios/<rung>/")

    def test_every_rung_directory_names_a_real_server_rung(self) -> None:
        root = _repo_root() / "scripts" / "scenario" / "scenarios"
        found = sorted(p.name for p in root.iterdir() if p.is_dir())
        for name in found:
            self.assertIn(name, SERVER_RUNGS, f"scenarios/{name}/ is not a rung that ships a server")
```

- [ ] **Step 4: Run the tests and the real report**

```bash
cd scripts/scenario && python3 test_morph_scenario.py 2>&1 | tail -3
```

Expected: `OK`.

```bash
cd /home/yaraslau/repo/morph && python3 scripts/scenario/scenario_coverage.py; echo "exit=$?"
```

Expected: `exit=1`, and the protocol-axis numbers unchanged from before the move (4/8 kinds, 4/17 refusals). If they changed, the recursion is picking up or dropping files — fix it before continuing.

- [ ] **Step 5: Update the README's file table**

In `scripts/scenario/README.md`, replace the table that lists `pastebin.scenario`, `bookmarks_login.scenario` and `pastebin_broken.scenario` with one describing the new layout:

```markdown
`scenarios/` holds one directory per rung, and one file per workflow:

| Directory | Server | What it covers |
|---|---|---|
| `pastebin/` | `ladder_pastebin_server` | paste lifecycle, privacy, hostile envelopes |
| `bookmarks/` | `ladder_bookmarks_server` | sign-in, session handling, forged and borrowed tokens |

The rung a scenario belongs to is its parent directory name — that is how
per-rung action coverage is attributed, so a file loose in `scenarios/` is
counted against no rung and is refused by the self-test.

`pastebin/broken-on-purpose.scenario` is *meant to fail* — see below.
```

- [ ] **Step 6: Commit**

```bash
git add -A scripts/scenario
git commit -s -m "scenario: one directory per rung, one file per workflow

The corpus is about to grow from three files to dozens, and the workflow
axis attributes coverage per rung. Both need the rung to be structural
rather than guessed from a filename prefix, so it is the parent directory.

git mv preserves history. A self-test pins the layout in both directions: no
file may sit loose in scenarios/ (it would be counted against no rung and
silently excluded from every floor), and no directory may name something
that is not a rung shipping a server."
```

---

### Task 4: Report and gate the workflow axis

**Files:**
- Modify: `scripts/scenario/scenario_coverage.py`
- Modify: `scripts/scenario/coverage_allowlist.json`
- Modify: `scripts/scenario/README.md`
- Test: `scripts/scenario/test_morph_scenario.py`

**Interfaces:**
- Consumes: everything from Tasks 1–3.
- Produces:
  - `WORKFLOW_FLOORS: dict[str, int]` — per-rung minimum workflow counts.
  - `workflow_problems(actions, facts, allowlist) -> list[str]` — unexercised actions and unmet floors.
  - `Allowlist` gains an `actions: dict[str, str]` section, keyed `"<rung>/<Action>"`.

The floors, from the design spec: `pastebin` 8, `polls` 10, `bookmarks` 12, `ledger` 15, `kanban` 20.

**This task is expected to leave the gate failing.** After it, the tool reports the real workflow gap — 8 of 71 actions, 0 of 65 required workflows. Later work closes it. Do not weaken the floors to make it pass.

- [ ] **Step 1: Write the failing test**

Append to `scripts/scenario/test_morph_scenario.py`; extend the import block with `WORKFLOW_FLOORS`, `workflow_problems`.

```python
class WorkflowGateTest(unittest.TestCase):
    _ACTIONS = {"demo": frozenset({"Alpha", "Beta", "Gamma"})}

    def _facts(self, count: int, actions: frozenset) -> list:
        return [
            ScenarioFacts(rung="demo", path=f"scenarios/demo/{i}.scenario",
                          actions=actions, chained_steps=2, is_workflow=True)
            for i in range(count)
        ]

    def test_reports_an_action_no_scenario_dispatches(self) -> None:
        facts = self._facts(1, frozenset({"Alpha", "Beta"}))
        problems = workflow_problems(self._ACTIONS, facts, Allowlist(kinds={}, messages={}, actions={}),
                                     floors={"demo": 1})
        self.assertTrue(any("Gamma" in p for p in problems))

    def test_an_allowlisted_action_is_not_reported(self) -> None:
        facts = self._facts(1, frozenset({"Alpha", "Beta"}))
        allow = Allowlist(kinds={}, messages={}, actions={"demo/Gamma": "runner-only principal"})
        problems = workflow_problems(self._ACTIONS, facts, allow, floors={"demo": 1})
        self.assertFalse(any("Gamma" in p for p in problems))

    def test_reports_a_rung_below_its_workflow_floor(self) -> None:
        facts = self._facts(2, frozenset({"Alpha", "Beta", "Gamma"}))
        problems = workflow_problems(self._ACTIONS, facts, Allowlist(kinds={}, messages={}, actions={}),
                                     floors={"demo": 5})
        self.assertTrue(any("floor" in p and "demo" in p for p in problems))

    def test_non_workflow_files_do_not_count_towards_the_floor(self) -> None:
        # Five files that are flat lists must not satisfy a floor of 5.
        flat = [
            ScenarioFacts(rung="demo", path=f"scenarios/demo/{i}.scenario",
                          actions=frozenset({"Alpha", "Beta", "Gamma"}),
                          chained_steps=0, is_workflow=False)
            for i in range(5)
        ]
        problems = workflow_problems(self._ACTIONS, flat, Allowlist(kinds={}, messages={}, actions={}),
                                     floors={"demo": 5})
        self.assertTrue(any("floor" in p for p in problems))

    def test_the_shipped_floors_name_only_real_rungs(self) -> None:
        for rung in WORKFLOW_FLOORS:
            self.assertIn(rung, SERVER_RUNGS)
        self.assertEqual(set(WORKFLOW_FLOORS), set(SERVER_RUNGS))
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cd scripts/scenario && python3 test_morph_scenario.py 2>&1 | tail -5
```

Expected: FAIL — `ImportError: cannot import name 'WORKFLOW_FLOORS' from 'scenario_coverage'`.

- [ ] **Step 3: Extend `Allowlist` with an actions section**

In `scenario_coverage.py`, add `actions: dict[str, str]` to the `Allowlist` dataclass, and read it in `load_allowlist` via the existing `_allowlist_section` helper:

```python
    return Allowlist(
        kinds=_allowlist_section(raw.get("kinds", {}), "kinds", path),
        messages=_allowlist_section(raw.get("messages", {}), "messages", path),
        actions=_allowlist_section(raw.get("actions", {}), "actions", path),
    )
```

Every existing `Allowlist(...)` construction in the tests must gain `actions={}` — update them.

- [ ] **Step 4: Write the gate**

```python
# Minimum number of qualifying workflows per rung, scaled to how finite that
# rung's space of meaningful journeys is. pastebin's six actions admit a
# near-exhaustive set; kanban's twenty-two do not, so its floor targets the
# important journeys rather than the closure. See the design spec, "How many,
# per rung". Floors, not quotas -- a rung may carry more, and deleting
# workflows below the floor is meant to fail CI rather than erode quietly.
WORKFLOW_FLOORS = {
    "pastebin": 8,
    "polls": 10,
    "bookmarks": 12,
    "ledger": 15,
    "kanban": 20,
}


def workflow_problems(
    actions: dict[str, frozenset[str]],
    facts: list[ScenarioFacts],
    allowlist: Allowlist,
    floors: dict[str, int] | None = None,
) -> list[str]:
    """Reports unexercised actions and rungs below their workflow floor.

    @param actions   Rung name to its registered action names.
    @param facts     One entry per parsed scenario.
    @param allowlist Exemptions; `actions` is keyed `"<rung>/<Action>"`.
    @param floors    Per-rung workflow minimums; defaults to `WORKFLOW_FLOORS`.
    @return One message per problem; empty when both gates are satisfied.
    """
    effective = WORKFLOW_FLOORS if floors is None else floors
    problems: list[str] = []

    for rung, registered in sorted(actions.items()):
        dispatched: set[str] = set()
        workflows = 0
        for fact in facts:
            if fact.rung != rung:
                continue
            dispatched |= fact.actions
            if fact.is_workflow:
                workflows += 1

        for name in sorted(registered - dispatched):
            if f"{rung}/{name}" in allowlist.actions:
                continue
            problems.append(f"{rung}: action '{name}' is dispatched by no scenario")

        floor = effective.get(rung, 0)
        if workflows < floor:
            problems.append(
                f"{rung}: {workflows} workflows, floor is {floor} "
                "(a file counts only if it chains actions through captured state)"
            )
    return problems
```

- [ ] **Step 5: Wire it into `main` and `_render`**

In `main`, after the existing protocol gate work, build the action universe and facts, and fold the result into both the report and the exit code. The workflow gate produces exit `1` (a real gap), and a broken *extraction* produces exit `2`:

```python
    try:
        actions = extract_actions(shipped_action_sources(root))
    except (OSError, SurfaceError) as exc:
        print(f"scenario_coverage: {exc}", file=sys.stderr)
        return 2
    if not args.no_floor:
        action_problems = action_floor_violations(actions)
        if action_problems:
            for problem in action_problems:
                print(f"scenario_coverage: {problem}", file=sys.stderr)
            return 2

    facts = [scenario_facts(scenario) for scenario in scenarios]
    flow_problems = workflow_problems(actions, facts, allowlist)
```

Print a workflow section from `_render` (pass `actions` and `facts` in), showing per rung: actions dispatched over registered, workflows over floor. Then return `1` if `flow_problems` is non-empty, keeping the existing protocol gate too.

- [ ] **Step 6: Allowlist the one action known to be undrivable**

Add an `actions` section to `scripts/scenario/coverage_allowlist.json`, keeping `kinds` and `messages` exactly as they are:

```json
  "actions": {
    "ledger/RunReportJob": "LedgerModel::execute(const RunReportJob&) refuses any principal but kReportRunnerPrincipal -- it is dispatched by ledger::app::App's report runner, never by a user client (see report_dto.hpp). A scenario can submit a report and assert Pending, and can assert the refusal when a user client tries to run it, but cannot drive the job to Done: that needs the server's own runner to tick, and waiting on it would mean a sleep, which this runner forbids by design."
  }
```

- [ ] **Step 7: Run everything**

```bash
cd scripts/scenario && python3 test_morph_scenario.py 2>&1 | tail -3
```

Expected: `OK`.

```bash
cd /home/yaraslau/repo/morph && python3 scripts/scenario/scenario_coverage.py; echo "exit=$?"
```

Expected: `exit=1`, with a workflow section reporting roughly 8/71 actions dispatched and every rung below its floor. **That is the correct outcome** — it is the gap the next plan closes. Confirm the per-rung action counts match Task 1's pinned numbers.

- [ ] **Step 8: Document it**

Add to `scripts/scenario/README.md`, in the coverage section, a paragraph describing the workflow axis: what counts as a workflow (a later step consuming an earlier `capture`), the per-rung floors, and that an action which genuinely cannot be driven gets an allowlist entry with a reason.

- [ ] **Step 9: Commit**

```bash
git add -A scripts/scenario
git commit -s -m "scenario: gate on workflow coverage as well as protocol

Two new conditions: every registered action is dispatched by some scenario
in its rung, and each rung meets a workflow floor counting only files that
chain actions through captured state.

The second is what stops the first being satisfied by a flat list. Twenty
one-shot calls cover twenty actions and prove nothing about sequencing --
which is where strand ordering, exactly-once replay and second-call
authorization actually live.

The gate fails today: 8 of 71 actions, and no rung meets its floor. That is
the measurement working, and closing it is the next plan. One action is
allowlisted with its reason -- ledger's RunReportJob refuses any principal
but the report runner's, so no client scenario can dispatch it."
```

---

## Self-Review

**Spec coverage.** Every section of `2026-08-30-scenario-workflows-design.md` maps to a task: the action universe (Task 1), the workflow definition and per-rung attribution (Task 2), the layout (Task 3), measurement, floors, gating and the known `RunReportJob` exemption (Task 4). The spec's "Where the workflows come from" section is guidance for the *next* plan, which authors the workflows — nothing to implement here.

**Not in this plan, by design.** Authoring the workflows themselves, `run_all.sh`, and CI wiring. This plan makes the gap measurable; the next closes it. Splitting here means the authoring plan can be written against a real report rather than a prediction, exactly as the protocol-axis plan was.

**Placeholder scan.** No TBD/TODO. Every code step carries complete code; every command carries expected output — including the two steps whose expected output is a *failing* gate, which is the intended state.

**Type consistency.** `ScenarioFacts(rung, path, actions, chained_steps, is_workflow)` is constructed with those exact fields in Task 4's tests. `extract_actions` returns `dict[str, frozenset[str]]` and is consumed as such by `action_floor_violations` and `workflow_problems`. `Allowlist` gains `actions` in Task 4 Step 3 *before* any code constructs it with three sections. `load_scenarios`'s new `recursive` parameter defaults to `False`, so Task 2 does not break Task 3's not-yet-written call site.
