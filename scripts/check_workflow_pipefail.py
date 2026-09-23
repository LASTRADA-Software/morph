#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Usage: python3 scripts/check_workflow_pipefail.py [REPO_ROOT]

Fails if a workflow `run:` block contains a shell pipeline whose exit status
nothing reads.

## The mechanism, which is one line of shell and has cost this repository twice

A `run:` block with no `shell:` key runs under `bash -e {0}` -- `-e` but **not**
`-o pipefail`. A pipeline's status is then its *last* command's, so

    bash scripts/mutation.sh core-forms | tee mutation-core-forms.log

exits with `tee`'s status, which is 0 unless the disk fills. The script's
`exit 1` is discarded and the step is green.

It is not a hypothetical and it is not a one-off:

  * morph#479: `clang-tidy-diff.py … | tee` -- the changed-lines lint could not
    fail on anything it found. Fixed by `set -o pipefail` in that block.
  * morph#730: `scripts/mutation.sh … | tee` -- a campaign that exited 1 having
    produced no report left the step green, and the failure surfaced one step
    later as `check_mutation_regression.py` failing to open a missing file.
    Two weeks of scheduled runs measured nothing and nothing said so.

Between those two, `pipefail` was set in three other steps by hand
(`spec-sync.yml`, `wasm-ladder.yml`, `ci.yml`). The guard was known and applied
inconsistently, which is the state a gate exists for: the next unguarded pipe is
written by someone who has not read any of those three.

Note that `shell: bash` -- the explicit spelling -- *is*
`bash --noprofile --norc -eo pipefail {0}`, so it carries the guard. Only the
*default*, an absent `shell:` key, does not. That asymmetry is most of why this
keeps happening.

## The rule

For every `run:` block in `.github/workflows/*.yml` that runs under a
bash/sh-family shell, every pipeline in it must be covered by one of:

  1. an effective shell that sets pipefail -- `shell: bash`, or an explicit
     `shell: bash -e -o pipefail {0}`-style spelling, from the step, the job's
     `defaults.run.shell` or the workflow's;
  2. a `set … pipefail` command earlier in the same block;
  3. a `# pipefail-ok: <reason>` comment on the pipeline's line or on the line
     above it.

