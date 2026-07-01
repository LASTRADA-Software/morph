#!/usr/bin/env python3
"""Rewrite an lcov file's branch records to llvm-cov's *aggregate* coverage.

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


def main():
    in_lcov, json_path, out_lcov = sys.argv[1], sys.argv[2], sys.argv[3]
    branches = load_branch_aggregate(json_path)

    out = []
    cur = None
    for raw in open(in_lcov):
        line = raw.rstrip("\n")
        if line.startswith("SF:"):
            cur = line[3:]
            out.append(line)
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
