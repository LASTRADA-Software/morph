#!/usr/bin/env python3
"""Gate include/morph's *branch* coverage, which nothing has ever scored.

Usage:
    python3 scripts/check_branch_coverage.py [LCOV] [--objects coverage_objects.txt]
    python3 scripts/check_branch_coverage.py --self-test

Why this exists (morph#404)
---------------------------
This repository measures branch coverage and then gates on lines. Branch data is
produced and deliberately preserved -- scripts/coverage.sh exports LCOV BRDA
records and runs scripts/aggregate_lcov_branches.py to collapse llvm-cov's
per-template-instantiation noise into one record per source branch -- and
Codecov receives it. But every `status` and every component in codecov.yml
scores lines, and Codecov has no branch target at all, so a branch taken one way
only is visible in the report and gates nothing.

A line target cannot substitute. A line is covered the moment control reaches
it, whatever the condition on it evaluated to: `if (a && b)` is one line and
counts as hit for any `a` and `b`. The property this repository actually wants
held -- a wrong comparison, a dropped `!`, a `<` that should be `<=` is caught
by a test -- is a statement about branches, not about lines.

What it measures, and on which file
-----------------------------------
The *aggregated* LCOV, build/clang-coverage/coverage.lcov, which is
aggregate_lcov_branches.py's output -- never coverage.lcov.raw. That matters
enough that the ticket makes it a constraint: llvm-cov emits one BRDA record per
template instantiation, and morph is a header-only template library, so a raw
branch percentage counts one source branch dozens of times and scores it
"partial" for every instantiation that happened not to take an arm. The
aggregation is therefore load-bearing for this gate rather than cosmetic, and
running this on the raw file would produce a number that means nothing.

Two things are reported. **Branch coverage** is taken arms over total arms.
**Partial lines** are lines that were executed and carry at least one untaken
arm -- the 179 places in include/morph where a condition has been evaluated but
has only ever come out one way, and where inverting the test cannot fail a test.
They are the same lines Codecov counts against the line percentage, listed here
per file so a regression is localisable rather than averaged into one number.

The floors, and their margin
----------------------------
Set from measurement, the way every target in codecov.yml is, and deliberately
three points below it. The margin is not slack: these numbers were measured on
clang 22.1.8 locally while the coverage leg pins clang 20, and branch counts are
a property of the instrumentation, not of the tests -- two clang versions can
disagree about how many arms a decision has. Three points absorbs that while
still catching a real regression: the drop codecov.yml's own history records as
the thing nobody noticed was 5.46 points.

**The margin is provisional and should be tightened to about one point once the
coverage leg has produced a clang-20 number**, at which point these become
floors in the same sense the line targets are. Until then a gate that cannot
fire on toolchain noise is worth more than a tight one that fires on it and
gets deleted.

Two floors are not measured-minus-three. detail, render and qt measure 100%, and
a floor of 97% there would fail the moment anyone adds an untested branch to a
54-line file -- which is the correct behaviour, so they keep a real floor rather
than a generous one.
"""

from __future__ import annotations

import collections
import json
import os
import sys
import tempfile

# subsystem -> (floor %, measured % at the time the floor was set)
#
# Measured on 2026-09-02 over build/clang-coverage/coverage.lcov, clang 22.1.8,
# the CI coverage leg's configure (MORPH_BUILD_NET/OFFLINE_SQLITE/QT/LADDER=ON,
# MORPH_LADDER_RUNGS=all), with all 2435 ctest cases passing -- and, critically,
# after morph#403: include/morph/net contributed zero files to every report
# before that, so its 338 branches and 66 partial lines are new to the
# denominator rather than new to the code.
FLOORS = {
    "include/morph/core": (90.0, 93.22),
    "include/morph/detail": (97.0, 100.00),
    "include/morph/forms": (89.0, 92.36),
    "include/morph/journal": (96.0, 99.23),
    "include/morph/net": (72.0, 75.15),
    "include/morph/offline": (83.0, 86.25),
    "include/morph/qt": (97.0, 100.00),
    "include/morph/render": (97.0, 100.00),
    "include/morph/session": (92.0, 95.83),
    "include/morph/util": (91.0, 93.99),
}