Rule 3 is not decoration. `find A B | head -1` is a pipeline whose left side is
*expected* to fail -- one search root absent, or `head` closing the pipe and
SIGPIPE-ing `find` -- and `ci.yml`'s clang-tidy step deliberately sets pipefail
*after* it for exactly that reason (morph#479's own comment says so). A gate
with no way to say "this one, and here is why" would be turned off within the
week. The reason is mandatory: an exemption without one records only that
somebody was annoyed.

And it has to stay necessary. A marker that excuses no pipeline -- because the
line was rewritten, or because the block gained a `set -o pipefail` above it --
is an error, not a harmless leftover: it sits inert on a line, ready to excuse
whatever pipeline is written there next, silently. That is the same shape as
the defect this gate is about.

## What this gate does not see

Stated rather than left to be discovered:

  * **Scope.** `set -o pipefail` is matched by line order within the block, so a
    `set` inside an `if` branch or a `( … )` subshell is credited to the whole
    block from that line down. Under-strict, never over-strict.
  * **Quoting.** A pipe inside `bash -c "a | b"` is quoted, so it is not seen at
    all. Also under-reporting.
  * **Other shells.** `shell: pwsh` and `shell: python` blocks are skipped
    entirely; `$?`-style semantics there are a different question.
  * **Composite actions.** Only `.github/workflows/*.yml` is read. `run:` inside
    an `action.yml` is out of scope, and there are none in this tree.

## Anti-vacuity

Two floors, both on what was *found* rather than on what failed, because a
parser that silently stops recognising `run:` blocks or pipelines would
otherwise report a clean tree:

  * fewer than `MIN_RUN_BLOCKS` run blocks parsed is an error;
  * fewer than `MIN_PIPELINES` pipelines seen across the tree is an error.

A gate that finds pipelines and clears them all is working. A gate that finds
none has stopped looking.

Reads text only; compiles and runs nothing. No third-party imports, so it runs
on a bare runner in seconds.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# Floors for the anti-vacuity check. The tree this landed against carries 169
# `run:` blocks and 6 pipelines; the floors sit far enough below both that
# ordinary editing does not trip them and a parser that has stopped parsing
# does.
MIN_RUN_BLOCKS = 100
MIN_PIPELINES = 4

# `run: …` as a step key, either as the first key of a list item (`- run: …`)
# or a later one. group(1) is the indentation up to the `run` token.
RUN_RE = re.compile(r"^(\s*)(?:-\s+)?run:(.*)$")

# A YAML block scalar introducer: `|`, `>`, with optional chomping/indent
# indicators.
BLOCK_SCALAR_RE = re.compile(r"^\s*[|>][-+]?\d*\s*(#.*)?$")

SHELL_RE = re.compile(r"^\s*shell:\s*(.+?)\s*(?:#.*)?$")

# `set -o pipefail`, `set -euo pipefail`, `set -eo pipefail`, …
SET_PIPEFAIL_RE = re.compile(r"^\s*set\s+[^#]*\bpipefail\b")

EXEMPT_RE = re.compile(r"#\s*pipefail-ok:\s*(\S.*)$")

# A GitHub expression. `${{ inputs.scope || 'core-forms' }}` carries a `||` that
# is not shell, and could carry a `|`; it is substituted before bash sees it.
EXPRESSION_RE = re.compile(r"\$\{\{.*?\}\}")

SH_SHELLS = ("bash", "sh", "/bin/bash", "/bin/sh", "/usr/bin/bash")


def uses_pipefail_shell(shell: str | None) -> bool:
    """Does this `shell:` spelling start bash with pipefail set?

    `shell: bash` is documented as `bash --noprofile --norc -eo pipefail {0}`.
    An explicit command line counts only if it says so itself.
    """
    if shell is None:
        return False
    stripped = shell.strip().strip("\"'")
    if stripped == "bash":
        return True
    return "pipefail" in stripped


def is_sh_family(shell: str | None) -> bool:
    """True for a shell this gate understands (bash/sh), or the default."""
    if shell is None:
        return True  # the default, `bash -e {0}`
    first = shell.strip().strip("\"'").split()[0] if shell.strip() else ""
    return first in SH_SHELLS


def strip_shell_quoting(line: str) -> str:
    """Blank out quoted spans and the trailing comment.

    A `|` inside quotes is a literal character, not a pipeline; a `|` after `#`
    is prose. Both are removed so the caller can scan what is left.
    """
    out: list[str] = []
    quote: str | None = None
    i = 0
    while i < len(line):
        char = line[i]
        if quote is not None:
            if char == quote:
                quote = None
            i += 1
            continue
        if char == "\\":
            i += 2
            continue
        if char in "'\"":
            quote = char
            i += 1
            continue
        if char == "#":
            break
        out.append(char)
        i += 1
    return "".join(out)


def has_pipeline(line: str) -> bool:
    """Does this shell line contain a pipeline operator?

    `||` is a disjunction, `>|` is a clobbering redirection, and `|&` is a
    pipeline that carries stderr too.
    """
    text = strip_shell_quoting(EXPRESSION_RE.sub("EXPR", line))
    i = 0
    while i < len(text):
        if text[i] != "|":
            i += 1
            continue
        if text[i : i + 2] == "||":
            i += 2
            continue
        if i > 0 and text[i - 1] == ">":
            i += 1
            continue
        return True
    return False


def default_shell(lines: list[str], rel: str, fail) -> str | None:
    """The workflow- or job-level `defaults.run.shell`, if there is one.

    Only the canonical three-line shape is understood. A `defaults:` block this
    cannot read is an error rather than a silent "no default": mis-reading it
    the other way would excuse, or wrongly accuse, every block in the file.
    """
    found: str | None = None
    for i, line in enumerate(lines):
        match = re.match(r"^(\s*)defaults:\s*(#.*)?$", line)
        if not match:
            continue
        indent = match.group(1)
        if (
            i + 2 < len(lines)
            and re.match(rf"^{indent}  run:\s*(#.*)?$", lines[i + 1])
            and SHELL_RE.match(lines[i + 2])
            and lines[i + 2].startswith(indent + "    shell:")
        ):
            found = SHELL_RE.match(lines[i + 2]).group(1)
            continue
        fail(
            f"{rel}:{i + 1}: a `defaults:` block this gate cannot read. It "
            f"understands only `defaults:` / `run:` / `shell: …` on three "
            f"consecutive lines. Either write it that way or teach "
            f"{Path(__file__).name} the shape -- guessing would excuse every "
            f"block in the file."
        )
    return found


def step_shell(lines: list[str], run_index: int, key_indent: int) -> str | None:
    """The `shell:` key of the step that owns the `run:` at `run_index`."""
    item_indent = max(key_indent - 2, 0)
    start = 0
    for i in range(run_index, -1, -1):
        line = lines[i]
        if re.match(rf"^ {{{item_indent}}}-\s", line):
            start = i
            break
    end = len(lines)
    for i in range(run_index + 1, len(lines)):
        line = lines[i]
        if not line.strip():
            continue
        if re.match(rf"^ {{{item_indent}}}-\s", line) or (
            len(line) - len(line.lstrip()) < item_indent
        ):
            end = i
            break
    for i in range(start, end):
        stripped = lines[i].strip()
        if stripped.startswith("#"):
            continue
        indent = len(lines[i]) - len(lines[i].lstrip())
        if indent != key_indent:
            continue
        match = SHELL_RE.match(lines[i])
        if match:
            return match.group(1)
    return None


def run_blocks(lines: list[str]) -> list[tuple[int, int, list[str]]]:
    """(run-key line index, key indent, body lines) for every `run:` in a file.

    The body of an inline `run: cmd` is that one line; the body of a block
    scalar is every following line indented past the key.
    """
    blocks: list[tuple[int, int, list[str]]] = []
    i = 0
    while i < len(lines):
        match = RUN_RE.match(lines[i])
        if not match:
            i += 1
            continue
        key_indent = lines[i].index("run:")
        rest = match.group(2)
        if BLOCK_SCALAR_RE.match(rest) or not rest.strip():
            body: list[tuple[int, str]] = []
            j = i + 1
            while j < len(lines):
                line = lines[j]
                if line.strip() and (len(line) - len(line.lstrip())) <= key_indent:
                    break
                body.append((j, line))
                j += 1
            blocks.append((i, key_indent, body))
            i = j
            continue
        blocks.append((i, key_indent, [(i, rest.strip())]))
        i += 1
    return blocks


def main(argv: list[str]) -> int:
    root = Path(argv[1] if len(argv) > 1 else Path(__file__).resolve().parent.parent)
    workflows = sorted((root / ".github" / "workflows").glob("*.yml"))
    if not workflows:
        print(f"error: no workflows found under {root}/.github/workflows", file=sys.stderr)
        return 1

    failures = 0
    total_blocks = 0
    total_pipelines = 0
    # (file, 0-based line) for every `# pipefail-ok:` marker seen, and for
    # every one that excused a pipeline. An exemption has to be necessary to be
    # allowed to stay: a marker left behind after its pipeline was rewritten
    # sits there inert, ready to excuse whatever pipeline is written on that
    # line next -- silently, which is the shape of defect this whole gate is
    # about.
    markers: list[tuple[str, int, str]] = []
    used: set[tuple[str, int]] = set()

    def fail(msg: str) -> None:
        nonlocal failures
        print(f"error: {msg}", file=sys.stderr)
        failures += 1

    for path in workflows:
        rel = str(path.relative_to(root))
        lines = path.read_text(encoding="utf-8").split("\n")
        file_default = default_shell(lines, rel, fail)

        for run_index, key_indent, body in run_blocks(lines):
            total_blocks += 1
            shell = step_shell(lines, run_index, key_indent) or file_default
            if not is_sh_family(shell):
                continue
            shell_guards = uses_pipefail_shell(shell)

            set_at: int | None = None
            for position, (line_no, line) in enumerate(body):
                if set_at is None and SET_PIPEFAIL_RE.match(strip_shell_quoting(line)):
                    set_at = line_no
                marker = EXEMPT_RE.search(line)
                if marker is not None:
                    markers.append((rel, line_no, marker.group(1).strip()))
                if not has_pipeline(line):
                    continue
                total_pipelines += 1
                if shell_guards:
                    print(f"ok: {rel}:{line_no + 1}: pipeline under a pipefail shell")
                    continue
                if set_at is not None and set_at < line_no:
                    print(f"ok: {rel}:{line_no + 1}: pipeline after `set … pipefail`")
                    continue
                exempt, exempt_line = EXEMPT_RE.search(line), line_no
                if exempt is None:
                    for prev_no, prev_line in reversed(body[:position]):
                        if prev_line.strip():
                            exempt, exempt_line = EXEMPT_RE.search(prev_line), prev_no
                            break
                if exempt is not None:
                    used.add((rel, exempt_line))
                    print(f"ok: {rel}:{line_no + 1}: excused -- {exempt.group(1).strip()}")
                    continue
                fail(
                    f"{rel}:{line_no + 1}: this pipeline's exit status is its "
                    f"last command's, and nothing sets pipefail before it.\n"
                    f"    {line.strip()}\n"
                    f"    A `run:` with no `shell:` key is `bash -e {{0}}`: `-e` "
                    f"but not `-o pipefail`, so a failing left-hand side is\n"
                    f"    discarded (morph#479, morph#730). Add `set -o pipefail` "
                    f"above it, or -- if the left side is *expected* to fail --\n"
                    f"    a `# pipefail-ok: <reason>` comment on that line or the "
                    f"one before it."
                )

    # -- Exemption hygiene ----------------------------------------------------
    for rel, line_no, reason in markers:
        if (rel, line_no) in used:
            continue
        fail(
            f"{rel}:{line_no + 1}: a `# pipefail-ok:` marker that excuses "
            f"nothing -- the line it sits on, and the line below it, carry no "
            f"pipeline.\n"
            f"    Its reason was: {reason}\n"
            f"    Remove it. An exemption nothing needs is an exemption waiting "
            f"to excuse the next pipeline written on that line."
        )

    # -- Anti-vacuity ---------------------------------------------------------
    if total_blocks < MIN_RUN_BLOCKS:
        fail(
            f"only {total_blocks} `run:` block(s) parsed across "
            f"{len(workflows)} workflow file(s), against a floor of "
            f"{MIN_RUN_BLOCKS}. Either the workflows shrank drastically or this "
            f"gate has stopped recognising `run:` blocks -- in which case it is "
            f"reporting a clean tree it never read."
        )
    if total_pipelines < MIN_PIPELINES:
        fail(
            f"only {total_pipelines} pipeline(s) found across "
            f"{len(workflows)} workflow file(s), against a floor of "
            f"{MIN_PIPELINES}. A gate that finds no pipelines is not a tree "
            f"without pipes; it is a scanner that has stopped matching them."
        )

    if failures:
        print(f"\n{failures} workflow pipefail check(s) failed", file=sys.stderr)
        return 1

    print(
        f"\nok: all {total_pipelines} pipeline(s) in {total_blocks} `run:` "
        f"block(s) read their left-hand side's status"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
