#!/usr/bin/env bash
# Usage: bash scripts/mutation.sh [SCOPE]
#
# Mutation-tests include/morph against the framework's own Catch2 suite and
# prints a mutation score. SCOPE is one of `core-forms` (default), `core`,
# `forms` or `net`; it selects both which headers get instrumented and which
# mutants are run.
#
# Requires MULL_PREFIX to point at an unpacked Mull install (see "Installing
# the tool" below). Everything else -- the build directory, the Mull config,
# the instrumented binary -- this script creates.
#
# ── What this answers that scripts/coverage.sh cannot ────────────────────────
#
# Coverage answers "did a test run this line". It cannot answer "would a test
# have noticed if this line were wrong", and the two come apart exactly where a
# test drives code without asserting on the result. include/morph sits at
# 95.69% lines, and a high line number is compatible with a suite that calls
# everything and checks little -- codecov.yml records two occasions where that
# turned out to be what was happening (rule_model.cpp's 60.76%, crm's
# attachActionLog pair). Those were found because the code was never
# *executed*. A path that is executed and not asserted is invisible to every
# measurement this repository took before this script existed.
#
# Mutation testing asks the second question directly: change the program, run
# the suite, and require it to fail. A mutant the suite still passes on is a
# place where the code could be wrong and no test would say so.
#
# ── The tool, and the version constraint that pins it ────────────────────────
#
# Mull 0.34.0 (https://mull-project.com). Two halves, and they are not
# interchangeable:
#
#   * `mull-ir-frontend-<N>` is an **LLVM pass plugin**, loaded into clang at
#     compile time with -fpass-plugin. A pass plugin is linked against LLVM's
#     C++ ABI and is only loadable by the clang whose major version it was
#     built for. <N> must equal the clang major doing the compiling, exactly.
#   * `mull-runner-<N>` re-runs the instrumented binary once per mutant.
#
# Mull ships prebuilt packages for LLVM 13 through 22. CI pins clang 22
# (.github/workflows/ci.yml's CLANG_VERSION, kept honest by
# scripts/check_ci_clang_pin.sh) and this workstation runs clang 22.1.8, so
# both are served by the same -22 package and the numbers below were taken on
# the compiler CI itself uses. That removes the obstacle this comment used to
# name: the mutant set is generated from LLVM IR and two clang majors do not
# emit the same IR, but there is no major difference here to carry the score
# across. Measured on `mull-runner-22 0.34.0`, LLVM 22.1.2, against clang
# 22.1.8. What still keeps a campaign out of CI is its cost -- 46+ minutes for
# core and forms alone -- and that Mull is on no runner image; see
# docs/spec/testing_charter.md and morph#408, not a toolchain mismatch.
#
# Installing the tool (not automated on purpose -- this is a local
# investigation tool, and a script that downloads a toolchain behind your back
# is worse than one line of documentation):
#
#     v=0.34.0; n=22
#     curl -sSLO https://github.com/mull-project/mull/releases/download/${v}/mull-${n}_${v}_amd64.deb
#     dpkg-deb -x mull-${n}_${v}_amd64.deb "$PWD/mull-${n}"
#     export MULL_PREFIX="$PWD/mull-${n}/usr"
#
# ── The step that is easy to get wrong ───────────────────────────────────────
#
# Mull's config file is read **twice**: by the IR frontend at compile time, to
# decide which code to embed mutants into, and by the runner at run time, to
# decide which of the embedded mutants to execute. `includePaths` therefore has
# to be right *before the build*, not just before the run.
#
# Getting that wrong does not produce an error. A binary compiled with one
# scope and run under a wider one reports:
#
#     [info] No mutants found. Mutation score: infinitely high
#
# -- a clean exit, a plausible-looking log, and a score of no meaning. That is
# why this script owns the config file and writes it before configuring, rather
# than leaving it to the caller: the two scopes cannot disagree if only one of
# them exists.
#
# ── The first score, and where the survivors went ────────────────────────────
#
# 2026-09-03, scope core-forms, Mull 0.34.0 / LLVM 22.1.2 on clang 22.1.8:
# **999 mutants, 640 killed, 359 survived -- a mutation score of 64%** against a
# suite of 1279 passing Catch2 cases over an include/morph that measures 95.69%
# lines and 91.19% branches. That gap between 95.69% and 64% is the whole reason
# morph#405 exists, and the number is recorded here rather than defended.
#
# Of the 359: 11 are equivalent mutants (hash-combine arithmetic and reserve()
# capacity hints, listed with a reason each), 27 are diagnostic side channels
# nothing asserts on (19 logging, 8 metrics), and 321 are code morph_tests runs
# and would not notice being wrong. scripts/mutation_survivors.json carries the
# triage; morph#408 owns what to do about it. One of them --
# wire.hpp's kMaxEnvelopeBytes cap, whose neighbouring test was already named
# "at the size limit boundary" while asserting a 1 KiB envelope -- is now a test
# that fails against the mutant and passes without it, which is what proves this
# loop closes rather than merely reports.
#
# Both of those runs -- and every number quoted above -- were taken with
# `mutators: cxx_all`, which this script no longer uses. `cxx_all` includes
# `cxx_remove_void_call`, which Mull 0.34.0 silently fails to apply at
# member/operator call sites (morph#434, mechanism confirmed in morph#448), so
# it reported 148 of those 352 survivors over code it never actually changed.
# The mutator is excluded below. The scores above are therefore the last
# *measured* ones and are kept as the historical record; they are not
# comparable to a run of the reduced set, and no score for the reduced set has
# been measured. Re-run before quoting a current number -- ~46 minutes.
#
# ── Cost, and the two knobs that decide it ───────────────────────────────────
#
# The suite is re-run once per mutant. morph_tests takes ~22s wall (it is mostly
# waiting on timeouts, not computing -- 0.5s of that is user time), so the run
# is bounded by mutants/workers * 22s. The instrumented build is -O0 -g plus the
# plugin over 105 translation units and takes ~30 minutes on 12 cores. Both are
# why this is a local script with a scope argument rather than a CI job;
# morph#408 owns the question of what, if anything, belongs in CI.
#
# --timeout is passed explicitly and matters more than it looks. Mull's default
# is max(baseline * 10, --minimum-timeout), which against a 22s baseline is 220s
# -- so a mutant that makes the suite hang holds a worker for nearly four
# minutes, and measured throughput was 6.7 mutants/minute on twelve workers
# instead of the ~33 the arithmetic predicts. Capping it at 2.7x the baseline
# recovers most of that. A mutant that outlives the cap is scored killed, which
# is the standard reading and the right one here: nothing in morph_tests is
# legitimately three times slower than the baseline, so an over-cap mutant has
# changed the program's termination behaviour.
#
# ── Why the cap is measured here and not written down (morph#732) ────────────
#
# That 2.7x used to be a constant -- 60000 ms, arithmetic done once against the
# 22s this workstation takes -- and a constant derived on one machine and
# enforced on another is a threshold calibrated where the feature is cheap. The
# same timeout bounds mull's **warm-up** run of the *unmutated* suite, so when
# the baseline goes over the cap mull does not slow down, it aborts:
#
#     [info] Warm up run (threads: 1)
#            [################################] 1/1. Finished in 1m0.0s
#     [error] Original test failed (warmup run)
#     status: Timedout
#
# -- run 35592789912, 2026-09-21, ubuntu-26.04. No mutation score was produced
# for two weeks, and no one saw it because the workflow's own reporting was
# broken twice over (morph#730, morph#731).
#
# The numbers, from the campaign's own logs rather than from an estimate:
#
#   | run          | date       | warm-up (1 thread) | mutant phase   |
#   |--------------|------------|--------------------|----------------|
#   | 34349442137  | 2026-09-09 | completed          | completed      |
#   | 34836153375  | 2026-09-14 | **24.21s**         | 121m13.7s /784 |
#   | 35592789912  | 2026-09-21 | **> 60s** (killed) | never started  |
#
# So the hosted runner is not systematically slower than this workstation --
# 24.21s against 22s is ~10%. What it is, is *variable*: the same job's
# instrumented build took 14 minutes on 09-14 and 29 minutes on 09-21, and the
# baseline moved with it, from 2.5x under the cap to over it. A fixed cap with
# no margin for a 2x machine is the defect; a second, larger constant would only
# move the cliff.
#
# So the baseline is measured on whatever machine is actually running, once,
# before mull is invoked, and the cap is 2.7x *that*. On this workstation that
# reproduces the old 60000 ms almost exactly (22s * 2.7 = 59.4s), which is why
# MULL_MIN_TIMEOUT_MS floors it at 60000: the ratio is what was measured, and
# the floor keeps a fast machine from deriving a cap too tight for a mutant that
# is merely slow.
#
# The measurement costs one extra run of the suite -- ~24s against a campaign of
# two hours -- and pays for itself twice over: it is also the only place the
# unmutated suite's own output is ever seen. Mull runs it too, and reports a
# failure as `Original test failed (warmup run)` with `stdout: ''`, `stderr: ''`
# -- a verdict with the evidence stripped out. Running it here first means a
# suite that does not pass says so in its own words, before two hours of mutants
# are scored against it.
#
# One caveat on the number, in the direction of *over*-counting kills.
# tests/test_outbox.cpp names its scratch file after the address of a
# stack object, which is stable across processes, so twelve concurrent copies of
# morph_tests can collide on it. A collision fails a case, and a failed case
# reads to Mull as a killed mutant. It cannot manufacture a *survivor*, so the
# survivor list is if anything conservative -- but a survivor that is being
# triaged should be re-confirmed by running the suite against that one mutant
# rather than trusted from the report alone.
set -euo pipefail

