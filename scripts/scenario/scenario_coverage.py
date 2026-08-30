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

import morph_scenario
from morph_scenario import Scenario

# Floors for the plausibility check. A regex extractor over someone else's
# source fails *open* -- rename `makeErr` and it finds nothing and cheerfully
# reports full coverage over an empty universe. The real header currently
# yields 8 kinds and 21 refusal messages (14 exact + 7 prefixes, measured
# after widening `_MESSAGE_SOURCES` to tolerate multi-line call sites); the
# real header must never drop below these floors without somebody noticing,
# so falling short is an error rather than a small number. MIN_KINDS has zero
# headroom on purpose -- losing an envelope kind is a protocol change a human
# must notice. MIN_MESSAGES keeps the same small headroom (2) below the true
# total as before.
MIN_KINDS = 8
MIN_MESSAGES = 19

_KIND = re.compile(r'env\.kind\s*==\s*"([a-z_]+)"')
# Three ways a refusal string reaches the wire, plus the one case where it is
# built into a local named `message` before being handed to rejectAndRelease
# (the payload-completeness gate). Over-inclusion is safe: a string that is not
# really a refusal only makes the report stricter, never weaker.
#
# Every pattern tolerates whitespace (including newlines, via re.S) between
# the open paren/`=` and the string literal: clang-format is free to reflow a
# call across lines, and a tight regex that assumes the literal sits directly
# against the paren silently drops the message when that happens. That is
# this tool's own worst failure mode -- it fails open, reporting full
# coverage over a shrunken universe instead of erroring.
#
# `handleInline does not support execute (reply is asynchronous)` is expected
# to show up as uncovered once these patterns pick it up: `handleInline` is a
# synchronous in-process C++ API, not something reachable over the WebSocket
# wire the scenario runner drives, so the scenario corpus can never exercise
# it. It belongs in the allowlist a later task introduces.
_MESSAGE_SOURCES = (
    re.compile(r'makeErr\(\s*"([^"]*)"', re.S),
    re.compile(r'rejectAndRelease\(\s*"([^"]*)"', re.S),
    re.compile(r'runtime_error\(\s*"([^"]*)"', re.S),
    re.compile(r'const std::string message\s*=\s*"([^"]*)"', re.S),
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
