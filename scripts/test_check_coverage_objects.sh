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
# Asserts eight directions:
#
#   1. every ctest binary profiled                     -> pass
#   2. an unprofiled binary with no stated reason      -> fail, naming it
#   3. an unprofiled binary that IS excluded by name   -> pass, printing why
#   4. a missing/empty manifest                        -> fail
#   5. a manifest naming a binary that was not built   -> fail
#   6. a wrapper-driven test (subject is an argument)  -> fail on the subject
#   7. ctest listing no tests at all                   -> fail, not a vacuous pass
#   8. an unfixed GAP entry                            -> pass, but as a warning
#
# 3, 5 and 7 matter as much as 2. Without 3 the gate could be "fixed" by making
# every exclusion fatal, which would simply move the pressure onto deleting the
# check; without 5 a manifest could name a target the build silently dropped
# and the report would be computed over the survivors -- the same shrinking
# denominator by another route; and without 7 the gate could satisfy every
# other case by examining nothing, which is the failure it exists to detect
# committed by the detector.
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

manifest() {
    local dir="$1"
    shift
    printf '%s\n' "$@" > "${dir}/coverage_objects.txt"
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

# `grep <<<` rather than `printf '%s' "$output" | grep`: under `set -o pipefail`
# a `grep -q` that matches early can close the pipe under printf, and the
# pipeline then reports printf's SIGPIPE status rather than grep's match. Every
# assertion below inverts a grep, so that would turn a correct gate into a
# reported self-test failure, intermittently and only on longer messages.
mentions() {
    grep -q -- "$1" <<< "$2"
}

# ── 1. every ctest binary profiled -> pass ───────────────────────────────────
dir="$(case_dir all_profiled)"
tests_exe="$(binary "$dir" morph_tests)"
net_exe="$(binary "$dir" morph_net_tests)"
manifest "$dir" "$tests_exe" "$net_exe"
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
manifest "$dir" "$tests_exe"
json="$(ctest_json "$dir" "$tests_exe" "$net_exe")"

if output="$(bash "$checker" "$dir" "$json" 2>&1)"; then
    fail "an unprofiled test binary was accepted -- this is the defect the gate exists for:"
    printf '%s\n' "$output" >&2
elif ! mentions 'morph_net_tests' "$output"; then
    fail "the unprofiled binary was rejected, but the message does not name morph_net_tests:"
    printf '%s\n' "$output" >&2
else
    note "ok: an unprofiled test binary is rejected, and named"
fi

# ── 3. an unprofiled binary with a stated reason -> pass, printing why ───────
dir="$(case_dir excluded)"
tests_exe="$(binary "$dir" morph_tests)"
bench_exe="$(binary "$dir" morph_bench)"
manifest "$dir" "$tests_exe"
json="$(ctest_json "$dir" "$tests_exe" "$bench_exe")"

if output="$(bash "$checker" "$dir" "$json" 2>&1)"; then
    if mentions 'morph_bench' "$output"; then
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
elif ! mentions 'ladder_crm_tests' "$output"; then
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
elif ! mentions 'morph_forms_demo_lookalike' "$output"; then
    fail "the wrapper-driven subject was not the binary reported:"
    printf '%s\n' "$output" >&2
elif mentions 'wrapper[.]sh' "$output"; then
    fail "the wrapper itself was reported; only build-tree binaries can carry coverage:"
    printf '%s\n' "$output" >&2
else
    note "ok: a wrapper-driven test is checked on its subject, not on its wrapper"
fi

# ── 7. ctest lists no tests at all -> fail, not a vacuous pass ──────────────
# Every check this gate makes is a statement about the binaries ctest runs, so
# an empty test list satisfies all of them by having nothing to satisfy. That
# is what an unconfigured build directory, or a test registration that silently
# produced nothing, looks like from here -- and reporting it as clean would be
# this gate committing the failure it exists to detect. scripts/
# check_rung_filters.sh guards its own end the same way.
dir="$(case_dir empty_ctest)"
tests_exe="$(binary "$dir" morph_tests)"
manifest "$dir" "$tests_exe"
printf '{"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":[]}' > "${dir}/ctest.json"

if output="$(bash "$checker" "$dir" "${dir}/ctest.json" 2>&1)"; then
    fail "an empty ctest test list was reported as clean -- the gate verified nothing:"
    printf '%s\n' "$output" >&2
elif ! mentions 'no test binaries' "$output"; then
    fail "the empty test list was rejected, but not for being empty:"
    printf '%s\n' "$output" >&2
else
    note "ok: an empty ctest test list is rejected rather than passing vacuously"
fi

# ── 8. a GAP must not read as a decision ────────────────────────────────────
# The exclusion table holds two unlike things: decisions (a benchmark is not a
# test) and defects nobody has fixed yet. If both print "deliberately not
# profiled", the second stops being legible as a defect and the table becomes
# the suppression list this gate was written against. morph_concepts_tests is a
# real, currently-unfixed gap; assert it announces itself as one.
dir="$(case_dir gap_wording)"
tests_exe="$(binary "$dir" morph_tests)"
gap_exe="$(binary "$dir" morph_concepts_tests)"
manifest "$dir" "$tests_exe"
json="$(ctest_json "$dir" "$tests_exe" "$gap_exe")"

if output="$(bash "$checker" "$dir" "$json" 2>&1)"; then
    if ! mentions '^warning: morph_concepts_tests' "$output"; then
        fail "a GAP entry did not announce itself as one:"
        printf '%s\n' "$output" >&2
    elif mentions 'deliberately not profiled -- GAP' "$output"; then
        fail "a GAP entry printed as a deliberate exclusion:"
        printf '%s\n' "$output" >&2
    elif ! mentions 'unfixed gap' "$output"; then
        fail "the summary line does not count the unfixed gaps:"
        printf '%s\n' "$output" >&2
    else
        note "ok: an unfixed gap reads as a warning, not as a decision"
    fi
else
    fail "a listed gap was rejected outright; the leg must stay green while it is carried:"
    printf '%s\n' "$output" >&2
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d self-test check(s) failed\n' "$failures" >&2
    exit 1
fi

printf '\nall self-test checks passed\n'
