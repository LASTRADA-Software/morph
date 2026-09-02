#!/usr/bin/env bash
# Usage: bash scripts/test_check_coverage_objects.sh
#
# Self-test for scripts/check_coverage_objects.sh, the gate that fails when
# ctest runs a binary llvm-cov is never handed (morph#403). A lint gate that is
# never itself tested reports green whether or not it still detects anything --
# and this one is especially exposed to that, because the defect it guards
# against is *already* a silence: an unprofiled suite makes the coverage run
# succeed, the report upload, and the figure come out over a smaller
# denominator with nothing to say so. A gate that had gone blind would look
# exactly like a gate that found nothing wrong.
#
# The gate needs no build to test: it compares a manifest file against
# `ctest --show-only=json-v1` output, and both can be written by hand. The
# fixtures below are throwaway executables in a temp directory plus a JSON
# document naming them, which is also what lets this run in drift-guard.yml
# next to the other checkers' self-tests rather than behind a 20-minute build.
#
# Asserts six directions:
#
#   1. every ctest binary profiled                     -> pass
#   2. an unprofiled binary with no stated reason      -> fail, naming it
#   3. an unprofiled binary that IS excluded by name   -> pass, printing why
#   4. a missing/empty manifest                        -> fail
#   5. a manifest naming a binary that was not built   -> fail
#   6. a wrapper-driven test (subject is an argument)  -> fail on the subject
#
# 3 and 5 matter as much as 2. Without 3 the gate could be "fixed" by making
# every exclusion fatal, which would simply move the pressure onto deleting the
# check; without 5 a manifest could name a target the build silently dropped
# and the report would be computed over the survivors -- the same shrinking
# denominator by another route.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="${repo_root}/scripts/check_coverage_objects.sh"

failures=0