scope="${1:-core-forms}"
readonly scope

: "${MULL_PREFIX:?set MULL_PREFIX to an unpacked Mull install (see the header of this script)}"
readonly frontend="${MULL_PREFIX}/lib/mull-ir-frontend-${MULL_LLVM_MAJOR:-22}"
readonly runner="${MULL_PREFIX}/bin/mull-runner-${MULL_LLVM_MAJOR:-22}"

for _tool in "$frontend" "$runner"; do
    if [ ! -e "$_tool" ]; then
        echo "scripts/mutation.sh: not found: $_tool" >&2
        echo "  MULL_PREFIX=${MULL_PREFIX} MULL_LLVM_MAJOR=${MULL_LLVM_MAJOR:-22}" >&2
        exit 1
    fi
done

# The pass plugin has to be loadable by *this* clang. Checking the majors match
# up front turns an unreadable pile of linker errors 30 minutes into the build
# into one line before it starts.
readonly clang_major="$(${CXX:-clang++} -dumpversion | cut -d. -f1)"
if [ "$clang_major" != "${MULL_LLVM_MAJOR:-22}" ]; then
    echo "scripts/mutation.sh: mull-ir-frontend-${MULL_LLVM_MAJOR:-22} is an LLVM pass plugin and" >&2
    echo "  is only loadable by clang ${MULL_LLVM_MAJOR:-22}; ${CXX:-clang++} is clang ${clang_major}." >&2
    echo "  Install the matching Mull package, or set MULL_LLVM_MAJOR=${clang_major}." >&2
    exit 1
