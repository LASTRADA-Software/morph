#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measure how much of morph's wire-protocol surface the scenario corpus covers.

The universe is not "every use case" -- that is unbounded and duplicates the
in-process C++ suite. It is the *protocol surface*: every envelope kind
`RemoteServer::dispatchMessage` handles, and every refusal string it can put on
the wire. Both are enumerable from source -- the kinds from
`include/morph/core/remote.hpp`, the refusals from that header's *server half*
plus `include/morph/core/wire.hpp` -- which makes "are we covering everything?"
a number rather than an opinion.

Two scoping rules keep the universe honest, in both directions. Refusals are
read only from the part of `remote.hpp` above `class SimulatedRemoteBackend`:
everything below it is the in-process *client* stub, whose throws are C++
exceptions no WebSocket peer can ever assert, so counting them would set
unreachable targets. And refusals are also read from `wire.hpp`, but only from
`wire::decode`'s body: that is the one `wire.hpp` function whose throw
`dispatchMessage`'s catch turns into a real `err` reply. `encode`'s throw
never reaches a client (nothing is sent when encoding fails), and
`interpretHelloReply`'s throw runs on the *client* inspecting a reply it
already received -- neither is wire-observable, so scanning the rest of the
file would set unreachable targets the same way the client stub would.

A third rule keeps concatenation from inflating the exact-message set: a
literal that starts with a known prefix is that prefix's runtime suffix, not
a standalone message an assertion could ever equal, and is dropped.

See docs/superpowers/specs/2026-08-30-scenario-coverage-design.md.

Standard library only. Needs no server: it reads source and scenario text.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from dataclasses import dataclass

import morph_scenario
from morph_scenario import Scenario

# Floors for the plausibility check. A regex extractor over someone else's
# source fails *open* -- rename `makeErr` and it finds nothing and cheerfully
# reports full coverage over an empty universe. The real sources currently
# yield 8 envelope kinds and 17 refusal messages (14 exact + 3 prefixes:
# `remote.hpp`'s server half above `class SimulatedRemoteBackend`, plus the
# body of `wire::decode` in `wire.hpp`); those sources must never drop below
# these floors without somebody noticing, so falling short is an error rather
# than a small number. MIN_KINDS has zero headroom on purpose -- losing an
# envelope kind is a protocol change a human must notice. MIN_MESSAGES keeps
# the same small headroom (2) below the true total.
#
# The floors only catch the surface *shrinking*. A surface that *grows* is
# caught instead by the two tests that pin the exact extracted sets
# (`test_real_header_carries_the_kinds_the_spec_records` and
# `test_real_headers_carry_exactly_these_refusals`): a new kind or refusal
# fails them, and a human decides whether it needs a scenario or an allowlist
# entry.
MIN_KINDS = 8
MIN_MESSAGES = 15

_KIND = re.compile(r'env\.kind\s*==\s*"([a-z_]+)"')

# Where the server half of `remote.hpp` ends and the in-process client stub
# begins. Refusal extraction stops here; see `server_half`.
SERVER_HALF_MARKER = "class SimulatedRemoteBackend"

# The signature that opens `wire::decode` in `wire.hpp`. Refusal extraction
# from `wire.hpp` is scoped to this function's body alone; see `decode_body`.
DECODE_FUNCTION_MARKER = "inline Envelope decode(std::string_view json)"

# Three ways a refusal string reaches the wire, plus the one case where it is
# built into a local named `message` before being handed to rejectAndRelease
# (the payload-completeness gate). Over-inclusion is safe: a string that is not
# really a refusal only makes the report stricter, never weaker.
#
# Every pattern tolerates whitespace between the open paren/`=` and the string
# literal: clang-format is free to reflow a call across lines, and a tight
# regex that assumes the literal sits directly against the paren silently drops
# the message when that happens. That is this tool's own worst failure mode --
# it fails open, reporting full coverage over a shrunken universe instead of
# erroring. `\s` already matches a newline with no flag set, so no flag is
# needed here (`re.S` would be inert: it only changes what `.` matches, and
# these patterns use none).
_MESSAGE_SOURCES = (
    re.compile(r'makeErr\(\s*"([^"]*)"'),
    re.compile(r'rejectAndRelease\(\s*"([^"]*)"'),
    re.compile(r'runtime_error\(\s*"([^"]*)"'),
    re.compile(r'const std::string message\s*=\s*"([^"]*)"'),
)


