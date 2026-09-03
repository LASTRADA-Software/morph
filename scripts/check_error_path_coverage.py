#!/usr/bin/env python3
"""Measure how many of include/morph's throw sites and catch arms a test
actually drives, instead of leaving the question answered by nothing.

Usage:
    python3 scripts/check_error_path_coverage.py [LCOV]
    python3 scripts/check_error_path_coverage.py --self-test

Why this exists (morph#406)
----------------------------
include/morph has 148 `throw` sites and 81 `catch` arms (grep's own count,
reproduced by this script's `find_sites()` below). Line coverage says a
`throw` statement was executed; it says nothing about whether the matching
`catch` ran, whether the *right* one ran, or whether a test asserted on what
came out. A `catch` arm that swallows the wrong thing, or a `throw` whose
message names the wrong action, changes nothing an ordinary test observes --
and this repository has already been bitten there twice, both times in
`include/morph/core`: morph#347 (a `Completion` could settle with a null
`exception_ptr`, silently dropping `.onError()`) and morph#348/#351 (a throw
out of `dispatchExecute` stranded an ordering ticket no exit path released).

What "covered" means here, precisely
-------------------------------------
A `throw` site is covered if the aggregated LCOV (`coverage.lcov`, the same
file scripts/check_branch_coverage.py reads -- never the `.raw` export, see
that script's own comment for why a header-only template library's raw BRDA
records overcount) shows its line with a nonzero hit count. A `catch` arm is
covered the same way: the `catch` line itself ran, which only happens when
control entered the handler body. Neither claim is "the right exception was
thrown" or "the handler did the right thing" -- this script counts whether the
path was *exercised*, which is the floor morph#347 and morph#348 both sat
below (their sites executed in production and in no test), not a claim that
exercising it once is a sufficient test.

A raw throw-site count is not itself a target (the issue's own words). Many
of the 148 are defensive guards this repository has already decided are
unreachable in practice -- `codecov.yml` documents several such "turns an
impossible state into a typed error instead of a dereference" cases for
ordinary lines, and this script extends the same discipline to throw/catch
sites specifically, via `scripts/error_path_allowlist.json`. An entry there
needs a reason, not a checkbox, and is audited in both directions on every
run, exactly as `check_branch_coverage.py`'s own allowlist is: a line that
stops being a throw/catch site, or that a test now covers, fails the gate
until the entry is removed.

What this cannot tell you, stated rather than left to be discovered
----------------------------------------------------------------------
Line-hit coverage of a `throw` statement or a `catch (...)` line is a
necessary condition for a test exercising that path, not a sufficient one.
A `catch (...) { /* swallow */ }` that runs is "covered" by this script's own
definition even though nothing asserts what it swallowed -- finding those is
a job for mutation testing (morph#405's survivor triage), not this script,
and is named here so this instrument is not mistaken for that one.
"""
import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_branch_coverage  # noqa: E402 -- see parse_lcov_hits() below

REPO_ROOT_MARKER = "include/morph"

# Matches the issue's own methodology exactly (`grep -rn 'throw '`,
# `grep -rnE 'catch *\('`), so this script's counts reproduce the numbers the
# issue measured by hand rather than a stricter or looser variant quietly
# changing the denominator between the report and the ticket that asked for it.
THROW_RE = re.compile(r"throw ")
CATCH_RE = re.compile(r"catch *\(")

ALLOWLIST = "scripts/error_path_allowlist.json"


def find_sites(repo_root):
    """{(path, line, kind): source_text} for every throw/catch site under
    include/morph/**/*.hpp, kind is "throw" or "catch"."""
    sites = {}
    base = os.path.join(repo_root, "include", "morph")
    for dirpath, _dirnames, filenames in os.walk(base):
        for filename in sorted(filenames):
            if not filename.endswith(".hpp"):
                continue
            full = os.path.join(dirpath, filename)
            rel = os.path.relpath(full, repo_root).replace(os.sep, "/")
            with open(full, encoding="utf-8") as handle:
                for number, text in enumerate(handle, 1):
                    if THROW_RE.search(text):
                        sites[(rel, number, "throw")] = text.strip()
                    if CATCH_RE.search(text):
                        sites[(rel, number, "catch")] = text.strip()
    return sites


