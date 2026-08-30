# Scenario Coverage Report Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `scripts/scenario/scenario_coverage.py`, which measures how much of morph's wire-protocol surface the scenario corpus exercises, and fails when something is uncovered without a written exemption.

**Architecture:** One standard-library Python script beside the existing runner. It extracts the protocol surface (envelope kinds and server refusal strings) from `include/morph/core/remote.hpp` by regex, extracts what the corpus exercises by re-using the runner's own parser (`parse_scenario`), diffs the two against a JSON allowlist, and exits non-zero on any unexempted gap. Because a regex extractor over someone else's source fails *open*, it carries a plausibility floor and is itself tested against fixtures with known-wrong answers.

**Tech Stack:** Python 3.9+ standard library only. `unittest` for tests, run via `python3 scripts/scenario/test_morph_scenario.py`. No build step, no `pip install`.

## Global Constraints

- **Standard library only.** No third-party imports, no `pip install`, no build step. Verified on CPython 3.9.6, 3.12, 3.14.
- **Python 3.9 floor.** `from __future__ import annotations` at the top of every module so `list[str]` / `X | None` annotations parse on 3.9.
- **SPDX header.** Every new file starts with `# SPDX-License-Identifier: Apache-2.0`.
- **No second parser.** Scenario files are read only through `morph_scenario.parse_scenario`. Never re-implement scenario parsing.
- **Docstrings on every public function**, matching the existing style in `morph_scenario.py` (a one-line summary, then detail).
- Source of truth for the protocol surface is `include/morph/core/remote.hpp`. Paths are resolved relative to the repository root, found by walking up from `__file__`.

---

### Task 1: Extract the protocol surface from `remote.hpp`

**Files:**
- Create: `scripts/scenario/scenario_coverage.py`
- Test: `scripts/scenario/test_morph_scenario.py` (append a new `TestCase`)

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `Surface` — frozen dataclass with fields `kinds: frozenset[str]`, `exact_messages: frozenset[str]`, `message_prefixes: frozenset[str]`.
  - `extract_surface(header_text: str) -> Surface`
  - `floor_violations(surface: Surface) -> list[str]` — empty list means the surface is plausible.
  - `MIN_KINDS: int = 8`, `MIN_MESSAGES: int = 18`

- [ ] **Step 1: Write the failing test**

Append to `scripts/scenario/test_morph_scenario.py`, and add `scenario_coverage` imports at the top of the file (keep the existing `from morph_scenario import (...)` block untouched; add a second import block below it):

```python
from scenario_coverage import (
    MIN_KINDS,
    MIN_MESSAGES,
    Surface,
    extract_surface,
    floor_violations,
)

_FIXTURE_HEADER = '''
    if (env.kind == "register") {
    } else if (env.kind == "execute") {
        reply(makeErr("model not found", env.callId));
    } else {
        reply(makeErr("unknown envelope kind: " + env.kind, env.callId));
    }
    rejectAndRelease("server busy");
    throw std::runtime_error("register requires a typeId");
    const std::string message = "payload missing required field(s): " + missing;
'''


class SurfaceExtractionTest(unittest.TestCase):
    def test_finds_every_envelope_kind(self) -> None:
        surface = extract_surface(_FIXTURE_HEADER)
        self.assertEqual(surface.kinds, frozenset({"register", "execute"}))

    def test_separates_exact_messages_from_prefixes(self) -> None:
        surface = extract_surface(_FIXTURE_HEADER)
        self.assertIn("model not found", surface.exact_messages)
        self.assertIn("server busy", surface.exact_messages)
        self.assertIn("register requires a typeId", surface.exact_messages)
        # A message ending in ": " is a prefix -- the runtime value carries a
        # suffix the source cannot know.
        self.assertIn("unknown envelope kind: ", surface.message_prefixes)
        self.assertIn("payload missing required field(s): ", surface.message_prefixes)
        self.assertNotIn("unknown envelope kind: ", surface.exact_messages)

    def test_floor_rejects_an_implausibly_small_surface(self) -> None:
        # The whole point: a rename in remote.hpp must break this loudly rather
        # than report full coverage over an empty universe.
        violations = floor_violations(extract_surface(_FIXTURE_HEADER))
        self.assertTrue(violations)
        self.assertTrue(any("kind" in v for v in violations))

    def test_floor_accepts_the_real_header(self) -> None:
        surface = extract_surface(_real_header_text())
        self.assertEqual(floor_violations(surface), [])
        self.assertGreaterEqual(len(surface.kinds), MIN_KINDS)
        self.assertGreaterEqual(
            len(surface.exact_messages) + len(surface.message_prefixes), MIN_MESSAGES
        )

    def test_real_header_carries_the_kinds_the_spec_records(self) -> None:
        surface = extract_surface(_real_header_text())
        self.assertEqual(
            surface.kinds,
            frozenset({"register", "execute", "deregister", "hello",
                       "attach", "assign", "instances", "schemas"}),
        )
```

