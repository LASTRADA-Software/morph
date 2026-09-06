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


# Directories under `examples/` that ship a `ladder_<name>_server`, and so can
# be driven by a scenario. `lims` and `crm` are deliberately absent: neither
# ships a server (crm ships no client either -- see its README's "What is not
# built"), so no scenario can reach them. lims's missing server is filed
# separately.
#
# `bank` is here and is *not* a ladder rung: it is absent from
# examples/rungs.txt, never calls morph_add_rung(), and takes no rung number --
# it is unnumbered prior art that predates the ladder, which examples/LADDER.md
# records beside its numbered table. What this tuple has always actually meant
# is "the set with a src/server/main.cpp" -- the property that decides whether a
# scenario can reach the thing -- and bank has had a model surface worth driving
# for far longer than it has lacked a server. Being
# a rung and having scenarios were already independent in the other direction:
# `lims` and `crm` are rungs with no scenarios.
SERVER_RUNGS = ("pastebin", "bookmarks", "polls", "kanban", "ledger", "bank")

# Plausibility floor for the whole action universe, measured at 72. Same
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


def shipped_action_sources(examples: pathlib.Path) -> dict[str, str]:
    """Reads every C++ source under `<examples>/<rung>/` for each server rung.

    @param examples Directory holding each rung's example tree -- the real
           run passes the repository's `examples/` directory; a test passes a
           throwaway directory shaped the same way, since this is the one
           input to `main` that used to be hardwired to the repo layout.
    @return Rung name to concatenated source text, ready for `extract_actions`.
    @throws SurfaceError if a rung's directory is missing -- a renamed or moved
            rung must break this loudly rather than silently drop its actions.
    """
    sources: dict[str, str] = {}
    for rung in SERVER_RUNGS:
        directory = examples / rung
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
    """What one scenario file dispatches, and whether it is a workflow.

    `succeeded` is the subset of `actions` the file dispatches with an
    `expect ok` on the reply -- an action it drives to completion, rather than
    merely calls to assert a refusal. The action allowlist is audited against
    it; see `action_allowlist_problems`.
    """

    rung: str
    path: str
    actions: frozenset[str]
    chained_steps: int
    is_workflow: bool
    succeeded: frozenset[str] = frozenset()


def scenario_facts(scenario: Scenario) -> ScenarioFacts:
    """Summarises one parsed scenario for the workflow axis.

    A step counts as *chained* only when it is a `do` step whose arguments
    reference a name an earlier step captured. That is what distinguishes a
    journey from a list: threading state through the rung's *domain actions*
    means each dispatch depends on what the last one returned. Chaining on any
    other verb is deliberately not counted -- a `session principal=$who
    token=$token` step reuses one login's credentials rather than carrying a
    result forward, so a file that installs the same token on three clients
    and then fires four unrelated `do` calls would otherwise qualify while
    threading nothing at all between its actions.

    @param scenario A scenario already parsed by `morph_scenario.parse_scenario`.
    @return Its rung, dispatched actions, chained-step count and workflow verdict.
    """
    path = pathlib.PurePath(scenario.path)
    rung = path.parent.name if path.parent.name != "scenarios" else ""

    actions: set[str] = set()
    succeeded: set[str] = set()
    captured: set[str] = set()
    chained = 0
    for step in scenario.steps:
        is_do = step.verb == "do" and bool(step.args)
        if is_do:
            actions.add(step.args[0])
            if any(
                assertion.kind == "compare"
                and assertion.path == "@kind"
                and assertion.expected_token == "ok"
                for assertion in step.assertions
            ):
                succeeded.add(step.args[0])
        # Read references before recording this step's own captures: a step
        # cannot chain off a value it produces itself.
        if is_do and any(
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
        succeeded=frozenset(succeeded),
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
    # 16, not the design's original 15: morph#361 added `CreateLedger` and
    # `bootstrap-a-book-over-the-wire.scenario` with it, the one ledger file
    # that names no seeded id. Raised deliberately -- a floor left at 15 would
    # let that file be deleted without anything noticing, which is precisely
    # what it exists to keep from happening again.
    "ledger": 16,
    "kanban": 20,
    # Bank registers 41 actions across eleven models -- nearly twice kanban's
    # surface, and the largest in the tree -- so its floor is the highest here.
    # Scaled the same way as the rest: not a quota, but the number of genuinely
    # distinct journeys the domain supports, which for a retail bank is roughly
    # one per money-moving shape (deposit/withdraw, transfer, bill, scheduled
    # payment, standing order, loan, card) plus the cross-cutting ones
    # (authorization between two customers, the anonymous session, the stateful
    # account cache).
    "bank": 22,
}


