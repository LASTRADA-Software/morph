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
it, and Codecov faithfully counts each stray `DA:<line>,0` as a real missed
line -- inflating the diff's miss count with lines nothing could ever have
exercised (`Q_OBJECT`'s macro line itself, its class's doc comments, blank
lines, `public:`/`signals:`/`private:`).

When `tr()`'s phantom region happens to be the file's *only* content (a
header holding nothing but declarations, all real bodies living in a
matching .cpp), `llvm-cov`'s own line-count aggregation disagrees with its
own per-line export: the file's `LF`/`LH` totals are `0` (llvm-cov report
shows "Lines: 0, Cover: -", excluding it from its percentage), yet the raw
per-line `DA:` records get emitted anyway. This script drops every
`DA:`/`FN:`/`FNDA:`/`FNF:`/`FNH:` record for such a file, matching what
`llvm-cov report` already shows -- no JSON lookup needed, since `LF:`/`LH:`
already appear in the raw lcov itself once each file's whole record is
buffered.

But a header can also mix real, hit lines (inline bodies, other classes'
declarations) with one `tr()`'s phantom span -- there `LF`/`LH` are
correctly nonzero, so the whole-file drop above never triggers, and the
phantom span's stray `DA:` records survive untouched (confirmed via real
CI data: examples/pastebin/include/pastebin/app/app.hpp showed LF:2/LH:2 --
its 2 real lines both hit -- while still carrying 21 stray `DA:40..60,0`
records from `App::tr()`, dragging the file to 10% covered).
`_ZN...2trEPKc...i` is the Itanium-mangled `tr(char const*, char const*,
int)` -- a stable signature across every `QObject`-derived class regardless
of namespace/class name -- so any `FN:` entry whose mangled name contains
that substring and whose `FNDA:` count is `0` gets its own `DA:` span (from
its own line to the line before the next `FN:`, or EOF) dropped, independent
of the file's overall LF/LH.

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


def dead_tr_spans(block):
    """Line ranges [start, end) of phantom, never-called Q_OBJECT tr()
    functions in this file's block -- see module docstring. Each FN: gives a
    candidate start line; FNDA: (by mangled name, order-independent) gives
    its call count; the span runs to the next-highest FN: start line in the
    file, or to infinity (EOF) if none is higher.
    """
    fn_lines = []  # (line, mangled_name), in file order but not necessarily line order
    call_count = {}  # mangled_name -> count
    for bline in block:
        if bline.startswith("FN:"):
            ln_str, name = bline[3:].split(",", 1)
            fn_lines.append((int(ln_str), name))
        elif bline.startswith("FNDA:"):
            count_str, name = bline[5:].split(",", 1)
            call_count[name] = call_count.get(name, 0) + int(count_str)

    starts = sorted(ln for ln, _name in fn_lines)
    spans = []
    for ln, name in fn_lines:
        if "2trEPKc" not in name or call_count.get(name, 0) != 0:
            continue
        later = [s for s in starts if s > ln]
        end = min(later) if later else float("inf")
        spans.append((ln, end))
    return spans


def main():
    in_lcov, json_path, out_lcov = sys.argv[1], sys.argv[2], sys.argv[3]
    branches = load_branch_aggregate(json_path)

    out = []
    # Each file's record is buffered so it can be dropped/filtered once
    # LF:/LH: -- which only appear at the very end of the block, right
    # before end_of_record -- and every FN:/FNDA: pair are known.
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
                # File has real content, so the whole-file drop above never
                # fires -- but it may still carry one or more phantom tr()
                # spans mixed in with real lines. Drop only their FN/FNDA and
                # the DA: records whose line falls in [start, end), then
                # shrink FNF: to match (each dropped tr() had FNDA:0, so
                # FNH: is unaffected). LF:/LH: need no adjustment at all --
                # confirmed against real data (examples/pastebin/include/
                # pastebin/app/app.hpp's own LF:2/LH:2 already counted only
                # its 2 real lines; llvm-cov's own line-count aggregation
                # never included the 21 stray tr()-span DA: records in the
                # first place, matching the whole-file LF:0/LH:0 case above.
                dead = dead_tr_spans(block)
                dropped_fns = 0
                for bline in block:
                    if bline.startswith(("FN:", "FNDA:")):
                        _, rest = bline.split(":", 1)
                        name = rest.split(",", 1)[1]
                        if "2trEPKc" in name:
                            if bline.startswith("FN:"):
                                dropped_fns += 1
                            continue  # drop this tr()'s own FN:/FNDA: record
                    elif bline.startswith("DA:"):
                        ln = int(bline[3:].split(",", 1)[0])
                        if any(start <= ln < end for start, end in dead):
                            continue
                    elif bline.startswith("FNF:"):
                        out.append(f"FNF:{int(bline[4:]) - dropped_fns}")
                        continue
                    out.append(bline)
                out.extend(branch_records)
                out.append(line)
            block = []
        else:
            block.append(line)

    open(out_lcov, "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
