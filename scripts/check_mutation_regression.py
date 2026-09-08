#!/usr/bin/env python3
"""Fails the scheduled mutation campaign when survivors regress against the
last recorded baseline, instead of the score being read by nobody until
someone re-runs the campaign by hand.

Usage:
    python3 scripts/check_mutation_regression.py REPORT SCOPE
    python3 scripts/check_mutation_regression.py --self-test

Run by .github/workflows/mutation.yml after scripts/mutation.sh (morph#408).
REPORT is the IDE-format report scripts/mutation.sh's own `mull-runner`
invocation writes (`build/mutation-<scope>/mutation-<scope>.txt`); only its
first line is read -- the same "Survived mutants (N/M)" header
scripts/mutation.sh already parses to print its own score. SCOPE is whatever
scripts/mutation.sh was run with (`core-forms`, `core`, `forms`, `net`) and is
the key this script's baseline is recorded under, in `scripts/mutation_baseline.json`.

Why this exists
----------------
docs/spec/testing_charter.md names the gap in its own words: "Mutation score
has no floor. scripts/mutation.sh reports a number; nothing fails a build if
it drops. A regression would have to be noticed by someone re-running the
campaign and comparing by hand." That is this script's job, run on a
schedule rather than per-PR -- see #408's own decision comment for why a
per-PR gate was rejected (the tool is known-defective for one mutator family,
the per-PR economics are bad, and a gate nobody can afford to run is not a
gate).

Deliberately a total-survivor count, not per-mutator-family
-------------------------------------------------------------
#408's original design asked for a per-mutator-family comparison specifically
to survive `cxx_remove_void_call`'s known-broken noise (morph#434: 148 of the
352 survivors in the last measured run were a tool artifact on a mutator that
silently never removed the call it claimed to, at member/operator call
sites). scripts/mutation.sh now excludes that mutator from its `mutators:`
list outright (see that script's own header comment) -- the noise a
per-family split existed to route around is gone from the mutant population
*before the runner ever executes a mutant*, not merely filtered out of the
report afterward. A plain total-survivor count over the reduced set is no
longer measuring a mixture of real signal and a known-broken tool artifact,
so it is no longer the wrong unit. Re-introducing a per-family split remains
possible later (Mull's IDE report names each survivor's mutator per line) if
a *different* mutator in this reduced set turns out to need the same
treatment -- nothing here rules it out, it is just not needed for this one.

Ratchets down, never up
-------------------------
The recorded baseline only ever moves to a lower-or-equal survivor count. A
run with more survivors than the baseline fails and leaves the baseline
untouched, so the next run is compared against the same number until a human
either closes the regression or -- for a survivor judged equivalent, the
same standard scripts/mutation_survivors.json already applies -- updates
scripts/mutation_baseline.json by hand with a reason recorded there. A run
that improves on the baseline (or the first run for a scope, which has
nothing to compare against) updates it automatically: there is no reason to
require a human to bless a suite getting *better* at catching mutations.

Invariant 7 (the negative control this gate must not be blind to)
---------------------------------------------------------------------
Zero mutants is always an error here, never a legitimate outcome.
scripts/mutation.sh's campaign is not diff-scoped (`gitDiffRef`-based
incremental scoping was #408's rejected per-PR option, not this scheduled
job's), so there is no "the diff touched nothing in scope" explanation for an
empty mutant population the way there would be for a per-PR gate. An empty
population here can only mean the instrumented build and the runner's
`includePaths` disagree -- scripts/mutation.sh's own "The step that is easy
to get wrong," which silently prints "Mutation score: infinitely high" if
this script did not refuse it outright.
"""
import json
import os
import re
import sys

REPORT_HEADER_RE = re.compile(r"Survived mutants \((\d+)/(\d+)\)")

DEFAULT_BASELINE_PATH = "scripts/mutation_baseline.json"


def parse_report(report_path):
    """Returns (survived, mutants) read from an IDE-format Mull report's
    first line. Raises ValueError if the header is not the expected shape --
    e.g. a report mull-runner never actually wrote (see
    scripts/mutation.sh's own check for that failure)."""
    with open(report_path, encoding="utf-8", errors="replace") as handle:
        header = handle.readline()
    match = REPORT_HEADER_RE.search(header)
    if not match:
        raise ValueError(
            f"{report_path} does not start with Mull's survivor header "
            f"('Survived mutants (N/M)'); got: {header!r}")
    return int(match.group(1)), int(match.group(2))


