#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Usage: python3 scripts/check_workflow_option_coverage.py [REPO_ROOT]

Fails if any `option(MORPH_BUILD_...)` declared in this tree is built by no job
in .github/workflows/.

Why this gate exists (morph#605, morph#604): `MORPH_BUILD_BANK_GUI` defaulted
OFF, was set ON only by a `CMakePresets.json` preset (`linux-everything`) that
no workflow ever names, and was therefore compiled by nothing in CI. bank_gui
duly stopped compiling on master with every gate green (morph#604), and the job
called "Linux / all optional features" went on passing, because its option list
is hand-written and nothing compared it to the options that exist.

Adding the missing flag closes that hole once. It does not close the *class*:
nothing stops the next `option(MORPH_BUILD_...)` from being declared next month
and enabled by nobody, at which point this issue gets filed a third time. So
the check derives both sides -- the declared options from the CMake files, the
enabled ones from the workflows -- rather than asserting a list. It goes red
when a new option appears uncovered, which a hand-maintained list cannot do.

What counts as covered, in order:

  1. The option is declared with default ON. It is then built by every job that
     does not explicitly turn it off, so no workflow has to name it.
  2. Some *native* workflow passes `-DMORPH_BUILD_<NAME>=ON` outside a comment.
  3. Some native workflow passes `-DMORPH_BUILD_<NAME>=${{ matrix.<key> }}` and
     some matrix entry in that same file sets `<key>` to ON. This is how
     MORPH_BUILD_FUZZERS is covered (clang only -- libFuzzer needs it), and
     resolving it rather than special-casing the name means a matrix that
     quietly went all-OFF would fail here.
  4. The option is in EXEMPT below, with a written reason.

Two things this deliberately does not count.

Comment lines are stripped before scanning, so a flag *discussed* in a comment
never counts as coverage. That is not hypothetical: ci.yml's own configure-step
comments mention `-DMORPH_BUILD_TESTS=ON` and `MORPH_BUILD_FORMS_QML=ON` in
prose.

An Emscripten workflow does not count either, and MORPH_BUILD_BANK_GUI is the
reason the distinction had to be drawn: wasm-demo.yml has passed
`-DMORPH_BUILD_BANK_GUI=ON` all along, yet the target morph#604 found broken
was the *native* one. Under EMSCRIPTEN, examples/bank/CMakeLists.txt descends
into `gui_wasm/` and returns before the native `gui/` subdirectory exists, so
the same option name selects a disjoint subtree. Counting that as coverage
would have made this whole gate vacuous for the one option it was written for.
A workflow is treated as Emscripten when its text invokes `emcmake`, `emsdk` or
`qt-cmake` -- derived from the workflow, so a new WASM workflow is classified
by being written rather than by being added to a list here.

Two rules keep EXEMPT from becoming the stale list this gate exists to replace:
an entry for an option that is not declared anywhere is an error, and an entry
for an option that *is* covered is an error. An exemption has to be necessary
to be allowed to stay.

Finding zero declared options is a failure, not a pass -- a gate with nothing
left to check reports green exactly as loudly as one that checked everything.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

# Declared options that no job builds, and should not. The reason is the
# payload: an exemption without one is the hand-maintained record this gate
# replaces. Removing an option from here is always allowed; adding one needs an
# argument that a reviewer can check.
EXEMPT: dict[str, str] = {
    # Not a build feature: setting it makes every compile in the tree run
    # clang-tidy with warnings-as-errors. ci.yml's `clang-tidy` job covers the
    # same ground by running clang-tidy-diff.py over the pull request's diff
    # instead, which is both faster and scoped to changed lines -- turning the
    # CMake option on in some other job would re-tidy the whole tree on every
    # push to no additional effect.
    #
    # MORPH_BUILD_DOCUMENTATION is deliberately *not* here, though it also has
    # a dedicated job: docs.yml's configure step passes
    # -DMORPH_BUILD_DOCUMENTATION=ON literally, so it is covered by rule 2 and
    # an exemption for it would be a false record.
    "MORPH_BUILD_CLANG_TIDY": "covered by ci.yml's `clang-tidy` job, which runs "
    "clang-tidy-diff.py over the PR diff rather than through the CMake option",
}

OPTION_RE = re.compile(
    r"""^[ \t]*option[ \t]*\([ \t]*(MORPH_BUILD_[A-Z0-9_]+)[ \t]+"[^"]*"[ \t]+(ON|OFF)[ \t]*\)""",
    re.MULTILINE,
)
# The value alternative takes `${{ ... }}` first: an Actions expression carries
# spaces inside its braces, so a plain `\S+` would capture the literal `${{`
# and every matrix-valued flag would silently read as unresolvable.
FLAG_RE = re.compile(r"-D(MORPH_BUILD_[A-Z0-9_]+)=(\$\{\{[^}]*\}\}|\S+)")
MATRIX_EXPR_RE = re.compile(r"^\$\{\{\s*matrix\.([A-Za-z0-9_-]+)\s*\}\}$")
EMSCRIPTEN_RE = re.compile(r"\bemcmake\b|\bemsdk\b|qt-cmake")


def tracked_cmake_files(root: Path) -> list[Path]:
    """Every tracked CMakeLists.txt / *.cmake, so an option declared in a
    subdirectory counts too (examples/vetted_hmac/CMakeLists.txt declares two)."""
    out = subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "ls-files",
            "-z",
            "CMakeLists.txt",
            "*/CMakeLists.txt",
            "*.cmake",
        ],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    return [root / name for name in out.split("\0") if name]


def strip_comments(text: str) -> str:
    """Drop whole-line comments. Covers both YAML comments and the shell
    comments inside `run: |` blocks, which is where ci.yml discusses flags it
    does not pass."""
    return "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))


def declared_options(root: Path) -> dict[str, str]:
    """Option name -> declared default ('ON' or 'OFF')."""
    found: dict[str, str] = {}
    for path in tracked_cmake_files(root):
        for name, default in OPTION_RE.findall(path.read_text(encoding="utf-8")):
            found[name] = default
    return found


def matrix_key_can_be_on(body: str, key: str) -> bool:
    """True when some matrix entry in this workflow sets `key` to ON."""
    return re.search(rf"^\s*{re.escape(key)}:\s*['\"]?ON['\"]?\s*$", body, re.MULTILINE) is not None


def enabled_options(root: Path) -> tuple[dict[str, list[str]], dict[str, list[str]]]:
    """(native, wasm-only): option name -> the workflow files that enable it.

    Kept apart rather than merged because an Emscripten configure of the same
    option name can compile an entirely different subtree -- see the module
    docstring. Only the first return value counts as coverage; the second is
    reported so an option that is enabled *only* under WASM says so in the
    failure output instead of looking like it was never enabled at all."""
    native: dict[str, list[str]] = {}
    wasm: dict[str, list[str]] = {}
    for path in sorted((root / ".github" / "workflows").glob("*.yml")):
        raw_text = path.read_text(encoding="utf-8")
        body = strip_comments(raw_text)
        bucket = wasm if EMSCRIPTEN_RE.search(body) else native
        for name, value in FLAG_RE.findall(body):
            if value.rstrip("\\,;:)\"'") == "ON":
                bucket.setdefault(name, []).append(path.name)
                continue
            expr = MATRIX_EXPR_RE.match(value)
            if expr and matrix_key_can_be_on(body, expr.group(1)):
                bucket.setdefault(name, []).append(path.name)
    return native, wasm


def main(argv: list[str]) -> int:
    root = Path(argv[1] if len(argv) > 1 else ".").resolve()

    declared = declared_options(root)
    if not declared:
        print(
            "::error::found no option(MORPH_BUILD_...) declarations at all. Either "
            "the declaration syntax changed or this checker is scanning the wrong "
            "tree; a coverage gate with nothing left to check is not a passing gate.",
            file=sys.stderr,
        )
        return 1

    enabled, wasm_only = enabled_options(root)
    errors: list[str] = []

    for name in sorted(declared):
        if declared[name] == "ON" or name in enabled or name in EXEMPT:
            continue
        note = ""
        if name in wasm_only:
            note = (
                f" It is enabled by {', '.join(sorted(set(wasm_only[name])))}, but "
                f"that is an Emscripten build: the same option can select a "
                f"different subtree there (examples/bank/CMakeLists.txt builds "
                f"gui_wasm/ and returns), so it does not prove the native code "
                f"compiles."
            )
        errors.append(
            f"{name} is declared in this tree, defaults OFF, and no native job in "
            f".github/workflows/ passes -D{name}=ON -- so nothing in CI ever "
            f"compiles the code it guards.{note} "
            f"Cover it: add -D{name}=ON to the configure step of a job whose "
            f"runner already has what it needs. 'Linux / all optional features' "
            f"in ci.yml is the leg meant to carry these, and already installs Qt "
            f"from aqtinstall plus the ODBC/SQLite/yaml-cpp/libzip set. "
            f"A CMakePresets.json preset that sets the option is not coverage -- "
            f"a preset no workflow names is exactly how morph#605 happened."
        )

    for name, reason in sorted(EXEMPT.items()):
        if name not in declared:
            errors.append(
                f"{name} is exempted by this checker but is declared by no CMake "
                f"file in the tree. Delete the exemption: it is a stale record of "
                f"an option that no longer exists."
            )
        elif name in enabled:
            errors.append(
                f"{name} is exempted by this checker (reason: {reason}) but is in "
                f"fact enabled by {', '.join(sorted(set(enabled[name])))}. Delete "
                f"the exemption -- one that is not doing anything is cover for the "
                f"next one that is wrong."
            )
        elif declared.get(name) == "ON":
            errors.append(
                f"{name} is exempted by this checker but is declared with default "
                f"ON, so it is already built everywhere. Delete the exemption."
            )

    if errors:
        for err in errors:
            print(f"::error::{err}", file=sys.stderr)
        print(
            f"\n{len(errors)} problem(s). {len(declared)} MORPH_BUILD_* option(s) "
            f"declared, {len(enabled)} enabled by a workflow, {len(EXEMPT)} exempt.",
            file=sys.stderr,
        )
        return 1

    print(f"{len(declared)} MORPH_BUILD_* option(s) declared, all accounted for:")
    for name in sorted(declared):
        if name in enabled:
            where = "enabled by " + ", ".join(sorted(set(enabled[name])))
        elif declared[name] == "ON":
            where = "declared default ON (built unless a job turns it off)"
        else:
            where = f"exempt -- {EXEMPT[name]}"
        if name in wasm_only:
            where += " [also under WASM: " + ", ".join(sorted(set(wasm_only[name]))) + "]"
        print(f"  {name}: {where}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