note() { printf '%s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# ── Fixture construction ─────────────────────────────────────────────────────
# `case_dir <name>` makes a fresh build-dir-shaped directory. `binary` makes an
# executable file in it; `manifest`/`ctest_json` write the two inputs.
case_dir() {
    local dir="${work}/$1"
    rm -rf "$dir"
    mkdir -p "$dir"
    printf '%s' "$dir"
}

binary() {
    local dir="$1" name="$2"
    printf '#!/bin/sh\nexit 0\n' > "${dir}/${name}"
    chmod +x "${dir}/${name}"
    printf '%s' "${dir}/${name}"
}

ctest_json() {
    local dir="$1"
    shift
    {
        printf '{"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":['
        local first=1 path
        for path in "$@"; do
            [ "$first" -eq 1 ] || printf ','
            first=0
            printf '{"name":"%s","command":["%s","--some-case"]}' "$(basename "$path")" "$path"
        done
        printf ']}'
    } > "${dir}/ctest.json"
    printf '%s' "${dir}/ctest.json"
}

# ── 1. every ctest binary profiled -> pass ───────────────────────────────────
dir="$(case_dir all_profiled)"
tests_exe="$(binary "$dir" morph_tests)"
net_exe="$(binary "$dir" morph_net_tests)"
printf '%s\n%s\n' "$tests_exe" "$net_exe" > "${dir}/coverage_objects.txt"
json="$(ctest_json "$dir" "$tests_exe" "$net_exe")"

if output="$(bash "$checker" "$dir" "$json" 2>&1)"; then
    note "ok: a fully profiled suite is accepted"
else
    fail "a fully profiled suite was rejected:"
    printf '%s\n' "$output" >&2
fi

# ── 2. an unprofiled, unexplained binary -> fail, naming it ──────────────────
# This is morph#403 itself, in miniature: morph_net_tests runs under ctest and
# is absent from the object list. A nonzero exit alone is not enough -- the
# gate has other failure paths, and one of them firing for an unrelated reason
# would look like a pass of this case -- so the message must name the binary.
dir="$(case_dir unprofiled)"
tests_exe="$(binary "$dir" morph_tests)"
net_exe="$(binary "$dir" morph_net_tests)"
printf '%s\n' "$tests_exe" > "${dir}/coverage_objects.txt"
json="$(ctest_json "$dir" "$tests_exe" "$net_exe")"

if output="$(bash "$checker" "$dir" "$json" 2>&1)"; then
    fail "an unprofiled test binary was accepted -- this is the defect the gate exists for:"
    printf '%s\n' "$output" >&2
elif ! printf '%s' "$output" | grep -q 'morph_net_tests'; then
    fail "the unprofiled binary was rejected, but the message does not name morph_net_tests:"
    printf '%s\n' "$output" >&2
else
    note "ok: an unprofiled test binary is rejected, and named"
fi

# ── 3. an unprofiled binary with a stated reason -> pass, printing why ───────
dir="$(case_dir excluded)"
tests_exe="$(binary "$dir" morph_tests)"
bench_exe="$(binary "$dir" morph_bench)"
printf '%s\n' "$tests_exe" > "${dir}/coverage_objects.txt"
json="$(ctest_json "$dir" "$tests_exe" "$bench_exe")"

if output="$(bash "$checker" "$dir" "$json" 2>&1)"; then
    if printf '%s' "$output" | grep -q 'morph_bench'; then
        note "ok: a named exclusion is accepted, and its reason reported"
    else
        fail "morph_bench was accepted silently; an exclusion must state itself:"
        printf '%s\n' "$output" >&2
    fi
else
    fail "a documented exclusion (morph_bench) was rejected:"
    printf '%s\n' "$output" >&2
fi

# ── 4. a missing manifest -> fail ────────────────────────────────────────────
# What an unconfigured, wrongly-pointed, or non-coverage build directory looks
# like. Reporting it as clean would be a coverage run over no binaries at all.
dir="$(case_dir no_manifest)"
tests_exe="$(binary "$dir" morph_tests)"
json="$(ctest_json "$dir" "$tests_exe")"

if output="$(bash "$checker" "$dir" "$json" 2>&1)"; then
    fail "a build directory with no coverage_objects.txt was accepted:"
    printf '%s\n' "$output" >&2
else
    note "ok: a missing manifest is rejected"
fi

# ── 5. a manifest naming an unbuilt binary -> fail ───────────────────────────
dir="$(case_dir stale_manifest)"
tests_exe="$(binary "$dir" morph_tests)"
printf '%s\n%s\n' "$tests_exe" "${dir}/ladder_crm_tests" > "${dir}/coverage_objects.txt"
json="$(ctest_json "$dir" "$tests_exe")"

if output="$(bash "$checker" "$dir" "$json" 2>&1)"; then
    fail "a manifest naming a binary the build never produced was accepted:"
    printf '%s\n' "$output" >&2
elif ! printf '%s' "$output" | grep -q 'ladder_crm_tests'; then
    fail "the stale manifest entry was rejected, but the message does not name it:"
    printf '%s\n' "$output" >&2
else
    note "ok: a manifest entry with no binary behind it is rejected, and named"
fi

# ── 6. a wrapper-driven test: the subject is an argument, not command[0] ─────
# examples/forms registers `node <script.mjs> <morph_forms_demo>` and
# `test_repl.sh <morph_forms_demo>`. Checking command[0] alone would check the
# interpreter and the shell script -- neither of which can carry coverage --
# and would let the binary actually under test go unprofiled, which is the very
# thing this gate exists to catch. Scoping to the build tree is what separates
# the two: the wrapper and its script live outside it, the subject inside.
dir="$(case_dir wrapper_driven)"
tests_exe="$(binary "$dir" morph_tests)"
demo_exe="$(binary "$dir" morph_forms_demo_lookalike)"
wrapper="${work}/wrapper.sh"
printf '#!/bin/sh\nexit 0\n' > "$wrapper"
chmod +x "$wrapper"
printf '%s\n' "$tests_exe" > "${dir}/coverage_objects.txt"
cat > "${dir}/ctest.json" <<JSON
{"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":[
  {"name":"morph_tests","command":["${tests_exe}"]},
  {"name":"wrapped","command":["${wrapper}","${work}/driver.mjs","${demo_exe}"]}
]}
JSON

if output="$(bash "$checker" "$dir" "${dir}/ctest.json" 2>&1)"; then
    fail "a wrapper-driven test's unprofiled subject was accepted:"
    printf '%s\n' "$output" >&2
elif ! printf '%s' "$output" | grep -q 'morph_forms_demo_lookalike'; then
    fail "the wrapper-driven subject was not the binary reported:"
    printf '%s\n' "$output" >&2
elif printf '%s' "$output" | grep -q 'wrapper.sh'; then
    fail "the wrapper itself was reported; only build-tree binaries can carry coverage:"
    printf '%s\n' "$output" >&2
else
    note "ok: a wrapper-driven test is checked on its subject, not on its wrapper"
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d self-test check(s) failed\n' "$failures" >&2
    exit 1
fi

printf '\nall self-test checks passed\n'