def parse_lcov_hits(path, repo_root):
    """{source path: {line: hit_count}} -- the DA: half of the aggregated LCOV.

    A thin adapter over check_branch_coverage.parse_lcov(), which already
    parses this exact file format (SF:/DA:/BRDA:/end_of_record, with the same
    repo_root-prefix stripping) for morph#404's branch gate. Reimplementing
    that loop here would be the second copy of it in this repository, and the
    first one is already a fix for a defect found three times over
    (morph#349, morph#355, morph#419) -- a second, independently-maintained
    parser is how that class of defect gets a fourth chance. Only `hits` is
    read out of each record; `arms` (BRDA: data) is branch-arm information a
    throw/catch site's own question -- "did control reach this line" -- has
    no use for.
    """
    files = check_branch_coverage.parse_lcov(path, repo_root)
    return {source: {line: record["hits"] for line, record in lines.items()}
            for source, lines in files.items()}


def resolve_allowlist(repo_root, sites, hits, allowlist_path, failures):
    """Audit scripts/error_path_allowlist.json in both directions; return the
    {(path, line, kind)} set it accounts for.

    The source-text-keyed line resolution (find `source`'s current line,
    catching a moved or ambiguous hint) is check_branch_coverage.py's
    resolve_allowlist_source_line() -- shared rather than reimplemented, since
    it is the fix for a defect this repository has already found three times
    over in an allowlist keyed by line number alone (morph#349, morph#355,
    morph#419). What is specific to this script, and stays here: validating
    `kind`, checking the resolved line is still a throw/catch site of that
    kind (not just any line), and checking it against `hits` rather than a
    branch-arm partial set.
    """
    accounted = set()
    if not os.path.exists(allowlist_path):
        failures.append(
            f"{allowlist_path} is missing. It is where a throw/catch site nothing "
            f"can reach is recorded with its reason; without it, an unreachable site "
            f"and an untested one are indistinguishable."
        )
        return accounted

    with open(allowlist_path, encoding="utf-8") as handle:
        document = json.load(handle)

    site_kinds = {(path, line, kind) for (path, line, kind) in sites}

    for entry in document.get("entries", []):
        path, hint, kind = entry["file"], entry["line"], entry["kind"]
        wanted, reason = entry["source"].strip(), entry.get("reason", "").strip()
        if kind not in ("throw", "catch"):
            failures.append(f"{path}:{hint} names kind {kind!r}, which is neither "
                            f"'throw' nor 'catch'.")
            continue
        if not reason:
            failures.append(f"{path}:{hint} is allowlisted with no reason. A bare "
                            f"suppression is not a disposition.")
            continue

        resolved = check_branch_coverage.resolve_allowlist_source_line(
            repo_root, path, hint, wanted, allowlist_path, failures)
        if resolved is None:
            continue

        if (path, resolved, kind) not in site_kinds:
            failures.append(
                f"{path}:{resolved} is allowlisted as an unreachable {kind} site, but "
                f"grep does not find a {kind} there any more. Either the code changed "
                f"shape -- delete the entry -- or `kind` is wrong."
            )
            continue

        if hits.get(path, {}).get(resolved, 0) > 0:
            failures.append(
                f"{path}:{resolved} is allowlisted as an unreachable {kind} site, but "
                f"it has a nonzero hit count in this report. Either a test now covers "
                f"it -- in which case delete the entry, it is suppressing a site that no "
                f"longer needs it -- or the hit is spurious and worth its own look before "
                f"trusting this figure."
            )
            continue

        accounted.add((path, resolved, kind))

    return accounted


