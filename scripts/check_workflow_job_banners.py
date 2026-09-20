#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Usage: python3 scripts/check_workflow_job_banners.py [REPO_ROOT]

Fails if a `  # ── … ──` section banner in a workflow does not introduce the
job it describes, or if a job that should carry one has lost it.

Why this gate exists (morph#621): seven of ci.yml's seventeen banners sat above
the *previous* job's trailing `sccache stats` / `Save sccache` steps, so the
banner and its multi-paragraph rationale described a job that began 6-31 lines
further down, behind another job's steps. `# ── Valgrind (memcheck) ──` was
followed by three steps belonging to `linux-all-features` and only then by
`valgrind:`. These banners are not decoration: for several jobs the paragraphs
under the banner are the only written record of why the job exists at all
(`# ── Linux: every rung's tests under AddressSanitizer + UBSan ──` carries ~30
lines on why ASan and UBSan but not TSan, citing morph#128), and a reader who
scrolls into that argument attributes it to whichever job's steps surround it.

YAML comments are inert, so nothing else in this repository can notice. The
displacement accumulated because cache steps were appended to each job after
the next job's banner already existed, and were inserted above it rather than
below -- which will happen again the next time a step is appended to the end of
a job.

## The check this deliberately is *not*

Checking only "no banner is displaced" would be vacuous: a tree with every
banner **deleted** scores a perfect zero displacements. That is this
repository's named failure mode -- a control that reports success while
measuring nothing -- so the check is a two-way pairing rather than a one-way
scan:

  A. Every banner must introduce a job. Skipping its own continuation comment
     lines and blank lines, the next line after a banner must be a top-level
     `<job-id>:` key. This is the displacement catcher.

  B. Every job must be introduced by a banner. A job with no banner is an
     error unless it is in UNBANNERED below with a written reason. This is what
     makes rule A non-vacuous: delete a banner to satisfy A and B fails.

Two rules keep UNBANNERED from becoming a stale list, copied from
scripts/check_workflow_option_coverage.py's EXEMPT: an entry for a job that
does not exist is an error, and an entry for a job that *does* have a banner is
an error. An exemption has to be necessary to be allowed to stay.

## Scope

Workflows that contain no banner at all are skipped, and that is derived from
the file rather than listed here: docs.yml, mutation.yml, spec-sync.yml,
suppression-guard.yml and the two wasm workflows are single-job files that have
never used the banner style, and a gate that demanded they adopt it would be
inventing a convention rather than enforcing one. A file that adopts its first
banner opts into both rules for all of its jobs.

Finding zero banners across the whole tree is a failure, not a pass -- a gate
with nothing left to check reports green exactly as loudly as one that checked
everything.

Reads text only; compiles and runs nothing.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

# Jobs allowed to carry no section banner, and why. The reason is the payload.
#
# Keyed by workflow file name, then job id. Listing the jobs *positively* --
# rather than deriving "everything before the file's first banner is a preamble"
# -- is deliberate: under that derivation, deleting a file's *first* banner
# would silently reclassify its job as preamble and the deletion would go
# unreported, which is exactly the hole rule B exists to close.
UNBANNERED: dict[str, dict[str, str]] = {
    "ci.yml": {
        "probe-self-hosted": "not a section of the build matrix but the runner "
        "selection that every Linux leg below consumes through `needs:`; its "
        "rationale is written as a plain comment block because it introduces no "
        "section",
    },
    "drift-guard.yml": {
        "sanitizer-can-fail": "part of the file's un-sectioned preamble: the "
        "first three jobs predate the `# ──` sectioning that starts at the "
        "ladder rung-list gate",
        "dep-cache-selftest": "part of the file's un-sectioned preamble (see "
        "sanitizer-can-fail)",
        "prose-lint": "part of the file's un-sectioned preamble (see "
        "sanitizer-can-fail)",
    },
}

# A banner: a top-level (two-space) comment opening with the box-drawing rule.
BANNER_RE = re.compile(r"^  # ── ")
# A top-level comment line, banner or continuation. Six-space comments belong
# to steps, not to sections, and must not be skipped over.
TOP_COMMENT_RE = re.compile(r"^  #")
# A top-level job key. `jobs:` itself is at column 0, so two spaces is a job.
JOB_KEY_RE = re.compile(r"^  ([A-Za-z0-9_-]+):\s*(#.*)?$")


def job_keys(lines: list[str]) -> list[tuple[int, str]]:
    """(index, job-id) for every top-level key under `jobs:`."""
    out: list[tuple[int, str]] = []
    in_jobs = False
    for i, line in enumerate(lines):
        if re.match(r"^jobs:\s*(#.*)?$", line):
            in_jobs = True
            continue
        if in_jobs and line and not line[0].isspace():
            in_jobs = False
        if not in_jobs:
            continue
        m = JOB_KEY_RE.match(line)
        if m:
            out.append((i, m.group(1)))
    return out


def introduced_job(lines: list[str], banner: int) -> tuple[int, str] | None:
    """The job a banner introduces, or None when the banner is displaced.

    Walks forward past the banner's own continuation comments and blank lines.
    """
    j = banner + 1
    while j < len(lines) and (not lines[j].strip() or TOP_COMMENT_RE.match(lines[j])):
        j += 1
    if j >= len(lines):
        return None
    m = JOB_KEY_RE.match(lines[j])
    return (j, m.group(1)) if m else None


def main(argv: list[str]) -> int:
    root = Path(argv[1] if len(argv) > 1 else Path(__file__).resolve().parent.parent)
    workflows = sorted((root / ".github" / "workflows").glob("*.yml"))
    if not workflows:
        print(f"error: no workflows found under {root}/.github/workflows", file=sys.stderr)
        return 1

    failures = 0
    banners_seen = 0

    def fail(msg: str) -> None:
        nonlocal failures
        print(f"error: {msg}", file=sys.stderr)
        failures += 1

    for path in workflows:
        rel = path.relative_to(root)
        lines = path.read_text(encoding="utf-8").split("\n")
        banners = [i for i, line in enumerate(lines) if BANNER_RE.match(line)]
        if not banners:
            print(f"ok: {rel}: no section banners, not in the banner style")
            continue

        banners_seen += len(banners)
        jobs = job_keys(lines)
        if not jobs:
            fail(f"{rel}: {len(banners)} section banner(s) but no top-level job keys")
            continue

        exempt = UNBANNERED.get(path.name, {})
        bannered: dict[str, int] = {}

        # -- Rule A: every banner introduces a job ---------------------------
        for b in banners:
            target = introduced_job(lines, b)
            if target is None:
                landed = next(
                    (
                        f"{k + 1}: {lines[k].strip()}"
                        for k in range(b + 1, len(lines))
                        if lines[k].strip() and not TOP_COMMENT_RE.match(lines[k])
                    ),
                    "end of file",
                )
                fail(
                    f"{rel}:{b + 1}: this section banner does not introduce a job --\n"
                    f"    {lines[b].strip()}\n"
                    f"    the next non-comment line is {landed}\n"
                    f"    Move the banner (and the paragraphs under it) down to sit\n"
                    f"    directly above the `<job-id>:` key it describes."
                )
                continue
            _, job = target
            bannered[job] = b
            print(f"ok: {rel}:{b + 1}: introduces `{job}`")

        # -- Rule B: every job is introduced by a banner ---------------------
        job_ids = {name for _, name in jobs}
        for idx, name in jobs:
            if name in bannered:
                continue
            if name in exempt:
                print(f"ok: {rel}:{idx + 1}: `{name}` bannerless by exemption")
                continue
            fail(
                f"{rel}:{idx + 1}: job `{name}` is introduced by no section banner.\n"
                f"    Every job in a workflow that uses `  # ── ` banners needs one:\n"
                f"    without this rule, deleting a banner outright would satisfy the\n"
                f"    displacement check instead of failing it. Add the banner, or add\n"
                f"    `{name}` to UNBANNERED in {Path(__file__).name} with a reason."
            )

        # -- UNBANNERED hygiene ----------------------------------------------
        for name in exempt:
            if name not in job_ids:
                fail(
                    f"{rel}: UNBANNERED names `{name}`, which is not a job in this "
                    f"workflow. Remove the stale entry."
                )
            elif name in bannered:
                fail(
                    f"{rel}: UNBANNERED names `{name}`, but it now has a banner at "
                    f"line {bannered[name] + 1}. Remove the unnecessary entry."
                )

    # -- Anti-vacuity --------------------------------------------------------
    if banners_seen == 0:
        fail(
            "no `  ── ` section banner found in any workflow. Either the "
            "convention was abandoned or it was reworded out of the shape this gate "
            "reads -- both leave the gate checking nothing while still exiting 0."
        )

    if failures:
        print(f"\n{failures} workflow job-banner check(s) failed", file=sys.stderr)
        return 1

    print(f"\nok: all {banners_seen} section banner(s) introduce the job they describe")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
