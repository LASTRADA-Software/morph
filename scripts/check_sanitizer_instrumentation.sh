#!/usr/bin/env bash
# Usage: bash scripts/check_sanitizer_instrumentation.sh <build-dir> <asan|tsan|ubsan>
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
set -euo pipefail

build_dir="${1:?usage: check_sanitizer_instrumentation.sh <build-dir> <asan|tsan|ubsan>}"
mode="${2:?usage: check_sanitizer_instrumentation.sh <build-dir> <asan|tsan|ubsan>}"

case "${mode}" in
    asan)  symbol="__asan_" ;;
    tsan)  symbol="__tsan_" ;;
    ubsan) symbol="__ubsan_" ;;
    *) echo "::error::check_sanitizer_instrumentation: unknown mode '${mode}' (expected asan, tsan or ubsan)"; exit 2 ;;
esac

# Binaries that are deliberately uninstrumented, with the reason. Keep this
# list short and justified: every entry is a hole in the check.
#
#   (none today -- morph_test_main is a static library, never a ctest command,
#   so its documented exemption at CMakeLists.txt:582 does not reach here.)
allowlist=()

mapfile -t commands < <(
    ctest --test-dir "${build_dir}" --show-only=json-v1 2>/dev/null \
        | jq -r '.tests[]?.command[0]? // empty' \
        | sort -u
)

if [ "${#commands[@]}" -eq 0 ]; then
    echo "::error::check_sanitizer_instrumentation: ctest listed no tests in ${build_dir} -- this check would pass having examined nothing"
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
    # `grep -c`, not `grep -q`: under `set -o pipefail`, a `grep -q` that finds
    # its match exits at once, `nm` takes SIGPIPE, and the pipeline reports
    # failure -- which would mark every *instrumented* binary as uninstrumented.
    # Caught by running this check against a known-good build before trusting
    # it; a checker that inverts its own verdict is the worst kind.
    symbol_count="$(nm -C "${binary}" 2>/dev/null | grep -c -- "${symbol}" || true)"
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
