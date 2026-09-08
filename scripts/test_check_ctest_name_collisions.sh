#!/usr/bin/env bash
# Usage: bash scripts/test_check_ctest_name_collisions.sh
#
# Self-test for scripts/check_ctest_name_collisions.sh, the gate that fails
# when two ctest tests in one build tree share a name (morph#464).
#
# A lint gate nobody tests reports green whether or not it still detects
# anything, and this one guards a defect that is *already* silent: two
# same-named ctest entries make `ctest -L <label>` run the wrong binary
# alongside the right one, and an over-selecting filter passes exactly as
# loudly as a correct one. A gate that had gone blind would be
# indistinguishable from a gate finding nothing wrong.
#
# Asserts six directions:
#
#   1. all names distinct                          -> pass
#   2. one name registered twice                   -> fail, naming it and both binaries
#   3. one name registered three times             -> fail, reporting all three
#   4. ctest listing no tests at all               -> fail, not a vacuous pass
#   5. output that is not a ctest JSON document    -> fail, not a vacuous pass
#   6. a REAL two-directory CMake project          -> fail, driving ctest itself
#
# 4 and 5 matter as much as 2: without them the gate could satisfy every other
# case by examining nothing, which is the failure it exists to detect committed
# by the detector. 6 matters because 1-5 all feed the checker a hand-written
# document: it is the case that shows CMake really does accept the duplicate
# (`add_test` refuses one only within a single directory), that the checker's
# own `ctest --show-only=json-v1` invocation works, and therefore that the
# other five are testing a gate wired to something real.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="${repo_root}/scripts/check_ctest_name_collisions.sh"

failures=0