fi

# Scope selects three things at once, and they have to agree: which headers the
# IR frontend instruments, which mutants the runner executes, and which binary
# it re-runs. morph_tests is the framework's own suite and is what morph#405
# asks the score to be driven by; it compiles nothing under include/morph/net,
# whose code lives behind MORPH_BUILD_NET in a suite of its own -- so a `net`
# scope pointed at morph_tests would instrument nothing and report an
# "infinitely high" score over an empty mutant set.
extra_cmake_args=()
case "$scope" in
    core-forms)
        paths=('.*include/morph/core/.*' '.*include/morph/forms/.*')
        target=morph_tests; binary=tests/morph_tests ;;
    core)
        paths=('.*include/morph/core/.*')
        target=morph_tests; binary=tests/morph_tests ;;
    forms)
        paths=('.*include/morph/forms/.*')
        target=morph_tests; binary=tests/morph_tests ;;
    net)
        paths=('.*include/morph/net/.*')
        target=morph_net_tests; binary=tests/net/morph_net_tests
        extra_cmake_args=(-DMORPH_BUILD_NET=ON) ;;
    *) echo "scripts/mutation.sh: unknown scope '$scope' (core-forms|core|forms|net)" >&2; exit 1 ;;
esac
readonly target binary

readonly build_dir="build/mutation-${scope}"
mkdir -p "$build_dir"

