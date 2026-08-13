#!/usr/bin/env bash
# Usage: bash scripts/test_check_test_type_names.sh
#
# Self-test for scripts/check_test_type_names.sh, the CI gate for the
# file-scope struct/class name collision bug class (issue #84). A lint gate
# that is never itself tested reports green whether or not it still detects
# anything, so this asserts both directions against the fixtures in
# tests/lint/test_type_names/:
#
#   valid/    -- one directory of files with no collision; must pass as a set
#   invalid/  -- each subdirectory holds a colliding pair; each must be
#                rejected on its own
#
# Unlike scripts/test_check_deprecated_markers.sh's fixtures (independent
# per-file cases), this bug class is inherently cross-file -- a single file
# cannot demonstrate a name collision by itself -- so each invalid/ case is a
# whole subdirectory checked as a unit, not a single file dropped into a
# scratch directory.
set -euo pipefail

readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly checker="${repo_root}/scripts/check_test_type_names.sh"
readonly fixtures="${repo_root}/tests/lint/test_type_names"

failures=0

note() { printf '%s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; failures=$((failures + 1)); }

# ── valid/ must pass ─────────────────────────────────────────────────────────
if output="$(bash "$checker" "${fixtures}/valid" 2>&1)"; then
    note "ok: valid fixtures accepted"
else
    fail "valid fixtures were rejected by the checker:"
    printf '%s\n' "$output" >&2
fi

# ── every invalid/ subdirectory must be rejected on its own ─────────────────
shopt -s nullglob
invalid_dirs=("${fixtures}"/invalid/*/)
shopt -u nullglob

if [ "${#invalid_dirs[@]}" -eq 0 ]; then
    fail "no fixtures found in ${fixtures}/invalid -- the self-test would pass vacuously"
fi

for dir in "${invalid_dirs[@]}"; do
    name="$(basename "$dir")"
    if bash "$checker" "$dir" >/dev/null 2>&1; then
        fail "invalid fixture ${name} was accepted; the checker no longer detects it"
    else
        note "ok: invalid fixture ${name} rejected"
    fi
done

if [ "$failures" -ne 0 ]; then
    printf '\n%s self-test check(s) failed\n' "$failures" >&2
    exit 1
fi

note "all test-type-name checker self-tests passed"