@dataclass(frozen=True)
class RungTally:
    """One rung's workflow-axis arithmetic, computed once and shared.

    `dispatched`, `exempt` and `undispatched` partition `registered`: every
    registered action lands in exactly one bucket, so
    `len(dispatched) + len(exempt) + len(undispatched) == len(registered)`
    always holds. An action that is both allowlisted and dispatched lands in
    `dispatched`, not `exempt` -- `exempt` is scoped to the undispatched
    remainder, because a scenario may legitimately call an allowlisted action
    (e.g. to assert its refusal) without driving it to completion, and such a
    call already counts as dispatched.
    """

    rung: str
    registered: frozenset[str]
    dispatched: frozenset[str]
    exempt: frozenset[str]
    undispatched: frozenset[str]
    workflows: int
    floor: int


def rung_tallies(
    actions: dict[str, frozenset[str]],
    facts: list[ScenarioFacts],
    allowlist: Allowlist,
    floors: dict[str, int] | None = None,
) -> dict[str, RungTally]:
    """Computes the workflow-axis arithmetic once per rung, for both callers.

    `workflow_problems` (the gate) and `_render` (the report) used to walk
    `facts` and recompute this independently; that let the printed report and
    the exit code disagree if the two implementations ever drifted. Both now
    call this and share one answer.

    @param actions   Rung name to its registered action names.
    @param facts     One entry per parsed scenario.
    @param allowlist Exemptions; `actions` is keyed `"<rung>/<Action>"`.
    @param floors    Per-rung workflow minimums; defaults to `WORKFLOW_FLOORS`.
    @return Rung name to its `RungTally`, ordered the same way @p actions was.
    """
    effective = WORKFLOW_FLOORS if floors is None else floors
    result: dict[str, RungTally] = {}
    for rung, registered in sorted(actions.items()):
        dispatched_by_scenario: set[str] = set()
        workflows = 0
        for fact in facts:
            if fact.rung != rung:
                continue
            dispatched_by_scenario |= fact.actions
            if fact.is_workflow:
                workflows += 1

        dispatched = registered & dispatched_by_scenario
        # Exempt is scoped to the undispatched remainder, not to every
        # registered action: a scenario may still legitimately call an
        # allowlisted action (e.g. to assert its refusal) without being able
        # to drive it to completion, and such a call already counts as
        # dispatched. Scoping this way keeps the three buckets disjoint, so
        # dispatched + exempt + undispatched always equals registered.
        undispatched_all = registered - dispatched
        exempt = {name for name in undispatched_all if f"{rung}/{name}" in allowlist.actions}
        undispatched = undispatched_all - exempt

        result[rung] = RungTally(
            rung=rung,
            registered=registered,
            dispatched=frozenset(dispatched),
            exempt=frozenset(exempt),
            undispatched=frozenset(undispatched),
            workflows=workflows,
            floor=effective.get(rung, 0),
        )
    return result


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
    problems: list[str] = []
    for rung, tally in rung_tallies(actions, facts, allowlist, floors).items():
        for name in sorted(tally.undispatched):
            problems.append(f"{rung}: action '{name}' is dispatched by no scenario")
        if tally.workflows < tally.floor:
            problems.append(
                f"{rung}: {tally.workflows} workflows, floor is {tally.floor} "
                "(a file counts only if it chains actions through captured state)"
            )
    return problems


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
    actions: dict[str, str]


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
        return Allowlist(kinds={}, messages={}, actions={})
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
        actions=_allowlist_section(raw.get("actions", {}), "actions", path),
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