note() { printf '%s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

case_dir() {
    local dir="${work}/$1"
    rm -rf "$dir"
    mkdir -p "$dir"
    printf '%s' "$dir"
}

# `ctest_json <dir> <name>=<binary> ...` writes a `ctest --show-only=json-v1`
# document registering each name against each binary, in order.
ctest_json() {
    local dir="$1"
    shift
    {
        printf '{"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":['
        local first=1 entry name binary
        for entry in "$@"; do
            name="${entry%%=*}"
            binary="${entry#*=}"
            [ "$first" -eq 1 ] || printf ','
            first=0
            printf '{"name":"%s","command":["%s","%s"]}' "$name" "$binary" "$name"
        done
        printf ']}'
    } > "${dir}/ctest.json"
    printf '%s' "${dir}/ctest.json"
}

# `grep <<<` rather than a pipe: see scripts/test_check_coverage_objects.sh's
# note on SIGPIPE turning a correct gate into a reported self-test failure.
mentions() {
    grep -q -- "$1" <<< "$2"
}

# ── 1. all names distinct -> pass ────────────────────────────────────────────
dir="$(case_dir distinct)"
json="$(ctest_json "$dir" \
    "crm.CreateAccount rejects an empty name=/b/ladder_crm_tests" \
    "lims.CreateAccount rejects an empty name=/b/ladder_lims_tests")"
if output="$(bash "$checker" "$dir" "$json" 2>&1)"; then
    note "ok: a build tree whose ctest names are all distinct is accepted"
else
    fail "a build tree with distinct ctest names was rejected:"
    printf '%s\n' "$output" >&2
fi

# ── 2. one name registered twice -> fail, naming it and both binaries ────────
# This is morph#464 itself, in miniature: two rung binaries defining a
# TEST_CASE of the same name, each registered under that bare name.
dir="$(case_dir duplicate_pair)"
json="$(ctest_json "$dir" \
    "CreateAccount rejects an empty name=/b/ladder_crm_tests" \
    "CreateAccount rejects an empty name=/b/ladder_lims_tests" \
    "crm.something else=/b/ladder_crm_tests")"
if output="$(bash "$checker" "$dir" "$json" 2>&1)"; then
    fail "a duplicated ctest name was accepted -- the gate detects nothing:"
    printf '%s\n' "$output" >&2
elif ! mentions "CreateAccount rejects an empty name" "$output"; then
    fail "the duplicate was rejected without naming it:"
    printf '%s\n' "$output" >&2
elif ! mentions "ladder_crm_tests" "$output" || ! mentions "ladder_lims_tests" "$output"; then
    fail "the duplicate was rejected without naming both binaries, so the report does not say where to look:"
    printf '%s\n' "$output" >&2
else
    note "ok: a name registered by two binaries is rejected, naming the name and both binaries"
fi

# ── 3. one name registered three times -> fail, reporting all three ──────────
dir="$(case_dir duplicate_triple)"
json="$(ctest_json "$dir" \
    "Journey: an account is created=/b/ladder_crm_tests" \
    "Journey: an account is created=/b/ladder_lims_tests" \
    "Journey: an account is created=/b/ladder_ledger_tests")"
if output="$(bash "$checker" "$dir" "$json" 2>&1)"; then
    fail "a name registered three times was accepted:"
    printf '%s\n' "$output" >&2
elif ! mentions "x3" "$output"; then
    fail "a name registered three times was rejected but not reported as three:"
    printf '%s\n' "$output" >&2
else
    note "ok: a name registered three times is rejected, reporting the count"
fi

# ── 4. ctest listing no tests at all -> fail, not a vacuous pass ─────────────
dir="$(case_dir empty)"
printf '{"kind":"ctestInfo","version":{"major":1,"minor":0},"tests":[]}' > "${dir}/ctest.json"
if output="$(bash "$checker" "$dir" "${dir}/ctest.json" 2>&1)"; then
    fail "an empty ctest test list was reported as clean -- the gate verified nothing:"
    printf '%s\n' "$output" >&2
elif ! mentions "no tests at all" "$output"; then
    fail "an empty ctest test list was rejected for the wrong reason:"
    printf '%s\n' "$output" >&2
else
    note "ok: an empty ctest test list is rejected rather than passing vacuously"
fi

# ── 5. output that is not a ctest document -> fail, not a vacuous pass ───────
# A discovery script writing to stdout, or a configure error, lands here.
dir="$(case_dir malformed)"
printf 'CMake Error: something went wrong\n' > "${dir}/ctest.json"
if output="$(bash "$checker" "$dir" "${dir}/ctest.json" 2>&1)"; then
    fail "unparseable ctest output was reported as clean:"
    printf '%s\n' "$output" >&2
elif ! mentions "could not read the test list" "$output"; then
    fail "unparseable ctest output was rejected for the wrong reason:"
    printf '%s\n' "$output" >&2
else
    note "ok: output that is not a ctest JSON document is rejected"
fi

# ── 6. a real two-directory CMake project -> fail, driving ctest itself ──────
# Everything above hands the checker a document. This case builds a real build
# tree and lets the checker run `ctest --show-only=json-v1` against it, which
# is how it is invoked in CI. It also demonstrates the premise: CMake rejects a
# duplicate `add_test` NAME within one directory and accepts it across two,
# which is exactly the shape two rung test binaries have.
if ! command -v cmake > /dev/null 2>&1; then
    fail "cmake is not on PATH, so the end-to-end case could not run -- five of the six directions above feed the checker a hand-written document and would pass with the ctest invocation itself broken"
else
    project="$(case_dir real_project)/src"
    mkdir -p "${project}/one" "${project}/two"
    cat > "${project}/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.20)
project(ctest_name_collision_fixture NONE)
enable_testing()
add_subdirectory(one)
add_subdirectory(two)
CMAKE
    cat > "${project}/one/CMakeLists.txt" <<'CMAKE'
add_test(NAME "shared case name" COMMAND "${CMAKE_COMMAND}" -E true)
add_test(NAME "one only" COMMAND "${CMAKE_COMMAND}" -E true)
CMAKE
    cat > "${project}/two/CMakeLists.txt" <<'CMAKE'
add_test(NAME "shared case name" COMMAND "${CMAKE_COMMAND}" -E true)
add_test(NAME "two only" COMMAND "${CMAKE_COMMAND}" -E true)
CMAKE
    build="${work}/real_project/build"
    if ! configure_output="$(cmake -S "$project" -B "$build" 2>&1)"; then
        fail "the fixture project did not configure, so the end-to-end case proved nothing:"
        printf '%s\n' "$configure_output" >&2
    elif output="$(bash "$checker" "$build" 2>&1)"; then
        fail "a real build tree registering one name from two directories was accepted:"
        printf '%s\n' "$output" >&2
    elif ! mentions "shared case name" "$output"; then
        fail "the real duplicate was rejected without naming it:"
        printf '%s\n' "$output" >&2
    else
        note "ok: a real build tree registering one name from two directories is rejected"
        # And the same tree with the collision removed must pass, or the case
        # above would be satisfied by a gate that rejects every build tree.
        cat > "${project}/two/CMakeLists.txt" <<'CMAKE'
add_test(NAME "two's own case name" COMMAND "${CMAKE_COMMAND}" -E true)
add_test(NAME "two only" COMMAND "${CMAKE_COMMAND}" -E true)
CMAKE
        if ! cmake -S "$project" -B "$build" > /dev/null 2>&1; then
            fail "the de-collided fixture project did not reconfigure"
        elif output="$(bash "$checker" "$build" 2>&1)"; then
            note "ok: the same tree passes once the two names differ"
        else
            fail "a real build tree with distinct names was rejected:"
            printf '%s\n' "$output" >&2
        fi
    fi
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d self-test case(s) failed.\n' "$failures" >&2
    exit 1
fi
printf '\nAll scripts/check_ctest_name_collisions.sh self-tests passed.\n'
