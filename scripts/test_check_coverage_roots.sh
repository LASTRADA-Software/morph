#!/usr/bin/env bash
# Usage: bash scripts/test_check_coverage_roots.sh
#
# Self-test for scripts/check_coverage_roots.sh, the gate that fails when the
# coverage mapping names a file outside this checkout (morph#426).
#
# The defect that gate exists to catch is a silence: a compiler cache serves an
# object built in another worktree, its coverage records carry that worktree's
# absolute paths, coverage.sh's relative source filters match none of them, and
# the report is computed over the remainder while every command exits 0. A gate
# against a silence is itself only visible when it is tested, because a gate
# that had gone blind and a tree with nothing wrong in it produce identical
# output.
#
# No build is needed. The gate reads `llvm-cov export -summary-only` JSON, and
# its second argument takes that JSON from a file -- so the fixtures here are
# hand-written documents, which is also what lets this run in drift-guard.yml
# beside the other checkers' self-tests rather than behind a coverage build.
#
# Asserts five directions:
#
#   1. every file under the checkout                   -> pass
#   2. one file under another worktree                 -> fail, naming it
#   3. a sibling directory sharing the root's prefix   -> fail
#   4. a mapping naming no files at all                -> fail, not a vacuous pass
#   5. no manifest / no profile in the build directory -> fail
#   6. a manifest of nothing but blank lines            -> fail, saying so
#   7. a manifest naming a binary that was not built    -> fail, naming it
#
6 and 7 are the standalone-run cases. Run through coverage.sh this gate is
# preceded by check_coverage_objects.sh, which rejects both; run on its own it
# would otherwise abort on `objects[0]: unbound variable` under `set -u`, or
# hand llvm-cov a path that does not exist, and neither message mentions
# coverage.
#
# 3 and 4 matter as much as 2. A prefix comparison written without the trailing
# separator accepts `/repo/morph-wt/372-old/...` as living under
# `/repo/morph-wt/372`, which is the same family of bug as the relative-path
# filter this gate guards; and a gate that passes over an empty file list has
# committed the very failure it was written to detect.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
readonly checker="${repo_root}/scripts/check_coverage_roots.sh"

failures=0

note() { printf '%s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# `llvm-cov export -summary-only` shape, reduced to the one field this gate
# reads. Every other key is omitted deliberately: a fixture that mirrors the
# whole document would have to be updated whenever llvm-cov's schema moves, for
# no gain in what is being asserted.
write_export() {
    local out="$1"; shift
    {
        printf '{"data":[{"files":['
        local first=1
        for name in "$@"; do
            [ "$first" = 1 ] || printf ','
            first=0
            printf '{"filename":"%s","summary":{}}' "$name"
        done
        printf ']}],"version":"2.0.1","type":"llvm.coverage.json.export"}'
    } > "$out"
}

run_checker() {
    (cd "$repo_root" && bash "$checker" "$1" "$2")
}

# 1. Everything under the checkout.
write_export "$tmp/local.json" \
    "${repo_root}/include/morph/core/bridge.hpp" \
    "${repo_root}/examples/crm/src/models/account_model.cpp"
if run_checker "$tmp/nonexistent-build" "$tmp/local.json" > "$tmp/1.out" 2>&1; then
    note "ok 1: a mapping entirely under the checkout passes"
else
    fail "1: a mapping entirely under the checkout was rejected"
    cat "$tmp/1.out" >&2
fi

# 2. One record from another worktree -- the morph#426 shape exactly.
write_export "$tmp/foreign.json" \
    "${repo_root}/include/morph/core/bridge.hpp" \
    "/home/somebody/repo/morph-wt/999/examples/crm/src/models/account_model.cpp"
if run_checker "$tmp/nonexistent-build" "$tmp/foreign.json" > "$tmp/2.out" 2>&1; then
    fail "2: a file from another worktree was accepted"
else
    if grep -q "morph-wt/999/examples/crm/src/models/account_model.cpp" "$tmp/2.out"; then
        note "ok 2: a file from another worktree fails, and is named"
    else
        fail "2: rejected, but did not name the offending path"
        cat "$tmp/2.out" >&2
    fi
fi

# 3. A sibling whose path shares the checkout's prefix as a string.
write_export "$tmp/sibling.json" \
    "${repo_root}/include/morph/core/bridge.hpp" \
    "${repo_root}-old/include/morph/core/bridge.hpp"
if run_checker "$tmp/nonexistent-build" "$tmp/sibling.json" > "$tmp/3.out" 2>&1; then
    fail "3: a sibling directory sharing the checkout's prefix was accepted"
else
    note "ok 3: a sibling sharing the checkout's prefix fails"
fi

# 4. A mapping with no files. Passing here would mean the gate can be satisfied
#    by examining nothing.
write_export "$tmp/empty.json"
if run_checker "$tmp/nonexistent-build" "$tmp/empty.json" > "$tmp/4.out" 2>&1; then
    fail "4: a mapping naming no files was accepted"
else
    note "ok 4: a mapping naming no files fails"
fi

# 5. Neither a manifest nor a profile to read, and no fixture to stand in for
#    them: the gate must say so rather than pass over an absent measurement.
mkdir -p "$tmp/emptybuild"
if run_checker "$tmp/emptybuild" "" > "$tmp/5.out" 2>&1; then
    fail "5: a build directory with no coverage manifest was accepted"
else
    note "ok 5: a build directory with no coverage manifest fails"
fi

# 6. A manifest that is non-empty on disk and names nothing.
mkdir -p "$tmp/blankbuild"
printf '\n\n   \n' > "$tmp/blankbuild/coverage_objects.txt"
printf 'not a real profile' > "$tmp/blankbuild/merged.profdata"
if run_checker "$tmp/blankbuild" "" > "$tmp/6.out" 2>&1; then
    fail "6: a manifest of blank lines was accepted"
elif grep -q "contains no binary paths" "$tmp/6.out"; then
    note "ok 6: a manifest of blank lines fails, and says why"
else
    fail "6: rejected, but not with the coverage diagnostic"
    cat "$tmp/6.out" >&2
fi

# 7. A manifest naming a binary the build did not produce.
mkdir -p "$tmp/stalebuild"
printf '%s\n' "$tmp/stalebuild/morph_gone_tests" > "$tmp/stalebuild/coverage_objects.txt"
printf 'not a real profile' > "$tmp/stalebuild/merged.profdata"
if run_checker "$tmp/stalebuild" "" > "$tmp/7.out" 2>&1; then
    fail "7: a manifest naming an unbuilt binary was accepted"
elif grep -q "morph_gone_tests" "$tmp/7.out"; then
    note "ok 7: a manifest naming an unbuilt binary fails, and names it"
else
    fail "7: rejected, but did not name the missing binary"
    cat "$tmp/7.out" >&2
fi

if [ "$failures" -ne 0 ]; then
    printf '\n%d self-test(s) failed.\n' "$failures" >&2
    exit 1
fi
printf '\nAll scripts/check_coverage_roots.sh self-tests passed.\n'
