#!/usr/bin/env python3
"""Rewrite an lcov file's branch records to llvm-cov's *aggregate* coverage,
and drop stray per-line records for files llvm-cov itself doesn't count.

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

It also strips a second, unrelated kind of noise (see morph#93): `Q_OBJECT`
implicitly declares a static `tr()` overload for the enclosing class, whose
coverage-map region llvm-cov attributes starting at the `Q_OBJECT` line and
running to the start of whatever comes next in the file (the next class, or
EOF) -- easily dozens of lines, spanning doc comments, blank lines and access
specifiers that were never executable in the first place. `tr()` itself is
practically never called directly (Qt's own i18n machinery calls it, not
application code), so this region is always "uncovered", and `llvm-cov show`/
`export -format=lcov` faithfully emit a `DA:<line>,0` for every line inside
it. Crucially, `llvm-cov`'s own line-count aggregation *disagrees* with its
own per-line export here: every such file's `LF`/`LH` totals are `0`, i.e.
llvm-cov report itself doesn't consider these "real" lines at all (hence
these files show `Lines: 0, Cover: -` in its report table) -- but the raw
per-line `DA:` records get emitted anyway, and Codecov faithfully counts
each stray `DA:<line>,0` as a real missed line, inflating the diff's miss
count with lines nothing could ever have exercised (`Q_OBJECT`'s macro line
itself, its class's doc comments, blank lines, `public:`/`signals:`/
`private:`). This script uses llvm-cov's own verdict -- a file whose block
ends with `LF:0`/`LH:0` -- to drop every `DA:`/`FN:`/`FNDA:`/`FNF:`/`FNH:`
record for that file, matching what `llvm-cov report` already shows. No
JSON lookup is needed for this part: `LF:`/`LH:` are already present in the
raw lcov itself, once each file's whole record is buffered.

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
    # Each file's record is buffered so it can be dropped in its entirety
    # (line/function noise) once LF:/LH: -- which only appear at the very
    # end of the block, right before end_of_record -- are known.
    block = []
    cur = None
    for raw in open(in_lcov):
        line = raw.rstrip("\n")
        if line.startswith("SF:"):
            cur = line[3:]
            block = [line]
        elif line.startswith(("BRDA:", "BRF:", "BRH:")):
            continue  # drop per-instantiation branch data; re-emitted below
        elif line == "end_of_record":
            by_line = branches.get(cur, {})
            brf = brh = 0
            branch_records = []
            idx = 0
            for ln in sorted(by_line):
                for (_c0, _c0b, t, fc) in by_line[ln]:
                    for taken in (t, fc):
                        branch_records.append(f"BRDA:{ln},0,{idx},{taken}")
                        idx += 1
                        brf += 1
                        if taken > 0:
                            brh += 1
            if brf:
                branch_records.append(f"BRF:{brf}")
                branch_records.append(f"BRH:{brh}")

            lf = lh = None
            for bline in block:
                if bline.startswith("LF:"):
                    lf = int(bline[3:])
                elif bline.startswith("LH:"):
                    lh = int(bline[3:])
            if lf == 0 and lh == 0:
                # llvm-cov's own verdict is "nothing here to count" (matches
                # `llvm-cov report`'s "Lines: 0, Cover: -"), but it still
                # emitted stray FN/FNDA/DA records for this file (morph#93).
                # Keep only SF:, drop everything the block carried, including
                # the branch records just built above.
                out.append(block[0])
                out.append("end_of_record")
            else:
                out.extend(block)  # BRDA:/BRF:/BRH: never reach `block` -- filtered out above
                out.extend(branch_records)
                out.append(line)
            block = []
        else:
            block.append(line)

    open(out_lcov, "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