# The library as a whole, which is what morph#404 asks to be "reported as its own
# number and carry a target".
TOTAL_FLOOR = 88.0
TOTAL_MEASURED = 91.19

# Subsystems that only reach the report when an off-by-default CMake option is
# on, and the test binary whose presence in coverage_objects.txt proves it was.
#
# `cmake --preset clang-coverage` alone -- the flow scripts/coverage.sh's own
# usage line documents -- profiles exactly one binary, morph_tests, and
# morph_tests compiles nothing under include/morph/net or include/morph/qt:
# those come from tests/net and tests/qt, gated behind MORPH_BUILD_NET and
# MORPH_BUILD_QT, both `option(... OFF)`. Without this table the vacuity check
# below fails that configure while blaming morph#403 -- a fixed defect -- for an
# option simply being off, which is the same shape of wrong diagnosis as a
# citation that has drifted.
#
# The mapping is checked in both directions rather than trusted, so it cannot
# rot into a permanent skip: a subsystem whose binary is absent must have *no*
# records (otherwise the mapping is stale and this gate says so), and one whose
# binary is present must have records (which is the morph#403 shape and stays
# fatal). A subsystem not named here is required unconditionally, so a new
# directory under include/morph defaults to the strict side.
OPTIONAL_SUBSYSTEMS = {
    "include/morph/net": "morph_net_tests",
    "include/morph/qt": "morph_qt_tests",
}

# Headers directly under include/morph/ (attributes.hpp, version.hpp) belong to
# no subsystem directory. Today both are macros and constexpr and emit no
# branch records at all, so this bucket never appears; if one ever does, it is
# reported in the table and counted in the include/morph TOTAL rather than
# failing with "carries no floor", which is a message no FLOORS edit can
# satisfy while the key names a file instead of a directory.
TOP_LEVEL_BUCKET = "include/morph (top level)"


def parse_lcov(path, repo_root):
    """{source path: {line: {"hits": int, "arms": [taken, ...]}}}"""
    prefix = os.path.realpath(repo_root) + os.sep
    files: dict[str, dict[int, dict]] = {}
    current = None
    with open(path) as handle:
        for raw in handle:
            line = raw.rstrip("\n")
            if line.startswith("SF:"):
                source = line[3:]
                if source.startswith(prefix):
                    source = source[len(prefix):]
                current = files.setdefault(source, {})
            elif line.startswith("DA:") and current is not None:
                number, _, hits = line[3:].partition(",")
                current.setdefault(int(number), {"hits": 0, "arms": []})["hits"] += int(hits)
            elif line.startswith("BRDA:") and current is not None:
                fields = line[5:].split(",")
                record = current.setdefault(int(fields[0]), {"hits": 0, "arms": []})
                record["arms"].append(fields[-1] not in ("-", "0"))
            elif line == "end_of_record":
                current = None
    return files


def summarise(files):
    """subsystem -> [arms, taken, partial lines], plus per-file partial counts."""
    subsystems = collections.defaultdict(lambda: [0, 0, 0])
    per_file = collections.Counter()
    for path, lines in files.items():
        if not path.startswith("include/morph/"):
            continue
        parts = path.split("/")
        subsystem = "/".join(parts[:3]) if len(parts) > 3 else TOP_LEVEL_BUCKET
        bucket = subsystems[subsystem]
        for record in lines.values():
            bucket[0] += len(record["arms"])
            bucket[1] += sum(1 for taken in record["arms"] if taken)
            if record["hits"] > 0 and record["arms"] and not all(record["arms"]):
                bucket[2] += 1
                per_file[path] += 1
    return subsystems, per_file


ALLOWLIST = "scripts/branch_partial_allowlist.json"