# Written before configure, and read by both halves of Mull. See "The step that
# is easy to get wrong" above. The frontend picks it up as ./mull.yml relative
# to the compiler's working directory, which under Ninja is the build tree.
{
    # Deliberately NOT `cxx_all`. This is exactly Mull 0.34.0's documented
    # expansion of `cxx_all` --
    #
    #   cxx_assignment, cxx_increment, cxx_decrement, cxx_arithmetic,
    #   cxx_comparison, cxx_boundary, cxx_bitwise, cxx_calls
    #
    # (docs/generated/Mutators.rst at tag 0.34.0) -- with the two-member
    # `cxx_calls` group spelled out as just its other half,
    # `cxx_replace_scalar_call`, so that `cxx_remove_void_call` is dropped and
    # nothing else changes.
    #
    # Why: `cxx_remove_void_call` is broken for member/operator call sites in
    # this version (morph#434, mechanism confirmed in morph#448). Its IR pass
    # silently does not remove the call it claims to -- the mutant's machine
    # code disassembles byte-identical to its `_original` twin, proven in an
    # isolated single-TU build where linker deduplication cannot be the
    # explanation, and proven scoped rather than total by the same shape of
    # free-function call mutating and being killed correctly. A mutant that is
    # not a mutant is passed by the suite, and reported as a *survivor* over
    # code that was never changed.
    #
    # It accounted for 148 of the 352 survivors in
    # scripts/mutation_survivors.json's runs[1] -- ~42% of a list read as
    # "places the suite would not notice being wrong". Keeping the mutator
    # enabled does not merely waste run time; it manufactures findings.
    #
    # Do not re-add it without first re-checking the upstream defect (no
    # matching report was found on mull-project/mull as of 2026-09-04, so a
    # newer Mull will not have fixed it silently): compile one TU containing a
    # void-returning member call, and disassemble the mutant against its
    # `_original`. If they are byte-identical, the mutator is still broken.
    echo "mutators:"
    echo "  - cxx_assignment"
    echo "  - cxx_increment"
    echo "  - cxx_decrement"
    echo "  - cxx_arithmetic"
    echo "  - cxx_comparison"
    echo "  - cxx_boundary"
    echo "  - cxx_bitwise"
    echo "  - cxx_replace_scalar_call"
    echo "includePaths:"
    for _p in "${paths[@]}"; do echo "  - ${_p}"; done
    # _deps is fetched third-party code and tests/ is the suite doing the
    # asserting; mutating either measures something other than include/morph.
    echo "excludePaths:"
    echo "  - .*_deps.*"
    echo "  - .*/tests/.*"
    # No `timeout:` here. It is appended below, once the baseline it is derived
    # from has been measured on this machine -- see "Why the cap is measured
    # here and not written down". The frontend has read this file by then and
    # does not consume `timeout` at all; only the runner does, and the runner
    # has not started.
} > "${build_dir}/mull.yml"

# USE_COMPILER_CACHE=OFF: the objects carry mutation metadata keyed to this
# build, and a cache serving one across trees is the same hazard morph#426
# recorded for coverage objects. AF_COVERAGE is off -- llvm-cov instrumentation
# and Mull's IR pass measure different questions and there is no reason to pay
# for both.
cmake -S . -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
    -DMORPH_BUILD_TESTS=ON \
    -DUSE_COMPILER_CACHE=OFF \
    "${extra_cmake_args[@]}" \
    -DCMAKE_CXX_FLAGS="-fpass-plugin=${frontend} -g -O0 -grecord-command-line"

cmake --build "$build_dir" --target "$target"

# A binary with no .mull_mutants section is the silent failure described above:
# the runner would exit 0 and report an "infinitely high" score over nothing.
if ! readelf -SW "${build_dir}/${binary}" | grep -q '\.mull_mutants'; then
    echo "scripts/mutation.sh: ${build_dir}/${binary} carries no .mull_mutants section." >&2
    echo "  The IR frontend did not instrument anything -- check that" >&2
    echo "  ${build_dir}/mull.yml's includePaths match code this binary actually compiles." >&2
    exit 1
fi

# ── The baseline, measured on the machine that is about to run the campaign ──
#
# One unmutated run of the suite, single-threaded, exactly as mull's warm-up
# will run it. See "Why the cap is measured here and not written down" for what
# this replaces and the three runs that made it necessary (morph#732).
readonly baseline_log="${build_dir}/baseline-${scope}.log"
echo "scripts/mutation.sh: measuring the unmutated suite (mull runs it once, single-threaded, before any mutant)..."
baseline_start=${SECONDS}
set +e
"${build_dir}/${binary}" > "$baseline_log" 2>&1
baseline_status=$?
set -e
readonly baseline_seconds=$(( SECONDS - baseline_start ))
readonly baseline_status

if [ "$baseline_status" -ne 0 ]; then
    echo "scripts/mutation.sh: the unmutated suite exited ${baseline_status} after ${baseline_seconds}s." >&2
    echo "  Every mutant is scored against this run, so a failing baseline makes the" >&2
    echo "  whole campaign meaningless. Mull reports this as 'Original test failed" >&2
    echo "  (warmup run)' with the suite's own output stripped out; here it is:" >&2
    tail -n 30 "$baseline_log" >&2
    exit 1
