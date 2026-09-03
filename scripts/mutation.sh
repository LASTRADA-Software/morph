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
# Mull ships prebuilt packages for LLVM 13 through 22, so both the CI pin
# (clang 20, .github/workflows/ci.yml) and this workstation (clang 22) are
# covered -- but by *different packages*. Measured here on
# `mull-runner-22 0.34.0`, LLVM 22.1.2, against clang 22.1.8. Nothing about
# these numbers has been observed on clang 20; whoever moves this into CI
# should install the -20 package and re-measure rather than assume the score
# carries over, because the mutant set is generated from LLVM IR and two clang
# majors do not emit the same IR.
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
# instead of the ~33 the arithmetic predicts. Capping it at 60s (2.7x baseline)
# recovers most of that. A mutant that outlives the cap is scored killed, which
# is the standard reading and the right one here: nothing in morph_tests is
# legitimately three times slower than the baseline, so an over-cap mutant has
# changed the program's termination behaviour.
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
    echo "mutators:"
    echo "  - cxx_all"
    echo "includePaths:"
    for _p in "${paths[@]}"; do echo "  - ${_p}"; done
    # _deps is fetched third-party code and tests/ is the suite doing the
    # asserting; mutating either measures something other than include/morph.
    echo "excludePaths:"
    echo "  - .*_deps.*"
    echo "  - .*/tests/.*"
    # Per-mutant timeout. morph_tests' own wall time is ~22s and several of its
    # cases wait on real timeouts, so a mutant that merely makes the suite slow
    # must not be scored as killed-by-timeout.
    echo "timeout: 60000"
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

# IDE only. Mull's SQLite reporter aborts on this project --
# "Failed to write SQLite report: string or blob too big" -- and Mull treats a
# reporter error as fatal, so it exits *after* the 46-minute run and *before*
# printing the score. The mutants are large: cxx_remove_void_call quotes the
# whole call it deletes, and remote.hpp's dispatch lambdas run to dozens of
# lines. The IDE reporter writes the same information as text and does not.
MULL_CONFIG="${PWD}/${build_dir}/mull.yml" "$runner" \
    --workers "${MULL_WORKERS:-$(nproc)}" \
    --timeout "${MULL_TIMEOUT_MS:-60000}" \
    --reporters IDE \
    --report-dir "${build_dir}" \
    --report-name "mutation-${scope}" \
    "${build_dir}/${binary}"

# Printed here rather than left to Mull. mull-runner emits its own
# "Mutation score: N%" line for some reporter selections and not for others --
# with --reporters IDE it prints only "Surviving mutants: N" -- and a script
# whose headline number the reader has to reconstruct is not the "documented
# command producing a mutation score" morph#405 asks for. The IDE report's first
# line carries both halves, so the score is derived from the artefact rather
# than from stdout.
readonly report="${build_dir}/mutation-${scope}.txt"
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
