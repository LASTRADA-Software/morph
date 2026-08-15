#!/usr/bin/env python3
"""Rewrite an lcov file's branch records to llvm-cov's *aggregate* coverage,
and drop phantom zero-line function records for implicit-special-member-only
headers.

`llvm-cov export -format=lcov` emits branch data (BRDA) once per template
instantiation. A branch that is fully covered in aggregate is therefore still
reported with an untaken arm for every instantiation that happened not to take
it, and Codecov scores each such line as "partial". For header-only templated
code (e.g. completion.hpp, bridge.hpp) this manufactures dozens of spurious
partials even though `llvm-cov report` shows the files at ~100% branch coverage.

This script keeps branch coverage (we do NOT drop it) but collapses the
per-instantiation records into one record per *source branch*, keyed by the
branch's source location taken from the JSON export. The result mirrors exactly
what `llvm-cov report` counts, so genuinely-uncovered branches stay uncovered
and only the per-instantiation noise disappears.

It also strips a second, unrelated kind of noise (see morph#93): a header that
declares a class with no user-provided special members (no explicit
destructor, e.g. a `QObject`-derived adapter whose body lives entirely in a
matching .cpp) gets an implicitly-defined destructor synthesized by the
compiler. On the Itanium ABI that implicit destructor gets its own
coverage-map function/region record, attributed back to the class's
declaration in the header, but -- being compiler-synthesized rather than
user-written -- it carries no real line span of its own and is scored
permanently "missed" regardless of how many times the class is actually
destroyed. `llvm-cov report` already discounts this (such a file shows
`Lines: 0, Cover: -`, excluded from the percentage rather than scored against
it), but `llvm-cov export -format=lcov` still emits the FN/FNDA records for
these phantom functions, so Codecov counts them as real uncovered lines/
functions in the diff. A file with functions but zero real instrumented lines
is exactly `llvm-cov report`'s own "Lines: 0" signal, so this script uses that
same signal to drop the file's FN/FNDA/FNF/FNH records -- the file's DA/BRDA
records are untouched (there are none to touch: zero lines means the JSON
export has no `segments` for it either).

Usage: aggregate_lcov_branches.py <in.lcov> <cov.json> <out.lcov>
"""
import json
import sys
from collections import defaultdict, OrderedDict


def load_branch_aggregate(json_path):
    """file-basename-independent: map absolute filename -> {line: [taken_arms...]}."""
    data = json.load(open(json_path))
    per_file = {}
    for f in data["data"][0]["files"]:
        # Aggregate each source branch (identified by its start/end location)
        # across every instantiation, summing the true/false execution counts.
        agg = defaultdict(lambda: [0, 0])
        for b in f.get("branches", []):
            key = (b[0], b[1], b[2], b[3])  # l0, c0, l1, c1
            agg[key][0] += b[4]  # true count
            agg[key][1] += b[5]  # false count
        by_line = defaultdict(list)
        for (l0, c0, _l1, c0b), (t, fc) in sorted(agg.items()):
            by_line[l0].append((c0, c0b, t, fc))
        per_file[f["filename"]] = by_line
    return per_file


def load_files_with_no_real_lines(json_path):
    """Filenames (as they appear in the JSON export) that have zero
    instrumented source lines but at least one function record --
    `llvm-cov report`'s own "Lines: 0" files (shown with `Cover: -`,
    excluded from its percentage). A file with any real, user-written
    function body has `summary.lines.count > 0`; the only way to have
    function records with zero lines is a phantom implicit-special-member
    record (morph#93).
    """
    data = json.load(open(json_path))
    no_lines = set()
    for f in data["data"][0]["files"]:
        summary = f.get("summary", {})
        if summary.get("lines", {}).get("count", 0) == 0 and summary.get("functions", {}).get("count", 0) > 0:
            no_lines.add(f["filename"])
    return no_lines


def main():
    in_lcov, json_path, out_lcov = sys.argv[1], sys.argv[2], sys.argv[3]
    branches = load_branch_aggregate(json_path)
    phantom_only_files = load_files_with_no_real_lines(json_path)

    out = []
    cur = None
    cur_is_phantom_only = False
    for raw in open(in_lcov):
        line = raw.rstrip("\n")
        if line.startswith("SF:"):
            cur = line[3:]
            cur_is_phantom_only = cur in phantom_only_files
            out.append(line)
        elif line.startswith(("FN:", "FNDA:", "FNF:", "FNH:")) and cur_is_phantom_only:
            continue  # drop phantom implicit-special-member function records (morph#93)
        elif line.startswith(("BRDA:", "BRF:", "BRH:")):
            continue  # drop per-instantiation branch data; re-emitted below
        elif line == "end_of_record":
            by_line = branches.get(cur, {})
            brf = brh = 0
            idx = 0
            for ln in sorted(by_line):
                for (_c0, _c0b, t, fc) in by_line[ln]:
                    for taken in (t, fc):
                        out.append(f"BRDA:{ln},0,{idx},{taken}")
                        idx += 1
                        brf += 1
                        if taken > 0:
                            brh += 1
            if brf:
                out.append(f"BRF:{brf}")
                out.append(f"BRH:{brh}")
            out.append(line)
        else:
            out.append(line)

    open(out_lcov, "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