def action_allowlist_problems(
    allowlist: Allowlist,
    actions: dict[str, frozenset[str]],
    facts: list[ScenarioFacts],
) -> list[str]:
    """Audits the `actions` section of the allowlist, in both directions.

    The companion to `allowlist_problems`, which audits the protocol axis; this
    one needs the workflow axis's universe and corpus, which the protocol audit
    never sees. Same three checks: a written reason, an entry that still names
    something real, and an entry that has not outlived its justification.

    What retires an action exemption is deliberately *success*, not dispatch.
    An entry here claims the action cannot be driven to completion by any
    WebSocket client -- the server refuses every principal but its own, or no
    action on the wire hands back the id it needs. A scenario that calls such
    an action precisely to assert that refusal is the exemption's evidence, not
    its refutation, so being dispatched leaves the entry standing. An action
    that some scenario dispatches with an `expect ok` *has* been driven to
    completion, which is exactly what the reason said was impossible, and the
    entry must go.

    @param allowlist Exemptions; `actions` is keyed `"<rung>/<Action>"`.
    @param actions   Rung name to its registered action names.
    @param facts     One entry per parsed scenario.
    @return One message per problem; empty when every entry is honest.
    """
    succeeded: dict[str, set[str]] = {rung: set() for rung in actions}
    for fact in facts:
        if fact.rung in succeeded:
            succeeded[fact.rung] |= set(fact.succeeded)

    problems: list[str] = []
    for key, reason in sorted(allowlist.actions.items()):
        if not reason.strip():
            problems.append(f"allowlisted action '{key}' has no written reason")
        rung, separator, name = key.partition("/")
        if not separator or not rung or not name:
            problems.append(
                f"allowlisted action '{key}' is not keyed '<rung>/<Action>'"
            )
            continue
        if rung not in actions:
            problems.append(
                f"allowlisted action '{key}' names rung '{rung}', which registers no actions "
                "-- the rung was renamed or does not ship a server"
            )
            continue
        if name not in actions[rung]:
            problems.append(
                f"allowlisted action '{key}' is no longer registered by '{rung}' "
                "-- a rename left the exemption behind"
            )
        elif name in succeeded[rung]:
            problems.append(
                f"allowlisted action '{key}' is driven to success by a scenario "
                "-- drop the exemption"
            )
    return problems


def _render(
    surface: Surface,
    exercised: Exercised,
    allowlist: Allowlist,
    actions: dict[str, frozenset[str]],
    facts: list[ScenarioFacts],
    floors: dict[str, int] | None = None,
) -> str:
    """Builds the human-readable report, protocol axis then workflow axis.

    @param surface   The protocol surface extracted from the headers.
    @param exercised What the scenario corpus puts on the wire and asserts on.
    @param allowlist Exemptions for both axes.
    @param actions   Rung name to its registered action names (the workflow
                      axis's universe).
    @param facts     One entry per parsed scenario (the workflow axis's
                      corpus).
    @param floors    Per-rung workflow minimums; defaults to `WORKFLOW_FLOORS`.
                      Mirrors the same hidden, testing-only override
                      `workflow_problems` accepts, so a fixture's printed
                      report and its exit code always agree.
    """
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

    lines.append("")
    lines.append("workflow coverage")
    lines.append("")
    total_registered = 0
    total_dispatched = 0
    gap_lines: list[str] = []
    granting: set[str] = set()
    for rung, tally in rung_tallies(actions, facts, allowlist, floors).items():
        total_registered += len(tally.registered)
        total_dispatched += len(tally.dispatched)
        granting |= {f"{rung}/{name}" for name in tally.exempt}

        lines.append(
            f"  {rung:<10} actions {len(tally.dispatched)}/{len(tally.registered)} dispatched "
            f"({len(tally.exempt)} exempt), workflows {tally.workflows}/{tally.floor}"
        )
        if tally.undispatched:
            gap_lines.append(
                f"    {rung}: {', '.join(sorted(tally.undispatched))} dispatched by no scenario"
            )
        if tally.workflows < tally.floor:
            gap_lines.append(f"    {rung}: {tally.workflows} workflows, floor is {tally.floor}")

    lines.append("")
    lines.append(f"  total actions dispatched: {total_dispatched}/{total_registered}")
    # Split the allowlist's action entries by whether they actually granted an
    # exemption in the tallies above. `exempt` is scoped to the undispatched
    # remainder, so an entry whose action *is* dispatched (a scenario calling
    # it only to assert its refusal) grants nothing and is counted in no rung's
    # "(n exempt)". Printing every entry under one "exempt" heading claimed
    # exemptions the arithmetic never granted; the two groups are labelled
    # apart so the list and the counts cannot be read as disagreeing.
    if allowlist.actions:
        granted = [(k, r) for k, r in sorted(allowlist.actions.items()) if k in granting]
        recorded = [(k, r) for k, r in sorted(allowlist.actions.items()) if k not in granting]
        if granted:
            lines.append("")
            lines.append("Actions exempt, with reasons:")
            for name, reason in granted:
                lines.append(f"    {name}: {reason}")
        if recorded:
            lines.append("")
            lines.append(
                "Actions recorded as undrivable, granting no exemption "
                "(a scenario already dispatches them, to assert their refusal):"
            )
            for name, reason in recorded:
                lines.append(f"    {name}: {reason}")
    if gap_lines:
        lines.append("")
        lines.append("WORKFLOW GAPS:")
        lines.extend(gap_lines)
    else:
        lines.append("")
        lines.append("Every registered action is dispatched and every rung meets its floor.")
    return "\n".join(lines)