class SurfaceError(Exception):
    """Raised when the source no longer looks like what this tool expects."""


def _repo_root() -> pathlib.Path:
    """Returns the repository root, walking up from this file."""
    return pathlib.Path(__file__).resolve().parents[2]


@dataclass(frozen=True)
class Surface:
    """The protocol surface a scenario could possibly exercise."""

    kinds: frozenset[str]
    exact_messages: frozenset[str]
    message_prefixes: frozenset[str]


def server_half(header_text: str) -> str:
    """Returns the part of `remote.hpp` above `class SimulatedRemoteBackend`.

    Everything below that declaration is the in-process client stub. Its
    throws (`register failed: `, `attach failed: `, and the rest) are C++
    exceptions raised on the *client* when it receives an `err` reply -- no
    WebSocket peer can ever assert them, so counting them would inflate the
    denominator with unreachable targets.

    @throws SurfaceError if the marker is absent. A scoping rule that silently
            degrades to "scan everything" is the same failure this tool exists
            to prevent, so a rename must stop the run rather than quietly
            widen the universe.
    """
    index = header_text.find(SERVER_HALF_MARKER)
    if index < 0:
        raise SurfaceError(
            f"cannot find the marker {SERVER_HALF_MARKER!r} that separates remote.hpp's "
            "server half from the in-process client stub -- it was renamed or removed, "
            "and refusal extraction has no safe scope without it"
        )
    return header_text[:index]


def decode_body(wire_text: str) -> str:
    """Returns the body of `wire::decode`, brace-matched from its signature.

    `decode` is the only function in `wire.hpp` whose thrown text becomes an
    `err` reply -- `dispatchMessage` wraps its call in a try/catch and turns
    `exc.what()` into the reply's message. `encode`'s throw fires only when
    nothing has been sent yet, and `interpretHelloReply`'s throw runs on the
    *client* inspecting an already-received reply; scanning either into the
    universe would set unreachable targets. Scoping to `decode`'s body alone
    keeps both out without naming their strings by hand.

    @throws SurfaceError if @p wire_text does not contain
            `DECODE_FUNCTION_MARKER`, or if the braces after it never balance.
            A scoping rule that silently degrades to "scan the whole file" is
            the same failure this tool exists to prevent, so a rename must
            stop the run rather than quietly widen the universe.
    """
    start = wire_text.find(DECODE_FUNCTION_MARKER)
    if start < 0:
        raise SurfaceError(
            f"cannot find the marker {DECODE_FUNCTION_MARKER!r} that opens wire::decode -- "
            "it was renamed or removed, and refusal extraction has no safe scope in wire.hpp "
            "without it"
        )
    open_brace = wire_text.index("{", start)
    depth = 0
    for i in range(open_brace, len(wire_text)):
        char = wire_text[i]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return wire_text[open_brace : i + 1]
    raise SurfaceError(
        f"found {DECODE_FUNCTION_MARKER!r} but its braces never balance -- wire.hpp is "
        "malformed or decode's shape changed in a way this scan cannot follow"
    )