def resolve_allowlist_source_line(repo_root, path, hint, wanted, allowlist_path, failures):
    """Resolve one allowlist entry's `source` text to its current line number.

    Shared by this module's own resolve_allowlist() (keyed on partial branch
    lines) and scripts/check_error_path_coverage.py's (keyed on throw/catch
    sites, morph#406) -- both audit an allowlist entry the same way up to the
    point where they check the resolved line against their own kind of site,
    which is where the two callers diverge and this function stops. Factored
    out rather than left as two copies: the "moved line"/"ambiguous match"
    resolution here is exactly the fix for a defect this repository has found
    three times over in an allowlist keyed by line number alone (morph#349,
    morph#355, morph#419), and a second, independently-maintained copy of the
    fix is how that class of defect gets a fourth chance.

    Returns (resolved_line, source_lines) on success -- source_lines is the
    file's current content, so a caller that needs to search it again (this
    module's own caller does not; check_error_path_coverage.py's does, for a
    different site) is not made to re-read the file. Returns (None, None) and
    appends to `failures` on any failure: a missing source file, `wanted` text
    that no longer exists, or an ambiguous match. A "moved" resolution is
    still success (the returned line is correct) but is also reported to
    `failures`, since the allowlist's own `line` hint has drifted and update
    is still needed -- see the "has moved to line" message below.
    """
    source_file = os.path.join(repo_root, path)
    if not os.path.exists(source_file):
        failures.append(f"{allowlist_path} names {path}, which does not exist.")
        return None, None
    with open(source_file) as handle:
        source_lines = handle.read().splitlines()

    matches = [n for n, text in enumerate(source_lines, 1) if text.strip() == wanted]
    if not matches:
        failures.append(
            f"{path}:{hint} is allowlisted for a line reading {wanted!r}, which is "
            f"nowhere in the file any more. The code changed and the disposition "
            f"did not; re-read it rather than moving the number."
        )
        return None, source_lines
    if hint in matches:
        return hint, source_lines
    if len(matches) == 1:
        resolved = matches[0]
        failures.append(
            f"{path}:{hint} has moved to line {resolved}. The text still matches, so "
            f"nothing is wrong with the disposition -- update the `line` hint."
        )
        return None, source_lines

    failures.append(
        f"{path}:{hint} is allowlisted by a source line that appears {len(matches)} "
        f"times (lines {matches}), and none of them is {hint}, so which one is "
        f"meant is not decidable. Make the entry unambiguous."
    )
    return None, source_lines


def resolve_allowlist(repo_root, partial_lines, allowlist_path, failures):
    """Audit the allowlist in both directions; return the set it accounts for.

    `source`, not `line`, is the key. A comment citing a bare line number is the
    defect this repository has found three times (morph#349, morph#355,
    morph#419), and an allowlist keyed that way would rot the same way while
    still suppressing something. The line number is carried as a hint and
    reported back when it has moved.

    Both directions are audited because only one of them is obvious. An entry
    whose line is no longer partial is a stale suppression -- the test that
    covers it now exists, and the entry would go on hiding the next regression
    at that line. That is the failure mode scripts/scenario/coverage_allowlist.json
    already audits for, and the reason this file is not simply a list of things
    to ignore.
    """
    accounted = set()
    if not os.path.exists(allowlist_path):
        failures.append(
            f"{allowlist_path} is missing. It is where a partial line that no test "
            f"can cover is recorded with its reason; without it, an uncoverable line "
            f"and an untested one are indistinguishable."
        )
        return accounted

    with open(allowlist_path) as handle:
        document = json.load(handle)

    for entry in document.get("entries", []):
        path, hint = entry["file"], entry["line"]
        wanted, reason = entry["source"].strip(), entry.get("reason", "").strip()
        if not reason:
            failures.append(f"{path}:{hint} is allowlisted with no reason. A bare "
                            f"suppression is not a disposition.")
            continue

        resolved, _source_lines = resolve_allowlist_source_line(
            repo_root, path, hint, wanted, allowlist_path, failures)
        if resolved is None:
            continue

        if (path, resolved) not in partial_lines:
            failures.append(
                f"{path}:{resolved} is allowlisted as an uncoverable partial branch, but it "
                f"is not a partial line in this report. Either a test now covers it -- in "
                f"which case delete the entry, it is suppressing a line that no longer "
                f"needs it -- or the line stopped being a branch at all."
            )
            continue

        accounted.add((path, resolved))

    return accounted