def _parse_floors(raw: str) -> dict[str, int]:
    """Parses the `--floors` JSON object into a per-rung workflow-floor mapping.

    Hidden, testing-only knob: it lets this tool's own fixtures satisfy the
    workflow floor without authoring dozens of throwaway workflow scenarios
    per rung. A real run never passes `--floors`, so `workflow_problems` falls
    back to the shipped `WORKFLOW_FLOORS`.

    @param raw The `--floors` argument text, expected to be a JSON object of
           rung name to integer floor.
    @return The parsed mapping.
    @throws ValueError if @p raw is not JSON, or not shaped as expected.
    """
    parsed = json.loads(raw)
    if not isinstance(parsed, dict) or not all(
        isinstance(key, str) and isinstance(value, int) for key, value in parsed.items()
    ):
        raise ValueError("must be a JSON object of rung name -> integer floor")
    return parsed


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
        "--examples",
        default=str(root / "examples"),
        help="directory holding each rung's example tree, the source of registered actions",
    )
    parser.add_argument(
        "--no-floor",
        action="store_true",
        help="skip the plausibility check (for this tool's own fixtures only)",
    )
    parser.add_argument(
        "--floors",
        default=None,
        help=(
            "hidden, testing-only: JSON object of rung name -> integer, overriding the "
            "workflow floors for this tool's own fixtures; real runs never pass this "
            "and get the shipped WORKFLOW_FLOORS"
        ),
    )
    args = parser.parse_args(argv)

    floors: dict[str, int] | None = None
    if args.floors is not None:
        try:
            floors = _parse_floors(args.floors)
        except ValueError as exc:
            print(f"scenario_coverage: --floors {exc}", file=sys.stderr)
            return 2

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

    try:
        actions = extract_actions(shipped_action_sources(pathlib.Path(args.examples)))
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
    stale_exemptions = action_allowlist_problems(allowlist, actions, facts)
    if stale_exemptions:
        for problem in stale_exemptions:
            print(f"scenario_coverage: {problem}", file=sys.stderr)
        return 2

    flow_problems = workflow_problems(actions, facts, allowlist, floors=floors)

    print(_render(surface, exercised, allowlist, actions, facts, floors=floors))
    uncovered_kinds, uncovered_messages = covers(surface, exercised)
    protocol_gap = (uncovered_kinds - set(allowlist.kinds)) or (uncovered_messages - set(allowlist.messages))
    if protocol_gap or flow_problems:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