def check(report_path, scope, baseline_path=DEFAULT_BASELINE_PATH, out=sys.stdout):
    """Checks REPORT's survivor count for SCOPE against BASELINE_PATH's
    recorded baseline, updating it on an improvement or a first run.
    Returns 0 (pass) or 1 (fail); writes its verdict to `out`."""
    try:
        survived, mutants = parse_report(report_path)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=out)
        return 1

    if mutants == 0:
        print(
            f"error: {report_path}: 0 mutants for scope {scope!r}. This campaign is not "
            f"diff-scoped, so an empty mutant population is always a configuration bug (a "
            f"scope mismatch between the instrumented build and the runner config -- see "
            f"scripts/mutation.sh's own \"The step that is easy to get wrong\"), never a "
            f"legitimate result. Refusing to read this as a clean run.",
            file=out)
        return 1

    with open(baseline_path, encoding="utf-8") as handle:
        doc = json.load(handle)
    baselines = doc.setdefault("baselines", {})
    recorded = baselines.get(scope)

    def save():
        with open(baseline_path, "w", encoding="utf-8") as handle:
            json.dump(doc, handle, indent=2)
            handle.write("\n")

    if recorded is None:
        baselines[scope] = {"survived": survived, "mutants": mutants}
        save()
        print(
            f"ok: no recorded baseline for scope {scope!r}; recording this run as the first "
            f"one ({survived} survived / {mutants} mutants). Nothing to compare against yet.",
            file=out)
        return 0

    if survived > recorded["survived"]:
        print(
            f"error: regression for scope {scope!r}: {survived} survivors this run, up from a "
            f"recorded baseline of {recorded['survived']} ({mutants} mutants this run vs "
            f"{recorded['mutants']} baseline). A new survivor means the suite stopped noticing "
            f"a mutation it used to catch (or would have). Triage it in "
            f"scripts/mutation_survivors.json; if it is genuinely equivalent, update "
            f"{baseline_path} by hand with a reason recorded there. The baseline is left "
            f"unchanged.",
            file=out)
        return 1

    if survived < recorded["survived"] or mutants != recorded["mutants"]:
        baselines[scope] = {"survived": survived, "mutants": mutants}
        save()
        print(
            f"ok: {survived} survivors for scope {scope!r} ({mutants} mutants) -- at or below "
            f"the recorded baseline of {recorded['survived']}; baseline updated (ratchets down "
            f"only, never up).",
            file=out)
        return 0

    print(f"ok: {survived} survivors for scope {scope!r}, unchanged from the recorded baseline.", file=out)
    return 0


