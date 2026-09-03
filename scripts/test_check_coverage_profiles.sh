#!/usr/bin/env bash
# Usage: bash scripts/test_check_coverage_profiles.sh
#
# Self-test for scripts/check_coverage_profiles.sh, the profile-discovery gate
# extracted from scripts/coverage.sh for morph#430. A gate nobody tests reports
# green whether or not it still detects anything, and this one guards exactly
# the kind of defect that is a silence: an empty profile set with no failure
# is a report computed over nothing, printed as if it were real.
#
# Needs no build: the gate takes a build-dir-shaped directory and looks for
# *.profraw in it, and a throwaway directory with hand-placed files is
# indistinguishable from one a real coverage run produced.
#
# Asserts three directions:
#
#   1. .profraw files present               -> pass, printing every path
#   2. no .profraw anywhere                 -> fail, naming the build dir
#   3. .profraw present only in a subdir    -> pass (the find is recursive by
#                                               design -- ctest's cwd nests
#                                               "$OUT" under itself, see
#                                               scripts/coverage.sh)
#
# What this file does NOT assert, stated rather than left to be discovered:
# that a *stale* file from a previous run is excluded. check_coverage_profiles.sh
# does not filter on age; morph#430's actual fix is that scripts/coverage.sh
# deletes every path this gate returns once it has merged them, so nothing
# stale is ever left for a later run's find to pick up. That deletion is
# scripts/coverage.sh's own end-of-run `rm -f $PROFILES`, not a claim in this
# script's stdout, and it is what removes the failure mode a directory
# fixture cannot demonstrate on its own.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="${repo_root}/scripts/check_coverage_profiles.sh"

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

mentions() {
    grep -q -- "$1" <<< "$2"
}

# Like mentions(), but for a literal string rather than a BRE pattern --
# used for a filesystem path from `mktemp -d`, which is not this test's own
# text and so is never guaranteed free of BRE metacharacters ((), [], *, .)
# that would make mentions() match something other than the literal path it
# was given. Safe under this repository's own CI (a plain Linux mktemp
# template has none of those), but self-tests are not proof against every
# environment they might run in.
mentions_literal() {
    grep -qF -- "$1" <<< "$2"
}

# ── 1. .profraw files present -> pass, printing every path ──────────────────
dir="$(case_dir present)"
: > "${dir}/a-123.profraw"
: > "${dir}/b-456.profraw"

if output="$(bash "$checker" "$dir" 2>&1)"; then
    if mentions 'a-123\.profraw' "$output" && mentions 'b-456\.profraw' "$output"; then
        note "ok: present .profraw files are found and printed"
    else
        fail "the gate passed but did not print both fixture files:"
        printf '%s\n' "$output" >&2
    fi
else
    fail "a build dir with .profraw files was rejected:"
    printf '%s\n' "$output" >&2
fi

# ── 2. no .profraw anywhere -> fail, naming the build dir ───────────────────
dir="$(case_dir empty)"

if output="$(bash "$checker" "$dir" 2>&1)"; then
    fail "an empty build dir was accepted -- this is the defect the gate exists for:"
    printf '%s\n' "$output" >&2
elif ! mentions_literal "$dir" "$output"; then
    fail "the empty build dir was rejected, but the message does not name it:"
    printf '%s\n' "$output" >&2
else
    note "ok: a build dir with no .profraw is rejected, and named"
fi

# ── 3. .profraw present only in a subdirectory -> pass ───────────────────────
# This is the shape scripts/coverage.sh's own comment documents: ctest's
# working directory nests "$OUT" under itself, so real profile data lands at
# $OUT/tests/build/clang-coverage/*.profraw, not directly under $OUT. The find
# has to be recursive to find that at all; morph#430's fix bounds it by
# deleting what it merges, not by narrowing the search.
dir="$(case_dir nested)"
mkdir -p "${dir}/tests/build/clang-coverage"
: > "${dir}/tests/build/clang-coverage/nested-789.profraw"

if output="$(bash "$checker" "$dir" 2>&1)"; then
    if mentions 'nested-789\.profraw' "$output"; then
        note "ok: a nested .profraw is found"
    else
        fail "the gate passed but did not print the nested fixture file:"
        printf '%s\n' "$output" >&2
    fi
else
    fail "a build dir with only a nested .profraw was rejected:"
    printf '%s\n' "$output" >&2
fi

# ── 4. BUILD_DIR does not exist -> fail, naming it, not a bare `find` crash ──
# Under `set -e`/`pipefail`, `find` on a directory that does not exist exits
# nonzero even with stderr redirected away -- without an explicit existence
# check ahead of it, `profiles=$(find ... | tr ...)` would abort the whole
# script right there, silently: no stdout, no "No .profraw files found"
# message, just a bare exit 1 indistinguishable from a crash.
dir="${work}/does_not_exist"
rm -rf "$dir"

if output="$(bash "$checker" "$dir" 2>&1)"; then
    fail "a nonexistent build dir was accepted:"
    printf '%s\n' "$output" >&2
elif [ -z "$output" ]; then
    fail "a nonexistent build dir failed with no message at all -- this is the" \
         "silent-abort defect this case exists to catch"
elif ! mentions_literal "$dir" "$output"; then
    fail "a nonexistent build dir was rejected, but the message does not name it:"
    printf '%s\n' "$output" >&2
else
    note "ok: a nonexistent build dir fails with a message naming it, not a bare crash"
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d self-test check(s) failed\n' "$failures" >&2
    exit 1
fi

printf '\nall self-test checks passed\n'
