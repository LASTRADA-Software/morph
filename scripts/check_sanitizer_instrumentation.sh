#!/usr/bin/env bash
# Usage:
#   bash scripts/check_sanitizer_instrumentation.sh <build-dir> <asan|tsan|ubsan>
#   bash scripts/check_sanitizer_instrumentation.sh --binary <file> <asan|tsan|ubsan>
#
# A sanitizer job whose binaries are not actually instrumented is worse than no
# job: it runs the whole suite, reports success, and every reader treats that as
# evidence the suite was checked (morph#542).
#
# CI already asserted this, but only for the ladder's own binaries and only for
# `__asan_`. Everything else was on trust, and the trust was misplaced: measured
# on a `clang-asan` configure, `morph_offline_sqlite_tests` (1 TU) and
# `morph_concepts_tests` (8 TUs) compiled with *zero* sanitizer flags while the
# job that builds them names the SQLite queue as "where the memory/threading/UB
# risk actually lives".
#
# This walks the binaries ctest will actually run, rather than a hand-kept list
# of target names -- a list is exactly what let two suites be added without
# anyone noticing they were never covered.
#
# The expected symbol is keyed on the mode. An `__asan_`-only assertion is
# vacuous on the ubsan and tsan legs, which is the same "control that measures
# nothing" this check exists to prevent.
#
# ── The two questions, and why --binary exists (morph#675) ───────────────────
#
# The sweep above answers "did this check examine a representative set?", and
# its floor (below) is what makes that answer mean something. A developer who
# has built *one* target under a sanitizer preset is asking a different
# question -- "is this one binary instrumented?" -- which is a yes/no about a
# single file and needs no floor at all. The sweep refused that case outright
# ("only examined 1 binaries ... too few for this check to mean anything"), so
# it was answered by hand with `nm | grep __tsan_` instead, which is how the
# per-mode symbol table and the SIGPIPE trap below get re-derived, wrongly,
# each time.
#
# `--binary <file> <mode>` answers the second question against the same symbol
# table, and skips the floor because there is no set to be representative of.
#
# It cannot be used to satisfy the first question. It refuses outright when
# GITHUB_ACTIONS is set in the environment, so no step of any workflow in this
# repository can reach it -- a CI invocation that tried would fail with exit 2
# rather than silently pass having examined one file. That is the whole safety
# of the addition: the narrow mode is unavailable exactly where the floor was
# meant to apply. (`env -u GITHUB_ACTIONS` defeats it, as it defeats any
# environment-keyed guard. That is deliberate circumvention, not the accident
# the guard is for.)
#
# scripts/test_check_sanitizer_instrumentation.sh drives both modes against
# fixtures whose right answers are known, including the refusal above and the
# floor the sweep must still enforce.
set -euo pipefail

usage="usage: check_sanitizer_instrumentation.sh <build-dir> <asan|tsan|ubsan>
   or: check_sanitizer_instrumentation.sh --binary <file> <asan|tsan|ubsan>   (not available under GITHUB_ACTIONS)"

narrow=0
if [ "${1:-}" = "--binary" ]; then
    narrow=1
    shift
fi

target="${1:?${usage}}"
mode="${2:?${usage}}"

case "${mode}" in
    asan)  symbol="__asan_" ;;
    tsan)  symbol="__tsan_" ;;
    ubsan) symbol="__ubsan_" ;;
    *) echo "::error::check_sanitizer_instrumentation: unknown mode '${mode}' (expected asan, tsan or ubsan)"; exit 2 ;;
esac

# `grep -c`, not `grep -q`: under `set -o pipefail`, a `grep -q` that finds its
# match exits at once, `nm` takes SIGPIPE, and the pipeline reports failure --
# which would mark every *instrumented* binary as uninstrumented. Caught by
# running this check against a known-good build before trusting it; a checker
# that inverts its own verdict is the worst kind. Both modes go through here so
# there is one copy of that reasoning rather than one per caller.
count_symbols() {
    nm -C "$1" 2>/dev/null | grep -c -- "${symbol}" || true
}

# ── Narrow mode: one named file, no floor, never in CI (morph#675) ───────────
#
# The refusal below is what keeps the floor intact. Everything after it is a
# statement about a single file, so there is no set for a floor to be about --
# but for the same reason it must never be reachable from a workflow step,
# where "examined 1 binary, all good" is exactly the vacuous pass the sweep's
# floor exists to prevent. GITHUB_ACTIONS is set by the runner for every step
# of every job, so the check below is unconditional there.
if [ "${narrow}" -eq 1 ]; then
    if [ -n "${GITHUB_ACTIONS:-}" ]; then
        echo "::error::check_sanitizer_instrumentation: --binary is a local, single-file mode and is refused under GITHUB_ACTIONS -- a CI leg must run the build-tree sweep, whose floor is the only thing that makes 'all instrumented' mean 'all of them were looked at'"
        exit 2
    fi

    # A narrow answer over a path that is not there, or is not a binary, would
    # be the vacuous pass in miniature: nothing examined, nothing reported.
    if [ ! -f "${target}" ]; then
        echo "::error::check_sanitizer_instrumentation: --binary ${target}: no such file -- nothing was examined"
        exit 1
    fi
    if ! file -b "${target}" 2>/dev/null | grep -qE 'ELF|Mach-O'; then
        echo "::error::check_sanitizer_instrumentation: --binary ${target}: not an ELF or Mach-O binary -- nm has nothing to report on it"
        exit 1
    fi

    narrow_base="$(basename "${target}")"
    narrow_count="$(count_symbols "${target}")"
    if [ "${narrow_count}" -eq 0 ]; then
        echo "::error file=${target}::check_sanitizer_instrumentation: ${narrow_base} carries no ${symbol} symbols -- it is not ${mode}-instrumented, so any result it printed proves nothing"
        exit 1
    fi

    echo "check_sanitizer_instrumentation: ${narrow_base} carries ${narrow_count} ${symbol} symbols -- ${mode}-instrumented."
    echo "check_sanitizer_instrumentation: this examined one file and applied no floor; it is not a substitute for the build-tree sweep a CI leg runs."
    exit 0
