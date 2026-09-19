#!/usr/bin/env bash
# Usage: bash scripts/check_sanitizer_can_fail.sh
#
# A sanitizer leg that cannot fail is worse than no sanitizer leg: it reports
# success over a build riddled with undefined behaviour, and every reader
# treats that green tick as evidence (morph#541).
#
# Plain `-fsanitize=undefined` *recovers* by default -- it prints the
# diagnostic and carries on -- so a job that judges the run by its exit status
# learns nothing. Measured before this check existed: a translation unit that
# constructs `Rational{INT64_MIN, DecimalPlaces{2}}` printed three
# `runtime error: negation of -9223372036854775808` lines and exited 0 on the
# `clang-ubsan` leg, which is why morph#537's defect survived a sanitizer job
# that was green the whole time.
#
# This drives `apply_sanitizers()` from cmake/compiler_options.cmake rather
# than grepping that file for a flag: what has to hold is behavioural -- a
# program with real undefined behaviour, compiled exactly the way a sanitizer
# leg compiles it, must make the process exit non-zero. A check that only
# looked for the flag would keep passing if the flag stopped reaching the
# compile line, which is the failure mode morph#298 already produced once.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

# Signed overflow: undefined, caught by UBSan's default check set, and not
# something the optimiser can turn into a crash on its own.
cat > "${work}/ub.cpp" <<'CPP'
#include <cstdio>

int main() {
    volatile int value = 2147483647;
    value = value + 1;  // signed integer overflow -- undefined behaviour
    std::puts("still alive: the sanitizer recovered and this leg would report success");
    return 0;
}
CPP

cat > "${work}/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.25)
project(morph_sanitizer_can_fail CXX)
include("${MORPH_COMPILER_OPTIONS}")
add_executable(ub ub.cpp)
apply_sanitizers(ub "${MORPH_SANITIZER_MODE}")
CMAKE

status=0

# `tsan` is not covered: ThreadSanitizer carries no UB checks, so there is
# nothing for this program to trip. `asan` is, because its arm is
# `-fsanitize=address,undefined` and therefore carries recovering UBSan too --
# the leg most likely to be assumed safe because "it has a sanitizer".
for mode in ubsan asan; do
    build="${work}/build-${mode}"
    if ! cmake -S "${work}" -B "${build}" \
            -DCMAKE_BUILD_TYPE=Debug \
            -DMORPH_COMPILER_OPTIONS="${repo_root}/cmake/compiler_options.cmake" \
            -DMORPH_SANITIZER_MODE="${mode}" > "${build}.configure.log" 2>&1; then
        echo "::error::check_sanitizer_can_fail: configuring the ${mode} probe failed -- see ${build}.configure.log"
        cat "${build}.configure.log"
        status=1
        continue
    fi
    if ! cmake --build "${build}" > "${build}.build.log" 2>&1; then
        echo "::error::check_sanitizer_can_fail: building the ${mode} probe failed"
        cat "${build}.build.log"
        status=1
        continue
    fi

    set +e
    output="$("${build}/ub" 2>&1)"
    exit_code=$?
    set -e

    if ! grep -q "runtime error" <<< "${output}"; then
        echo "::error::check_sanitizer_can_fail: the ${mode} probe did not report the undefined behaviour at all -- the sanitizer is not reaching the compile line"
        echo "${output}"
        status=1
    elif [ "${exit_code}" -eq 0 ]; then
        echo "::error::check_sanitizer_can_fail: the ${mode} probe detected undefined behaviour and still exited 0, so the ${mode} leg cannot fail on it"
        echo "${output}"
        status=1
    else
        echo "check_sanitizer_can_fail: ${mode} reports undefined behaviour and exits ${exit_code}."
    fi
done

# ── An unrecognised mode must fail the configure, not the sanitizing ─────────
#
# apply_sanitizers() used to be `if asan / elseif tsan / elseif ubsan / endif`
# with no else arm, so `-DAF_SANITIZER=msan`, `=ASAN` or a typo produced a
# fully uninstrumented build that configured, compiled and ran the whole suite
# green (morph#541). That is the same "a leg that cannot fail" defect as a
# recovering UBSan, arriving through a different door: the job asks for a
# sanitizer, gets none, and reports success.
#
# Checked here rather than by reading the CMake, for this script's own stated
# reason: what has to hold is behavioural.
bad_build="${work}/build-badmode"
if cmake -S "${work}" -B "${bad_build}" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DMORPH_COMPILER_OPTIONS="${repo_root}/cmake/compiler_options.cmake" \
        -DMORPH_SANITIZER_MODE="msan" > "${bad_build}.configure.log" 2>&1; then
    echo "::error::check_sanitizer_can_fail: apply_sanitizers() accepted the unknown mode 'msan' and configured anyway -- that build would run the whole suite with no instrumentation at all and report success"
    status=1
else
    if grep -q "unknown sanitizer mode" "${bad_build}.configure.log"; then
        echo "check_sanitizer_can_fail: an unknown sanitizer mode fails the configure."
    else
        echo "::error::check_sanitizer_can_fail: the unknown-mode probe failed to configure, but not with apply_sanitizers()'s own diagnostic -- see ${bad_build}.configure.log"
        cat "${bad_build}.configure.log"
        status=1
    fi
fi

exit "${status}"