fi

# 2.7x, floored. The ratio is what was measured on a 12-core workstation (22s
# baseline, 60s cap); the floor keeps a fast machine from deriving a cap too
# tight for a mutant that is merely slow, and reproduces the old constant
# exactly on the machine that constant came from.
readonly timeout_floor_ms="${MULL_MIN_TIMEOUT_MS:-60000}"
derived_timeout_ms=$(( baseline_seconds * 2700 ))
if [ "$derived_timeout_ms" -lt "$timeout_floor_ms" ]; then
    derived_timeout_ms="$timeout_floor_ms"
fi
readonly timeout_ms="${MULL_TIMEOUT_MS:-$derived_timeout_ms}"
echo "scripts/mutation.sh: unmutated ${target} baseline: ${baseline_seconds}s on $(nproc) cores."
echo "  per-mutant timeout: ${timeout_ms} ms (2.7x baseline, floored at ${timeout_floor_ms} ms${MULL_TIMEOUT_MS:+, overridden by MULL_TIMEOUT_MS})."

# Appended now rather than written with the rest of the config: the frontend
# read mull.yml at compile time and does not consume `timeout`, and the runner
# -- which does -- has not started. Both halves therefore see one number, which
# is the property the "step that is easy to get wrong" section is about.
echo "timeout: ${timeout_ms}" >> "${build_dir}/mull.yml"

# IDE only. Mull's SQLite reporter aborts on this project --
# "Failed to write SQLite report: string or blob too big" -- and Mull treats a
# reporter error as fatal, so it exits *after* the 46-minute run and *before*
# printing the score. The mutants are large: cxx_remove_void_call quotes the
# whole call it deletes, and remote.hpp's dispatch lambdas run to dozens of
# lines. The IDE reporter writes the same information as text and does not.
# `set +e` around the runner, and it is load-bearing rather than sloppy:
# mull-runner exits non-zero whenever mutants survive, which is the normal
# outcome and the one this script exists to report. Under `set -e` that ended the
# script immediately after a 46-minute run and before the score was printed --
# with the log ending on mull's own "Surviving mutants: N" line, which reads
# exactly like a successful finish.
set +e
MULL_CONFIG="${PWD}/${build_dir}/mull.yml" "$runner" \
    --workers "${MULL_WORKERS:-$(nproc)}" \
    --timeout "${timeout_ms}" \
    --reporters IDE \
    --report-dir "${build_dir}" \
    --report-name "mutation-${scope}" \
    "${build_dir}/${binary}"
runner_status=$?
set -e
readonly runner_status

# Printed here rather than left to Mull. mull-runner emits its own
# "Mutation score: N%" line for some reporter selections and not for others --
# with --reporters IDE it prints only "Surviving mutants: N" -- and a script
# whose headline number the reader has to reconstruct is not the "documented
# command producing a mutation score" morph#405 asks for. The IDE report's first
# line carries both halves, so the score is derived from the artefact rather
# than from stdout.
readonly report="${build_dir}/mutation-${scope}.txt"
# The report is the artefact, so its absence is the real failure -- and it is
# also how a genuine runner error (a missing library, a crashed baseline) is told
# apart from the ordinary non-zero of "some mutants survived".
if [ ! -s "$report" ]; then
    echo "scripts/mutation.sh: mull-runner exited ${runner_status} and wrote no" >&2
    echo "  report at ${report}." >&2
    exit 1
fi
python3 - "$report" "$scope" <<'PYTHON'
import re, sys

report, scope = sys.argv[1], sys.argv[2]
with open(report, encoding="utf-8", errors="replace") as handle:
    header = handle.readline()

match = re.search(r"Survived mutants \((\d+)/(\d+)\)", header)
if not match:
    print(f"scripts/mutation.sh: {report} does not start with Mull's survivor "
          f"header, so no score can be read from it.", file=sys.stderr)
    raise SystemExit(1)

survived, total = int(match.group(1)), int(match.group(2))
killed = total - survived
print()
print(f"mutation score ({scope}): {100.0 * killed / total:.2f}%  "
      f"({killed} killed, {survived} survived, {total} mutants)")
print(f"survivors: {report}")
print("triage:    scripts/mutation_survivors.json")
PYTHON