fi

build_dir="${target}"

# Binaries that are deliberately uninstrumented, with the reason. Keep this
# list short and justified: every entry is a hole in the check.
#
#   (none today -- morph_test_main is a static library, never a ctest command,
#   so its documented exemption at CMakeLists.txt:582 does not reach here.)
allowlist=()

# stderr goes to a file rather than /dev/null, and the exit status is kept.
#
# `2>/dev/null` made the two ways this list can come back empty
# indistinguishable: "ctest enumerated the tree and it registers no tests" and
# "ctest itself failed before printing any JSON". morph#690 was the second one,
# and it cost three CI runs and two local sessions to name, because the
# sentence that named it was being discarded one pipe away from the error
# message. What ctest actually wrote, reproduced locally against a
# DISCOVERY_MODE PRE_TEST suite whose binary aborts at listing time:
#
#     CMake Error at .../CatchAddTests.cmake:307 (message):
#       Error listing tests from executable '.../gui_tests':
#         Result: Subprocess aborted
#
# with an exit status of 8 and an entirely empty stdout -- one suite's failed
# discovery takes the whole listing down, not just its own entries. The stream
# still has to be kept off stdout (it is not JSON and jq would choke on it),
# so it is captured and printed only on the path that needs it.
ctest_stderr="$(mktemp)"
ctest_stdout="$(mktemp)"
trap 'rm -f "${ctest_stderr}" "${ctest_stdout}"' EXIT

ctest_status=0
ctest --test-dir "${build_dir}" --show-only=json-v1 \
    >"${ctest_stdout}" 2>"${ctest_stderr}" || ctest_status=$?

mapfile -t commands < <(
    jq -r '.tests[]?.command[0]? // empty' <"${ctest_stdout}" 2>/dev/null \
        | sort -u
)

if [ "${#commands[@]}" -eq 0 ]; then
    echo "::error::check_sanitizer_instrumentation: ctest listed no tests in ${build_dir} -- this check would pass having examined nothing"
    echo "check_sanitizer_instrumentation: \`ctest --show-only=json-v1\` exited ${ctest_status} and wrote $(wc -c <"${ctest_stdout}") bytes of stdout."
    if [ -s "${ctest_stderr}" ]; then
        echo "check_sanitizer_instrumentation: its stderr follows -- a non-empty stderr here means the listing *failed*, not that the tree registers no tests:"
        sed 's/^/    | /' "${ctest_stderr}"
    else
        echo "check_sanitizer_instrumentation: it wrote nothing to stderr, so this is a build tree that genuinely registers no tests rather than a listing that failed."
    fi
    exit 1
fi

checked=0
missing=0
skipped=0

build_dir_abs="$(cd "${build_dir}" && pwd)"

for binary in "${commands[@]}"; do
    # ctest commands include interpreters and shell helpers -- `/usr/bin/node`
    # drives the QML tests -- and a system interpreter is not something this
    # project builds or can instrument. Only binaries produced into the build
    # tree are in scope.
    [ -f "${binary}" ] || continue
    [ -x "${binary}" ] || continue
    case "${binary}" in
        "${build_dir_abs}"/*) ;;
        *) continue ;;
    esac
    if ! file -b "${binary}" 2>/dev/null | grep -qE 'ELF|Mach-O'; then
        continue
    fi

    base="$(basename "${binary}")"
    exempt=0
    for allowed in ${allowlist[@]+"${allowlist[@]}"}; do
        if [ "${base}" = "${allowed}" ]; then
            exempt=1
            break
        fi
    done
    if [ "${exempt}" -eq 1 ]; then
        skipped=$((skipped + 1))
        continue
    fi

    checked=$((checked + 1))
    symbol_count="$(count_symbols "${binary}")"
    if [ "${symbol_count}" -eq 0 ]; then
        echo "::error file=${binary}::check_sanitizer_instrumentation: ${base} is run by ctest on the ${mode} leg but carries no ${symbol} symbols -- it is not instrumented, so running it proves nothing"
        missing=$((missing + 1))
    fi
done

# A floor, for the same reason wire.md's table check has per-category floors: a
# check that parsed nothing must fail rather than pass quietly. Every sanitizer
# preset builds at least the main suite plus the net suite.
if [ "${checked}" -lt 2 ]; then
    echo "::error::check_sanitizer_instrumentation: only examined ${checked} binaries in ${build_dir} -- too few for this check to mean anything"
    exit 1
fi

if [ "${missing}" -gt 0 ]; then
    echo "::error::check_sanitizer_instrumentation: ${missing} of ${checked} ctest binaries are not ${mode}-instrumented"
    exit 1
fi

echo "check_sanitizer_instrumentation: ${checked} ctest binaries all carry ${symbol} symbols (${skipped} allowlisted)."
