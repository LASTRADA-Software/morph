#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Usage: python3 scripts/check_workflow_job_references.py [REPO_ROOT]

Fails if a workflow *comment* names a job that does not exist.

Why this gate exists (morph#637, morph#645): a comment in ci.yml said "the
`check-workflow-option-coverage` job" when the job is `option-coverage` and
`check_workflow_option_coverage.py` is the script it runs. YAML comments are
inert, so nothing in this repository could notice; a reader who searched the
workflows for that name found nothing and had to guess which of the two the
sentence meant.

scripts/check_workflow_job_banners.py (morph#638) pairs a `# ── … ──` banner to
the job it introduces. A job id inside a comment *body* is not a banner, so it
is outside that gate by construction -- rule A only inspects the line after a
banner, rule B only inspects whether a job has one.

## The rule, and why it is this narrow

A reference is a token immediately followed by the word "job", "jobs" or
"job's", where the token is lowercase and hyphenated -- the shape every job id
in this repository has. It may be wrapped in backticks and may carry a
possessive; neither changes what it names.

The hyphen is the whole discriminator, and dropping it is not an option.
Measured on the tree this landed against, `.github/workflows/*.yml` carries
**207** occurrences of "<token> job" in comments. **193** of them are ordinary
English -- "this job", "every job", "a sanitizer job", "the next job's build".
Restricting to hyphenated tokens leaves **13**, of which 11 resolve, one is a
live defect (see below) and one is a compound adjective in EXEMPT. A rule
without the hyphen would be 193 false positives and would be turned off within
the day.

The cost of that narrowness, stated plainly rather than discovered later: a
stale reference to a job whose id is a *single* word -- `valgrind`, `windows`,
`mutation`, `clang-tidy` -- is invisible to this gate, because "the valgrind
job" and "a sanitizer job" are the same shape and only a dictionary
distinguishes them. This catches the hyphenated majority (30 of the 36 job ids
in the tree today) and says so, rather than claiming a coverage it does not
have.

The second blind spot is the line: the scan is line-based, so a reference whose
id and whose "job" fall either side of a wrap is not seen. Both blind spots are
under-reporting rather than false alarms, which is the direction a prose gate
has to err in if it is to be left switched on.

## It was not vacuous on arrival

The commit that added this gate had one real reference to repair:
`ci.yml`'s `CLANG_VERSION` block said "the clang-tidy-diff job", and the job is
`clang-tidy` -- `clang-tidy-diff.py` is the tool that job runs. That is
morph#637's defect exactly, one directory over, live on master eleven weeks
after morph#637 was fixed.

## What keeps EXEMPT honest

The same two rules as scripts/check_workflow_option_coverage.py's EXEMPT and
check_workflow_job_banners.py's UNBANNERED:

  * an entry whose token no longer appears before "job" in that file is an
    error -- the exemption outlived its sentence;
  * an entry whose token *does* resolve to a job id is an error -- it is being
    excused from a rule it satisfies.

An exemption has to be necessary to be allowed to stay.

## Anti-vacuity

Finding zero references across the whole tree is a failure, not a pass. This
gate reads prose, and prose is rewritten: reword every "the ladder-tests job"
into "the ladder-tests leg" and the scan matches nothing while still exiting 0
-- a control that reports success having measured nothing, which is this
repository's named failure mode. The floor is deliberately on the *candidate*
count, not the failure count: a gate that finds references and clears them all
is working; a gate that finds none has stopped looking.

Reads text only; compiles and runs nothing. No third-party imports, so it runs
on a bare runner in seconds.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# Tokens excused from the rule, by workflow file, with the reason. The reason
# is the payload: an entry without one records only that somebody was annoyed.
EXEMPT: dict[str, dict[str, str]] = {
    "drift-guard.yml": {
        "dependency-free": "not a reference -- `its own fast, dependency-free "
        "job` is a compound adjective describing the job, not naming one. The "
        "only English construction in the tree that takes this shape.",
    },
}

# A comment line, at any indentation. `#` inside a `run:` block is a shell
# comment and is not scanned: this gate is about the prose that describes the
# workflow, and a shell comment inside a step is code.
COMMENT_RE = re.compile(r"^\s*#")

# `foo-bar job`, `` `foo-bar` job ``, `foo-bar's job`, `foo-bar jobs`.
# The trailing word is matched case-insensitively ("Job" opens a sentence);
# the token is not, because job ids are lowercase and `Foo-Bar` is prose.
REFERENCE_RE = re.compile(
    r"(?<![\w`-])"                 # not mid-token, and not the tail of a longer id
    r"`?([a-z0-9]+(?:-[a-z0-9]+)+)`?"
    r"(?:'s)?\s+"
    r"(?:job|jobs|job's)(?![\w-])",
    re.IGNORECASE,
)

# A top-level job key. `jobs:` itself is at column 0, so two spaces is a job.
JOB_KEY_RE = re.compile(r"^  ([A-Za-z0-9_-]+):\s*(#.*)?$")


def job_ids(lines: list[str]) -> set[str]:
    """Every top-level key under this workflow's `jobs:` mapping."""
    out: set[str] = set()
    in_jobs = False
    for line in lines:
        if re.match(r"^jobs:\s*(#.*)?$", line):
            in_jobs = True
            continue
        if in_jobs and line and not line[0].isspace():
            in_jobs = False
        if not in_jobs:
            continue
        m = JOB_KEY_RE.match(line)
        if m:
            out.add(m.group(1))
    return out


def references(lines: list[str]) -> list[tuple[int, str]]:
    """(1-based line, token) for every hyphenated job reference in a comment."""
    out: list[tuple[int, str]] = []
    for i, line in enumerate(lines, 1):
        if not COMMENT_RE.match(line):
            continue
        for m in REFERENCE_RE.finditer(line):
            out.append((i, m.group(1)))
    return out


def main(argv: list[str]) -> int:
    root = Path(argv[1] if len(argv) > 1 else Path(__file__).resolve().parent.parent)
    workflows = sorted((root / ".github" / "workflows").glob("*.yml"))
    if not workflows:
        print(f"error: no workflows found under {root}/.github/workflows", file=sys.stderr)
        return 1

    failures = 0
    candidates = 0

    def fail(msg: str) -> None:
        nonlocal failures
        print(f"error: {msg}", file=sys.stderr)
        failures += 1

    # A reference resolves against *any* workflow in the tree, not just its own
    # file: three of the references on the tree this landed against point at a
    # job in the other file (ci.yml's comments name drift-guard.yml's jobs and
    # the reverse), and a per-file rule would reject every one of them.
    parsed = {path: path.read_text(encoding="utf-8").split("\n") for path in workflows}
    declared: set[str] = set()
    for lines in parsed.values():
        declared |= job_ids(lines)
    if not declared:
        fail("no job ids found in any workflow -- every reference would fail")
        return 1

    for path, lines in parsed.items():
        rel = path.relative_to(root)
        exempt = EXEMPT.get(path.name, {})
        seen: set[str] = set()

        for line_no, token in references(lines):
            candidates += 1
            seen.add(token)
            if token in declared:
                print(f"ok: {rel}:{line_no}: `{token}` is a job")
                continue
            if token in exempt:
                print(f"ok: {rel}:{line_no}: `{token}` excused -- {exempt[token]}")
                continue
            fail(
                f"{rel}:{line_no}: this comment names a `{token}` job, and no "
                f"workflow declares one.\n"
                f"    {lines[line_no - 1].strip()}\n"
                f"    Job ids in this tree: {', '.join(sorted(declared))}.\n"
                f"    Name the job, not the script or the tool it runs -- that\n"
                f"    substitution is morph#637. If the phrase is English rather\n"
                f"    than a reference, add `{token}` to EXEMPT in "
                f"{Path(__file__).name} with the reason."
            )

        # -- EXEMPT hygiene ---------------------------------------------------
        for token, reason in exempt.items():
            if token not in seen:
                fail(
                    f"{rel}: EXEMPT names `{token}`, which no longer appears before "
                    f'the word "job" in this file. Remove the stale entry -- its '
                    f"reason was: {reason}"
                )
            elif token in declared:
                fail(
                    f"{rel}: EXEMPT names `{token}`, which is now a real job id. "
                    f"Remove the unnecessary entry; the rule it is excused from is "
                    f"one it satisfies."
                )

    # -- Anti-vacuity ---------------------------------------------------------
    if candidates == 0:
        fail(
            'no "<hyphenated-id> job" reference found in any workflow. Either the '
            "comments stopped naming jobs or they were reworded out of the shape "
            "this gate reads -- both leave it checking nothing while still exiting 0."
        )

    if failures:
        print(f"\n{failures} workflow job-reference check(s) failed", file=sys.stderr)
        return 1

    print(f"\nok: all {candidates} job reference(s) in workflow comments resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