Add this helper next to the fixture:

```python
def _real_header_text() -> str:
    """Reads the actual remote.hpp this repository ships."""
    return (_repo_root() / "include" / "morph" / "core" / "remote.hpp").read_text(encoding="utf-8")
```

and import `_repo_root` from `scenario_coverage` in the same import block.

- [ ] **Step 2: Run test to verify it fails**

```bash
cd scripts/scenario && python3 test_morph_scenario.py -v 2>&1 | tail -5
```

Expected: FAIL — `ModuleNotFoundError: No module named 'scenario_coverage'`.

- [ ] **Step 3: Write minimal implementation**

Create `scripts/scenario/scenario_coverage.py`:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measure how much of morph's wire-protocol surface the scenario corpus covers.

The universe is not "every use case" -- that is unbounded and duplicates the
in-process C++ suite. It is the *protocol surface*: every envelope kind
`RemoteServer::dispatchMessage` handles, and every refusal string it can put on
the wire. Both are enumerable from `include/morph/core/remote.hpp`, which makes
"are we covering everything?" a number rather than an opinion.

See docs/superpowers/specs/2026-08-30-scenario-coverage-design.md.

Standard library only. Needs no server: it reads source and scenario text.
"""

from __future__ import annotations

import pathlib
import re
from dataclasses import dataclass

# Floors for the plausibility check. A regex extractor over someone else's
# source fails *open* -- rename `makeErr` and it finds nothing and cheerfully
# reports full coverage over an empty universe. These are the counts measured
# at master 82c0d7bc; the real header must never drop below them without
# somebody noticing, so falling short is an error rather than a small number.
MIN_KINDS = 8
MIN_MESSAGES = 18

_KIND = re.compile(r'env\.kind\s*==\s*"([a-z]+)"')
# Three ways a refusal string reaches the wire, plus the one case where it is
# built into a local named `message` before being handed to rejectAndRelease
# (the payload-completeness gate). Over-inclusion is safe: a string that is not
# really a refusal only makes the report stricter, never weaker.
_MESSAGE_SOURCES = (
    re.compile(r'makeErr\("([^"]*)"'),
    re.compile(r'rejectAndRelease\("([^"]*)"'),
    re.compile(r'runtime_error\("([^"]*)"'),
    re.compile(r'const std::string message = "([^"]*)"'),
)


def _repo_root() -> pathlib.Path:
    """Returns the repository root, walking up from this file."""
    return pathlib.Path(__file__).resolve().parents[2]


@dataclass(frozen=True)
class Surface:
    """The protocol surface a scenario could possibly exercise."""

    kinds: frozenset[str]
    exact_messages: frozenset[str]
    message_prefixes: frozenset[str]


def extract_surface(header_text: str) -> Surface:
    """Extracts every envelope kind and refusal string from `remote.hpp` text.

    A refusal ending in `": "` is recorded as a *prefix*: the runtime value
    carries a suffix (a type id, an exception's `what()`) that the source
    cannot know, so it can only ever be matched by prefix.
    """
    kinds = frozenset(_KIND.findall(header_text))
    exact: set[str] = set()
    prefixes: set[str] = set()
    for pattern in _MESSAGE_SOURCES:
        for found in pattern.findall(header_text):
            if not found:
                continue
            if found.endswith(": "):
                prefixes.add(found)
            else:
                exact.add(found)
    return Surface(
        kinds=kinds,
        exact_messages=frozenset(exact),
        message_prefixes=frozenset(prefixes),
    )


def floor_violations(surface: Surface) -> list[str]:
    """Reports every way @p surface is too small to be believable.

    An empty list means the extraction is plausible. A non-empty one means the
    extractor has stopped matching what it is supposed to match -- treat it as
    a broken tool, never as a small protocol surface.
    """
    problems: list[str] = []
    if len(surface.kinds) < MIN_KINDS:
        problems.append(
            f"found {len(surface.kinds)} envelope kinds, expected at least {MIN_KINDS} "
            "-- the extractor is probably broken, not the header"
        )
    total = len(surface.exact_messages) + len(surface.message_prefixes)
    if total < MIN_MESSAGES:
        problems.append(
            f"found {total} refusal messages, expected at least {MIN_MESSAGES} "
            "-- the extractor is probably broken, not the header"
        )
    return problems
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cd scripts/scenario && python3 test_morph_scenario.py -v 2>&1 | tail -5
```

Expected: `OK` — every existing test still passes plus the five new ones.

- [ ] **Step 5: Commit**

```bash
git add scripts/scenario/scenario_coverage.py scripts/scenario/test_morph_scenario.py
git commit -s -m "scenario: extract the protocol surface from remote.hpp

The universe a scenario could cover, enumerated rather than estimated:
every envelope kind dispatchMessage handles and every refusal string it can
put on the wire.

Carries a plausibility floor because a regex extractor over someone else's
source fails open -- rename makeErr and it finds nothing and reports full
coverage over an empty universe. Falling below the floor is an error about
the tool, not a small number about the header."
```

---

### Task 2: Extract what the corpus exercises

**Files:**
- Modify: `scripts/scenario/scenario_coverage.py`
- Test: `scripts/scenario/test_morph_scenario.py`

**Interfaces:**
- Consumes: `Surface` from Task 1.
- Produces:
  - `Exercised` — frozen dataclass with `kinds: frozenset[str]`, `messages: frozenset[str]`.
  - `exercised_by(scenarios: list) -> Exercised` — takes `morph_scenario.Scenario` objects.
  - `load_scenarios(directory: pathlib.Path) -> list` — returns `morph_scenario.Scenario` objects, sorted by path.
  - `covers(surface: Surface, exercised: Exercised) -> tuple[frozenset[str], frozenset[str]]` — returns `(uncovered_kinds, uncovered_messages)`, where an uncovered message is rendered as its exact string or its prefix.

- [ ] **Step 1: Write the failing test**

Append to `scripts/scenario/test_morph_scenario.py`:

```python
_FIXTURE_SCENARIO = '''
model PasteModel
client alice
do CreatePaste content="x"
expect ok field id ~ .
do GetPaste id=nope
expect err message == "model not found"
send instances typeId=PasteModel
expect err message == "unknown envelope kind: instances"
deregister
expect ok
'''


class ExercisedExtractionTest(unittest.TestCase):
    def _parsed(self):
        return [parse_scenario(_FIXTURE_SCENARIO, "fixture.scenario")]

    def test_client_counts_as_hello_and_register(self) -> None:
        used = exercised_by(self._parsed())
        self.assertIn("hello", used.kinds)
        self.assertIn("register", used.kinds)

    def test_do_counts_as_execute_and_send_counts_as_its_own_kind(self) -> None:
        used = exercised_by(self._parsed())
        self.assertIn("execute", used.kinds)
        self.assertIn("instances", used.kinds)
        self.assertIn("deregister", used.kinds)

    def test_collects_only_messages_asserted_on_an_err_reply(self) -> None:
        used = exercised_by(self._parsed())
        self.assertEqual(used.messages, frozenset({"model not found", "unknown envelope kind: instances"}))

    def test_a_prefix_message_is_covered_by_an_assertion_carrying_a_suffix(self) -> None:
        surface = Surface(
            kinds=frozenset({"execute"}),
            exact_messages=frozenset({"model not found"}),
            message_prefixes=frozenset({"unknown envelope kind: "}),
        )
        uncovered_kinds, uncovered_messages = covers(surface, exercised_by(self._parsed()))
        self.assertEqual(uncovered_kinds, frozenset())
        self.assertEqual(uncovered_messages, frozenset())

    def test_reports_what_the_corpus_never_asserts(self) -> None:
        surface = Surface(
            kinds=frozenset({"execute", "assign"}),
            exact_messages=frozenset({"server busy"}),
            message_prefixes=frozenset(),
        )
        uncovered_kinds, uncovered_messages = covers(surface, exercised_by(self._parsed()))
        self.assertEqual(uncovered_kinds, frozenset({"assign"}))
        self.assertEqual(uncovered_messages, frozenset({"server busy"}))
```

Extend the `scenario_coverage` import block with `Exercised`, `covers`, `exercised_by`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cd scripts/scenario && python3 test_morph_scenario.py -v 2>&1 | tail -5
```

Expected: FAIL — `ImportError: cannot import name 'exercised_by' from 'scenario_coverage'`.

- [ ] **Step 3: Write minimal implementation**

Append to `scripts/scenario/scenario_coverage.py` (and add `import morph_scenario` plus `from morph_scenario import Scenario` to the imports at the top):

```python
@dataclass(frozen=True)
class Exercised:
    """What the scenario corpus actually puts on the wire and asserts on."""

    kinds: frozenset[str]
    messages: frozenset[str]


def load_scenarios(directory: pathlib.Path) -> list[Scenario]:
    """Parses every `*.scenario` in @p directory through the runner's own parser.

    Deliberately re-uses `morph_scenario.parse_scenario` rather than reading the
    files here: a second parser could drift from the real one, and then this
    report would be measuring a format nothing runs.
    """
    out: list[Scenario] = []
    for path in sorted(directory.glob("*.scenario")):
        out.append(morph_scenario.parse_scenario(path.read_text(encoding="utf-8"), str(path)))
    return out


def exercised_by(scenarios: list[Scenario]) -> Exercised:
    """Collects the envelope kinds sent and the refusal messages asserted."""
    kinds: set[str] = set()
    messages: set[str] = set()
    for scenario in scenarios:
        for step in scenario.steps:
            if step.verb == "client":
                # `client` opens the socket, says hello, then registers.
                kinds.update(("hello", "register"))
            elif step.verb == "do":
                kinds.add("execute")
            elif step.verb == "deregister":
                kinds.add("deregister")
            elif step.verb == "send" and step.args:
                kinds.add(step.args[0])

            # A message only counts when it is asserted on an `err` reply: an
            # `expect ok` step says nothing about a refusal.
            expects_err = any(
                a.kind == "compare" and a.path == "@kind" and a.expected_token == "err"
                for a in step.assertions
            )
            if not expects_err:
                continue
            for assertion in step.assertions:
                if assertion.kind != "compare" or assertion.path != "@message" or assertion.op != "==":
                    continue
                messages.add(str(morph_scenario.parse_value(assertion.expected_token, {})))
    return Exercised(kinds=frozenset(kinds), messages=frozenset(messages))


def covers(surface: Surface, exercised: Exercised) -> tuple[frozenset[str], frozenset[str]]:
    """Diffs @p surface against @p exercised.

    @return `(uncovered kinds, uncovered messages)`. An uncovered message is
            named by its exact string, or by its prefix for the prefix-shaped
            ones.
    """
    uncovered_kinds = surface.kinds - exercised.kinds
    uncovered_messages: set[str] = set()
    for message in surface.exact_messages:
        if message not in exercised.messages:
            uncovered_messages.add(message)
    for prefix in surface.message_prefixes:
        if not any(asserted.startswith(prefix) for asserted in exercised.messages):
            uncovered_messages.add(prefix)
    return frozenset(uncovered_kinds), frozenset(uncovered_messages)
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cd scripts/scenario && python3 test_morph_scenario.py -v 2>&1 | tail -5
```

Expected: `OK`.

- [ ] **Step 5: Commit**

```bash
git add scripts/scenario/scenario_coverage.py scripts/scenario/test_morph_scenario.py
git commit -s -m "scenario: measure what the corpus puts on the wire

Reads the scenario files through morph_scenario.parse_scenario rather than
re-parsing them, so the thing measuring the corpus cannot drift from the
thing running it.

A refusal counts as exercised only when asserted on an err reply: an
expect-ok step says nothing about a refusal. Prefix-shaped messages match by
prefix, since the runtime value carries a suffix the source cannot know."
```

---

### Task 3: The allowlist, checked in both directions

**Files:**
- Modify: `scripts/scenario/scenario_coverage.py`
- Create: `scripts/scenario/coverage_allowlist.json`
- Test: `scripts/scenario/test_morph_scenario.py`

**Interfaces:**
- Consumes: `Surface`, `Exercised`, `covers` from Tasks 1–2.
- Produces:
  - `Allowlist` — frozen dataclass with `kinds: dict[str, str]`, `messages: dict[str, str]` (item → reason).
  - `load_allowlist(path: pathlib.Path) -> Allowlist`
  - `allowlist_problems(allowlist: Allowlist, surface: Surface, exercised: Exercised) -> list[str]`

- [ ] **Step 1: Write the failing test**

Append to `scripts/scenario/test_morph_scenario.py`:

```python
class AllowlistTest(unittest.TestCase):
    _SURFACE = Surface(
        kinds=frozenset({"execute", "assign"}),
        exact_messages=frozenset({"server busy", "model not found"}),
        message_prefixes=frozenset(),
    )
    _EXERCISED = Exercised(kinds=frozenset({"execute"}), messages=frozenset({"model not found"}))

    def test_an_entry_for_a_genuinely_uncovered_item_is_accepted(self) -> None:
        allowlist = Allowlist(kinds={"assign": "no rung uses shared instances yet"},
                              messages={"server busy": "no server sets a LimitPolicy"})
        self.assertEqual(allowlist_problems(allowlist, self._SURFACE, self._EXERCISED), [])

    def test_an_entry_for_something_now_covered_is_a_problem(self) -> None:
        # The exemption has outlived its reason: the corpus covers this now.
        allowlist = Allowlist(kinds={"assign": "r", "execute": "stale"},
                              messages={"server busy": "r"})
        problems = allowlist_problems(allowlist, self._SURFACE, self._EXERCISED)
        self.assertTrue(any("execute" in p and "already covered" in p for p in problems))

    def test_an_entry_naming_something_that_no_longer_exists_is_a_problem(self) -> None:
        # A rename in remote.hpp left this behind.
        allowlist = Allowlist(kinds={"assign": "r", "telepathy": "gone"},
                              messages={"server busy": "r"})
        problems = allowlist_problems(allowlist, self._SURFACE, self._EXERCISED)
        self.assertTrue(any("telepathy" in p and "no longer" in p for p in problems))

    def test_an_empty_reason_is_a_problem(self) -> None:
        allowlist = Allowlist(kinds={"assign": ""}, messages={"server busy": "r"})
        problems = allowlist_problems(allowlist, self._SURFACE, self._EXERCISED)
        self.assertTrue(any("reason" in p for p in problems))

    def test_the_shipped_allowlist_parses(self) -> None:
        allowlist = load_allowlist(_repo_root() / "scripts" / "scenario" / "coverage_allowlist.json")
        for reason in list(allowlist.kinds.values()) + list(allowlist.messages.values()):
            self.assertTrue(reason.strip(), "every entry needs a written reason")
```

Extend the `scenario_coverage` import block with `Allowlist`, `allowlist_problems`, `load_allowlist`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cd scripts/scenario && python3 test_morph_scenario.py -v 2>&1 | tail -5
```

Expected: FAIL — `ImportError: cannot import name 'Allowlist' from 'scenario_coverage'`.

- [ ] **Step 3: Write minimal implementation**

Add `import json` to the imports, then append to `scripts/scenario/scenario_coverage.py`:

```python
@dataclass(frozen=True)
class Allowlist:
    """Items deliberately left uncovered, each with a written reason.

    Modelled on `morph::ladder::testkit::QmlSurfaceAudit::allowUnbound`, which
    established the shape here: a required reason, checked in both directions,
    so the list can only shrink deliberately.
    """

    kinds: dict[str, str]
    messages: dict[str, str]


def load_allowlist(path: pathlib.Path) -> Allowlist:
    """Reads the allowlist JSON. A missing file means an empty allowlist."""
    if not path.exists():
        return Allowlist(kinds={}, messages={})
    raw = json.loads(path.read_text(encoding="utf-8"))
    return Allowlist(kinds=dict(raw.get("kinds", {})), messages=dict(raw.get("messages", {})))


def allowlist_problems(allowlist: Allowlist, surface: Surface, exercised: Exercised) -> list[str]:
    """Audits the allowlist itself, in both directions.

    An exemption is only honest while both halves hold: the item still exists,
    and it is still uncovered. An entry for something the corpus now covers has
    outlived its reason; one naming something the header no longer has is a
    rename that left a stale exemption behind. Either way the list must not be
    allowed to quietly accumulate.
    """
    problems: list[str] = []
    uncovered_kinds, uncovered_messages = covers(surface, exercised)

    for name, reason in sorted(allowlist.kinds.items()):
        if not reason.strip():
            problems.append(f"allowlisted kind '{name}' has no written reason")
        if name not in surface.kinds:
            problems.append(f"allowlisted kind '{name}' no longer exists in remote.hpp")
        elif name not in uncovered_kinds:
            problems.append(f"allowlisted kind '{name}' is already covered -- drop the exemption")

    known = surface.exact_messages | surface.message_prefixes
    for name, reason in sorted(allowlist.messages.items()):
        if not reason.strip():
            problems.append(f"allowlisted message '{name}' has no written reason")
        if name not in known:
            problems.append(f"allowlisted message '{name}' no longer exists in remote.hpp")
        elif name not in uncovered_messages:
            problems.append(f"allowlisted message '{name}' is already covered -- drop the exemption")
    return problems
```

- [ ] **Step 4: Create the shipped allowlist**

Create `scripts/scenario/coverage_allowlist.json`. Every entry below is a decision recorded in the design spec; do not add entries without one.

```json
{
  "kinds": {},
  "messages": {
    "connection closed": "Emitted from the shared-instance attach path (remote.hpp, noteScopeAttachLocked) only when a connection's scope closes concurrently with an attach in flight on it. That is a genuine race; a scenario cannot drive it deterministically, and forcing it with a sleep would reintroduce the wall-clock flakiness this runner exists to avoid (morph#147). See the design spec, 'The measurable universe'."
  }
}
```

- [ ] **Step 5: Run test to verify it passes**

```bash
cd scripts/scenario && python3 test_morph_scenario.py -v 2>&1 | tail -5
```

Expected: `OK`.

- [ ] **Step 6: Commit**

```bash
git add scripts/scenario/scenario_coverage.py scripts/scenario/coverage_allowlist.json scripts/scenario/test_morph_scenario.py
git commit -s -m "scenario: allowlist uncovered surface, checked both ways

An exemption is honest only while the item still exists and is still
uncovered. An entry for something the corpus now covers has outlived its
reason; one naming something remote.hpp no longer has is a rename that left
a stale exemption behind. Both are failures, so the list can only shrink
deliberately -- the same discipline QmlSurfaceAudit::allowUnbound already
uses in the ladder testkit.

One entry ships: 'connection closed' is a concurrent-scope-close race that
no scenario can drive without a sleep."
```

---

### Task 4: The CLI, the report, and the gate

**Files:**
- Modify: `scripts/scenario/scenario_coverage.py`
- Modify: `scripts/scenario/README.md`
- Test: `scripts/scenario/test_morph_scenario.py`

**Interfaces:**
- Consumes: everything from Tasks 1–3.
- Produces: `main(argv: list[str] | None = None) -> int` — `0` fully covered or exempt, `1` gap found, `2` tool broken (floor violated, allowlist stale, unreadable input).

- [ ] **Step 1: Write the failing test**

Append to `scripts/scenario/test_morph_scenario.py`:

```python
class CoverageCliTest(unittest.TestCase):
    def test_exits_two_when_the_extractor_finds_an_implausible_surface(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            header = pathlib.Path(tmp) / "remote.hpp"
            header.write_text('if (env.kind == "register") {}\n', encoding="utf-8")
            scenarios = pathlib.Path(tmp) / "scenarios"
            scenarios.mkdir()
            code = coverage_main(["--header", str(header), "--scenarios", str(scenarios)])
        self.assertEqual(code, 2)

    def test_exits_one_when_something_is_uncovered_and_unexempt(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            header = pathlib.Path(tmp) / "remote.hpp"
            header.write_text(_FIXTURE_HEADER, encoding="utf-8")
            scenarios = pathlib.Path(tmp) / "scenarios"
            scenarios.mkdir()
            allow = pathlib.Path(tmp) / "allow.json"
            allow.write_text('{"kinds": {}, "messages": {}}', encoding="utf-8")
            code = coverage_main([
                "--header", str(header), "--scenarios", str(scenarios),
                "--allowlist", str(allow), "--no-floor",
            ])
        self.assertEqual(code, 1)

    def test_exits_zero_when_everything_uncovered_is_exempt(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            header = pathlib.Path(tmp) / "remote.hpp"
            header.write_text('if (env.kind == "register") {}\nreply(makeErr("nope", id));\n', encoding="utf-8")
            scenarios = pathlib.Path(tmp) / "scenarios"
            scenarios.mkdir()
            allow = pathlib.Path(tmp) / "allow.json"
            allow.write_text(
                '{"kinds": {"register": "fixture"}, "messages": {"nope": "fixture"}}', encoding="utf-8"
            )
            code = coverage_main([
                "--header", str(header), "--scenarios", str(scenarios),
                "--allowlist", str(allow), "--no-floor",
            ])
        self.assertEqual(code, 0)
```

Add `import pathlib` and `import tempfile` to the test file's imports if absent, and extend the `scenario_coverage` import block with `main as coverage_main`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cd scripts/scenario && python3 test_morph_scenario.py -v 2>&1 | tail -5
```

Expected: FAIL — `ImportError: cannot import name 'main' from 'scenario_coverage'`.

- [ ] **Step 3: Write minimal implementation**

Add `import argparse` and `import sys` to the imports, then append to `scripts/scenario/scenario_coverage.py`:

```python
def _render(surface: Surface, exercised: Exercised, allowlist: Allowlist) -> str:
    """Builds the human-readable report."""
    uncovered_kinds, uncovered_messages = covers(surface, exercised)
    gap_kinds = sorted(uncovered_kinds - set(allowlist.kinds))
    gap_messages = sorted(uncovered_messages - set(allowlist.messages))
    total_messages = len(surface.exact_messages) + len(surface.message_prefixes)

    lines = [
        "morph scenario coverage",
        "",
        f"  envelope kinds : {len(surface.kinds) - len(uncovered_kinds)}/{len(surface.kinds)} covered",
        f"  refusals       : {total_messages - len(uncovered_messages)}/{total_messages} covered",
        "",
    ]
    if gap_kinds:
        lines.append("UNCOVERED envelope kinds (no scenario sends these):")
        lines.extend(f"    {name}" for name in gap_kinds)
        lines.append("")
    if gap_messages:
        lines.append("UNCOVERED refusals (no scenario asserts these):")
        lines.extend(f"    {name}" for name in gap_messages)
        lines.append("")
    if allowlist.kinds or allowlist.messages:
        lines.append("Exempt, with reasons:")
        for name, reason in sorted(allowlist.kinds.items()):
            lines.append(f"    kind {name}: {reason}")
        for name, reason in sorted(allowlist.messages.items()):
            lines.append(f"    message {name!r}: {reason}")
        lines.append("")
    if not gap_kinds and not gap_messages:
        lines.append("Every kind and refusal is either covered or exempt.")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    """Runs the report. Returns a process exit code."""
    root = _repo_root()
    parser = argparse.ArgumentParser(
        prog="scenario_coverage.py",
        description="Measure scenario coverage of morph's wire-protocol surface.",
    )
    parser.add_argument("--header", default=str(root / "include" / "morph" / "core" / "remote.hpp"))
    parser.add_argument("--scenarios", default=str(pathlib.Path(__file__).with_name("scenarios")))
    parser.add_argument("--allowlist", default=str(pathlib.Path(__file__).with_name("coverage_allowlist.json")))
    parser.add_argument(
        "--no-floor",
        action="store_true",
        help="skip the plausibility check (for this tool's own fixtures only)",
    )
    args = parser.parse_args(argv)

    try:
        header_text = pathlib.Path(args.header).read_text(encoding="utf-8")
    except OSError as exc:
        print(f"scenario_coverage: cannot read {args.header}: {exc}", file=sys.stderr)
        return 2

    surface = extract_surface(header_text)
    if not args.no_floor:
        violations = floor_violations(surface)
        if violations:
            for problem in violations:
                print(f"scenario_coverage: {problem}", file=sys.stderr)
            return 2

    try:
        scenarios = load_scenarios(pathlib.Path(args.scenarios))
    except (OSError, morph_scenario.ScenarioError) as exc:
        print(f"scenario_coverage: cannot read scenarios: {exc}", file=sys.stderr)
        return 2

    exercised = exercised_by(scenarios)
    allowlist = load_allowlist(pathlib.Path(args.allowlist))

    problems = allowlist_problems(allowlist, surface, exercised)
    if problems:
        for problem in problems:
            print(f"scenario_coverage: {problem}", file=sys.stderr)
        return 2

    print(_render(surface, exercised, allowlist))
    uncovered_kinds, uncovered_messages = covers(surface, exercised)
    if (uncovered_kinds - set(allowlist.kinds)) or (uncovered_messages - set(allowlist.messages)):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 4: Run the tests and the real report**

```bash
cd scripts/scenario && python3 test_morph_scenario.py 2>&1 | tail -3
```

Expected: `OK`.

```bash
python3 scripts/scenario/scenario_coverage.py; echo "exit=$?"
```

Expected: `exit=1`, and a report naming `assign`, `attach`, `instances`, `schemas` as uncovered kinds plus the refusal gaps. **This is the correct outcome for this task** — the gaps are real and Plans 2–4 close them. Confirm the printed counts match the spec's baseline (4/8 kinds; 8 kinds and 20 refusals extracted); if they do not, the extractor is wrong and must be fixed before proceeding.

- [ ] **Step 5: Make it executable and document it**

```bash
chmod +x scripts/scenario/scenario_coverage.py
```

Add to `scripts/scenario/README.md`, immediately after the "Proving a scenario's assertions are real" section:

```markdown
## Measuring what the corpus covers

A corpus grows by whoever last added a file, and "have we covered everything?"
has no answer unless the universe is countable. `scenario_coverage.py` makes it
countable: it enumerates every envelope kind `RemoteServer::dispatchMessage`
handles and every refusal string it can put on the wire, straight out of
`include/morph/core/remote.hpp`, then diffs that against what the scenario
files actually send and assert.

```bash
python3 scripts/scenario/scenario_coverage.py
```

Exit `0` if every kind and refusal is covered or exempt, `1` if something is
uncovered, `2` if the tool itself is broken — a header it cannot read, an
extraction too small to be believable, or a stale allowlist.

Anything deliberately left uncovered goes in `coverage_allowlist.json` with a
**written reason**, and is checked in both directions: an entry for something
now covered, or for something `remote.hpp` no longer has, fails the run. The
list can only shrink deliberately.

The report is itself a control, so it is built not to pass while measuring
nothing: it refuses to run if it extracts an implausibly small surface (rename
`makeErr` and it fails loudly rather than reporting full coverage over an empty
universe), and `test_morph_scenario.py` drives it against fixtures whose right
answers are known, including one where the correct exit code is non-zero.
```

- [ ] **Step 6: Commit**

```bash
git add scripts/scenario/scenario_coverage.py scripts/scenario/README.md scripts/scenario/test_morph_scenario.py
git commit -s -m "scenario: report and gate protocol coverage

Exit 0 covered-or-exempt, 1 gap, 2 the tool is broken. The third code
matters as much as the others: an unreadable header, an implausible
extraction or a stale allowlist must not read as 'nothing to report'.

Running it today exits 1 and names the real gaps -- 4 of 8 envelope kinds
and 4 of 20 refusals are exercised. Closing them is the next plan."
```

---

## Self-Review

**Spec coverage.** Piece 1 of the design spec is fully covered: extraction (Task 1), corpus measurement (Task 2), allowlist with both-directions checking (Task 3), report/gate/docs (Task 4). Both mandatory guards from the spec's "Guarding against a report that measures nothing" are implemented and tested — the plausibility floor in Task 1, the known-wrong-answer fixtures in Task 4. The spec's `--json` output is **deliberately dropped** as YAGNI: nothing consumes it yet, and CI consumes the exit code. If a consumer appears, add it then.

**Not in this plan, by design.** Pieces 2–4 (protocol sweep, fixture knobs, rung breadth) and the CI wiring each get their own plan. Plan 2's task list is "close what the report names", so writing it before the report has run once would be guessing its output — the concrete gap list from Task 4 Step 4 is its input.

**Placeholder scan.** No TBD/TODO. Every code step carries complete code; every command carries expected output.

**Type consistency.** `Surface(kinds, exact_messages, message_prefixes)`, `Exercised(kinds, messages)`, and `Allowlist(kinds, messages)` are used with those exact field names in Tasks 2–4. `covers()` returns `(uncovered_kinds, uncovered_messages)` in that order at every call site. `_repo_root()` is defined in Task 1 and imported by the tests in Tasks 1 and 3. `main` is exported as `coverage_main` in the test import block to avoid colliding with the runner's own `main`.