def check(lcov_path, repo_root, out=sys.stdout, allowlist_path=None):
    sites = find_sites(repo_root)
    hits = parse_lcov_hits(lcov_path, repo_root)

    failures = []
    if allowlist_path is None:
        allowlist_path = os.path.join(repo_root, ALLOWLIST)
    allowlisted = resolve_allowlist(repo_root, sites, hits, allowlist_path, failures)

    # subsystem -> [throw_total, throw_covered, catch_total, catch_covered]
    subsystems = collections.defaultdict(lambda: [0, 0, 0, 0])
    uncovered = []  # (path, line, kind, text) not allowlisted and not hit

    for (path, line, kind), text in sorted(sites.items()):
        parts = path.split("/")
        subsystem = "/".join(parts[:3]) if len(parts) > 3 else "include/morph (top level)"
        bucket = subsystems[subsystem]
        line_hits = hits.get(path, {}).get(line, 0)
        covered = line_hits > 0
        idx = 0 if kind == "throw" else 2
        bucket[idx] += 1
        if covered:
            bucket[idx + 1] += 1
        elif (path, line, kind) not in allowlisted:
            uncovered.append((path, line, kind, text))

    out.write("include/morph error-path coverage (morph#406)\n")
    out.write(f"{'subsystem':<28}{'throw':>16}{'catch':>16}\n")
    grand = [0, 0, 0, 0]
    for subsystem in sorted(subsystems):
        t_total, t_hit, c_total, c_hit = subsystems[subsystem]
        for i, v in enumerate((t_total, t_hit, c_total, c_hit)):
            grand[i] += v
        t_pct = f"{t_hit}/{t_total}" if t_total else "-"
        c_pct = f"{c_hit}/{c_total}" if c_total else "-"
        out.write(f"{subsystem:<28}{t_pct:>16}{c_pct:>16}\n")
    t_total, t_hit, c_total, c_hit = grand
    out.write(f"{'TOTAL':<28}{f'{t_hit}/{t_total}':>16}{f'{c_hit}/{c_total}':>16}\n")
    if len(sites) != 148 + 81:
        # Not a failure: the issue's 148/81 was a snapshot, and this script's
        # job is to keep measuring as the tree changes, not to freeze the
        # count. Printed so a large jump is visible without being fatal.
        thrown = sum(1 for (*_, kind) in sites if kind == "throw")
        caught = len(sites) - thrown
        out.write(f"\n(note: {thrown} throw sites, {caught} catch arms found today; "
                 f"morph#406 measured 148/81)\n")

    if uncovered:
        out.write(f"\n{len(uncovered)} error-path site(s) no test drives:\n")
        for path, line, kind, text in uncovered:
            out.write(f"  {path}:{line} [{kind}] {text}\n")
        failures.append(
            f"{len(uncovered)} throw/catch site(s) are neither covered nor allowlisted "
            f"(listed above). Each needs either a test that reaches it, or an entry in "
            f"{os.path.relpath(allowlist_path, repo_root)} with the reason it cannot be."
        )

    if failures:
        out.write("\n")
        for failure in failures:
            out.write(f"error: {failure}\n")
        return 1

    return 0


# ── Self-test ────────────────────────────────────────────────────────────────
# No build needed: find_sites() reads hand-written fixture headers, and
# parse_lcov_hits() reads a hand-written LCOV fragment -- both files, both
# reproducible without a coverage configure.

def _fixture_tree(root, files):
    for rel, content in files.items():
        full = os.path.join(root, "include", "morph", rel)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w", encoding="utf-8") as handle:
            handle.write(content)


def _fixture_lcov(path, records):
    """records: {source: {line: hits}}"""
    with open(path, "w", encoding="utf-8") as handle:
        for source, lines in records.items():
            handle.write(f"SF:{source}\n")
            for number, hit_count in lines.items():
                handle.write(f"DA:{number},{hit_count}\n")
            handle.write("end_of_record\n")


