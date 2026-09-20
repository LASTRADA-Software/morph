#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Usage: python3 scripts/check_bidi_controls.py [PATH...]

Fails if any first-party file carries a *raw* Unicode bidirectional control
character. The escaped spelling (`\\u200E`, `\\u{200E}`, `&#x200E;` -- whatever
the file's own language provides) is always accepted; only the literal
codepoint is rejected.

With no PATH, scans every tracked file in the repository (`git ls-files`),
minus this gate's own fixtures. With PATHs, walks them on disk -- which is how
scripts/test_check_bidi_controls.sh reaches the fixtures, and how a caller
scopes the scan to one subtree.

## Why this gate exists (morph#628, morph#642, morph#610)

A bidi control is invisible. It renders as nothing, occupies no column, and
survives copy-paste, so a diff containing one looks exactly like a diff not
containing one. Every consequence of that is silent:

  - In a *string literal* it is a silently wrong test. morph#610's subject was
    nine assertion lines in src/qt/forms/tests/tst_i18n.qml whose expected
    values contained raw U+200E / U+200F / U+061C; a wrong one there asserts
    something nobody can read.
  - In a *comment* it is source that stops saying what it means. morph#628
    reproduced exactly this: typing the six characters `\\u061C` through a
    JSON-payload tool decoded the escape before the file was written, so a
    single invisible U+061C landed in include/morph/render/locale_format.hpp
    and in tests/test_render_locale_format.cpp.
  - Between the two lies the "trojan source" shape: an override or isolate
    that reorders how a line *renders* relative to how it *compiles*, so the
    reviewer and the compiler read different programs.

Comments are therefore in scope, not excluded. morph#610's original sketch said
"outside comments"; morph#628's own reproduction was inside comments, and the
trojan-source shape lives in comments by construction.

Review is not a control for this class. The PR that closed morph#610 -- the
ticket about this exact hazard -- was itself reviewed, and the review could not
have seen a raw control had one survived, because the diff renders them as
nothing. That is the argument for a lint rather than a habit.

## What is rejected

Twelve codepoints, in three classes, which is morph#610's list:

  U+061C                ARABIC LETTER MARK
  U+200E, U+200F        LEFT-TO-RIGHT / RIGHT-TO-LEFT MARK
  U+202A .. U+202E      the embeddings, overrides and their terminator
  U+2066 .. U+2069      the isolates and their terminator

The marks are the ones this repository has actually hit; the embeddings,
overrides and isolates are the sharper hazard, because those are the ones that
reorder rendered text rather than merely nudging a neighbouring run.

## The two vacuity traps, and how each is closed

A scan that reports "0 raw controls" is satisfied by a broken pattern exactly
as well as by a clean tree, and the tree *is* clean today (0 occurrences across
1242 tracked files), so this gate ships already green. It would therefore be
worth nothing unless both ways of going blind are closed:

  1. **The detector drifts away from the declared set.** Before scanning
     anything, the checker feeds itself a probe holding each declared
     codepoint, one at a time, and requires every one to come back flagged. A
     `detect()` rewritten as a range with the wrong bound, or narrowed to the
     two marks this repository happens to have hit, fails here on every run,
     with no fixture involved and on a clean tree.
  2. **The file set stops matching.** Scanning zero files is an error, not a
     pass -- that is what a wrong path argument, a `git ls-files` that failed,
     or a prune rule that swallowed the tree all look like from outside.

What that probe cannot see is the declared set itself *shrinking*: delete an
entry from BIDI_CONTROLS and the probe stops asking about it, because both
sides derive from the same table. Closing that is the job of
scripts/test_check_bidi_controls.sh, which holds its own independent list of
the twelve, drives the checker over the fixtures in tests/lint/bidi_controls/,
and requires every codepoint on *its* list to be named in the output. It also
pins that a directory with no scannable files is rejected rather than called
clean, that each diagnostic names its own file, that no diagnostic contains a
raw control, and that both EXEMPT hygiene rules fire.

## The diagnostic must be readable

A gate against invisible characters must not print them. Every offending line
is echoed with each control replaced by a visible `<U+200E>` marker, so the
failure message says where the character is instead of reproducing the exact
problem it is reporting. The self-test pins this: the diagnostic itself must
contain no raw control.

## Exemptions

EXEMPT below maps a repository-relative path to a written reason. The reason is
the payload: an exemption without one is the hand-maintained record this gate
replaces.

Two rules keep it from becoming stale, copied from
scripts/check_workflow_option_coverage.py's EXEMPT and
scripts/check_workflow_job_banners.py's UNBANNERED: an entry for a file that
does not exist is an error, and an entry for a file that has no raw controls is
an error. An exemption has to be necessary to be allowed to stay.

The grain is the file, not the line. That is deliberate: a file that genuinely
needs a raw control -- one proving a parser accepts a literal, say -- is a file
whose whole subject is that character, and an exemption for it should be
argued once in prose rather than sprinkled per line where it would drift back
into being invisible.

Reads text only; compiles and runs nothing.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

# Files allowed to carry raw bidi controls, and why.
#
# Empty, and that is the measured state of the tree rather than an aspiration:
# `git ls-files` lists 1242 files at c55ea5b7 and none of them contains any of
# the twelve codepoints below. Adding an entry needs an argument a reviewer can
# check; removing one is always allowed.
EXEMPT: dict[str, str] = {}

# The twelve rejected codepoints, written out one by one. The detector below is
# built from this list, and so is the probe that proves the detector works --
# but the probe tests each codepoint *individually*, so a mistake that narrows
# the set is caught even though both derive from here.
BIDI_CONTROLS: dict[int, str] = {
    0x061C: "ARABIC LETTER MARK",
    0x200E: "LEFT-TO-RIGHT MARK",
    0x200F: "RIGHT-TO-LEFT MARK",
    0x202A: "LEFT-TO-RIGHT EMBEDDING",
    0x202B: "RIGHT-TO-LEFT EMBEDDING",
    0x202C: "POP DIRECTIONAL FORMATTING",
    0x202D: "LEFT-TO-RIGHT OVERRIDE",
    0x202E: "RIGHT-TO-LEFT OVERRIDE",
    0x2066: "LEFT-TO-RIGHT ISOLATE",
    0x2067: "RIGHT-TO-LEFT ISOLATE",
    0x2068: "FIRST STRONG ISOLATE",
    0x2069: "POP DIRECTIONAL ISOLATE",
}

# This gate's own fixtures hold raw controls on purpose, so the repository-wide
# run must not see them -- exactly as scripts/check_nolint_directives.sh prunes
# tests/lint for its own inert directives. Naming a path inside the fixture
# tree still scans it, which is how the self-test reaches them.
FIXTURE_ROOT = "tests/lint/bidi_controls"

# Directories a disk walk never descends into. Only needed in PATH mode; the
# default mode asks git, which already knows.
PRUNE_DIRS = {".git", "_deps", "__pycache__", "node_modules", ".venv"}


def detect(text: str) -> set[int]:
    """Every rejected codepoint present in `text`. The one detector both the
    scan and the self-probe go through, so the probe cannot pass against a
    different rule from the one the tree is judged by."""
    return {ord(ch) for ch in text if ord(ch) in BIDI_CONTROLS}


def self_probe() -> list[str]:
    """Assert the detector still sees each of the twelve codepoints, one at a
    time. This is what stops a clean tree and a broken detector from producing
    the same green."""
    missed = [
        f"U+{cp:04X} {name} is no longer detected"
        for cp, name in sorted(BIDI_CONTROLS.items())
        if detect(chr(cp)) != {cp}
    ]
    return missed


def render(line: str) -> str:
    """The line with every control replaced by a visible marker, so a message
    about invisible characters is itself readable."""
    return "".join(
        f"<U+{ord(ch):04X}>" if ord(ch) in BIDI_CONTROLS else ch for ch in line
    )


def tracked_files(root: Path) -> list[Path]:
    """Every tracked file, which is the repository's own answer to 'what is
    first-party here' -- derived rather than a list this file would have to
    keep in step."""
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    return [root / name for name in out.split("\0") if name]


def walked_files(path: Path) -> list[Path]:
    if path.is_file():
        return [path]
    found: list[Path] = []
    for child in sorted(path.rglob("*")):
        if any(part in PRUNE_DIRS for part in child.parts):
            continue
        if child.is_file() and not child.is_symlink():
            found.append(child)
    return found


def scan(path: Path) -> tuple[list[tuple[int, int, int, str]], bool]:
    """Occurrences as (line, column, codepoint, line text), plus whether the
    file was read as text at all.

    A file holding a NUL byte is git's own definition of binary, and a byte
    sequence that happens to spell a control there is not source anybody reads.
    Decoding with `errors="replace"` is safe for the rest: a replacement
    character is never one of the twelve."""
    data = path.read_bytes()
    if b"\0" in data:
        return [], False
    text = data.decode("utf-8", errors="replace")
    hits: list[tuple[int, int, int, str]] = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        for col, ch in enumerate(line, start=1):
            if ord(ch) in BIDI_CONTROLS:
                hits.append((lineno, col, ord(ch), line))
    return hits, True


def main(argv: list[str]) -> int:
    root = Path.cwd()
    errors: list[str] = []

    missed = self_probe()
    if missed:
        for miss in missed:
            print(f"::error::{miss}", file=sys.stderr)
        print(
            "\nThe raw-bidi-control detector no longer recognises every codepoint "
            "it claims to. A scan run with it would report a clean tree whether "
            "or not the tree is clean, which is the one outcome this gate must "
            "never produce. Fix BIDI_CONTROLS / detect() before trusting any "
            "result from this script.",
            file=sys.stderr,
        )
        return 1

    if argv:
        candidates: list[Path] = []
        for arg in argv:
            path = Path(arg)
            if not path.exists():
                print(f"error: no such path: {arg}", file=sys.stderr)
                return 1
            candidates.extend(walked_files(path))
    else:
        try:
            candidates = tracked_files(root)
        except subprocess.CalledProcessError as exc:
            print(
                f"error: `git ls-files` failed in {root}: {exc.stderr.strip()}\n"
                f"       Without it this gate does not know which files are "
                f"first-party, and must not report success.",
                file=sys.stderr,
            )
            return 1
        candidates = [
            p
            for p in candidates
            if not p.relative_to(root).as_posix().startswith(f"{FIXTURE_ROOT}/")
        ]

    scanned = 0
    binary = 0
    offenders: dict[str, list[tuple[int, int, int, str]]] = {}
    found_any: set[str] = set()

    for path in candidates:
        try:
            rel = path.relative_to(root).as_posix()
        except ValueError:
            rel = path.as_posix()
        hits, is_text = scan(path)
        if not is_text:
            binary += 1
            continue
        scanned += 1
        if hits:
            found_any.add(rel)
            if rel not in EXEMPT:
                offenders[rel] = hits

    if scanned == 0:
        target = " ".join(argv) if argv else str(root)
        print(
            f"error: no text files found under: {target}\n"
            f"       This gate has nothing to check and must not report success. "
            f"Either the scan's file set or the path it was given has stopped "
            f"matching the tree.",
            file=sys.stderr,
        )
        return 1

    for rel, hits in sorted(offenders.items()):
        for lineno, col, cp, line in hits:
            errors.append(
                f"{rel}:{lineno}:{col}: raw U+{cp:04X} {BIDI_CONTROLS[cp]}\n"
                f"    {lineno} | {render(line)}"
            )

    for rel, reason in sorted(EXEMPT.items()):
        if not (root / rel).exists():
            errors.append(
                f"{rel} is exempted by this checker (reason: {reason}) but no such "
                f"file exists. Delete the exemption: it is a stale record of a "
                f"file that has moved or gone."
            )
        elif rel not in found_any:
            errors.append(
                f"{rel} is exempted by this checker (reason: {reason}) but contains "
                f"no raw bidi control. Delete the exemption -- one that is not "
                f"doing anything is cover for the next one that is wrong."
            )

    if errors:
        for err in errors:
            print(f"::error::{err}", file=sys.stderr)
        print(
            f"""
{len(errors)} problem(s). Scanned {scanned} text file(s), skipped {binary} binary.

A raw bidi control is invisible: it renders as nothing and a diff containing
one looks exactly like a diff that does not. In a string literal that is a
silently wrong assertion (morph#610); in a comment it is source that stops
saying what it means (morph#628); an override or isolate can make a line render
as a different program from the one that compiles.

Write the escape your file's language provides instead -- `\\u200E` in C++, QML
and JSON, `\\u{{200E}}` where braces are accepted, `&#x200E;` in markup. The
escaped form is what every one of the thirteen surviving occurrences in
src/qt/forms/tests/tst_i18n.qml uses.

If a raw control is genuinely necessary, add the file to EXEMPT in
scripts/check_bidi_controls.py with a reason a reviewer can check.""",
            file=sys.stderr,
        )
        return 1

    exemptions = f", {len(EXEMPT)} exemption(s)" if EXEMPT else ""
    print(
        f"bidi-control lint OK: {scanned} text file(s) scanned "
        f"({binary} binary skipped), {len(BIDI_CONTROLS)} codepoint(s) searched "
        f"for, 0 raw occurrence(s){exemptions}."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