def extract_surface(header_text: str, extra_message_text: str = "") -> Surface:
    """Extracts every envelope kind and refusal string from `remote.hpp` text.

    Envelope kinds come from @p header_text alone -- every `env.kind ==`
    comparison lives in the server. Refusals come from @p header_text *and*
    @p extra_message_text, which carries `wire::decode`'s body in the real
    run: the `envelope decode failed: ` that it throws becomes an `err` reply
    through `dispatchMessage`'s catch, so a scenario can assert it.

    A refusal ending in `": "` is recorded as a *prefix*: the runtime value
    carries a suffix (a type id, an exception's `what()`) that the source
    cannot know, so it can only ever be matched by prefix.

    An exact message that starts with one of the collected prefixes is
    dropped: such a string is not a separate refusal, it is a concatenation
    fragment -- the first piece of a `"prefix" + runtime_value + ...` call
    site that the regex happened to also match as a lone literal (e.g. a
    second, longer string literal earlier in the same concatenation). No
    assertion can ever equal a fragment like that, so keeping it would name a
    permanently unachievable coverage target. This is deliberately general --
    it drops *any* exact message that extends *any* collected prefix, not one
    hard-coded string, so the next such fragment is caught for free.
    """
    kinds = frozenset(_KIND.findall(header_text))
    exact: set[str] = set()
    prefixes: set[str] = set()
    for pattern in _MESSAGE_SOURCES:
        for found in pattern.findall(header_text) + pattern.findall(extra_message_text):
            if not found:
                continue
            if found.endswith(": "):
                prefixes.add(found)
            else:
                exact.add(found)
    exact = {message for message in exact if not any(message.startswith(prefix) for prefix in prefixes)}
    return Surface(
        kinds=kinds,
        exact_messages=frozenset(exact),
        message_prefixes=frozenset(prefixes),
    )


def extract_shipped_surface(header_text: str, wire_text: str) -> Surface:
    """Extracts the surface of the real headers, with the scoping rules applied.

    @p header_text is `remote.hpp` in full; only its server half is scanned
    for refusals. @p wire_text is `wire.hpp` in full; only the body of
    `wire::decode` is scanned for refusals -- see `decode_body`.
    """
    return extract_surface(server_half(header_text), decode_body(wire_text))


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

# Strips a trailing `//...` comment from each line before `_ACTION` runs.
# Unlike the message patterns (where over-inclusion is harmless noise), a
# commented-out registration is a real action name still present in the text
# but no longer live; counting it would silently overstate the universe. A
# per-line truncation is enough here -- no action name legitimately contains
# `//`.
#
# Known limitations: this strips per-line only and is blind to `//` inside
# string literals; block comments `/* ... */` are not handled at all. Neither
# occurs in the real examples/ tree today (verified). Both fail *closed* -- they
# can only drop a registration, never invent one. The name-set pin in
# test_real_tree_pins_the_known_action_names would catch any drop.
_LINE_COMMENT = re.compile(r"//.*")


def extract_actions(sources: dict[str, str]) -> dict[str, frozenset[str]]:
    """Extracts each rung's registered action names from its C++ text.

    @param sources Rung name to the concatenated text of that rung's sources.
    @return Rung name to the set of wire action names it registers.
    """
    result: dict[str, frozenset[str]] = {}
    for rung, text in sources.items():
        live = "\n".join(_LINE_COMMENT.sub("", line) for line in text.splitlines())
        result[rung] = frozenset(_ACTION.findall(live))
    return result


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


@dataclass(frozen=True)
class Exercised:
    """What the scenario corpus actually puts on the wire and asserts on."""

    kinds: frozenset[str]
    messages: frozenset[str]


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


def _client_options(step: morph_scenario.Step) -> dict[str, str]:
    """Parses a `client` step's `key=value` tokens the way the runner does.

    The first argument is the client's name, not an assignment. A token the
    runner would reject is skipped rather than raised on: this is a report,
    and such a scenario fails when it runs.
    """
    options: dict[str, str] = {}
    for token in step.args[1:]:
        try:
            name, value = morph_scenario.split_assignment(token)
        except morph_scenario.ScenarioError:
            continue
        options[name] = value
    return options