def self_test():
    import io
    import shutil
    import tempfile

    failures = 0

    def note(message):
        print(message)

    def fail(message, output=""):
        nonlocal failures
        failures += 1
        print(f"error: {message}", file=sys.stderr)
        if output:
            print(output, file=sys.stderr)

    work = tempfile.mkdtemp()
    try:
        # ── 1. a covered throw and an uncovered catch ────────────────────────
        root = os.path.join(work, "case1")
        os.makedirs(root)
        _fixture_tree(root, {
            "core/x.hpp": (
                "void f() {\n"
                "    throw std::runtime_error(\"boom\");\n"  # line 2
                "}\n"
                "void g() {\n"
                "    try { f(); } catch (const std::exception& e) {\n"  # line 5
                "        // handled\n"
                "    }\n"
                "}\n"
            ),
        })
        lcov = os.path.join(root, "coverage.lcov")
        _fixture_lcov(lcov, {"include/morph/core/x.hpp": {2: 3, 5: 0}})
        allowlist = os.path.join(root, "allowlist.json")
        with open(allowlist, "w", encoding="utf-8") as handle:
            json.dump({"entries": []}, handle)

        buf = io.StringIO()
        rc = check(lcov, root, out=buf, allowlist_path=allowlist)
        output = buf.getvalue()
        if rc != 1:
            fail("an uncovered, unallowlisted catch site did not fail the gate", output)
        elif "x.hpp:5 [catch]" not in output:
            fail("the uncovered catch site was not named in the output", output)
        elif "x.hpp:2 [throw]" in output:
            fail("the covered throw site was wrongly reported as uncovered", output)
        else:
            note("ok: a covered throw and an uncovered catch are told apart")

        # ── 2. the uncovered site, now allowlisted with a reason -> passes ───
        with open(allowlist, "w", encoding="utf-8") as handle:
            json.dump({"entries": [{
                "file": "include/morph/core/x.hpp", "line": 5, "kind": "catch",
                "source": "try { f(); } catch (const std::exception& e) {",
                "reason": "fixture: deliberately unreachable in this self-test",
            }]}, handle)
        buf = io.StringIO()
        rc = check(lcov, root, out=buf, allowlist_path=allowlist)
        if rc != 0:
            fail("a reasoned allowlist entry did not clear the gate", buf.getvalue())
        else:
            note("ok: an allowlisted, reasoned site passes")

        # ── 3. allowlisted with no reason -> fails, naming it a bare suppression ──
        with open(allowlist, "w", encoding="utf-8") as handle:
            json.dump({"entries": [{
                "file": "include/morph/core/x.hpp", "line": 5, "kind": "catch",
                "source": "try { f(); } catch (const std::exception& e) {",
                "reason": "",
            }]}, handle)
        buf = io.StringIO()
        rc = check(lcov, root, out=buf, allowlist_path=allowlist)
        if rc == 0:
            fail("a reason-less allowlist entry passed the gate", buf.getvalue())
        elif "bare suppression" not in buf.getvalue():
            fail("a reason-less entry failed, but not with the bare-suppression message",
                 buf.getvalue())
        else:
            note("ok: a reason-less allowlist entry is rejected")

        # ── 4. an allowlisted line that is now covered -> stale-entry failure ──
        with open(allowlist, "w", encoding="utf-8") as handle:
            json.dump({"entries": [{
                "file": "include/morph/core/x.hpp", "line": 2, "kind": "throw",
                "source": "throw std::runtime_error(\"boom\");",
                "reason": "fixture: pretend this was once unreachable",
            }]}, handle)
        buf = io.StringIO()
        rc = check(lcov, root, out=buf, allowlist_path=allowlist)
        if rc == 0:
            fail("an allowlist entry for a now-covered line did not fail", buf.getvalue())
        elif "no longer needs it" not in buf.getvalue():
            fail("a stale allowlist entry failed, but not with the expected message",
                 buf.getvalue())
        else:
            note("ok: an allowlist entry for a since-covered line is rejected as stale")

        # ── 5. an allowlist entry citing text that has moved -> resolved, warned ──
        _fixture_tree(root, {
            "core/x.hpp": (
                "// a new leading comment shifts everything down one line\n"
                "void f() {\n"
                "    throw std::runtime_error(\"boom\");\n"  # now line 3
                "}\n"
                "void g() {\n"
                "    try { f(); } catch (const std::exception& e) {\n"  # now line 6
                "        // handled\n"
                "    }\n"
                "}\n"
            ),
        })
        _fixture_lcov(lcov, {"include/morph/core/x.hpp": {3: 3, 6: 0}})
        with open(allowlist, "w", encoding="utf-8") as handle:
            json.dump({"entries": [{
                "file": "include/morph/core/x.hpp", "line": 5, "kind": "catch",
                "source": "try { f(); } catch (const std::exception& e) {",
                "reason": "fixture: moved by one line",
            }]}, handle)
        buf = io.StringIO()
        rc = check(lcov, root, out=buf, allowlist_path=allowlist)
        if rc != 1:
            fail("a moved-line entry did not fail (it should, until the hint is updated)",
                 buf.getvalue())
        elif "has moved to line 6" not in buf.getvalue():
            fail("a moved-line entry failed, but did not report the resolved line",
                 buf.getvalue())
        else:
            note("ok: an allowlist entry citing moved text resolves and reports the new line")

        # ── 6. missing allowlist file -> fails, naming it ────────────────────
        missing = os.path.join(root, "does_not_exist.json")
        buf = io.StringIO()
        rc = check(lcov, root, out=buf, allowlist_path=missing)
        if rc != 1:
            fail("a missing allowlist file did not fail the gate", buf.getvalue())
        elif missing not in buf.getvalue():
            fail("a missing allowlist failure did not name the path", buf.getvalue())
        else:
            note("ok: a missing allowlist file fails, and is named")

    finally:
        shutil.rmtree(work, ignore_errors=True)

    if failures:
        print(f"\n{failures} self-test check(s) failed", file=sys.stderr)
        return 1

    print("\nall self-test checks passed")
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    lcov = argv[0] if argv else "build/clang-coverage/coverage.lcov"
    if not os.path.exists(lcov):
        print(f"error: {lcov} does not exist. It is produced by scripts/coverage.sh, "
              f"which must run first.", file=sys.stderr)
        return 1
    return check(lcov, os.getcwd())


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
