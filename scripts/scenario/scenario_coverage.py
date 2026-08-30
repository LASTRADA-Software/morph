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