def partial_line_set(files):
    """{(path, line)} for every line that ran and left an arm untaken."""
    partials = set()
    for path, lines in files.items():
        if not path.startswith("include/morph/"):
            continue
        for number, record in lines.items():
            if record["hits"] > 0 and record["arms"] and not all(record["arms"]):
                partials.add((path, number))
    return partials


def read_profiled_binaries(objects_path):
    """Basenames from coverage_objects.txt, or None when it was not supplied.

    None means "assume the full configure and enforce everything", which is what
    a hand invocation on an LCOV file gets. scripts/coverage.sh always passes the
    manifest, so the gate it runs knows what the build actually contained.
    """
    if not objects_path or not os.path.exists(objects_path):
        return None
    with open(objects_path, encoding="utf-8") as handle:
        return {os.path.basename(line.strip()) for line in handle if line.strip()}


def check(lcov_path, repo_root, out=sys.stdout, allowlist_path=None, objects_path=None):
    files = parse_lcov(lcov_path, repo_root)
    subsystems, per_file = summarise(files)
    partial_lines = partial_line_set(files)

    failures = []
    # Kept apart from `failures` because the two are enforced under different
    # conditions. A structural finding -- a stale table, a subsystem nothing
    # scores, an allowlist entry that no longer describes the code -- is true of
    # the tree and fatal whatever was built. A floor is a comparison against a
    # number measured under CI's configure, and is only meaningful when this
    # build is that configure.
    floor_failures = []
    if allowlist_path is None:
        allowlist_path = os.path.join(repo_root, ALLOWLIST)
    allowlisted = resolve_allowlist(repo_root, partial_lines, allowlist_path, failures)

    # Vacuity, in both directions. A subsystem this gate names but the report
    # does not contain is morph#403 happening again -- include/morph/net was
    # absent from every uploaded report for exactly that reason, and a gate that
    # reports "ok" over a missing subsystem is the silence that let it last
    # through three occurrences. A subsystem the report contains but this gate
    # does not name is the same defect mirrored: a new directory under
    # include/morph would be scored by nothing.
    profiled = read_profiled_binaries(objects_path)
    unbuilt = []
    for name in FLOORS:
        optional_suite = OPTIONAL_SUBSYSTEMS.get(name)
        suite_absent = (
            profiled is not None
            and optional_suite is not None
            and optional_suite not in profiled
        )
        if suite_absent:
            # Both directions, so the mapping above cannot rot into a silent skip.
            if subsystems.get(name, [0])[0] != 0:
                failures.append(
                    f"{name} has branch records, but {optional_suite} -- the binary "
                    f"this gate's OPTIONAL_SUBSYSTEMS table says brings it in -- was "
                    f"not profiled. The table is stale: find what compiles {name} now."
                )
            else:
                unbuilt.append((name, optional_suite))
            continue
        if subsystems.get(name, [0])[0] == 0:
            failures.append(
                f"{name} contributes no branch records to {lcov_path}. Either it was "
                f"dropped from the report -- which is morph#403's defect -- or it no "
                f"longer exists and this gate's table is stale. Both are errors."
            )
    for name in subsystems:
        if name not in FLOORS and name != TOP_LEVEL_BUCKET:
            failures.append(
                f"{name} is in the report but carries no floor in this script's FLOORS "
                f"table, so its branches are scored by nothing. Measure it and add it."
            )

    print(f"{'subsystem':26} {'arms':>7} {'taken':>7} {'branch%':>9} "
          f"{'floor':>7} {'partial lines':>14}", file=out)
    total_arms = total_taken = total_partial = 0
    for name in sorted(subsystems):
        arms, taken, partial = subsystems[name]
        total_arms += arms
        total_taken += taken
        total_partial += partial
        percent = 100.0 * taken / arms if arms else 0.0
        floor = FLOORS.get(name, (None, None))[0]
        floor_text = f"{floor:.0f}%" if floor is not None else "--"
        marker = ""
        if floor is not None and percent < floor:
            marker = "  <-- BELOW FLOOR"
            floor_failures.append(
                f"{name} branch coverage is {percent:.2f}%, below its {floor:.0f}% floor "
                f"(measured at {FLOORS[name][1]:.2f}% when the floor was set). A branch "
                f"that stopped being taken both ways is a test that stopped checking "
                f"something."
            )
        print(f"{name:26} {arms:7} {taken:7} {percent:9.2f} {floor_text:>7} "
              f"{partial:14}{marker}", file=out)

    total_percent = 100.0 * total_taken / total_arms if total_arms else 0.0
    print(f"{'include/morph TOTAL':26} {total_arms:7} {total_taken:7} "
          f"{total_percent:9.2f} {TOTAL_FLOOR:6.0f}% {total_partial:14}", file=out)
    if total_arms and total_percent < TOTAL_FLOOR:
        floor_failures.append(
            f"include/morph branch coverage is {total_percent:.2f}%, below its "
            f"{TOTAL_FLOOR:.0f}% floor (measured at {TOTAL_MEASURED:.2f}%)."
        )

    if per_file:
        print(file=out)
        print("partial lines by file (a line that ran, with an arm nothing took):", file=out)
        for path, count in per_file.most_common(12):
            print(f"  {count:4}  {path}", file=out)

    print(file=out)
    print(f"{len(allowlisted)} of {total_partial} partial lines are recorded in "
          f"{os.path.relpath(allowlist_path, repo_root)} as uncoverable, with a reason each; "
          f"the remaining {total_partial - len(allowlisted)} are untested.", file=out)

    if not unbuilt:
        failures.extend(floor_failures)

    if unbuilt:
        # Report, do not enforce. The floors above were measured under CI's
        # configure, where several binaries contribute coverage to the same
        # header; a subset build reads lower on subsystems that are present too,
        # not only on the ones that are missing, so failing any of them here
        # would be scoring this configure against another one's denominator.
        # CI always takes the enforcing path, because it builds every suite.
        print(file=out)
        for name, suite in unbuilt:
            print(f"not enforced: {name} was not built ({suite} was not profiled).",
                  file=out)
        print("The floors above are calibrated to CI's configure "
              "(MORPH_BUILD_NET/OFFLINE_SQLITE/QT/LADDER=ON, MORPH_LADDER_RUNGS=all); "
              "this build is a subset, so the floors are printed and not gated.",
              file=out)
        for breach in floor_failures:
            print(f"  would fail under CI's configure: {breach}", file=out)
        print("Structural findings -- a stale table, an unscored subsystem, an "
              "allowlist entry that no longer matches the code -- are still fatal "
              "below, because those are true of the tree rather than of the "
              "configure.", file=out)
        if failures:
            print(file=out)
            for failure in failures:
                print(f"error: {failure}", file=sys.stderr)
            return 1
        return 0

    if failures:
        print(file=out)
        for failure in failures:
            print(f"error: {failure}", file=sys.stderr)
        return 1

    print(file=out)
    print(f"ok: include/morph branch coverage {total_percent:.2f}% over {total_arms} "
          f"arms; {total_partial} partial lines", file=out)
    return 0