def exercised_by(scenarios: list[Scenario]) -> Exercised:
    """Collects the envelope kinds sent and the refusal messages asserted."""
    kinds: set[str] = set()
    messages: set[str] = set()
    for scenario in scenarios:
        for step in scenario.steps:
            if step.verb == "client":
                # `client` opens the socket, then says hello and registers --
                # but only conditionally, and this must mirror
                # `Runner.do_client` exactly. `protocol=none` sends no `hello`
                # (that is how the pre-handshake path is tested), and a client
                # with no model name resolved sends no `register`. Crediting
                # them unconditionally reports kinds the runner never put on
                # the wire, which is the one thing this tool must never do.
                options = _client_options(step)
                if options.get("protocol", str(morph_scenario.PROTOCOL_VERSION)) != "none":
                    kinds.add("hello")
                if options.get("model", scenario.default_model):
                    kinds.add("register")
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
                # An empty captures dict: a $var inside an expected message would raise,
                # not silently mis-measure; no corpus message uses one today.
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


@dataclass(frozen=True)
class Allowlist:
    """Items deliberately left uncovered, each with a written reason.

    Modelled on `morph::ladder::testkit::QmlSurfaceAudit::allowUnbound`, which
    established the shape here: a required reason, checked in both directions,
    so the list can only shrink deliberately.
    """

    kinds: dict[str, str]
    messages: dict[str, str]


class AllowlistError(Exception):
    """Raised when the allowlist file cannot be read as an allowlist."""


def _allowlist_section(raw: object, name: str, path: pathlib.Path) -> dict[str, str]:
    """Validates one `{item: reason}` section of the allowlist document."""
    if not isinstance(raw, dict):
        raise AllowlistError(f"{path}: '{name}' must be an object of item -> reason")
    out: dict[str, str] = {}
    for key, reason in raw.items():
        if not isinstance(key, str) or not isinstance(reason, str):
            raise AllowlistError(
                f"{path}: every '{name}' entry must map a string to a written reason, "
                f"got {key!r} -> {reason!r}"
            )
        out[key] = reason
    return out


def load_allowlist(path: pathlib.Path) -> Allowlist:
    """Reads the allowlist JSON. A missing file means an empty allowlist.

    A malformed or wrongly-typed file is a broken *tool*, not a coverage gap:
    it raises `AllowlistError`, which the CLI turns into exit 2. Letting
    `json.loads` escape would exit 1, and CI reads 1 as "a real gap".
    """
    if not path.exists():
        return Allowlist(kinds={}, messages={})
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise AllowlistError(f"cannot read {path}: {exc}") from exc
    try:
        raw = json.loads(text)
    except ValueError as exc:
        raise AllowlistError(f"{path} is not valid JSON: {exc}") from exc
    if not isinstance(raw, dict):
        raise AllowlistError(f"{path}: the allowlist must be a JSON object")
    return Allowlist(
        kinds=_allowlist_section(raw.get("kinds", {}), "kinds", path),
        messages=_allowlist_section(raw.get("messages", {}), "messages", path),
    )


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
    parser.add_argument(
        "--wire-header",
        default=str(root / "include" / "morph" / "core" / "wire.hpp"),
        help="second refusal source: wire::decode's throws become err replies",
    )
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
        wire_text = pathlib.Path(args.wire_header).read_text(encoding="utf-8")
    except OSError as exc:
        print(f"scenario_coverage: cannot read a header: {exc}", file=sys.stderr)
        return 2

    try:
        surface = extract_shipped_surface(header_text, wire_text)
    except SurfaceError as exc:
        print(f"scenario_coverage: {exc}", file=sys.stderr)
        return 2
    if not args.no_floor:
        violations = floor_violations(surface)
        if violations:
            for problem in violations:
                print(f"scenario_coverage: {problem}", file=sys.stderr)
            return 2

    try:
        scenarios = load_scenarios(pathlib.Path(args.scenarios), recursive=True)
    except (OSError, morph_scenario.ScenarioError) as exc:
        print(f"scenario_coverage: cannot read scenarios: {exc}", file=sys.stderr)
        return 2

    exercised = exercised_by(scenarios)
    try:
        allowlist = load_allowlist(pathlib.Path(args.allowlist))
    except AllowlistError as exc:
        print(f"scenario_coverage: {exc}", file=sys.stderr)
        return 2

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