def self_test():
    import io
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

    def write_report(path, survived, mutants):
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(f"Survived mutants ({survived}/{mutants})\n")

    def write_baseline(path, baselines):
        with open(path, "w", encoding="utf-8") as handle:
            json.dump({"baselines": baselines}, handle)

    work = tempfile.mkdtemp()
    try:
        # ── 1. no recorded baseline -> records this run, passes ─────────────
        report = os.path.join(work, "first.txt")
        write_report(report, survived=10, mutants=100)
        baseline = os.path.join(work, "baseline1.json")
        write_baseline(baseline, {})
        buf = io.StringIO()
        rc = check(report, "core-forms", baseline_path=baseline, out=buf)
        with open(baseline, encoding="utf-8") as handle:
            after = json.load(handle)
        if rc != 0:
            fail("a first run with no recorded baseline failed instead of recording one", buf.getvalue())
        elif after["baselines"].get("core-forms") != {"survived": 10, "mutants": 100}:
            fail(f"the first run did not record its own baseline: {after}")
        else:
            note("ok: a first run with no baseline records one and passes")

        # ── 2. unchanged survivor count -> passes, baseline untouched ───────
        report = os.path.join(work, "unchanged.txt")
        write_report(report, survived=10, mutants=100)
        baseline = os.path.join(work, "baseline2.json")
        write_baseline(baseline, {"core-forms": {"survived": 10, "mutants": 100}})
        buf = io.StringIO()
        rc = check(report, "core-forms", baseline_path=baseline, out=buf)
        if rc != 0:
            fail("an unchanged survivor count failed the gate", buf.getvalue())
        else:
            note("ok: an unchanged survivor count passes")

        # ── 3. regression: more survivors than the baseline -> fails ────────
        report = os.path.join(work, "regressed.txt")
        write_report(report, survived=11, mutants=100)
        baseline = os.path.join(work, "baseline3.json")
        write_baseline(baseline, {"core-forms": {"survived": 10, "mutants": 100}})
        buf = io.StringIO()
        rc = check(report, "core-forms", baseline_path=baseline, out=buf)
        with open(baseline, encoding="utf-8") as handle:
            after = json.load(handle)
        if rc != 1:
            fail("a regressed (higher) survivor count did not fail the gate", buf.getvalue())
        elif "regression" not in buf.getvalue():
            fail("a regression was not named as one in the output", buf.getvalue())
        elif after["baselines"]["core-forms"] != {"survived": 10, "mutants": 100}:
            fail(f"a failed (regressed) run must not move the baseline, but it did: {after}")
        else:
            note("ok: a regressed survivor count fails the gate and leaves the baseline untouched")

        # ── 4. improvement: fewer survivors -> passes, baseline ratchets down ─
        report = os.path.join(work, "improved.txt")
        write_report(report, survived=9, mutants=100)
        baseline = os.path.join(work, "baseline4.json")
        write_baseline(baseline, {"core-forms": {"survived": 10, "mutants": 100}})
        buf = io.StringIO()
        rc = check(report, "core-forms", baseline_path=baseline, out=buf)
        with open(baseline, encoding="utf-8") as handle:
            after = json.load(handle)
        if rc != 0:
            fail("an improved (lower) survivor count failed the gate", buf.getvalue())
        elif after["baselines"]["core-forms"] != {"survived": 9, "mutants": 100}:
            fail(f"an improved run did not ratchet the baseline down: {after}")
        else:
            note("ok: an improved survivor count passes and ratchets the baseline down")

        # ── 5. zero mutants -> always fails, even with no recorded baseline ──
        report = os.path.join(work, "empty.txt")
        write_report(report, survived=0, mutants=0)
        baseline = os.path.join(work, "baseline5.json")
        write_baseline(baseline, {})
        buf = io.StringIO()
        rc = check(report, "core-forms", baseline_path=baseline, out=buf)
        if rc != 1:
            fail("a zero-mutant report passed the gate instead of being refused (invariant 7 / V1)",
                 buf.getvalue())
        else:
            note("ok: a zero-mutant report always fails, never reads as a clean run")

        # ── 6. a report that is not Mull's IDE format at all -> fails cleanly ─
        report = os.path.join(work, "garbage.txt")
        with open(report, "w", encoding="utf-8") as handle:
            handle.write("not a mull report\n")
        baseline = os.path.join(work, "baseline6.json")
        write_baseline(baseline, {})
        buf = io.StringIO()
        rc = check(report, "core-forms", baseline_path=baseline, out=buf)
        if rc != 1:
            fail("an unparseable report did not fail the gate", buf.getvalue())
        else:
            note("ok: an unparseable report fails cleanly rather than crashing or passing")

        # ── 7. a scope switch (mutant count changed) records the new pair ────
        # A scope's mutant *population* can legitimately change between runs
        # (code under includePaths grew or shrank) independent of the
        # survivor count moving; the baseline must track both numbers
        # together, not just ratchet survived in isolation against a stale
        # mutants figure.
        report = os.path.join(work, "repopulated.txt")
        write_report(report, survived=10, mutants=120)
        baseline = os.path.join(work, "baseline7.json")
        write_baseline(baseline, {"core-forms": {"survived": 10, "mutants": 100}})
        buf = io.StringIO()
        rc = check(report, "core-forms", baseline_path=baseline, out=buf)
        with open(baseline, encoding="utf-8") as handle:
            after = json.load(handle)
        if rc != 0:
            fail("an unchanged survivor count with a grown mutant population failed the gate",
                 buf.getvalue())
        elif after["baselines"]["core-forms"] != {"survived": 10, "mutants": 120}:
            fail(f"the mutant-population change was not recorded: {after}")
        else:
            note("ok: a changed mutant population updates the baseline even when survived is unchanged")
    finally:
        import shutil
        shutil.rmtree(work, ignore_errors=True)

    if failures:
        print(f"\n{failures} self-test check(s) failed", file=sys.stderr)
        return 1
    print("\nall self-test checks passed")
    return 0


def main(argv):
    if "--self-test" in argv:
        return self_test()
    if len(argv) != 3:
        print(__doc__)
        return 2
    return check(argv[1], argv[2])


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