# ── Self-test ───────────────────────────────────────────────────────────────
# A gate nobody tests reports green whether or not it still detects anything,
# and this one is exposed to that twice over: it parses a file format, and the
# thing it guards (a branch that stopped being taken both ways) leaves no other
# trace. Fixtures are written by hand, so this needs no build and runs in
# drift-guard.yml alongside the other checkers' self-tests.

def _fixture(path, records):
    """records: {source: [(line, hits, [taken, ...]), ...]}"""
    with open(path, "w") as handle:
        for source, lines in records.items():
            handle.write(f"SF:{source}\n")
            for number, hits, arms in lines:
                handle.write(f"DA:{number},{hits}\n")
                for index, taken in enumerate(arms):
                    handle.write(f"BRDA:{number},0,{index},{1 if taken else 0}\n")
            handle.write("end_of_record\n")


def _every_subsystem(overrides=None):
    """A fixture that is comfortably above every floor, minus any overrides."""
    records = {}
    for name in FLOORS:
        records[f"{name}/covered.hpp"] = [(10, 1, [True, True]), (11, 1, [True, True])]
    if overrides:
        records.update(overrides)
    return records


def self_test():
    failures = 0

    def note(message):
        print(message)

    def fail(message, output=""):
        nonlocal failures
        failures += 1
        print(f"error: {message}", file=sys.stderr)
        if output:
            print(output, file=sys.stderr)

    import io

    def run(records, allowlist=None, sources=None, profiled=None):
        # stderr is captured along with stdout: the diagnostics below are the
        # gate working correctly, and letting them escape would make a passing
        # self-test read like a failing one.
        with tempfile.TemporaryDirectory() as work:
            path = os.path.join(work, "coverage.lcov")
            _fixture(path, records)
            for relative, text in (sources or {}).items():
                target = os.path.join(work, relative)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "w") as handle:
                    handle.write(text)
            allowlist_path = os.path.join(work, "allowlist.json")
            with open(allowlist_path, "w") as handle:
                json.dump({"entries": allowlist or []}, handle)
            objects_path = None
            if profiled is not None:
                objects_path = os.path.join(work, "coverage_objects.txt")
                with open(objects_path, "w") as handle:
                    handle.write("".join(f"{work}/{name}\n" for name in profiled))
            buffer = io.StringIO()
            saved, sys.stderr = sys.stderr, buffer
            try:
                code = check(path, work, out=buffer, allowlist_path=allowlist_path,
                             objects_path=objects_path)
            finally:
                sys.stderr = saved
            return code, buffer.getvalue()

    # A source file and a report in which its line 3 is partial: the shared
    # fixture for every allowlist case below.
    UNCOVERABLE_SOURCE = "// header\nvoid f() {\n    if (!std::is_constant_evaluated()) {\n    }\n}\n"
    UNCOVERABLE_PATH = "include/morph/util/uncoverable.hpp"

    def with_partial_line(arms=(True, False)):
        # Enough fully-covered arms alongside the partial one to keep the
        # subsystem above its floor: these cases are about the allowlist, and a
        # floor breach firing at the same time would mask what they assert.
        padding = [(100 + n, 1, [True, True]) for n in range(40)]
        return _every_subsystem({UNCOVERABLE_PATH: [(3, 1, list(arms))] + padding})

    # 1. Everything above its floor is accepted.
    code, output = run(_every_subsystem())
    if code == 0:
        note("ok: a report above every floor is accepted")
    else:
        fail("a report above every floor was rejected", output)

    # 2. A subsystem below its floor fails. The defect this gate exists for: the
    #    lines still run, so a line target would not notice.
    sunk = {"include/morph/core/sunk.hpp": [(n, 1, [True, False]) for n in range(10, 60)]}
    code, output = run(_every_subsystem(sunk))
    if code == 0:
        fail("a subsystem below its branch floor was accepted", output)
    else:
        note("ok: a subsystem below its branch floor is rejected")

    # 3. A subsystem missing from the report fails rather than being skipped.
    #    This is morph#403 in this gate's own terms: include/morph/net was absent
    #    from every uploaded report, and absence read as nothing to check.
    without_net = {k: v for k, v in _every_subsystem().items()
                   if not k.startswith("include/morph/net/")}
    code, output = run(without_net)
    if code == 0:
        fail("a subsystem absent from the report was accepted", output)
    else:
        note("ok: a subsystem absent from the report is rejected, not skipped")

    # 4. A subsystem present in the report but carrying no floor fails, so a new
    #    directory under include/morph cannot arrive scored by nothing.
    extra = _every_subsystem({"include/morph/brandnew/thing.hpp": [(10, 1, [True, True])]})
    code, output = run(extra)
    if code == 0:
        fail("a subsystem with no floor was accepted", output)
    else:
        note("ok: a subsystem with no floor of its own is rejected")

    # 5. A partial line is a line that RAN with an untaken arm -- not a line that
    #    never ran. Conflating the two would let this gate report partials where
    #    the line data already says "miss", and miss them where it says "hit".
    mixed = _every_subsystem({
        "include/morph/util/mixed.hpp": [
            (10, 1, [True, False]),   # ran, one arm untaken -> partial
            (11, 0, [False, False]),  # never ran            -> not a partial
            (12, 1, [True, True]),    # fully covered        -> not a partial
        ],
    })
    code, output = run(mixed)
    if "  1  include/morph/util/mixed.hpp" not in output.replace("   1", "  1"):
        fail("a line that ran with an untaken arm was not counted as exactly one partial", output)
    else:
        note("ok: a partial line is one that ran and left an arm untaken")

    # 6. An allowlisted partial line is accounted for and stops being reported as
    #    untested -- the whole purpose of the file.
    entry = [{"file": UNCOVERABLE_PATH, "line": 3,
              "source": "if (!std::is_constant_evaluated()) {",
              "reason": "constant evaluation increments no counters"}]
    code, output = run(with_partial_line(), allowlist=entry,
                       sources={UNCOVERABLE_PATH: UNCOVERABLE_SOURCE})
    if code == 0 and "1 of 1 partial lines are recorded" in output:
        note("ok: a partial line with a stated reason is accounted for")
    else:
        fail("an allowlisted partial line was not accounted for", output)

    # 7. A stale entry fails. This is the direction that makes the file an audit
    #    rather than a suppression list: the line is covered now, so the entry is
    #    hiding nothing and would hide the next regression at that line.
    code, output = run(with_partial_line(arms=(True, True)), allowlist=entry,
                       sources={UNCOVERABLE_PATH: UNCOVERABLE_SOURCE})
    if code == 0:
        fail("an allowlist entry for a line that is no longer partial was accepted", output)
    else:
        note("ok: an allowlist entry whose line is no longer partial is rejected")

    # 8. An entry whose source text no longer exists fails, rather than
    #    suppressing whatever now happens to sit at that line number. This is the
    #    rot morph#349, morph#355 and morph#419 are each an instance of.
    edited = "// header\nvoid f() {\n    if (somethingElseEntirely()) {\n    }\n}\n"
    code, output = run(with_partial_line(), allowlist=entry,
                       sources={UNCOVERABLE_PATH: edited})
    if code == 0:
        fail("an allowlist entry whose cited source line no longer exists was accepted", output)
    else:
        note("ok: an allowlist entry whose cited source text is gone is rejected")

    # 9. An entry whose line moved but whose text is still unique fails with the
    #    new number, rather than silently following it -- the disposition is
    #    still right, but the file must say where the line is.
    shifted = "// header\n// inserted\nvoid f() {\n    if (!std::is_constant_evaluated()) {\n    }\n}\n"
    moved = with_partial_line()
    moved[UNCOVERABLE_PATH] = [(4, 1, [True, False])] + moved[UNCOVERABLE_PATH][1:]
    code, output = run(moved, allowlist=entry, sources={UNCOVERABLE_PATH: shifted})
    if code != 0 and "has moved to line 4" in output:
        note("ok: an allowlist entry whose line moved is rejected, and told where it went")
    else:
        fail("an allowlist entry whose line moved was not reported", output)

    # 10. An entry with no reason fails. morph#404: a bare suppression is not a
    #     disposition, and an allowlist that accepts one becomes a list of things
    #     nobody has to justify.
    reasonless = [dict(entry[0], reason="")]
    code, output = run(with_partial_line(), allowlist=reasonless,
                       sources={UNCOVERABLE_PATH: UNCOVERABLE_SOURCE})
    if code == 0:
        fail("an allowlist entry with no reason was accepted", output)
    else:
        note("ok: an allowlist entry with no stated reason is rejected")

    # ── The manifest-aware vacuity rule (morph#404 follow-up) ───────────────
    # `cmake --preset clang-coverage` with nothing else profiles morph_tests
    # alone, and morph_tests compiles nothing under include/morph/net or
    # include/morph/qt. Before these four cases the gate failed that configure
    # while naming morph#403 -- a fixed defect -- as the cause.
    partial_build = {name: recs for name, recs in _every_subsystem().items()
                     if not name.startswith(("include/morph/net/", "include/morph/qt/"))}

    code, output = run(partial_build, profiled=["morph_tests"])
    if code == 0 and "not enforced: include/morph/net was not built" in output:
        note("ok: a configure that did not build tests/net is reported, not failed")
    else:
        fail("a subset configure was failed rather than reported", output)

    code, output = run(partial_build, profiled=["morph_tests", "morph_net_tests",
                                                "morph_qt_tests"])
    if code != 0 and "morph#403" in output:
        note("ok: a subsystem missing while its suite WAS profiled still fails")
    else:
        fail("a profiled-but-absent subsystem was accepted", output)

    code, output = run(_every_subsystem(), profiled=["morph_tests"])
    if code != 0 and "table is stale" in output:
        note("ok: records present while the mapped suite was not profiled fails")
    else:
        fail("a stale OPTIONAL_SUBSYSTEMS mapping was accepted", output)

    code, output = run(partial_build)
    if code != 0 and "morph#403" in output:
        note("ok: with no manifest the gate still enforces every subsystem")
    else:
        fail("omitting the manifest silently disabled the vacuity check", output)

    # A header directly under include/morph/ has no subsystem directory. It must
    # not be turned into a key naming a file, which no FLOORS edit could satisfy.
    code, output = run(_every_subsystem({"include/morph/version.hpp":
                                         [(3, 1, [True, True])]}))
    if code == 0 and "include/morph/version.hpp is in the report" not in output:
        note("ok: a top-level header does not become an unfixable FLOORS key")
    else:
        fail("a top-level header produced a floor demand naming a file", output)

    # A floor breach is a comparison against a number measured under a different
    # configure, so it is reported on a subset build and enforced on a full one.
    # Without the first of these the documented local flow -- `cmake --preset
    # clang-coverage` and nothing else -- fails at its last step on floors it
    # cannot reach; without the second, "not gated" would quietly become "never
    # gated".
    low_core = {f"include/morph/core/covered.hpp": [(10, 1, [True, True]),
                                                    (11, 1, [True, False]),
                                                    (12, 1, [True, False]),
                                                    (13, 1, [True, False])]}
    subset_low = {name: recs for name, recs in _every_subsystem(low_core).items()
                  if not name.startswith(("include/morph/net/", "include/morph/qt/"))}

    code, output = run(subset_low, profiled=["morph_tests"])
    if code == 0 and "would fail under CI's configure" in output:
        note("ok: a floor breach on a subset build is reported, not failed")
    else:
        fail("a subset build was failed on a floor it cannot reach", output)

    code, output = run(_every_subsystem(low_core),
                       profiled=["morph_tests", "morph_net_tests", "morph_qt_tests"])
    if code != 0 and "below its 90% floor" in output:
        note("ok: a floor breach on a full build still fails")
    else:
        fail("a floor breach was accepted on a full build", output)

    # Last, and it has to be: a check placed after this one runs, prints its
    # error, increments the counter -- and then the function falls through to
    # "all self-test checks passed" and exits 0. That is what happened when the
    # manifest-aware cases above were appended below an earlier copy of this
    # block, and drift-guard.yml would have reported green over a broken gate.
    if failures:
        print(f"\n{failures} self-test check(s) failed", file=sys.stderr)
        return 1

    print("\nall self-test checks passed")
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    objects = None
    positional = []
    rest = list(argv)
    while rest:
        arg = rest.pop(0)
        if arg == "--objects":
            if not rest:
                print("error: --objects needs a path to coverage_objects.txt",
                      file=sys.stderr)
                return 1
            objects = rest.pop(0)
        else:
            positional.append(arg)
    lcov = positional[0] if positional else "build/clang-coverage/coverage.lcov"
    if not os.path.exists(lcov):
        print(f"error: {lcov} does not exist. It is produced by scripts/coverage.sh, "
              f"which must run first.", file=sys.stderr)
        return 1
    if lcov.endswith(".raw"):
        print("error: this gate must read the aggregated LCOV (coverage.lcov), not "
              "coverage.lcov.raw. llvm-cov emits one BRDA record per template "
              "instantiation, so a branch percentage over the raw file counts one "
              "source branch dozens of times -- see scripts/aggregate_lcov_branches.py.",
              file=sys.stderr)
        return 1
    return check(lcov, os.getcwd(), objects_path=objects)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
